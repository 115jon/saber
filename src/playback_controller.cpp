#include <boost/asio/spawn.hpp>
#include <boost/scope_exit.hpp>
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

	// Resolve stream URL
	SABER_TRY(
		auto url, m_stream_mgr.resolve_stream_url(track.webpage_url, yield));

	// Start ffmpeg
	SABER_TRY(auto resources, m_stream_mgr.start_ffmpeg_stream(url, yield));
	state.playback.active_stream = resources;

	BOOST_SCOPE_EXIT_ALL(&state) { state.playback.active_stream.reset(); };

	// Process audio stream
	auto should_stop = [&state] {
		return !state.connection || state.connection->is_shutdown();
	};

	return m_audio_proc.process_stream(
		resources->rp(), state.connection.get(), state.audio,
		state.playback.limiter_gain, state.playback.frames_sent,
		track.requester_id, track.id, should_stop, yield);
}

Result<> PlaybackController::start_loop(ekizu::Snowflake guild_id,
										const asio::yield_context &yield) {
	auto *state = m_state_mgr.get(guild_id);
	if ((state == nullptr) || !state->connection || !state->queue) {
		return boost::system::errc::operation_not_permitted;
	}

	auto *conn = state->connection.get();
	auto *queue = state->queue.get();

	while (true) {
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

		// Reset playback state
		state->playback.track_started = std::chrono::steady_clock::now();
		state->playback.paused_total =
			std::chrono::steady_clock::duration::zero();
		state->playback.pause_started =
			state->playback.paused
				? std::make_optional(state->playback.track_started)
				: std::nullopt;
		state->playback.frames_sent.store(0, std::memory_order_release);
		state->playback.limiter_gain.store(1.0F, std::memory_order_release);

		// Send initial track data
		if (state->playback.paused) {
			boost::system::error_code ec;
			conn->send_track_data(
				{{}, false, it->requester_id, track_id}, yield[ec]);
			conn->pause();
		}

		// Play track
		auto res = play_track(*state, *it, yield);

		if (conn->is_shutdown()) { break; }

		// Handle completion/error
		if (res.has_error()) {
			if (res.error() == boost::system::errc::operation_canceled) {
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
			}
			continue;
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

	// Cleanup
	{
		boost::system::error_code ec;
		conn->stop_speaking(yield[ec]);
	}

	state->playback.active_stream.reset();
	state->playback.running = false;
	state->playback.paused = false;
	state->playback.pause_started.reset();
	state->playback.paused_total = std::chrono::steady_clock::duration::zero();

	mark_persist_dirty(guild_id);
	return outcome::success();
}

}  // namespace saber