#ifndef SABER_PLAYER_HPP
#define SABER_PLAYER_HPP

#include <boost/asio/readable_pipe.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <ekizu/voice_connection.hpp>
#include <saber/guild_queue.hpp>
#include <saber/player_connection.hpp>
#include <saber/result.hpp>
#include <saber/track.hpp>

namespace saber {

struct Player {
	using Connector = std::function<Result<ekizu::VoiceConnectionConfig *>(
		ekizu::Snowflake, ekizu::Snowflake,
		const boost::asio::yield_context &)>;

	SABER_EXPORT explicit Player(Connector connector);

	[[nodiscard]] SABER_EXPORT Result<GuildQueue *> queue(
		ekizu::Snowflake guild_id);

	[[nodiscard]] SABER_EXPORT Result<bool> connect(
		ekizu::Snowflake guild_id, ekizu::Snowflake channel_id,
		const boost::asio::yield_context &yield);

	[[nodiscard]] SABER_EXPORT Result<Track> play(
		ekizu::Snowflake guild_id, std::string_view query,
		ekizu::Snowflake requester_id, const boost::asio::yield_context &yield);

	[[nodiscard]] SABER_EXPORT Result<> pause(ekizu::Snowflake guild_id);

	[[nodiscard]] SABER_EXPORT Result<> resume(ekizu::Snowflake guild_id);

	void shutdown();
	void attach_logger(std::function<void(ekizu::Log)> on_log) {
		m_on_log = std::move(on_log);
	}

   private:
	template <ekizu::LogLevel level, typename... Args>
	void log(fmt::format_string<Args...> fmtstr, Args &&...args) const {
		if (!m_on_log) { return; }

		m_on_log(ekizu::Log{
			level,
			fmt::format("player{{connections={}, queues={}}}: {}",
						m_connections.size(), m_queues.size(),
						fmt::format(fmtstr, std::forward<Args>(args)...))});
	}

	[[nodiscard]] Result<std::string> resolve_url(
		std::string_view query, const boost::asio::yield_context &yield);

	[[nodiscard]] Result<> play_sync(
		ekizu::Snowflake guild_id, std::string_view query,
		ekizu::Snowflake requester_id, uint64_t track_id,
		const boost::asio::yield_context &yield);

	[[nodiscard]] Result<> stream_ffmpeg(
		PlayerConnection *conn, std::string_view url,
		ekizu::Snowflake requester_id, uint64_t track_id,
		const boost::asio::yield_context &yield);

	[[nodiscard]] Result<> process_ogg_stream(
		PlayerConnection *conn, boost::asio::readable_pipe &rp,
		ekizu::Snowflake requester_id, uint64_t track_id,
		const boost::asio::yield_context &yield);

	Connector m_connector;
	boost::unordered_flat_map<ekizu::Snowflake, std::unique_ptr<GuildQueue>>
		m_queues;
	boost::unordered_flat_map<ekizu::Snowflake,
							  std::unique_ptr<PlayerConnection>>
		m_connections;
	std::function<void(ekizu::Log)> m_on_log;
};

}  // namespace saber

#endif	// SABER_PLAYER_HPP
