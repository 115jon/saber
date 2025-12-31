#include <algorithm>
#include <boost/asio/spawn.hpp>
#include <boost/scope_exit.hpp>
#include <chrono>
#include <saber/playback_controller.hpp>
#include <saber/stream_manager.hpp>

namespace saber {

void PlaybackController::mark_persist_dirty(ekizu::Snowflake guild_id) {
	auto *state = m_state_mgr.get(guild_id);
	if (state == nullptr) { return; }

	GuildPersistData data{
		guild_id,
		state->last_voice_channel,
		state->playback.paused,
		state->audio,
		{}};

	if (state->queue) {
		data.queue.current_track_id = state->queue->current_track_id;
		data.queue.last_track_id = state->queue->last_track_id;
		data.queue.tracks = state->queue->tracks;
	}

	m_persist_mgr.save_async(guild_id, data);
}

Result<> PlaybackController::play_track(GuildState &state, const Track &track,
										const asio::yield_context &yield) {
	if (track.webpage_url.empty()) {
		return boost::system::errc::invalid_argument;
	}

	// Check if we can reuse the existing stream
	// ONLY if it's for the SAME track AND track_id is still set
	const bool can_reuse_stream =
		state.playback.active_stream && state.playback.active_stream_track_id &&
		*state.playback.active_stream_track_id == track.id;

	if (can_reuse_stream) {
		log<ekizu::LogLevel::Info>(
			"Reusing existing stream for track {} (frames_sent={})", track.id,
			state.playback.frames_sent.load());
	} else {
		// Clear old stream if it exists but doesn't match
		if (state.playback.active_stream) {
			log<ekizu::LogLevel::Info>(
				"Clearing old stream (was track {}, now track {})",
				state.playback.active_stream_track_id.value_or(0), track.id);
			// Just reset the shared_ptr - cancel() was already called
			state.playback.active_stream.reset();
			state.playback.active_stream_track_id.reset();
		}

		// NEW TRACK: Reset opus encoder to prevent bleed from previous track
		m_audio_proc.reset_encoder();

		// Resolve stream URL
		SABER_TRY(auto url,
				  m_stream_mgr.resolve_stream_url(track.webpage_url, yield));

		log<ekizu::LogLevel::Info>(
			"Starting ffmpeg for track {} (frames_sent={})", track.id,
			state.playback.frames_sent.load());

		// Start ffmpeg
		SABER_TRY(auto resources, m_stream_mgr.start_ffmpeg_stream(url, yield));
		state.playback.active_stream = resources;
		state.playback.active_stream_track_id = track.id;
	}

	auto conn = state.connection;
	if (!conn) { return boost::system::errc::operation_canceled; }

	// Process audio stream
	auto should_stop = [conn] { return !conn || conn->is_shutdown(); };
	auto result = m_audio_proc.process_stream(
		state.playback.active_stream->rp(), std::move(conn), state.audio,
		state.playback.limiter_gain, state.playback.frames_sent,
		track.requester_id, track.id, should_stop, yield);

	log<ekizu::LogLevel::Info>(
		"process_stream returned: {}",
		result.has_error() ? result.error().message() : "success");

	// Only clean up stream if:
	// 1. Success (track finished normally)
	// 2. Error OTHER than operation_canceled (real error)
	if (!result.has_error() ||
		result.error() != boost::system::errc::operation_canceled) {
		log<ekizu::LogLevel::Info>("Cleaning up stream for track {}", track.id);
		state.playback.active_stream.reset();
		state.playback.active_stream_track_id.reset();
	} else {
		log<ekizu::LogLevel::Info>(
			"Preserving stream for track {} (operation_canceled)", track.id);
	}

	return result;
}

Result<> PlaybackController::start_loop(ekizu::Snowflake guild_id,
										const asio::yield_context &yield) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->connection || !state->queue) {
		return boost::system::errc::operation_not_permitted;
	}

	while (true) {
		auto conn = state->connection;
		auto queue = state->queue;

		if (!conn || !queue) { break; }
		if (conn->is_shutdown() || !queue->current_track_id) { break; }

		auto it = std::find_if(queue->tracks.begin(), queue->tracks.end(),
							   [id = *queue->current_track_id](const Track &t) {
								   return t.id == id;
							   });

		if (it == queue->tracks.end()) {
			queue->current_track_id.reset();
			mark_persist_dirty(guild_id);
			break;
		}

		const uint64_t track_id = it->id;

		const bool is_same_track = m_last_played_track[guild_id] &&
								   *m_last_played_track[guild_id] == track_id;
		m_last_played_track[guild_id] = track_id;

		// PRESERVE track position across transport interruptions
		if (is_same_track) {
			// Same track: preserve timing from before transport change
			log<ekizu::LogLevel::Debug>(
				"Continuing track {} (position preserved across transport "
				"change)",
				track_id);
		} else {
			// New track: reset timing
			state->playback.track_started = std::chrono::steady_clock::now();
			state->playback.paused_total =
				std::chrono::steady_clock::duration::zero();
			state->playback.pause_started =
				state->playback.paused
					? std::make_optional(state->playback.track_started)
					: std::nullopt;
			state->playback.frames_sent.store(0, std::memory_order_release);
			state->playback.limiter_gain.store(1.0F, std::memory_order_release);

			log<ekizu::LogLevel::Debug>(
				"Starting track {} (new/reset)", track_id);
		}

		// Send initial track data if paused
		if (state->playback.paused) {
			boost::system::error_code ec;
			SABER_TRY(conn->send_track_data(
				{{}, false, it->requester_id, track_id}, yield[ec]));
			SABER_TRY(conn->pause());
		}

		// Play track
		log<ekizu::LogLevel::Info>(
			"Calling play_track for track_id={} (is_same_track={})", track_id,
			is_same_track);
		auto res = play_track(*state, *it, yield);

		// Re-check connection state after async work
		if (!state->connection || state->connection->is_shutdown()) {
			log<ekizu::LogLevel::Info>("Connection shutdown after play_track");
			break;
		}

		// Handle completion/error
		if (res.has_error()) {
			log<ekizu::LogLevel::Info>(
				"play_track returned error: {}", res.error().message());
			if (res.error() == boost::system::errc::operation_canceled) {
				log<ekizu::LogLevel::Info>(
					"Continuing loop after operation_canceled");
				// Transport interruption (channel move, etc.): continue same
				// track
				continue;
			}

			// Skip to next on error
			if (queue->current_track_id &&
				*queue->current_track_id == track_id) {
				auto next = std::next(it);
				queue->current_track_id =
					(next != queue->tracks.end()) ? std::make_optional(next->id)
												  : std::nullopt;
				mark_persist_dirty(guild_id);
				continue;
			}
		}

		// Natural progression to next track
		if (!queue->current_track_id || *queue->current_track_id != track_id) {
			continue;
		}

		auto next = std::next(it);
		queue->current_track_id =
			(next != queue->tracks.end()) ? std::make_optional(next->id)
										  : std::nullopt;
		mark_persist_dirty(guild_id);

		if (!queue->current_track_id) { break; }
	}

	// Cleanup (at the end of start_loop)
	if (state->connection) {
		boost::system::error_code ec;
		SABER_TRY(state->connection->stop_speaking(yield[ec]));
	}

	// Make sure stream is cleaned up when loop exits
	state->playback.active_stream.reset();
	state->playback.active_stream_track_id.reset();
	state->playback.running = false;
	state->playback.paused = false;
	state->playback.pause_started.reset();
	state->playback.paused_total = std::chrono::steady_clock::duration::zero();
	mark_persist_dirty(guild_id);

	return outcome::success();
}

}  // namespace saber