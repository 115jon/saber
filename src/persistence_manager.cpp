#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/post.hpp>
#include <fstream>
#include <saber/persistence_manager.hpp>

namespace saber {

PersistenceManager::PersistenceManager(size_t thread_count)
	: m_pool(thread_count) {}

PersistenceManager::~PersistenceManager() { disable(); }

void PersistenceManager::disable() {
	m_disabled.store(true, std::memory_order_release);
	std::lock_guard lock(m_mutex);
	m_entries.clear();
	m_pool.stop();
	m_pool.join();
}

std::filesystem::path PersistenceManager::state_path(
	ekizu::Snowflake guild_id) {
	return std::filesystem::path{k_state_dir} /
		   fmt::format("{}.json", guild_id);
}

std::filesystem::path PersistenceManager::temp_path(ekizu::Snowflake guild_id) {
	return std::filesystem::path{k_state_dir} /
		   fmt::format("{}.json.tmp", guild_id);
}

void PersistenceManager::write_atomic(const std::filesystem::path &final,
									  const std::filesystem::path &temp,
									  std::string_view data) {
	std::error_code ec;
	std::filesystem::create_directories(final.parent_path(), ec);

	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		if (!out) { return; }
		out.write(data.data(), data.size());
		out.flush();
	}

	std::filesystem::remove(final, ec);
	std::filesystem::rename(temp, final, ec);
}

std::string PersistenceManager::to_json(const GuildPersistData &data) {
	nlohmann::json j{
		{"guild_id", data.guild_id},
		{"paused", data.paused},
		{"volume", data.audio.volume},
		{"fade_ms", data.audio.fade_ms},
		{"limiter_enabled", data.audio.limiter_enabled},
		{"limiter_threshold_db", data.audio.limiter_threshold_db},
		{"limiter_release_ms", data.audio.limiter_release_ms}};

	if (data.voice_channel_id) {
		j["voice_channel_id"] = *data.voice_channel_id;
	}

	nlohmann::json qj{
		{"last_track_id", data.queue.last_track_id},
		{"current_track_id", data.queue.current_track_id.value_or(0)},
		{"tracks", nlohmann::json::array()}};

	if (data.queue.current_track_id) {
		auto it = std::find_if(data.queue.tracks.begin(),
							   data.queue.tracks.end(), [&](const Track &t) {
								   return t.id == *data.queue.current_track_id;
							   });

		if (it == data.queue.tracks.end()) { it = data.queue.tracks.begin(); }

		for (size_t count = 0; it != data.queue.tracks.end() &&
							   count < GuildPersistData::k_max_persisted_tracks;
			 ++it, ++count) {
			qj["tracks"].push_back(nlohmann::json(*it));
		}
	}

	j["queue"] = std::move(qj);
	return j.dump();
}

std::optional<GuildPersistData> PersistenceManager::from_json(
	const nlohmann::json &j) {
	if (!j.is_object() || !j.contains("guild_id")) { return std::nullopt; }

	try {
		GuildPersistData data;
		data.guild_id = j["guild_id"].get<ekizu::Snowflake>();
		data.paused = j.value("paused", false);

		data.audio.volume = j.value("volume", AudioSettings::k_volume_default);
		data.audio.fade_ms = j.value("fade_ms", AudioSettings::k_fade_default);
		data.audio.limiter_enabled = j.value("limiter_enabled", true);
		data.audio.limiter_threshold_db = j.value(
			"limiter_threshold_db", AudioSettings::k_limiter_threshold_default);
		data.audio.limiter_release_ms = j.value(
			"limiter_release_ms", AudioSettings::k_limiter_release_default);
		data.audio.clamp();

		if (j.contains("voice_channel_id") &&
			!j["voice_channel_id"].is_null()) {
			data.voice_channel_id =
				j["voice_channel_id"].get<ekizu::Snowflake>();
		}

		if (j.contains("queue") && j["queue"].is_object()) {
			const auto &qj = j["queue"];
			data.queue.last_track_id = qj.value("last_track_id", 0ULL);

			if (qj.contains("current_track_id") &&
				!qj["current_track_id"].is_null()) {
				data.queue.current_track_id =
					qj["current_track_id"].get<uint64_t>();
			}

			if (qj.contains("tracks") && qj["tracks"].is_array()) {
				for (const auto &tj : qj["tracks"]) {
					data.queue.tracks.push_back(tj);
				}
			}
		}

		return data;
	} catch (...) { return std::nullopt; }
}

void PersistenceManager::save_async(ekizu::Snowflake guild_id,
									const GuildPersistData &data) {
	if (m_disabled.load(std::memory_order_acquire)) { return; }

	auto json = to_json(data);
	bool should_schedule = false;

	{
		std::lock_guard lock(m_mutex);
		auto &entry = m_entries[guild_id];
		entry.latest_json = std::move(json);
		++entry.generation;

		if (!entry.scheduled) {
			entry.scheduled = true;
			should_schedule = true;
		}
	}

	if (should_schedule) {
		asio::post(m_pool, [this, guild_id] { worker(guild_id); });
	}
}

void PersistenceManager::worker(ekizu::Snowflake guild_id) {
	if (m_disabled.load(std::memory_order_acquire)) { return; }

	std::string json;
	uint64_t gen{};

	{
		std::lock_guard lock(m_mutex);
		auto it = m_entries.find(guild_id);
		if (it == m_entries.end()) { return; }

		json = it->second.latest_json;
		gen = it->second.generation;
	}

	write_atomic(state_path(guild_id), temp_path(guild_id), json);

	{
		std::lock_guard lock(m_mutex);
		auto it = m_entries.find(guild_id);
		if (it == m_entries.end()) { return; }

		if (it->second.generation == gen) {
			it->second.scheduled = false;
		} else if (!m_disabled.load(std::memory_order_acquire)) {
			asio::post(m_pool, [this, guild_id] { worker(guild_id); });
		}
	}
}

Result<std::vector<GuildPersistData>> PersistenceManager::restore_all(
	const asio::yield_context &yield) {
	using Channel = asio::experimental::channel<void(
		boost::system::error_code, std::vector<nlohmann::json>)>;

	auto ch = std::make_shared<Channel>(yield.get_executor(), 1);

	asio::post(m_pool, [ch, ex = yield.get_executor()] {
		std::vector<nlohmann::json> files;

		std::error_code ec;
		if (std::filesystem::exists(k_state_dir, ec)) {
			for (const auto &entry :
				 std::filesystem::directory_iterator(k_state_dir, ec)) {
				if (ec || !entry.is_regular_file()) { continue; }
				if (entry.path().extension() != ".json") { continue; }

				try {
					std::ifstream in(entry.path());
					if (in) {
						nlohmann::json j;
						in >> j;
						files.push_back(std::move(j));
					}
				} catch (...) {}
			}
		}

		asio::post(ex, [ch, files = std::move(files)]() mutable {
			boost::system::error_code ec;
			ch->async_send(ec, std::move(files), [](auto) {});
		});
	});

	boost::system::error_code ec;
	auto json_files = ch->async_receive(yield[ec]);
	if (ec) { return ec; }

	std::vector<GuildPersistData> result;
	result.reserve(json_files.size());

	for (const auto &j : json_files) {
		if (auto data = from_json(j)) { result.push_back(std::move(*data)); }
	}

	return result;
}

}  // namespace saber