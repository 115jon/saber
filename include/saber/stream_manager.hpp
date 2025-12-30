#ifndef SABER_STREAM_MANAGER_HPP
#define SABER_STREAM_MANAGER_HPP

#include <fmt/format.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem/path.hpp>
#include <saber/result.hpp>
#include <string>
#include <vector>

namespace saber {

namespace asio = boost::asio;

struct StreamResources {
	explicit StreamResources(const asio::any_io_executor &ex,
							 const boost::filesystem::path &exe,
							 const std::vector<std::string> &args);
	StreamResources(const StreamResources &) = delete;
	StreamResources &operator=(const StreamResources &) = delete;
	StreamResources(StreamResources &&) = default;
	StreamResources &operator=(StreamResources &&) = default;
	~StreamResources();

	asio::readable_pipe &rp();

	void cancel();

   private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

struct StreamManager {
	struct TrackMetadata {
		std::string webpage_url;
		std::string title;
	};

	// Resolve metadata for a query (title, URL)
	Result<TrackMetadata> resolve_metadata(std::string_view query,
										   const asio::yield_context &yield);

	// Get streaming URL from webpage URL
	Result<std::string> resolve_stream_url(std::string_view webpage_url,
										   const asio::yield_context &yield);

	// Start ffmpeg streaming process
	Result<std::shared_ptr<StreamResources>> start_ffmpeg_stream(
		std::string_view url, const asio::yield_context &yield);

   private:
	static std::string make_search_arg(std::string_view query) {
		return boost::algorithm::starts_with(query, "http")
				   ? std::string{query}
				   : fmt::format("ytsearch1:{}", query);
	}

	static std::vector<std::string> split_nonempty(const std::string &s);
};

}  // namespace saber

#endif	// SABER_STREAM_MANAGER_HPP
