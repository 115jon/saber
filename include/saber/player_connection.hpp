#ifndef SABER_PLAYER_CONNECTION_HPP
#define SABER_PLAYER_CONNECTION_HPP

#include <saber/export.h>

#include <atomic>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <ekizu/voice_connection.hpp>
#include <queue>
#include <saber/guild_queue.hpp>
#include <saber/result.hpp>

namespace saber {

struct PlayerConnection {
	SABER_EXPORT explicit PlayerConnection(
		ekizu::VoiceConnectionConfig config, GuildQueue *queue,
		std::function<void(ekizu::Log)> on_log);

	SABER_EXPORT Result<> pause();
	SABER_EXPORT Result<> resume();

	SABER_EXPORT Result<> send_track_data(
		TrackData data, const boost::asio::yield_context &yield);

	void shutdown();
	bool is_shutdown() const {
		return m_shutdown.load(std::memory_order_acquire);
	}

   private:
	template <ekizu::LogLevel level, typename... Args>
	void log(fmt::format_string<Args...> fmtstr, Args &&...args) const {
		if (!m_on_log) { return; }

		m_on_log(ekizu::Log{
			level,
			fmt::format("player_connection{{speaking={}}}: {}", m_speaking,
						fmt::format(fmtstr, std::forward<Args>(args)...))});
	}

	Result<> do_send(TrackData data, const boost::asio::yield_context &yield);
	Result<> send(const TrackData &data,
				  const boost::asio::yield_context &yield);

	GuildQueue *m_queue;
	ekizu::VoiceConnectionConfig m_config;
	std::function<void(ekizu::Log)> m_on_log;
	std::optional<ekizu::VoiceConnection> m_voice_connection;
	std::map<uint64_t, std::queue<TrackData>> m_pending_tracks;

	std::optional<boost::asio::steady_timer> m_task_timer;
	std::optional<boost::asio::steady_timer> m_pause_timer;

	uint64_t m_tasks{};
	bool m_speaking{};
	std::atomic<bool> m_shutdown{false};
};

}  // namespace saber

#endif	// SABER_PLAYER_CONNECTION_HPP
