#ifndef SABER_PLAYER_HPP
#define SABER_PLAYER_HPP

#include <atomic>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <chrono>
#include <ekizu/voice_connection.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <saber/guild_queue.hpp>
#include <saber/player_connection.hpp>
#include <saber/result.hpp>
#include <saber/track.hpp>
#include <string>

namespace saber {

struct Player {
	using Connector = std::function<Result<ekizu::VoiceConnectionConfig *>(
		ekizu::Snowflake, ekizu::Snowflake,
		const boost::asio::yield_context &)>;

	SABER_EXPORT explicit Player(Connector connector);

	[[nodiscard]] SABER_EXPORT Result<GuildQueue *> queue(
		ekizu::Snowflake guild_id);

	[[nodiscard]] SABER_EXPORT Result<bool> connect(
		ekizu::Snowflake guild_id, ekizu::Snowflake channel_id,
		const boost::asio::yield_context &yield);

	[[nodiscard]] SABER_EXPORT Result<Track> play(
		ekizu::Snowflake guild_id, std::string_view query,
		ekizu::Snowflake requester_id, const boost::asio::yield_context &yield);

	[[nodiscard]] SABER_EXPORT Result<bool> skip(ekizu::Snowflake guild_id);
	[[nodiscard]] SABER_EXPORT Result<bool> previous(ekizu::Snowflake guild_id);

	[[nodiscard]] SABER_EXPORT Result<> pause(ekizu::Snowflake guild_id);
	[[nodiscard]] SABER_EXPORT Result<> resume(ekizu::Snowflake guild_id);

	[[nodiscard]] SABER_EXPORT bool has_connection(
		ekizu::Snowflake guild_id) const;
	[[nodiscard]] SABER_EXPORT bool has_queue(ekizu::Snowflake guild_id) const;

	[[nodiscard]] SABER_EXPORT Result<ekizu::Snowflake> voice_channel_id(
		ekizu::Snowflake guild_id) const;

	// Volume is a gain scalar applied in real-time (per 20ms frame) before Opus
	// encoding.
	// Suggested range: 0.0 (mute) to 2.0 (200%).
	[[nodiscard]] SABER_EXPORT Result<float> volume(
		ekizu::Snowflake guild_id) const;
	[[nodiscard]] SABER_EXPORT Result<> set_volume(ekizu::Snowflake guild_id,
												   float scalar);

	// Fade is applied at the start/end of tracks.
	// Note: changing this while a track is playing may only affect the next
	// track, because end-of-track fade-out requires buffering.
	[[nodiscard]] SABER_EXPORT Result<int> fade_ms(
		ekizu::Snowflake guild_id) const;
	[[nodiscard]] SABER_EXPORT Result<> set_fade_ms(ekizu::Snowflake guild_id,
													int ms);

	[[nodiscard]] SABER_EXPORT Result<bool> limiter_enabled(
		ekizu::Snowflake guild_id) const;
	[[nodiscard]] SABER_EXPORT Result<> set_limiter_enabled(
		ekizu::Snowflake guild_id, bool enabled);

	// Threshold in dBFS, typically negative (e.g. -1.0).
	[[nodiscard]] SABER_EXPORT Result<float> limiter_threshold_db(
		ekizu::Snowflake guild_id) const;
	[[nodiscard]] SABER_EXPORT Result<> set_limiter_threshold_db(
		ekizu::Snowflake guild_id, float db);

	[[nodiscard]] SABER_EXPORT Result<int> limiter_release_ms(
		ekizu::Snowflake guild_id) const;
	[[nodiscard]] SABER_EXPORT Result<> set_limiter_release_ms(
		ekizu::Snowflake guild_id, int ms);

	[[nodiscard]] SABER_EXPORT Result<> restore_all(
		const boost::asio::yield_context &yield);

	void shutdown();
	void attach_logger(std::function<void(ekizu::Log)> on_log) {
		m_on_log = std::move(on_log);
	}

   private:
	struct StreamResources;

	struct PlaybackState {
		std::atomic<float> volume{1.0F};
		std::atomic<int> fade_ms{120};
		std::atomic<bool> limiter_enabled{true};
		std::atomic<float> limiter_threshold_db{-1.0F};
		std::atomic<int> limiter_release_ms{120};

		bool running{false};
		bool paused{false};
		std::optional<std::chrono::steady_clock::time_point> pause_started;
		std::chrono::steady_clock::duration paused_total{};
		std::chrono::steady_clock::time_point track_started;

		size_t frames_sent{0};
		float limiter_gain{1.0F};

		std::shared_ptr<StreamResources> active_stream;

		PlaybackState() = default;
		PlaybackState(const PlaybackState &) = delete;
		PlaybackState &operator=(const PlaybackState &) = delete;

		// Move (atomics are still copied; the rest can be moved)
		PlaybackState(PlaybackState &&o) noexcept { move_from(std::move(o)); }

		PlaybackState &operator=(PlaybackState &&o) noexcept {
			if (this != &o) { move_from(std::move(o)); }
			return *this;
		}

		~PlaybackState() = default;

	   private:
		void move_from(PlaybackState &&o) {
			// Atomics: copy snapshot
			volume.store(o.volume.load(std::memory_order_relaxed),
						 std::memory_order_relaxed);
			fade_ms.store(o.fade_ms.load(std::memory_order_relaxed),
						  std::memory_order_relaxed);
			limiter_enabled.store(
				o.limiter_enabled.load(std::memory_order_relaxed),
				std::memory_order_relaxed);
			limiter_threshold_db.store(
				o.limiter_threshold_db.load(std::memory_order_relaxed),
				std::memory_order_relaxed);
			limiter_release_ms.store(
				o.limiter_release_ms.load(std::memory_order_relaxed),
				std::memory_order_relaxed);

			// Non-atomics: real move
			running = o.running;
			paused = o.paused;
			pause_started = o.pause_started;
			paused_total = o.paused_total;
			track_started = o.track_started;

			frames_sent = o.frames_sent;
			limiter_gain = o.limiter_gain;

			active_stream = std::move(o.active_stream);
		}
	};

	struct TrackMetadata {
		std::string webpage_url;
		std::string title;
	};

	static constexpr size_t k_persist_track_cap = 1000;

	struct PersistEntry {
		uint64_t generation{};
		bool scheduled{};
		std::string latest_json;
	};

	static float clamp_volume(float v);
	static int clamp_fade_ms(int ms);
	static float clamp_limiter_threshold_db(float db);
	static int clamp_limiter_release_ms(int ms);

	void mark_persist_dirty(ekizu::Snowflake guild_id);
	[[nodiscard]] std::string build_persist_json(
		ekizu::Snowflake guild_id) const;
	void persist_worker(ekizu::Snowflake guild_id);
	[[nodiscard]] Result<> restore_all_impl(
		const boost::asio::yield_context &yield);

	template <ekizu::LogLevel level, typename... Args>
	void log(fmt::format_string<Args...> fmtstr, Args &&...args) const {
		if (!m_on_log) { return; }

		m_on_log(ekizu::Log{
			level,
			fmt::format("player{{connections={}, queues={}}}: {}",
						m_connections.size(), m_queues.size(),
						fmt::format(fmtstr, std::forward<Args>(args)...))});
	}

	[[nodiscard]] Result<TrackMetadata> resolve_metadata(
		std::string_view query, const boost::asio::yield_context &yield);

	[[nodiscard]] Result<std::string> resolve_url(
		ekizu::Snowflake guild_id, std::string_view query,
		const boost::asio::yield_context &yield);

	[[nodiscard]] Result<> play_sync(ekizu::Snowflake guild_id, Track &track,
									 const boost::asio::yield_context &yield);

	[[nodiscard]] Result<> stream_ffmpeg(
		ekizu::Snowflake guild_id, PlayerConnection *conn, std::string_view url,
		ekizu::Snowflake requester_id, uint64_t track_id,
		const boost::asio::yield_context &yield);

	[[nodiscard]] Result<> process_pcm_stream(
		PlayerConnection *conn, boost::asio::readable_pipe &rp,
		ekizu::Snowflake requester_id, uint64_t track_id,
		ekizu::Snowflake guild_id, const boost::asio::yield_context &yield);

	[[nodiscard]] Result<> playback_loop(
		ekizu::Snowflake guild_id, const boost::asio::yield_context &yield);

	void cancel_active_stream(ekizu::Snowflake guild_id);
	std::chrono::steady_clock::duration playback_elapsed(
		const PlaybackState &st) const;

	Connector m_connector;
	boost::unordered_flat_map<ekizu::Snowflake, std::unique_ptr<GuildQueue>>
		m_queues;
	boost::unordered_flat_map<ekizu::Snowflake,
							  std::unique_ptr<PlayerConnection>>
		m_connections;
	boost::unordered_flat_map<ekizu::Snowflake, PlaybackState> m_playback;

	boost::unordered_flat_map<ekizu::Snowflake, ekizu::Snowflake>
		m_last_voice_channel;

	mutable std::mutex m_persist_mutex;
	boost::unordered_flat_map<ekizu::Snowflake, PersistEntry> m_persist_entries;
	std::atomic<bool> m_persist_disabled{false};
	boost::asio::thread_pool m_persist_pool;

	std::function<void(ekizu::Log)> m_on_log;
};

}  // namespace saber

#endif	// SABER_PLAYER_HPP
