#ifndef SABER_PLAYER_HPP
#define SABER_PLAYER_HPP

#include <saber/playback_controller.hpp>
#include <saber/stream_manager.hpp>

namespace saber {

struct Player {
	using Connector = std::function<Result<ekizu::VoiceConnectionConfig *>(
		ekizu::Snowflake, ekizu::Snowflake, const asio::yield_context &)>;

	explicit Player(Connector connector);

	// Connection management
	SABER_EXPORT Result<bool> connect(ekizu::Snowflake guild_id,
									  ekizu::Snowflake channel_id,
									  const asio::yield_context &yield);

	SABER_EXPORT Result<ekizu::Snowflake> voice_channel_id(
		ekizu::Snowflake guild_id) const;
	SABER_EXPORT bool has_connection(ekizu::Snowflake guild_id) const;
	SABER_EXPORT bool has_queue(ekizu::Snowflake guild_id) const;

	// Playback control
	SABER_EXPORT Result<Track> play(
		ekizu::Snowflake guild_id, std::string_view query,
		ekizu::Snowflake requester_id, const asio::yield_context &yield);

	SABER_EXPORT Result<bool> skip(ekizu::Snowflake guild_id);
	SABER_EXPORT Result<bool> previous(ekizu::Snowflake guild_id);
	SABER_EXPORT Result<> pause(ekizu::Snowflake guild_id);
	SABER_EXPORT Result<> resume(ekizu::Snowflake guild_id);

	// Audio settings (delegated to state manager)
	SABER_EXPORT Result<float> volume(ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_volume(ekizu::Snowflake guild_id, float scalar);
	SABER_EXPORT Result<int> fade_ms(ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_fade_ms(ekizu::Snowflake guild_id, int ms);
	SABER_EXPORT Result<bool> limiter_enabled(ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_limiter_enabled(ekizu::Snowflake guild_id,
											  bool enabled);
	SABER_EXPORT Result<float> limiter_threshold_db(
		ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_limiter_threshold_db(ekizu::Snowflake guild_id,
												   float db);
	SABER_EXPORT Result<int> limiter_release_ms(
		ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_limiter_release_ms(ekizu::Snowflake guild_id,
												 int ms);

	// Queue access
	SABER_EXPORT Result<GuildQueue *> queue(ekizu::Snowflake guild_id);

	// Persistence
	Result<> restore_all(const asio::yield_context &yield);

	// Lifecycle
	void shutdown();

	// Logging
	void attach_logger(std::function<void(ekizu::Log)> logger) {
		m_logger = std::move(logger);
	}

   private:
	Connector m_connector;
	std::function<void(ekizu::Log)> m_logger;

	// Component managers
	StreamManager m_stream_mgr;
	AudioProcessor m_audio_proc;
	GuildStateManager m_state_mgr{[this](ekizu::Log l) {
		if (m_logger) { m_logger(std::move(l)); }
	}};
	PersistenceManager m_persist_mgr{1};
	PlaybackController m_playback_ctrl{
		m_stream_mgr, m_audio_proc, m_state_mgr, m_persist_mgr};

	template <ekizu::LogLevel Level, typename... Args>
	void log(fmt::format_string<Args...> fmt, Args &&...args) const {
		if (!m_logger) { return; }
		m_logger(
			ekizu::Log{Level, fmt::format(fmt, std::forward<Args>(args)...)});
	}
};

}  // namespace saber

#endif	// SABER_PLAYER_HPP
