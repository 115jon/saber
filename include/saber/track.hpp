#ifndef SABER_TRACK_HPP
#define SABER_TRACK_HPP

#include <cstddef>
#include <cstdint>
#include <ekizu/snowflake.hpp>
#include <string>
#include <vector>


namespace saber {

struct Track {
	uint64_t id{};
	ekizu::Snowflake requester_id{};

	// Canonical YouTube video page URL (e.g.
	// https://www.youtube.com/watch?v=...)
	std::string webpage_url;

	// Human-readable title from yt-dlp metadata.
	std::string title;

	// Resolved direct media URL used by ffmpeg. This may change over time; it
	// is refreshed when the track starts playing.
	std::string stream_url;
};

struct TrackData {
	std::vector<std::byte> data;
	bool finished{};
	ekizu::Snowflake requester_id;
	uint64_t track_id{};
};

}  // namespace saber

#endif	// SABER_TRACK_HPP
