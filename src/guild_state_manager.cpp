#include <saber/guild_state_manager.hpp>
#include <saber/stream_manager.hpp>

namespace saber {

void GuildState::cancel_stream() const {
	if (playback.active_stream) { playback.active_stream->cancel(); }
	if (connection) { connection->interrupt_playback(); }
}

GuildState *GuildStateManager::get_or_create(ekizu::Snowflake guild_id) {
	auto it = m_states.find(guild_id);
	if (it != m_states.end()) { return it->second.get(); }

	auto state = std::make_unique<GuildState>();
	state->guild_id = guild_id;

	auto *ptr = state.get();
	m_states.emplace(guild_id, std::move(state));
	return ptr;
}

GuildState *GuildStateManager::get(ekizu::Snowflake guild_id) {
	auto it = m_states.find(guild_id);
	return it != m_states.end() ? it->second.get() : nullptr;
}

const GuildState *GuildStateManager::get(ekizu::Snowflake guild_id) const {
	auto it = m_states.find(guild_id);
	return it != m_states.end() ? it->second.get() : nullptr;
}

bool GuildStateManager::has_connection(ekizu::Snowflake guild_id) const {
	auto it = m_states.find(guild_id);
	return it != m_states.end() && it->second->connection != nullptr;
}

bool GuildStateManager::has_queue(ekizu::Snowflake guild_id) const {
	auto it = m_states.find(guild_id);
	return it != m_states.end() && it->second->queue != nullptr;
}

void GuildStateManager::remove(ekizu::Snowflake guild_id) {
	m_states.erase(guild_id);
}

void GuildStateManager::clear() { m_states.clear(); }

std::vector<ekizu::Snowflake> GuildStateManager::all_guilds() const {
	std::vector<ekizu::Snowflake> result;
	result.reserve(m_states.size());

	for (const auto &[id, _] : m_states) { result.push_back(id); }

	return result;
}

}  // namespace saber