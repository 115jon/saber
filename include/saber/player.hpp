#ifndef SABER_PLAYER_HPP
#define SABER_PLAYER_HPP

#include <saber/export.h>

#include <boost/asio/spawn.hpp>
#include <ekizu/snowflake.hpp>
#include <ekizu/voice_state.hpp>
#include <functional>
#include <saber/playback_controller.hpp>
#include <saber/result.hpp>
#include <saber/stream_manager.hpp>
#include <string_view>

namespace saber {

struct Player {
	using Connector = std::function<Result<ekizu::VoiceConnectionConfig>(
		ekizu::Snowflake, ekizu::Snowflake, const asio::yield_context &)>;

	explicit Player(asio::any_io_executor ex, Connector connector);

	// Connection management
	SABER_EXPORT Result<bool> connect(ekizu::Snowflake guild_id,
									  ekizu::Snowflake channel_id,
									  const asio::yield_context &yield);

	// Disconnect voice + stop playback; optionally clears the queue for this
	// guild
	SABER_EXPORT Result<> disconnect(ekizu::Snowflake guild_id,
									 bool clear_queue = true);

	SABER_EXPORT Result<> on_voice_state_update(
		ekizu::Snowflake guild_id, const ekizu::VoiceState &voice_state);

	// Used to rebind the voice transport on channel moves without resetting
	// playback state.
	SABER_EXPORT Result<> on_voice_server_update(
		ekizu::Snowflake guild_id, ekizu::VoiceConnectionConfig config);

	SABER_EXPORT Result<ekizu::Snowflake> voice_channel_id(
		ekizu::Snowflake guild_id) const;

	SABER_EXPORT bool has_connection(ekizu::Snowflake guild_id) const;
	SABER_EXPORT bool has_queue(ekizu::Snowflake guild_id) const;

	// Playback control
	SABER_EXPORT Result<Track> play(
		ekizu::Snowflake guild_id, std::string_view query,
		ekizu::Snowflake requester_id, const asio::yield_context &yield);

	SABER_EXPORT Result<bool> skip(ekizu::Snowflake guild_id);
	SABER_EXPORT Result<bool> previous(ekizu::Snowflake guild_id);
	SABER_EXPORT Result<bool> skip_to(ekizu::Snowflake guild_id,
									  uint64_t track_id);
	SABER_EXPORT Result<bool> shuffle(ekizu::Snowflake guild_id);
	SABER_EXPORT Result<bool> clear(ekizu::Snowflake guild_id);

	SABER_EXPORT Result<bool> is_paused(ekizu::Snowflake guild_id);
	SABER_EXPORT Result<> pause(ekizu::Snowflake guild_id);
	SABER_EXPORT Result<> resume(ekizu::Snowflake guild_id);

	// Audio settings (delegated to state manager)
	SABER_EXPORT Result<float> volume(ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_volume(ekizu::Snowflake guild_id, float scalar);

	SABER_EXPORT Result<int> fade_ms(ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_fade_ms(ekizu::Snowflake guild_id, int ms);

	SABER_EXPORT Result<bool> limiter_enabled(ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_limiter_enabled(ekizu::Snowflake guild_id,
											  bool enabled);

	SABER_EXPORT Result<float> limiter_threshold_db(
		ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_limiter_threshold_db(ekizu::Snowflake guild_id,
												   float db);

	SABER_EXPORT Result<int> limiter_release_ms(
		ekizu::Snowflake guild_id) const;
	SABER_EXPORT Result<> set_limiter_release_ms(ekizu::Snowflake guild_id,
												 int ms);

	// Queue access
	SABER_EXPORT Result<GuildQueue *> queue(ekizu::Snowflake guild_id);

	// Playback events (single listener; intended for Saber)
	void attach_playback_event_handler(
		std::function<void(const PlaybackEvent &)> handler) {
		m_on_event = handler;
		m_playback_ctrl.attach_event_handler(std::move(handler));
	}

	// Persistence
	Result<> restore_all(const asio::yield_context &yield);

	// Lifecycle
	void shutdown();

	// Logging
	void attach_logger(std::function<void(ekizu::Log)> logger) {
		m_logger = logger;
		m_state_mgr.attach_logger(logger);
		m_playback_ctrl.attach_logger(std::move(logger));
	}

   private:
	Connector m_connector;
	std::function<void(ekizu::Log)> m_logger;
	std::function<void(const PlaybackEvent &)> m_on_event;

	// Component managers
	StreamManager m_stream_mgr;
	AudioProcessor m_audio_proc;
	GuildStateManager m_state_mgr;
	PersistenceManager m_persist_mgr{1};
	PlaybackController m_playback_ctrl;

	void ensure_playback(ekizu::Snowflake guild_id, GuildState *state,
						 const asio::yield_context &yield) {
		if (state->playback.running) { return; }
		state->playback.running = true;

		asio::spawn(
			yield,
			[this, guild_id](const auto &y) {
				auto res = m_playback_ctrl.start_loop(guild_id, y);
				if (res.has_error() &&
					res.error() != boost::system::errc::operation_canceled) {
					log<ekizu::LogLevel::Error>(
						"Playback loop failed: {}", res.error().message());
				}
			},
			asio::detached);
	}

	Result<GuildState *> get_state(ekizu::Snowflake guild_id) {
		auto *state = m_state_mgr.get(guild_id);
		if ((state == nullptr) || !state->queue) {
			return boost::system::errc::operation_not_permitted;
		}
		return state;
	}

	template <typename Func>
	Result<bool> modify_state(ekizu::Snowflake guild_id, Func action) {
		SABER_TRY(auto *state, get_state(guild_id));
		if (action(state, state->queue.get())) {
			m_playback_ctrl.mark_persist_dirty(guild_id);
			return true;
		}
		return false;
	}

	template <ekizu::LogLevel Level, typename... Args>
	void log(fmt::format_string<Args...> fmtstr, Args &&...args) const {
		if (!m_logger) { return; }
		m_logger(ekizu::Log{
			Level, fmt::format(fmtstr, std::forward<Args>(args)...)});
	}
};

}  // namespace saber

#endif	// SABER_PLAYER_HPP
