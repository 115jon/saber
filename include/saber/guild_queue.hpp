#ifndef SABER_GUILD_QUEUE_HPP
#define SABER_GUILD_QUEUE_HPP

#include <saber/export.h>

#include <deque>
#include <ekizu/log.hpp>
#include <optional>
#include <saber/track.hpp>

namespace saber {

struct GuildQueue {
	GuildQueue(std::function<void(ekizu::Log)> on_log);

	SABER_EXPORT Track add_track(Track track);
	SABER_EXPORT std::optional<uint64_t> skip(uint64_t track_id);

	SABER_EXPORT std::optional<uint64_t> skip() {
		return skip(current_track_id.value_or(0) + 1);
	}

	SABER_EXPORT std::optional<uint64_t> previous() {
		return skip(current_track_id.value_or(1) - 1);
	}

	std::optional<uint64_t> current_track_id;
	uint64_t last_track_id{};
	std::deque<Track> tracks;

   private:
	template <ekizu::LogLevel level, typename... Args>
	void log(fmt::format_string<Args...> fmtstr, Args &&...args) const {
		if (!m_on_log) { return; }

		m_on_log(ekizu::Log{
			level,
			fmt::format(
				"guild_queue{{current_track_id={}, last_track_id={}, "
				"tracks={}}}: {}",
				current_track_id.value_or(0), last_track_id, tracks.size(),
				fmt::format(fmtstr, std::forward<Args>(args)...))});
	}

	std::function<void(ekizu::Log)> m_on_log;
};

}  // namespace saber

#endif	// SABER_GUILD_QUEUE_HPP
