#include <algorithm>
#include <boost/asio/spawn.hpp>
#include <random>
#include <saber/player.hpp>

namespace saber {

static std::mt19937 &rng() {
	static thread_local std::mt19937 rng{std::random_device{}()};
	return rng;
}

Player::Player(asio::any_io_executor ex, Connector connector)
	: m_connector(std::move(connector)),
	  m_stream_mgr(ex),
	  m_playback_ctrl(std::move(ex), m_stream_mgr, m_state_mgr, m_persist_mgr) {
}

bool Player::has_connection(ekizu::Snowflake guild_id) const {
	return m_state_mgr.has_connection(guild_id);
}

bool Player::has_queue(ekizu::Snowflake guild_id) const {
	return m_state_mgr.has_queue(guild_id);
}

Result<ekizu::Snowflake> Player::voice_channel_id(
	ekizu::Snowflake guild_id) const {
	const auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->last_voice_channel) {
		return boost::system::errc::operation_not_permitted;
	}
	return *state->last_voice_channel;
}

Result<GuildQueue *> Player::queue(ekizu::Snowflake guild_id) {
	SABER_TRY(auto *state, get_state(guild_id));
	return state->queue.get();
}

Result<bool> Player::connect(ekizu::Snowflake guild_id,
							 ekizu::Snowflake channel_id,
							 const asio::yield_context &yield) {
	if (m_state_mgr.has_connection(guild_id)) { return false; }

	auto *state = m_state_mgr.get_or_create(guild_id);
	state->last_voice_channel = channel_id;

	log<ekizu::LogLevel::Info>("Connecting to guild {}", guild_id);

	SABER_TRY(auto config, m_connector(guild_id, channel_id, yield));

	auto log_cb = [log = m_logger](ekizu::Log l) {
		if (log) { log(std::move(l)); }
	};

	if (!state->queue) { state->queue = std::make_shared<GuildQueue>(log_cb); }
	state->connection = std::make_shared<PlayerConnection>(
		std::move(config), state->queue.get(), log_cb);

	m_playback_ctrl.mark_persist_dirty(guild_id);
	return true;
}

Result<> Player::disconnect(ekizu::Snowflake guild_id, bool clear_queue) {
	auto *state = m_state_mgr.get(guild_id);
	if (state == nullptr) {
		return boost::system::errc::operation_not_permitted;
	}

	// Stop the current stream and wake any paused sender.
	state->cancel_stream();

	if (state->connection) {
		state->connection->shutdown();
		state->connection.reset();
	}

	if (clear_queue && state->queue) {
		state->queue->tracks.clear();
		state->queue->current_track_id.reset();
		state->queue->last_track_id = {};
	}

	state->playback.running = false;
	state->playback.paused = false;
	state->playback.pause_started.reset();
	state->playback.paused_total = std::chrono::steady_clock::duration::zero();
	state->playback.active_stream.reset();

	m_playback_ctrl.mark_persist_dirty(guild_id);
	return outcome::success();
}

Result<> Player::on_voice_state_update(ekizu::Snowflake guild_id,
									   const ekizu::VoiceState &voice_state) {
	auto *state = m_state_mgr.get(guild_id);
	if (state == nullptr) {
		return boost::system::errc::operation_not_permitted;
	}

	if (voice_state.channel_id) {
		const auto new_channel = *voice_state.channel_id;

		// Channel move: pause transport while waiting for VOICE_SERVER_UPDATE,
		// but do NOT reset playback/stream state.
		if (state->last_voice_channel &&
			(*state->last_voice_channel != new_channel)) {
			log<ekizu::LogLevel::Info>("Voice channel move: {} -> {}",
									   *state->last_voice_channel, new_channel);

			state->last_voice_channel = new_channel;

			if (state->connection) {
				// Transport-level pause only (do not touch
				// state->playback.paused).
				(void)state->connection->pause();
			}

			m_playback_ctrl.mark_persist_dirty(guild_id);
			return outcome::success();
		}

		state->last_voice_channel = new_channel;
		m_playback_ctrl.mark_persist_dirty(guild_id);
		return outcome::success();
	}

	// Bot disconnected from voice: drop the connection and stop playback,
	// but keep queue by default.
	return disconnect(guild_id, /*clear_queue=*/false);
}

Result<> Player::on_voice_server_update(ekizu::Snowflake guild_id,
										ekizu::VoiceConnectionConfig config) {
	auto *state = m_state_mgr.get(guild_id);
	if (state == nullptr) {
		return boost::system::errc::operation_not_permitted;
	}

	if (!state->connection) {
		// Nothing to rebind yet.
		return outcome::success();
	}

	SABER_TRY(state->connection->rebind_transport(std::move(config)));

	// If playback is running and the user didn't explicitly pause,
	// resume transport sending.
	if (state->playback.running && !state->playback.paused) {
		return state->connection->resume();
	}

	return outcome::success();
}

Result<Track> Player::play(ekizu::Snowflake guild_id, std::string_view query,
						   ekizu::Snowflake requester_id,
						   const asio::yield_context &yield) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->connection || !state->queue) {
		return boost::system::errc::operation_not_permitted;
	}

	SABER_TRY(auto meta, m_stream_mgr.resolve_metadata(query, yield));

	auto track = state->queue->add_track({0, requester_id, std::move(meta)});

	log<ekizu::LogLevel::Info>(
		"Enqueued track {} ({})", track.id, track.metadata.title);

	if (m_on_event) {
		m_on_event({PlaybackEventType::TrackEnqueued, guild_id, track, {}});
	}

	ensure_playback(guild_id, state, yield);
	m_playback_ctrl.mark_persist_dirty(guild_id);

	return track;
}

Result<bool> Player::skip(ekizu::Snowflake guild_id) {
	return modify_state(guild_id, [](GuildState *state, GuildQueue *q) {
		if (!q->current_track_id) { return false; }

		auto &tracks = q->tracks;
		auto it = std::find_if(
			tracks.begin(), tracks.end(),
			[id = *q->current_track_id](const auto &t) { return t.id == id; });

		if (it == tracks.end()) { return false; }

		auto next = std::next(it);
		if (next == tracks.end()) { return false; }

		q->current_track_id = next->id;
		state->cancel_stream();
		return true;
	});
}

Result<bool> Player::previous(ekizu::Snowflake guild_id) {
	return modify_state(guild_id, [](GuildState *state, GuildQueue *q) {
		if (!q->current_track_id) { return false; }

		if (state->elapsed() <
			PlaybackController::k_previous_restart_threshold) {
			auto &tracks = q->tracks;
			auto it = std::find_if(tracks.begin(), tracks.end(),
								   [id = *q->current_track_id](const auto &t) {
									   return t.id == id;
								   });

			if (it != tracks.end() && it != tracks.begin()) {
				q->current_track_id = std::prev(it)->id;
			}
		}

		state->cancel_stream();
		return true;
	});
}

Result<bool> Player::skip_to(ekizu::Snowflake guild_id, uint64_t track_id) {
	return modify_state(guild_id, [track_id](GuildState *state, GuildQueue *q) {
		if (!q->current_track_id) { return false; }
		if (*q->current_track_id == track_id) { return false; }

		if (!q->skip(track_id)) { return false; }

		state->cancel_stream();
		return true;
	});
}

Result<bool> Player::shuffle(ekizu::Snowflake guild_id) {
	return modify_state(guild_id, [](GuildState *, GuildQueue *q) {
		auto &tracks = q->tracks;
		if (tracks.size() < 2) { return false; }

		auto start_it = tracks.begin();
		if (q->current_track_id) {
			auto it = std::find_if(tracks.begin(), tracks.end(),
								   [id = *q->current_track_id](const auto &t) {
									   return t.id == id;
								   });

			if (it == tracks.end()) { return false; }
			start_it = std::next(it);
		}

		if (std::distance(start_it, tracks.end()) < 2) { return false; }

		std::shuffle(start_it, tracks.end(), rng());
		return true;
	});
}

Result<bool> Player::clear(ekizu::Snowflake guild_id) {
	return modify_state(guild_id, [](GuildState *, GuildQueue *q) {
		auto &tracks = q->tracks;
		if (tracks.empty()) { return false; }

		auto start_it = tracks.begin();
		if (q->current_track_id) {
			auto it = std::find_if(tracks.begin(), tracks.end(),
								   [id = *q->current_track_id](const auto &t) {
									   return t.id == id;
								   });
			if (it == tracks.end()) { return false; }
			start_it = std::next(it);
		}

		if (start_it == tracks.end()) { return false; }

		tracks.erase(start_it, tracks.end());
		return true;
	});
}

Result<bool> Player::is_paused(ekizu::Snowflake guild_id) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->connection) {
		return boost::system::errc::no_such_file_or_directory;
	}
	return state->playback.paused;
}

Result<> Player::pause(ekizu::Snowflake guild_id) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->connection) {
		return boost::system::errc::no_such_file_or_directory;
	}

	if (state->playback.running && !state->playback.paused) {
		state->playback.paused = true;
		state->playback.pause_started = std::chrono::steady_clock::now();
		m_playback_ctrl.mark_persist_dirty(guild_id);
	}

	return state->connection->pause();
}

Result<> Player::resume(ekizu::Snowflake guild_id) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->connection) {
		return boost::system::errc::no_such_file_or_directory;
	}

	if (state->playback.running && state->playback.paused) {
		if (state->playback.pause_started) {
			state->playback.paused_total += (std::chrono::steady_clock::now() -
											 *state->playback.pause_started);
			state->playback.pause_started.reset();
		}

		state->playback.paused = false;
		m_playback_ctrl.mark_persist_dirty(guild_id);
	}

	return state->connection->resume();
}

// Audio settings getters/setters
#define AUDIO_SETTING_IMPL(name, type)                                   \
	Result<type> Player::name(ekizu::Snowflake guild_id) const {         \
		auto *state = m_state_mgr.get(guild_id);                         \
		if (!state) return boost::system::errc::operation_not_permitted; \
		return state->audio.name;                                        \
	}                                                                    \
	Result<> Player::set_##name(ekizu::Snowflake guild_id, type value) { \
		auto *state = m_state_mgr.get(guild_id);                         \
		if (!state) return boost::system::errc::operation_not_permitted; \
		state->audio.name = value;                                       \
		state->audio.clamp();                                            \
		m_playback_ctrl.mark_persist_dirty(guild_id);                    \
		return outcome::success();                                       \
	}

AUDIO_SETTING_IMPL(volume, float)
AUDIO_SETTING_IMPL(fade_ms, int)
AUDIO_SETTING_IMPL(limiter_enabled, bool)
AUDIO_SETTING_IMPL(limiter_threshold_db, float)
AUDIO_SETTING_IMPL(limiter_release_ms, int)

Result<> Player::restore_all(const asio::yield_context &yield) {
	SABER_TRY(auto states, m_persist_mgr.restore_all(yield));

	for (auto &data : states) {
		auto *state = m_state_mgr.get_or_create(data.guild_id);
		state->last_voice_channel = data.voice_channel_id;
		state->audio = data.audio;
		state->playback.paused = data.paused;

		if (!state->queue) {
			state->queue =
				std::make_shared<GuildQueue>([log = m_logger](ekizu::Log l) {
					if (log) { log(std::move(l)); }
				});
		}

		state->queue->current_track_id = data.queue.current_track_id;
		state->queue->last_track_id = data.queue.last_track_id;
		state->queue->tracks = std::move(data.queue.tracks);

		if (state->last_voice_channel) {
			SABER_TRY(
				connect(data.guild_id, *state->last_voice_channel, yield));

			if (state->connection && state->queue->current_track_id) {
				ensure_playback(data.guild_id, state, yield);
			}
		}
	}

	return outcome::success();
}

void Player::shutdown() {
	log<ekizu::LogLevel::Info>("Shutting down player...");

	for (auto guild_id : m_state_mgr.all_guilds()) {
		if (auto *state = m_state_mgr.get(guild_id)) {
			state->cancel_stream();
			if (state->connection) { state->connection->shutdown(); }
		}
	}

	m_state_mgr.clear();
	m_persist_mgr.disable();

	log<ekizu::LogLevel::Info>("Player shutdown complete");
}

}  // namespace saber