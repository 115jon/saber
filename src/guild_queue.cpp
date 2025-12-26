#include <spdlog/spdlog.h>

#include <saber/guild_queue.hpp>
#include <utility>

namespace saber {

GuildQueue::GuildQueue(std::function<void(ekizu::Log)> on_log)
	: m_on_log(std::move(on_log)) {}

Track GuildQueue::add_track(Track track) {
	track.id = last_track_id++;
	if (!current_track_id) { current_track_id = track.id; }
	tracks.emplace_back(track);

	log<ekizu::LogLevel::Info>(
		"Added track: {} (ID: {})",
		track.title.empty() ? track.webpage_url : track.title, track.id);
	return track;
}

std::optional<uint64_t> GuildQueue::skip(uint64_t track_id) {
	if (!current_track_id) {
		log<ekizu::LogLevel::Warn>("No current track");
		return std::nullopt;
	}

	// last_track_id is the NEXT id to be assigned, so valid ids are <
	// last_track_id.
	if (track_id >= last_track_id) {
		log<ekizu::LogLevel::Warn>("No more tracks to skip");
		return std::nullopt;
	}

	if (track_id == *current_track_id) {
		log<ekizu::LogLevel::Warn>("Cannot skip to current track");
		return std::nullopt;
	}

	auto exists =
		std::find_if(tracks.begin(), tracks.end(),
					 [&](const Track &t) { return t.id == track_id; });
	if (exists == tracks.end()) {
		log<ekizu::LogLevel::Warn>("Invalid track ID: {}", track_id);
		return std::nullopt;
	}

	auto ret = *current_track_id;
	current_track_id = track_id;
	log<ekizu::LogLevel::Info>("Skipped track. New current ID: {}", track_id);
	return ret;
}

}  // namespace saber
