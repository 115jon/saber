#include <ekizu/json_util.hpp>
#include <saber/track.hpp>

using ekizu::json_util::deserialize;
using ekizu::json_util::serialize;

namespace saber {

void to_json(nlohmann::json &j, const TrackMetadata &metadata) {
	serialize(j, "webpage_url", metadata.webpage_url);
	serialize(j, "stream_url", metadata.stream_url);
	serialize(j, "title", metadata.title);
	serialize(j, "thumbnail_url", metadata.thumbnail_url);
	serialize(j, "duration_seconds", metadata.duration_seconds);
}

void from_json(const nlohmann::json &j, TrackMetadata &metadata) {
	deserialize(j, "webpage_url", metadata.webpage_url);
	deserialize(j, "stream_url", metadata.stream_url);
	deserialize(j, "title", metadata.title);
	deserialize(j, "thumbnail_url", metadata.thumbnail_url);
	deserialize(j, "duration_seconds", metadata.duration_seconds);
}

void to_json(nlohmann::json &j, const Track &track) {
	serialize(j, "id", track.id);
	serialize(j, "requester_id", track.requester_id);
	serialize(j, "metadata", track.metadata);
}

void from_json(const nlohmann::json &j, Track &track) {
	deserialize(j, "id", track.id);
	deserialize(j, "requester_id", track.requester_id);
	deserialize(j, "metadata", track.metadata);
}

}  // namespace saber
