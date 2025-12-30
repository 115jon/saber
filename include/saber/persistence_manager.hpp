#ifndef SABER_PERSISTENCE_MANAGER_HPP
#define SABER_PERSISTENCE_MANAGER_HPP

#include <boost/asio/thread_pool.hpp>
#include <deque>
#include <ekizu/snowflake.hpp>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <saber/audio_processor.hpp>
#include <saber/track.hpp>

namespace saber {

struct GuildPersistData {
	ekizu::Snowflake guild_id;
	std::optional<ekizu::Snowflake> voice_channel_id;
	bool paused{false};
	AudioSettings audio;

	struct QueueData {
		std::optional<uint64_t> current_track_id;
		uint64_t last_track_id{0};
		std::deque<Track> tracks;
	} queue;

	static constexpr size_t k_max_persisted_tracks = 100;
};

struct PersistenceManager {
	explicit PersistenceManager(size_t thread_count = 1);
	PersistenceManager(const PersistenceManager &) = delete;
	PersistenceManager &operator=(const PersistenceManager &) = delete;
	PersistenceManager(PersistenceManager &&) = delete;
	PersistenceManager &operator=(PersistenceManager &&) = delete;
	SABER_EXPORT ~PersistenceManager();

	// Queue async save
	void save_async(ekizu::Snowflake guild_id, const GuildPersistData &data);

	// Load all guild states
	Result<std::vector<GuildPersistData>> restore_all(
		const asio::yield_context &yield);

	// Disable persistence (on shutdown)
	void disable();

   private:
	struct PersistEntry {
		std::string latest_json;
		uint64_t generation{0};
		bool scheduled{false};
	};

	asio::thread_pool m_pool;
	std::mutex m_mutex;
	std::map<ekizu::Snowflake, PersistEntry> m_entries;
	std::atomic<bool> m_disabled{false};

	static constexpr auto k_state_dir = "saber_player_state";

	void worker(ekizu::Snowflake guild_id);
	static std::filesystem::path state_path(ekizu::Snowflake guild_id);
	static std::filesystem::path temp_path(ekizu::Snowflake guild_id);
	static void write_atomic(const std::filesystem::path &final,
							 const std::filesystem::path &temp,
							 std::string_view data);

	static std::string to_json(const GuildPersistData &data);
	static std::optional<GuildPersistData> from_json(const nlohmann::json &j);
};

}  // namespace saber

#endif	// SABER_PERSISTENCE_MANAGER_HPP
