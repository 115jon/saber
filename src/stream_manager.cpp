#include <fmt/format.h>

#include <boost/algorithm/string.hpp>
#include <saber/stream_manager.hpp>
#include <ytdlpp/downloader.hpp>
#include <ytdlpp/extractor.hpp>
#include <ytdlpp/http_client.hpp>

namespace saber {

struct StreamManager::Impl {
	explicit Impl(asio::any_io_executor ex)
		: http{std::make_shared<ytdlpp::net::HttpClient>(ex)},
		  extractor{http, ex} {}

	std::shared_ptr<ytdlpp::net::HttpClient> http;
	ytdlpp::youtube::Extractor extractor;
};

StreamManager::StreamManager(asio::any_io_executor ex)
	: m_impl{std::make_unique<Impl>(std::move(ex))} {}

StreamManager::~StreamManager() = default;
StreamManager::StreamManager(StreamManager &&) noexcept = default;
StreamManager &StreamManager::operator=(StreamManager &&) noexcept = default;

namespace {
std::string make_search_arg(std::string_view query) {
	return boost::algorithm::starts_with(query, "http")
			   ? std::string{query}
			   : fmt::format("ytsearch1:{}", query);
}
}  // namespace

Result<TrackMetadata> StreamManager::resolve_metadata(
	std::string_view query, const asio::yield_context &yield) {
	const auto search_arg = make_search_arg(query);
	const auto search_opts = ytdlpp::youtube::parse_search_url(search_arg);
	if (!search_opts) { return boost::system::errc::invalid_argument; }
	const auto search_result =
		m_impl->extractor.async_search(*search_opts, yield);
	if (!search_result) { return search_result.assume_error(); }
	const auto &results = search_result.value();
	if (results.empty()) { return boost::system::errc::no_message_available; }
	const auto &video_url = results.front().url;

	auto result = m_impl->extractor.async_process(video_url, yield);
	if (!result) { return result.assume_error(); }

	const auto &info = result.value();

	TrackMetadata meta{};
	meta.webpage_url = info.webpage_url;
	meta.title = info.title;
	meta.thumbnail_url = info.thumbnail;
	meta.duration_seconds =
		(info.duration >= 0) ? static_cast<std::uint64_t>(info.duration) : 0;

	return meta;
}

Result<std::string> StreamManager::resolve_stream_url(
	std::string_view webpage_url, const asio::yield_context &yield) {
	// Use ytdlpp Extractor to get video info
	auto result =
		m_impl->extractor.async_process(std::string{webpage_url}, yield);
	if (!result) { return result.assume_error(); }

	const auto &info = result.value();

	// Select best audio stream
	auto streams = ytdlpp::Downloader::select_streams(info, "bestaudio");

	if (streams.audio == nullptr) {
		return boost::system::errc::no_message_available;
	}

	return streams.audio->url;
}

}  // namespace saber