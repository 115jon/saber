// clang-format off
#define WIN32_LEAN_AND_MEAN
#include "fixed_utf8.hpp" // IWYU pragma: keep
// clang-format on

#include <boost/algorithm/string.hpp>
#include <boost/process/v2.hpp>
#include <boost/scope_exit.hpp>
#include <saber/stream_manager.hpp>

namespace {

namespace bp = boost::process::v2;
using namespace saber;

constexpr std::string_view k_user_agent =
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

std::string trim(std::string s) {
	boost::algorithm::trim(s);
	return s;
}

Result<std::string> read_process_output(
	bp::process &proc, asio::readable_pipe &rp, asio::readable_pipe &ep,
	const asio::yield_context &yield) {
	// Async drain stderr to prevent blocking
	asio::spawn(
		yield.get_executor(),
		[&ep](const auto &y) {
			std::array<char, 4096> buf{};
			boost::system::error_code ec;
			while (ep.async_read_some(asio::buffer(buf), y[ec]) > 0) { ; }
		},
		asio::detached);

	std::string output;
	output.reserve(8192);
	std::array<char, 4096> buf{};
	boost::system::error_code ec;

	while (auto n = rp.async_read_some(asio::buffer(buf), yield[ec])) {
		output.append(buf.data(), n);
	}

	if (ec && ec != asio::error::eof && ec != asio::error::broken_pipe) {
		return ec;
	}

	proc.async_wait(yield[ec]);
	if (ec) { return ec; }
	return output;
}
}  // namespace

namespace saber {

struct StreamResources::Impl {
	explicit Impl(const asio::any_io_executor &ex,
				  const boost::filesystem::path &exe,
				  const std::vector<std::string> &args)
		: rp{ex}, ep{ex}, proc{ex, exe, args, bp::process_stdio{{}, rp, ep}} {}

	void cancel() {
		boost::system::error_code ec;
		rp.cancel(ec);
		ep.cancel(ec);
		rp.close(ec);
		ep.close(ec);
		proc.request_exit(ec);
		proc.terminate(ec);
	}

	asio::readable_pipe rp;
	asio::readable_pipe ep;
	bp::process proc;
};

StreamResources::StreamResources(const asio::any_io_executor &ex,
								 const boost::filesystem::path &exe,
								 const std::vector<std::string> &args)
	: m_impl{std::make_unique<Impl>(ex, exe, args)} {}

StreamResources::~StreamResources() = default;

asio::readable_pipe &StreamResources::rp() { return m_impl->rp; }
void StreamResources::cancel() { m_impl->cancel(); }

std::vector<std::string> StreamManager::split_nonempty(const std::string &s) {
	std::vector<std::string> lines;
	boost::algorithm::split(lines, s, boost::algorithm::is_any_of("\r\n"),
							boost::algorithm::token_compress_on);

	lines.erase(std::remove_if(lines.begin(), lines.end(),
							   [](std::string &line) {
								   boost::algorithm::trim(line);
								   return line.empty();
							   }),
				lines.end());
	return lines;
}

Result<StreamManager::TrackMetadata> StreamManager::resolve_metadata(
	std::string_view query, const asio::yield_context &yield) {
	auto exe = bp::environment::find_executable("yt-dlp");
	if (exe.empty()) { return boost::system::errc::no_such_file_or_directory; }

	const auto search_arg = make_search_arg(query);
	const std::vector<std::string> args{
		"--no-playlist",
		"--no-warnings",
		"--encoding",
		"utf-8",
		"-O",
		"webpage_url",
		"-O",
		"title",
		search_arg};

	asio::readable_pipe rp{yield.get_executor()};
	asio::readable_pipe ep{yield.get_executor()};
	bp::process proc{
		yield.get_executor(), exe, args, bp::process_stdio{{}, rp, ep}};

	SABER_TRY(auto output, read_process_output(proc, rp, ep, yield));
	auto lines = split_nonempty(output);

	if (lines.size() < 2) { return boost::system::errc::no_message_available; }

	return TrackMetadata{
		std::move(lines[0]),
		boost::algorithm::join(
			boost::make_iterator_range(lines.begin() + 1, lines.end()), "\n")};
}

Result<std::string> StreamManager::resolve_stream_url(
	std::string_view webpage_url, const asio::yield_context &yield) {
	auto exe = bp::environment::find_executable("yt-dlp");
	if (exe.empty()) { return boost::system::errc::no_such_file_or_directory; }

	const std::vector<std::string> args{
		"--no-playlist",
		"--quiet",
		"--no-warnings",
		"--encoding",
		"utf-8",
		"--get-url",
		"-f",
		"bestaudio",
		std::string{webpage_url}};

	asio::readable_pipe rp{yield.get_executor()};
	asio::readable_pipe ep{yield.get_executor()};
	bp::process proc{
		yield.get_executor(), exe, args, bp::process_stdio{{}, rp, ep}};

	SABER_TRY(auto url, read_process_output(proc, rp, ep, yield));
	url = trim(std::move(url));

	return url.empty()
			   ? Result<std::string>{boost::system::errc::no_message_available}
			   : std::move(url);
}

Result<std::shared_ptr<StreamResources>> StreamManager::start_ffmpeg_stream(
	std::string_view url, const asio::yield_context &yield) {
	auto exe = bp::environment::find_executable("ffmpeg");
	if (exe.empty()) { return boost::system::errc::no_such_file_or_directory; }

	const std::vector<std::string> args{
		"-reconnect",
		"1",
		"-reconnect_streamed",
		"1",
		"-reconnect_at_eof",
		"1",
		"-reconnect_delay_max",
		"5",
		"-user_agent",
		std::string{k_user_agent},
		"-loglevel",
		"warning",
		"-i",
		std::string{url},
		"-map",
		"0:a",
		"-ac",
		"2",
		"-ar",
		"48000",
		"-f",
		"s16le",
		"-"};

	return std::make_shared<StreamResources>(yield.get_executor(), exe, args);
}

}  // namespace saber