#include <boost/asio/spawn.hpp>
#include <saber/player.hpp>

namespace saber {

Player::Player(Connector connector) : m_connector(std::move(connector)) {}

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
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->queue) {
		return boost::system::errc::operation_not_permitted;
	}
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

	if (!state->queue) {
		state->queue = std::make_unique<GuildQueue>([this](ekizu::Log l) {
			if (m_logger) { m_logger(std::move(l)); }
		});
	}

	state->connection = std::make_unique<PlayerConnection>(
		*config, state->queue.get(), [this](ekizu::Log l) {
			if (m_logger) { m_logger(std::move(l)); }
		});

	m_playback_ctrl.mark_persist_dirty(guild_id);
	return true;
}

Result<Track> Player::play(ekizu::Snowflake guild_id, std::string_view query,
						   ekizu::Snowflake requester_id,
						   const asio::yield_context &yield) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->connection || !state->queue) {
		return boost::system::errc::operation_not_permitted;
	}

	SABER_TRY(auto meta, m_stream_mgr.resolve_metadata(query, yield));

	auto track = state->queue->add_track(
		{0,
		 requester_id,
		 std::move(meta.webpage_url),
		 std::move(meta.title),
		 {}});

	log<ekizu::LogLevel::Info>("Enqueued track {} ({})", track.id, track.title);

	if (!state->playback.running) {
		state->playback.running = true;
		asio::spawn(
			yield,
			[this, guild_id](const auto &y) {
				auto res = m_playback_ctrl.start_loop(guild_id, y);
				if (res.has_error() &&
					res.error() != boost::system::errc::operation_canceled) {
					log<ekizu::LogLevel::Error>(
						"Playback loop failed: {}", res.error().message());
				}
			},
			asio::detached);
	}

	m_playback_ctrl.mark_persist_dirty(guild_id);
	return track;
}

Result<bool> Player::skip(ekizu::Snowflake guild_id) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->queue ||
		!state->queue->current_track_id) {
		return boost::system::errc::operation_not_permitted;
	}

	auto it = std::find_if(
		state->queue->tracks.begin(), state->queue->tracks.end(),
		[id = *state->queue->current_track_id](const Track &t) {
			return t.id == id;
		});

	if (it == state->queue->tracks.end()) { return false; }

	auto next = std::next(it);
	if (next == state->queue->tracks.end()) { return false; }

	state->queue->current_track_id = next->id;
	m_playback_ctrl.mark_persist_dirty(guild_id);
	state->cancel_stream();
	return true;
}

Result<bool> Player::previous(ekizu::Snowflake guild_id) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->queue ||
		!state->queue->current_track_id) {
		return boost::system::errc::operation_not_permitted;
	}

	const auto elapsed = state->elapsed();

	if (elapsed < PlaybackController::k_previous_restart_threshold) {
		auto it = std::find_if(
			state->queue->tracks.begin(), state->queue->tracks.end(),
			[id = *state->queue->current_track_id](const Track &t) {
				return t.id == id;
			});

		if (it != state->queue->tracks.end() &&
			it != state->queue->tracks.begin()) {
			state->queue->current_track_id = std::prev(it)->id;
		}
	}

	state->cancel_stream();
	return true;
}

Result<> Player::pause(ekizu::Snowflake guild_id) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->connection) {
		return boost::system::errc::no_such_file_or_directory;
	}

	if (state->playback.running && !state->playback.paused) {
		state->playback.paused = true;
		state->playback.pause_started = std::chrono::steady_clock::now();
	}

	m_playback_ctrl.mark_persist_dirty(guild_id);
	return state->connection->pause();
}

Result<> Player::resume(ekizu::Snowflake guild_id) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->connection) {
		return boost::system::errc::no_such_file_or_directory;
	}

	if (state->playback.running && state->playback.paused) {
		if (state->playback.pause_started) {
			state->playback.paused_total += std::chrono::steady_clock::now() -
											*state->playback.pause_started;
		}
		state->playback.pause_started.reset();
		state->playback.paused = false;
	}

	m_playback_ctrl.mark_persist_dirty(guild_id);
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
			state->queue = std::make_unique<GuildQueue>([this](ekizu::Log l) {
				if (m_logger) { m_logger(std::move(l)); }
			});
		}

		state->queue->current_track_id = data.queue.current_track_id;
		state->queue->last_track_id = data.queue.last_track_id;
		state->queue->tracks = std::move(data.queue.tracks);

		if (state->last_voice_channel) {
			(void)connect(data.guild_id, *state->last_voice_channel, yield);
		}

		if (state->connection && state->queue->current_track_id) {
			if (!state->playback.running) {
				state->playback.running = true;
				asio::spawn(
					yield,
					[this, guild_id = data.guild_id](const auto &y) {
						(void)m_playback_ctrl.start_loop(guild_id, y);
					},
					asio::detached);
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