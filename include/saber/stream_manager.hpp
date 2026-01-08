#ifndef SABER_STREAM_MANAGER_HPP
#define SABER_STREAM_MANAGER_HPP

#include <saber/export.h>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <saber/result.hpp>
#include <saber/track.hpp>
#include <string>


// Forward declarations
namespace ytdlpp::net {
class HttpClient;
}  // namespace ytdlpp::net
namespace ytdlpp::youtube {
class Extractor;
}  // namespace ytdlpp::youtube

namespace saber {

namespace asio = boost::asio;

struct StreamManager {
	explicit StreamManager(asio::any_io_executor ex);
	SABER_EXPORT ~StreamManager();

	StreamManager(const StreamManager &) = delete;
	StreamManager &operator=(const StreamManager &) = delete;
	StreamManager(StreamManager &&) noexcept;
	StreamManager &operator=(StreamManager &&) noexcept;

	// Resolve metadata for a query (title, URL)
	Result<TrackMetadata> resolve_metadata(std::string_view query,
										   const asio::yield_context &yield);

	// Get streaming URL from webpage URL
	Result<std::string> resolve_stream_url(std::string_view webpage_url,
										   const asio::yield_context &yield);

   private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

}  // namespace saber

#endif	// SABER_STREAM_MANAGER_HPP
