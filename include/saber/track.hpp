#ifndef SABER_TRACK_HPP
#define SABER_TRACK_HPP

#include <saber/export.h>

#include <cstddef>
#include <cstdint>
#include <ekizu/snowflake.hpp>
#include <string>
#include <vector>

namespace saber {

struct TrackMetadata {
	// Canonical YouTube video page URL (e.g.
	// https://www.youtube.com/watch?v=...)
	std::string webpage_url;
	// Resolved direct media URL used by ffmpeg. This may change over time; it
	// is refreshed when the track starts playing.
	std::string stream_url;
	// Human-readable title from yt-dlp metadata.
	std::string title;
	std::string thumbnail_url;
	uint64_t duration_seconds{};
};

SABER_EXPORT void to_json(nlohmann::json &j, const TrackMetadata &metadata);
SABER_EXPORT void from_json(const nlohmann::json &j, TrackMetadata &metadata);

struct Track {
	uint64_t id{};
	ekizu::Snowflake requester_id{};
	TrackMetadata metadata;
};

SABER_EXPORT void to_json(nlohmann::json &j, const Track &track);
SABER_EXPORT void from_json(const nlohmann::json &j, Track &track);

struct TrackData {
	std::vector<std::byte> data;
	bool finished{};
	ekizu::Snowflake requester_id;
	uint64_t track_id{};
};

}  // namespace saber

#endif	// SABER_TRACK_HPP
