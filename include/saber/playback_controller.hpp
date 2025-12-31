#ifndef SABER_PLAYBACK_CONTROLLER_HPP
#define SABER_PLAYBACK_CONTROLLER_HPP

#include <saber/guild_state_manager.hpp>
#include <saber/persistence_manager.hpp>
#include <utility>

namespace saber {

struct AudioProcessor;
struct StreamManager;

struct PlaybackController {
	PlaybackController(StreamManager &stream_mgr, AudioProcessor &audio_proc,
					   GuildStateManager &state_mgr,
					   PersistenceManager &persist_mgr)
		: m_stream_mgr{stream_mgr},
		  m_audio_proc{audio_proc},
		  m_state_mgr{state_mgr},
		  m_persist_mgr{persist_mgr} {}

	static constexpr auto k_previous_restart_threshold =
		std::chrono::seconds(5);

	void mark_persist_dirty(ekizu::Snowflake guild_id);

	// Start playback loop for a guild
	Result<> start_loop(ekizu::Snowflake guild_id,
						const asio::yield_context &yield);

	// Play single track
	Result<> play_track(GuildState &state, const Track &track,
						const asio::yield_context &yield);

	void attach_logger(std::function<void(ekizu::Log)> on_log) {
		m_on_log = std::move(on_log);
	}

   private:
	template <ekizu::LogLevel level, typename... Args>
	void log(fmt::format_string<Args...> fmtstr, Args &&...args) const {
		if (!m_on_log) { return; }
		m_on_log(ekizu::Log{
			level,
			fmt::format("playback_controller{{last_played_track={}}}: {}",
						m_last_played_track.empty()
							? 0
							: m_last_played_track.begin()->second.value_or(0),
						fmt::format(fmtstr, std::forward<Args>(args)...))});
	}

	std::function<void(ekizu::Log)> m_on_log;

	// Track the last successfully played track ID to preserve position across
	// transport interruptions (channel moves, brief disconnects).
	std::unordered_map<ekizu::Snowflake, std::optional<uint64_t>>
		m_last_played_track;

	StreamManager &m_stream_mgr;
	AudioProcessor &m_audio_proc;
	GuildStateManager &m_state_mgr;
	PersistenceManager &m_persist_mgr;
};

}  // namespace saber

#endif	// SABER_PLAYBACK_CONTROLLER_HPP
