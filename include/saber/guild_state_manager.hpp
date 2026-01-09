#ifndef SABER_GUILD_STATE_MANAGER_HPP
#define SABER_GUILD_STATE_MANAGER_HPP

#include <chrono>
#include <ekizu/snowflake.hpp>
#include <memory>
#include <optional>
#include <saber/audio_processor.hpp>
#include <saber/player_connection.hpp>
#include <shared_mutex>
#include <ytdlpp/audio_streamer.hpp>

namespace saber {
struct GuildQueue;

struct PlaybackState {
	bool running{false};
	bool paused{false};
	std::optional<std::chrono::steady_clock::time_point> pause_started;
	std::chrono::steady_clock::duration paused_total{};
	std::chrono::steady_clock::time_point track_started;
	std::atomic<size_t> frames_sent{0};
	std::atomic<float> limiter_gain{1.0F};
	std::optional<ytdlpp::media::AudioStream> active_stream;
	std::optional<uint64_t> active_stream_track_id;
};

struct GuildState {
	ekizu::Snowflake guild_id;
	std::shared_ptr<PlayerConnection> connection;
	std::shared_ptr<GuildQueue> queue;
	PlaybackState playback;
	std::optional<ekizu::Snowflake> last_voice_channel;
	AudioSettings audio;
	AudioProcessor audio_processor;

	std::chrono::steady_clock::duration elapsed() const {
		if (!playback.running) {
			return std::chrono::steady_clock::duration::zero();
		}

		auto now = std::chrono::steady_clock::now();
		auto paused = playback.paused_total;

		if (playback.paused && playback.pause_started) {
			paused += (now - *playback.pause_started);
		}

		return now < playback.track_started
				   ? std::chrono::steady_clock::duration::zero()
				   : (now - playback.track_started) - paused;
	}

	void cancel_stream();
};

struct GuildStateManager {
	// Get or create guild state (thread-safe)
	GuildState *get_or_create(ekizu::Snowflake guild_id);

	// Get existing state (returns nullptr if not found, thread-safe)
	GuildState *get(ekizu::Snowflake guild_id);
	[[nodiscard]] const GuildState *get(ekizu::Snowflake guild_id) const;

	// Check existence (thread-safe)
	[[nodiscard]] bool has_connection(ekizu::Snowflake guild_id) const;
	[[nodiscard]] bool has_queue(ekizu::Snowflake guild_id) const;

	// Remove state (thread-safe)
	void remove(ekizu::Snowflake guild_id);

	// Clear all states (thread-safe)
	void clear();

	// Get all guild IDs (thread-safe)
	[[nodiscard]] std::vector<ekizu::Snowflake> all_guilds() const;

	void attach_logger(std::function<void(ekizu::Log)> on_log) {
		std::unique_lock lock{m_mtx};
		m_logger = std::move(on_log);
	}

   private:
	mutable std::shared_mutex m_mtx;
	std::map<ekizu::Snowflake, std::unique_ptr<GuildState>> m_states;
	std::function<void(ekizu::Log)> m_logger;
};

}  // namespace saber

#endif	// SABER_GUILD_STATE_MANAGER_HPP
