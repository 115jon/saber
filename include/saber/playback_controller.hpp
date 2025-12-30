#ifndef SABER_PLAYBACK_CONTROLLER_HPP
#define SABER_PLAYBACK_CONTROLLER_HPP

#include <saber/guild_state_manager.hpp>
#include <saber/persistence_manager.hpp>

namespace saber {
struct StreamManager;

struct PlaybackController {
	PlaybackController(StreamManager &stream_mgr, AudioProcessor &audio_proc,
					   GuildStateManager &state_mgr,
					   PersistenceManager &persist_mgr)
		: m_stream_mgr(stream_mgr),
		  m_audio_proc(audio_proc),
		  m_state_mgr(state_mgr),
		  m_persist_mgr(persist_mgr) {}

	static constexpr auto k_previous_restart_threshold =
		std::chrono::seconds(5);

	void mark_persist_dirty(ekizu::Snowflake guild_id);

	// Start playback loop for a guild
	Result<> start_loop(ekizu::Snowflake guild_id,
						const asio::yield_context &yield);

	// Play single track
	Result<> play_track(GuildState &state, const Track &track,
						const asio::yield_context &yield);

   private:
	StreamManager &m_stream_mgr;
	AudioProcessor &m_audio_proc;
	GuildStateManager &m_state_mgr;
	PersistenceManager &m_persist_mgr;
};

}  // namespace saber

#endif	// SABER_PLAYBACK_CONTROLLER_HPP
