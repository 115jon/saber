#ifndef SABER_TRACK_HPP
#define SABER_TRACK_HPP

#include <ekizu/snowflake.hpp>

namespace saber {

struct Track {
	uint64_t id{};
	ekizu::Snowflake requester_id{};
	std::string url;
};

struct TrackData {
	std::vector<std::byte> data;
	bool finished{};
	ekizu::Snowflake requester_id;
	uint64_t track_id{};
};

}  // namespace saber

#endif	// SABER_TRACK_HPP
