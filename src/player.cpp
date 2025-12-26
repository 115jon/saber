#include <ogg/ogg.h>
#include <spdlog/spdlog.h>

// clang-format off
#include "fixed_utf8.hpp" // NOLINT
// clang-format on

#include <algorithm>
#include <array>
#include <boost/asio/detached.hpp>
#include <boost/process/v2.hpp>
#include <boost/scope_exit.hpp>
#include <cctype>
#include <cstring>
#include <saber/player.hpp>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace bp = boost::process::v2;

namespace saber {

struct Player::StreamResources {
	explicit StreamResources(const asio::any_io_executor &ex,
							 const bp::filesystem::path &exe,
							 const std::vector<std::string> &args)
		: rp{ex}, ep{ex}, proc{ex, exe, args, bp::process_stdio{{}, rp, ep}} {}

	void cancel() {
		// Goal: make skip/previous reliably interrupt *blocked reads* and stop
		// the child process quickly.
		boost::system::error_code ignored;

		// 1) Abort any in-flight operations.
		rp.cancel(ignored);
		ep.cancel(ignored);

		// 2) Force the read side to unblock even if nothing was pending at the
		//    exact moment of cancel().
		rp.close(ignored);
		ep.close(ignored);

		// 3) Ask process to exit (best-effort), then hard-terminate as
		// fallback.
		proc.request_exit(ignored);
		proc.terminate(ignored);
	}

	asio::readable_pipe rp;
	asio::readable_pipe ep;
	bp::process proc;
};

static constexpr auto k_previous_restart_threshold = std::chrono::seconds(5);

namespace {
std::string trim_copy(std::string s) {
	while (!s.empty() &&
		   (std::isspace(static_cast<unsigned char>(s.back())) != 0)) {
		s.pop_back();
	}
	while (!s.empty() &&
		   (std::isspace(static_cast<unsigned char>(s.front())) != 0)) {
		s.erase(0, 1);
	}
	return s;
}

std::string make_search_arg(std::string_view query) {
	if (query.find("http") == 0) { return std::string{query}; }
	return fmt::format("ytsearch1:{}", query);
}

Result<std::string> read_process_stdout(
	bp::process &proc, asio::readable_pipe &rp, asio::readable_pipe &ep,
	const asio::yield_context &yield) {
	// Drain stderr to avoid deadlock if it fills up.
	asio::spawn(
		yield.get_executor(),
		[&ep](const auto &y) mutable {
			auto buf = std::make_shared<std::array<char, 4096>>();
			boost::system::error_code ignore;
			while (true) {
				size_t n = ep.async_read_some(asio::buffer(*buf), y[ignore]);
				if (ignore || n == 0) { break; }
			}
		},
		asio::detached);

	boost::system::error_code ec;
	std::string out;
	std::array<char, 4096> chunk{};
	while (true) {
		auto n = rp.async_read_some(asio::buffer(chunk), yield[ec]);
		if (n > 0) { out.append(chunk.data(), n); }
		if (ec) { break; }
	}

	if (ec && ec != asio::error::eof && ec != asio::error::broken_pipe) {
		return ec;
	}

	(void)proc.async_wait(yield[ec]);
	if (ec) { return ec; }

	return outcome::success(out);
}

std::vector<std::string> split_lines_nonempty(const std::string &s) {
	std::vector<std::string> lines;
	std::string cur;
	cur.reserve(256);

	for (char ch : s) {
		if (ch == '\r') { continue; }
		if (ch == '\n') {
			auto t = trim_copy(cur);
			if (!t.empty()) { lines.emplace_back(std::move(t)); }
			cur.clear();
			continue;
		}
		cur.push_back(ch);
	}

	auto t = trim_copy(cur);
	if (!t.empty()) { lines.emplace_back(std::move(t)); }

	return lines;
}
}  // namespace

Player::Player(Connector connector) : m_connector(std::move(connector)) {}

Result<GuildQueue *> Player::queue(ekizu::Snowflake guild_id) {
	auto it = m_queues.find(guild_id);
	if (it == m_queues.end()) {
		return outcome::failure(boost::system::errc::operation_not_permitted);
	}
	return outcome::success(it->second.get());
}

Result<bool> Player::connect(ekizu::Snowflake guild_id,
							 ekizu::Snowflake channel_id,
							 const asio::yield_context &yield) {
	if (m_connections.contains(guild_id)) { return false; }

	log<ekizu::LogLevel::Info>("Player connecting to guild {}", guild_id);
	SABER_TRY(auto config, m_connector(guild_id, channel_id, yield));

	if (!m_queues.contains(guild_id)) {
		m_queues.emplace(
			guild_id, std::make_unique<GuildQueue>([this](ekizu::Log l) {
				if (m_on_log) { m_on_log(std::move(l)); }
			}));
	}

	m_connections.emplace(
		guild_id, std::make_unique<PlayerConnection>(
					  *config, m_queues[guild_id].get(), [this](ekizu::Log l) {
						  if (m_on_log) { m_on_log(std::move(l)); }
					  }));

	// Ensure playback state entry exists.
	(void)m_playback[guild_id];

	return true;
}

std::chrono::steady_clock::duration Player::playback_elapsed(
	const PlaybackState &st) const {
	if (!st.running) { return std::chrono::steady_clock::duration::zero(); }

	auto now = std::chrono::steady_clock::now();
	auto paused_total = st.paused_total;

	if (st.paused && st.pause_started) {
		paused_total += (now - *st.pause_started);
	}

	if (now < st.track_started) {
		return std::chrono::steady_clock::duration::zero();
	}
	return (now - st.track_started) - paused_total;
}

void Player::cancel_active_stream(ekizu::Snowflake guild_id) {
	auto it = m_playback.find(guild_id);
	if (it == m_playback.end()) { return; }

	auto &st = it->second;
	if (st.active_stream) { st.active_stream->cancel(); }

	// If playback is paused, send() can be blocked on the pause timer. Wake it
	// so skip/previous can take effect immediately without leaking audio.
	auto conn_it = m_connections.find(guild_id);
	if (conn_it != m_connections.end() && conn_it->second) {
		conn_it->second->interrupt_playback();
	}
}

Result<bool> Player::skip(ekizu::Snowflake guild_id) {
	auto q_it = m_queues.find(guild_id);
	auto c_it = m_connections.find(guild_id);
	if (q_it == m_queues.end() || c_it == m_connections.end()) {
		return boost::system::errc::operation_not_permitted;
	}

	auto *q = q_it->second.get();
	if ((q == nullptr) || !q->current_track_id) { return false; }

	auto it = std::find_if(
		q->tracks.begin(), q->tracks.end(),
		[&](const Track &t) { return t.id == *q->current_track_id; });
	if (it == q->tracks.end()) { return false; }

	auto next = std::next(it);
	if (next == q->tracks.end()) { return false; }

	q->current_track_id = next->id;
	cancel_active_stream(guild_id);
	return true;
}

Result<bool> Player::previous(ekizu::Snowflake guild_id) {
	auto q_it = m_queues.find(guild_id);
	auto c_it = m_connections.find(guild_id);
	if (q_it == m_queues.end() || c_it == m_connections.end()) {
		return boost::system::errc::operation_not_permitted;
	}

	auto *q = q_it->second.get();
	if ((q == nullptr) || !q->current_track_id) { return false; }

	auto st_it = m_playback.find(guild_id);
	if (st_it == m_playback.end()) { return false; }

	auto &st = st_it->second;
	auto elapsed = playback_elapsed(st);

	// Spotify-like:
	// - If >= threshold => restart current.
	// - Else => go to previous track if exists, otherwise restart current.
	if (elapsed < k_previous_restart_threshold) {
		auto it = std::find_if(
			q->tracks.begin(), q->tracks.end(),
			[&](const Track &t) { return t.id == *q->current_track_id; });

		if (it != q->tracks.end() && it != q->tracks.begin()) {
			auto prev = std::prev(it);
			q->current_track_id = prev->id;
		}
	}

	cancel_active_stream(guild_id);
	return true;
}

Result<Player::TrackMetadata> Player::resolve_metadata(
	std::string_view query, const asio::yield_context &yield) {
	boost::system::error_code ec;

	auto exe = bp::environment::find_executable("yt-dlp");
	if (exe.empty()) {
		log<ekizu::LogLevel::Error>("yt-dlp executable not found");
		return boost::system::errc::no_such_file_or_directory;
	}

	const auto search_arg = make_search_arg(query);

	// -O/--print supports printing a field name (implies --quiet and
	// --simulate). We print webpage_url first, then title.
	std::vector<std::string> args{
		"--no-playlist", "--no-warnings", "-O", "webpage_url", "-O",
		"title",		 search_arg,
	};

	asio::readable_pipe rp{yield.get_executor()};
	asio::readable_pipe ep{yield.get_executor()};
	bp::process proc{
		yield.get_executor(), exe, args, bp::process_stdio{{}, rp, ep}};

	SABER_TRY(auto out, read_process_stdout(proc, rp, ep, yield));
	auto lines = split_lines_nonempty(out);

	if (lines.size() < 2) { return boost::system::errc::no_message_available; }

	TrackMetadata meta;
	meta.webpage_url = std::move(lines[0]);

	// Title may (rarely) contain newlines; conservatively join any extra lines.
	meta.title = std::move(lines[1]);
	for (size_t i = 2; i < lines.size(); ++i) {
		meta.title.append("\n");
		meta.title.append(lines[i]);
	}

	return outcome::success(std::move(meta));
}

Result<Track> Player::play(ekizu::Snowflake guild_id, std::string_view query,
						   ekizu::Snowflake requester_id,
						   const asio::yield_context &yield) {
	auto conn_it = m_connections.find(guild_id);
	if (conn_it == m_connections.end()) {
		return boost::system::errc::operation_not_permitted;
	}

	auto queue_it = m_queues.find(guild_id);
	if (queue_it == m_queues.end()) {
		return boost::system::errc::operation_not_permitted;
	}

	SABER_TRY(auto meta, resolve_metadata(query, yield));

	auto track = queue_it->second->add_track(
		{{},
		 requester_id,
		 std::move(meta.webpage_url),
		 std::move(meta.title),
		 {}});

	log<ekizu::LogLevel::Info>(
		"Enqueued track {} (Title: {})", track.id, track.title);

	auto &st = m_playback[guild_id];
	if (!st.running) {
		st.running = true;

		asio::spawn(
			yield,
			[this, guild_id](const auto &y) {
				auto res = playback_loop(guild_id, y);
				if (res.has_error() &&
					res.error() != boost::system::errc::operation_canceled) {
					log<ekizu::LogLevel::Error>(
						"Playback loop failed for guild {}: {}", guild_id,
						res.error().message());
				}
			},
			asio::detached);
	}

	return track;
}

Result<> Player::playback_loop(ekizu::Snowflake guild_id,
							   const asio::yield_context &yield) {
	auto conn_it = m_connections.find(guild_id);
	auto queue_it = m_queues.find(guild_id);
	if (conn_it == m_connections.end() || queue_it == m_queues.end()) {
		return boost::system::errc::operation_not_permitted;
	}

	auto *conn = conn_it->second.get();
	auto *q = queue_it->second.get();
	if ((conn == nullptr) || (q == nullptr)) {
		return boost::system::errc::operation_not_permitted;
	}

	auto &st = m_playback[guild_id];

	while (true) {
		if (conn->is_shutdown()) { break; }
		if (!q->current_track_id) { break; }

		auto cur_it = std::find_if(
			q->tracks.begin(), q->tracks.end(),
			[&](const Track &t) { return t.id == *q->current_track_id; });

		if (cur_it == q->tracks.end()) {
			q->current_track_id.reset();
			break;
		}

		// Reset per-track timing, but preserve pause state across track
		// changes.
		const auto now = std::chrono::steady_clock::now();
		st.track_started = now;
		st.paused_total = std::chrono::steady_clock::duration::zero();
		if (st.paused) {
			st.pause_started = now;
		} else {
			st.pause_started.reset();
		}

		log<ekizu::LogLevel::Info>(
			"Starting track {} (Title: {})", cur_it->id, cur_it->title);

		auto res = play_sync(guild_id, *cur_it, yield);

		if (conn->is_shutdown()) { break; }

		if (res.has_error()) {
			if (res.error() == boost::system::errc::operation_canceled) {
				// Skip/previous cancels the stream. Just loop and play whatever
				// track is currently selected.
				continue;
			}

			log<ekizu::LogLevel::Error>("Playback failed for track {}: {}",
										cur_it->id, res.error().message());

			// Try to advance to next track on errors.
			if (q->current_track_id && (*q->current_track_id == cur_it->id)) {
				auto it = std::find_if(
					q->tracks.begin(), q->tracks.end(),
					[&](const Track &t) { return t.id == cur_it->id; });

				if (it != q->tracks.end()) {
					auto next = std::next(it);
					if (next != q->tracks.end()) {
						q->current_track_id = next->id;
						continue;
					}
				}

				q->current_track_id.reset();
				break;
			}

			continue;
		}

		// Natural completion: advance only if user didn't change the selection.
		if (!q->current_track_id || (*q->current_track_id != cur_it->id)) {
			continue;
		}

		auto next = std::next(cur_it);
		if (next != q->tracks.end()) {
			q->current_track_id = next->id;
			continue;
		}

		q->current_track_id.reset();
		break;
	}

	// Stop speaking when done (best-effort).
	{
		boost::system::error_code ignored;
		(void)conn->stop_speaking(yield[ignored]);
	}

	st.active_stream.reset();
	st.running = false;
	st.paused = false;
	st.pause_started.reset();
	st.paused_total = std::chrono::steady_clock::duration::zero();

	log<ekizu::LogLevel::Info>("Playback loop exited for guild {}", guild_id);
	return outcome::success();
}

Result<> Player::pause(ekizu::Snowflake guild_id) {
	auto it = m_connections.find(guild_id);
	if (it == m_connections.end()) {
		return boost::system::errc::no_such_file_or_directory;
	}

	auto st_it = m_playback.find(guild_id);
	if (st_it != m_playback.end()) {
		auto &st = st_it->second;
		if (st.running && !st.paused) {
			st.paused = true;
			st.pause_started = std::chrono::steady_clock::now();
		}
	}

	return it->second->pause();
}

Result<> Player::resume(ekizu::Snowflake guild_id) {
	auto it = m_connections.find(guild_id);
	if (it == m_connections.end()) {
		return boost::system::errc::no_such_file_or_directory;
	}

	auto st_it = m_playback.find(guild_id);
	if (st_it != m_playback.end()) {
		auto &st = st_it->second;
		if (st.running && st.paused && st.pause_started) {
			st.paused_total +=
				(std::chrono::steady_clock::now() - *st.pause_started);
			st.pause_started.reset();
			st.paused = false;
		}
	}

	return it->second->resume();
}

void Player::shutdown() {
	log<ekizu::LogLevel::Info>("Player shutting down...");

	// Cancel any active ffmpeg/yt-dlp streams first to unblock read loops.
	for (auto &[guild_id, st] : m_playback) {
		if (st.active_stream) { st.active_stream->cancel(); }
	}

	for (auto &[guild_id, conn] : m_connections) {
		if (conn) {
			log<ekizu::LogLevel::Debug>(
				"Shutting down connection for guild {}", guild_id);
			conn->shutdown();
		}
	}

	m_connections.clear();
	m_queues.clear();
	m_playback.clear();

	log<ekizu::LogLevel::Info>("Player shutdown complete");
}

Result<std::string> Player::resolve_url(ekizu::Snowflake guild_id,
										std::string_view query,
										const asio::yield_context &yield) {
	boost::system::error_code ec;

	auto exe = bp::environment::find_executable("yt-dlp");
	if (exe.empty()) {
		log<ekizu::LogLevel::Error>("yt-dlp executable not found");
		return boost::system::errc::no_such_file_or_directory;
	}

	const auto search_arg = make_search_arg(query);

	std::vector<std::string> args{
		"--no-playlist", "--quiet", "--no-warnings", "--get-url", "-f",
		"bestaudio",	 search_arg};

	auto resources =
		std::make_shared<StreamResources>(yield.get_executor(), exe, args);

	auto &st = m_playback[guild_id];
	st.active_stream = resources;

	BOOST_SCOPE_EXIT_ALL(&st, &resources) {
		if (st.active_stream == resources) { st.active_stream.reset(); }
	};

	// Spawn a coroutine to drain stderr (prevent deadlock)
	asio::spawn(
		yield.get_executor(),
		[resources](const auto &y) mutable {
			auto buf = std::make_shared<std::array<char, 4096>>();
			boost::system::error_code ignore;
			while (true) {
				size_t n = resources->ep.async_read_some(
					asio::buffer(*buf), y[ignore]);
				if (ignore || n == 0) { break; }
			}
		},
		asio::detached);

	std::string url;
	std::array<char, 4096> chunk{};
	while (true) {
		auto n = resources->rp.async_read_some(asio::buffer(chunk), yield[ec]);
		if (n > 0) { url.append(chunk.data(), n); }
		if (ec) { break; }
	}

	// If cancellation closes the pipe, Windows can surface that as
	// bad_descriptor / invalid-handle instead of operation_aborted.
	if (ec == asio::error::operation_aborted ||
		ec == asio::error::bad_descriptor || !resources->rp.is_open()) {
		return boost::system::errc::operation_canceled;
	}
	if (ec && ec != asio::error::eof && ec != asio::error::broken_pipe) {
		return ec;
	}

	int exit_code = resources->proc.async_wait(yield[ec]);
	if (ec == asio::error::operation_aborted ||
		ec == asio::error::bad_descriptor) {
		return boost::system::errc::operation_canceled;
	}
	if (ec) { return ec; }

	url = trim_copy(std::move(url));

	if (exit_code != 0 || url.empty()) {
		return boost::system::errc::no_message_available;
	}

	return url;
}

Result<> Player::play_sync(ekizu::Snowflake guild_id, Track &track,
						   const asio::yield_context &yield) {
	if (track.webpage_url.empty()) {
		return boost::system::errc::invalid_argument;
	}

	SABER_TRY(auto url, resolve_url(guild_id, track.webpage_url, yield));
	track.stream_url = url;

	log<ekizu::LogLevel::Info>(
		"Resolved stream URL for track {}: {}", track.id, track.stream_url);

	auto conn_it = m_connections.find(guild_id);
	if (conn_it == m_connections.end()) {
		return boost::system::errc::no_such_file_or_directory;
	}

	PlayerConnection *conn = conn_it->second.get();

	// Check if connection was shut down
	if (conn->is_shutdown()) { return boost::system::errc::operation_canceled; }

	auto res = stream_ffmpeg(
		guild_id, conn, track.stream_url, track.requester_id, track.id, yield);

	// Send final packet to signal track end (only if not shutting down)
	if (!conn->is_shutdown()) {
		boost::system::error_code ignore;
		(void)conn->send_track_data(
			{{}, true, track.requester_id, track.id}, yield[ignore]);
	}

	return res;
}

Result<> Player::stream_ffmpeg(ekizu::Snowflake guild_id,
							   PlayerConnection *conn, std::string_view url,
							   ekizu::Snowflake requester_id, uint64_t track_id,
							   const asio::yield_context &yield) {
	auto ffmpeg_exe = bp::environment::find_executable("ffmpeg");
	if (ffmpeg_exe.empty()) {
		return boost::system::errc::no_such_file_or_directory;
	}

	std::vector<std::string> args{
		"-reconnect",
		"1",
		"-reconnect_streamed",
		"1",
		"-reconnect_at_eof",
		"1",
		"-reconnect_delay_max",
		"5",
		"-user_agent",
		"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
		"like Gecko) Chrome/120.0.0.0 Safari/537.36",
		"-loglevel",
		"warning",
		"-i",
		std::string{url},
		"-map",
		"0:a",
		"-c:a",
		"libopus",
		"-b:a",
		"128k",
		"-f",
		"ogg",
		"-"};

	auto resources = std::make_shared<StreamResources>(
		yield.get_executor(), ffmpeg_exe, args);

	auto &st = m_playback[guild_id];
	st.active_stream = resources;

	// Spawn a coroutine to drain stderr
	asio::spawn(
		yield.get_executor(),
		[resources](const auto &y) mutable {
			auto buf = std::make_shared<std::array<char, 4096>>();
			boost::system::error_code ignore;
			while (true) {
				size_t n = resources->ep.async_read_some(
					asio::buffer(*buf), y[ignore]);
				if (ignore || n == 0) { break; }
			}
		},
		asio::detached);

	BOOST_SCOPE_EXIT_ALL(&resources, &st) {
		boost::system::error_code ignore;
		resources->proc.request_exit(ignore);
		st.active_stream.reset();
	};

	log<ekizu::LogLevel::Info>("Starting ffmpeg stream for track {}", track_id);
	return process_ogg_stream(
		conn, resources->rp, requester_id, track_id, yield);
}

Result<> Player::process_ogg_stream(
	PlayerConnection *conn, asio::readable_pipe &rp,
	ekizu::Snowflake requester_id, uint64_t track_id,
	const asio::yield_context &yield) {
	ogg_sync_state oy{};
	ogg_stream_state os{};
	ogg_page og{};
	ogg_packet op{};
	bool stream_init = false;

	ogg_sync_init(&oy);

	// Persistent buffer – outlives any pending async operation
	auto read_buffer = std::make_shared<std::vector<uint8_t>>(8192);
	boost::system::error_code ec;

	// RAII cleanup that runs exactly once when coroutine exits (any path)
	struct Cleanup {
		asio::readable_pipe &rp;
		ogg_stream_state *os;
		bool *stream_init;
		ogg_sync_state *oy;

		~Cleanup() {
			boost::system::error_code ignored;
			rp.cancel(ignored);

			// Clean up ogg state
			if ((stream_init != nullptr) && *stream_init) {
				ogg_stream_clear(os);
			}
			ogg_sync_clear(oy);
		}
	};

	Cleanup cleanup_guard{rp, &os, &stream_init, &oy};
	(void)cleanup_guard;

	while (true) {
		// Check if connection was shut down
		if (conn->is_shutdown()) {
			log<ekizu::LogLevel::Info>(
				"Connection shutdown detected, stopping track {}", track_id);
			return boost::system::errc::operation_canceled;
		}

		// Prepare buffer
		read_buffer->resize(8192);
		size_t bytes_read =
			rp.async_read_some(asio::buffer(*read_buffer), yield[ec]);

		// Handle cancellation gracefully.
		//
		// On Windows, closing the pipe to force-cancel can surface as
		// bad_descriptor / invalid-handle, not just operation_aborted.
		if (ec == asio::error::operation_aborted ||
			ec == asio::error::bad_descriptor || !rp.is_open()) {
			log<ekizu::LogLevel::Info>(
				"Stream read cancelled for track {}", track_id);
			return boost::system::errc::operation_canceled;
		}

		if (ec) {
			if (ec != asio::error::eof && ec != asio::error::broken_pipe) {
				log<ekizu::LogLevel::Error>(
					"Stream read error for track {}: {}", track_id,
					ec.message());
			}
			break;
		}

		read_buffer->resize(bytes_read);
		if (bytes_read == 0) { break; }

		char *ogg_buf = ogg_sync_buffer(&oy, static_cast<long>(bytes_read));
		std::memcpy(ogg_buf, read_buffer->data(), bytes_read);
		ogg_sync_wrote(&oy, static_cast<long>(bytes_read));

		while (ogg_sync_pageout(&oy, &og) == 1) {
			if (!stream_init) {
				ogg_stream_init(&os, ogg_page_serialno(&og));
				stream_init = true;
			} else if (ogg_page_serialno(&og) != os.serialno) {
				ogg_stream_reset_serialno(&os, ogg_page_serialno(&og));
			}

			if (ogg_stream_pagein(&os, &og) < 0) { continue; }

			while (ogg_stream_packetout(&os, &op) != 0) {
				// Skip Opus header packets
				if (op.bytes > 8 &&
					(std::memcmp(op.packet, "OpusHead", 8) == 0 ||
					 std::memcmp(op.packet, "OpusTags", 8) == 0)) {
					continue;
				}

				auto span = boost::as_bytes(
					boost::span{op.packet, static_cast<size_t>(op.bytes)});

				auto res = conn->send_track_data(
					{{span.begin(), span.end()}, {}, requester_id, track_id},
					yield);

				if (res.has_error()) {
					if (res.error() ==
						boost::system::errc::operation_canceled) {
						log<ekizu::LogLevel::Info>(
							"Track {} canceled during playback", track_id);
					} else {
						log<ekizu::LogLevel::Error>(
							"Failed to send track data for {}: {}", track_id,
							res.error().message());
					}
					return res;
				}
			}
		}
	}

	return outcome::success();
}

}  // namespace saber
