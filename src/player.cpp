#include <ogg/ogg.h>
#include <spdlog/spdlog.h>

// clang-format off
#include "fixed_utf8.hpp" // NOLINT
// clang-format on

#include <boost/asio/detached.hpp>
#include <boost/process/v2.hpp>
#include <boost/scope_exit.hpp>
#include <saber/player.hpp>
#include <utility>

namespace asio = boost::asio;
namespace bp = boost::process::v2;

namespace saber {
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
	return true;
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

	auto track =
		queue_it->second->add_track({{}, requester_id, std::string{query}});

	log<ekizu::LogLevel::Info>(
		"Initiating play for track {} (Query: {})", track.id, query);

	// SIMPLIFIED: No cancellation signals needed
	// The connection's shutdown flag will handle cleanup
	asio::spawn(
		yield,
		[this, guild_id, q = track.url, requester_id,
		 tid = track.id](const auto &y) {
			auto res = play_sync(guild_id, q, requester_id, tid, y);
			if (res.has_error()) {
				// Only log non-cancellation errors
				if (res.error() != boost::system::errc::operation_canceled) {
					log<ekizu::LogLevel::Error>(
						"Playback failed for track {}: {}", tid,
						res.error().message());
				} else {
					log<ekizu::LogLevel::Info>(
						"Playback cancelled for track {}", tid);
				}
			}
		},
		asio::detached);

	return track;
}

Result<> Player::pause(ekizu::Snowflake guild_id) {
	auto it = m_connections.find(guild_id);
	if (it == m_connections.end()) {
		return boost::system::errc::no_such_file_or_directory;
	}
	return it->second->pause();
}

Result<> Player::resume(ekizu::Snowflake guild_id) {
	auto it = m_connections.find(guild_id);
	if (it == m_connections.end()) {
		return boost::system::errc::no_such_file_or_directory;
	}
	return it->second->resume();
}

void Player::shutdown() {
	log<ekizu::LogLevel::Info>("Player shutting down...");

	for (auto &[guild_id, conn] : m_connections) {
		if (conn) {
			log<ekizu::LogLevel::Debug>(
				"Shutting down connection for guild {}", guild_id);
			conn->shutdown();
		}
	}

	m_connections.clear();
	m_queues.clear();

	log<ekizu::LogLevel::Info>("Player shutdown complete");
}

Result<std::string> Player::resolve_url(std::string_view query,
										const asio::yield_context &yield) {
	boost::system::error_code ec;

	auto exe = bp::environment::find_executable("yt-dlp");
	if (exe.empty()) {
		log<ekizu::LogLevel::Error>("yt-dlp executable not found");
		return boost::system::errc::no_such_file_or_directory;
	}

	asio::readable_pipe rp{yield.get_executor()};
	asio::readable_pipe ep{yield.get_executor()};

	std::string search_arg;
	if (query.find("http") == 0) {
		search_arg = std::string{query};
	} else {
		search_arg = fmt::format("ytsearch1:{}", query);
	}

	std::vector<std::string> args{
		"--no-playlist", "--quiet", "--no-warnings", "--get-url", "-f",
		"bestaudio",	 search_arg};

	bp::process proc(
		yield.get_executor(), exe, args, bp::process_stdio{{}, rp, ep});

	// Spawn a coroutine to drain stderr (prevent deadlock)
	asio::spawn(
		yield.get_executor(),
		[ep = std::move(ep)](const auto &y) mutable {
			auto buf = std::make_shared<std::array<char, 4096>>();
			boost::system::error_code ignore;
			while (true) {
				size_t n = ep.async_read_some(asio::buffer(*buf), y[ignore]);
				if (ignore || n == 0) { break; }
			}
		},
		asio::detached);

	std::string url;
	std::array<char, 4096> chunk{};
	while (true) {
		auto n = rp.async_read_some(asio::buffer(chunk), yield[ec]);
		if (n > 0) { url.append(chunk.data(), n); }
		if (ec) { break; }
	}

	int exit_code = proc.async_wait(yield[ec]);

	// Trim whitespace
	while (!url.empty() && (std::isspace(url.back()) != 0)) { url.pop_back(); }
	while (!url.empty() && (std::isspace(url.front()) != 0)) {
		url.erase(0, 1);
	}

	if (exit_code != 0 || url.empty()) {
		return boost::system::errc::no_message_available;
	}

	return url;
}

Result<> Player::play_sync(ekizu::Snowflake guild_id, std::string_view query,
						   ekizu::Snowflake requester_id, uint64_t track_id,
						   const asio::yield_context &yield) {
	SABER_TRY(auto url, resolve_url(query, yield));
	log<ekizu::LogLevel::Info>("Resolved URL for track {}: {}", track_id, url);

	auto conn_it = m_connections.find(guild_id);
	if (conn_it == m_connections.end()) {
		return boost::system::errc::no_such_file_or_directory;
	}

	PlayerConnection *conn = conn_it->second.get();

	// Check if connection was shut down
	if (conn->is_shutdown()) { return boost::system::errc::operation_canceled; }

	auto res = stream_ffmpeg(conn, url, requester_id, track_id, yield);

	// Send final packet to signal track end (only if not shutting down)
	if (!conn->is_shutdown()) {
		boost::system::error_code ignore;
		(void)conn->send_track_data(
			{{}, true, requester_id, track_id}, yield[ignore]);
	}

	return res;
}

Result<> Player::stream_ffmpeg(PlayerConnection *conn, std::string_view url,
							   ekizu::Snowflake requester_id, uint64_t track_id,
							   const asio::yield_context &yield) {
	auto ffmpeg_exe = bp::environment::find_executable("ffmpeg");
	if (ffmpeg_exe.empty()) {
		return boost::system::errc::no_such_file_or_directory;
	}

	asio::readable_pipe rp{yield.get_executor()};
	asio::readable_pipe ep{yield.get_executor()};

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

	bp::process proc(
		yield.get_executor(), ffmpeg_exe, args, bp::process_stdio{{}, rp, ep});

	// Spawn a coroutine to drain stderr
	asio::spawn(
		yield.get_executor(),
		[ep = std::move(ep)](const auto &y) mutable {
			auto buf = std::make_shared<std::array<char, 4096>>();
			boost::system::error_code ignore;
			while (true) {
				size_t n = ep.async_read_some(asio::buffer(*buf), y[ignore]);
				if (ignore || n == 0) { break; }
			}
		},
		asio::detached);

	BOOST_SCOPE_EXIT_ALL(&proc) {
		boost::system::error_code ignore;
		proc.request_exit(ignore);
	};

	log<ekizu::LogLevel::Info>("Starting ffmpeg stream for track {}", track_id);
	return process_ogg_stream(conn, rp, requester_id, track_id, yield);
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
			rp.cancel();

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

		// Handle cancellation gracefully
		if (ec == asio::error::operation_aborted) {
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