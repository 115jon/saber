#include <spdlog/spdlog.h>

#include <boost/scope_exit.hpp>
#include <saber/player_connection.hpp>

namespace asio = boost::asio;
namespace saber {

PlayerConnection::PlayerConnection(ekizu::VoiceConnectionConfig config,
								   GuildQueue *queue,
								   std::function<void(ekizu::Log)> on_log)
	: m_queue{queue}, m_config{std::move(config)}, m_on_log{std::move(on_log)} {
	log<ekizu::LogLevel::Debug>("PlayerConnection created");
}

Result<> PlayerConnection::pause() {
	if (m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}
	if (m_pause_timer) {
		m_pause_timer->expires_at(std::chrono::steady_clock::time_point::max());
		log<ekizu::LogLevel::Info>("Playback paused");
	}
	return outcome::success();
}

Result<> PlayerConnection::resume() {
	if (m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}
	if (m_pause_timer) {
		m_pause_timer->expires_at(std::chrono::steady_clock::time_point::min());
		log<ekizu::LogLevel::Info>("Playback resumed");
	}
	return outcome::success();
}

void PlayerConnection::interrupt_playback() {
	if (m_shutdown.load(std::memory_order_acquire)) { return; }

	m_interrupted.store(true, std::memory_order_release);

	// Only intended to wake send() if it's currently blocked on the pause wait.
	if (m_pause_timer) { m_pause_timer->cancel(); }
}

void PlayerConnection::shutdown() {
	log<ekizu::LogLevel::Info>("PlayerConnection shutting down");

	// Set shutdown flag FIRST - this prevents any new operations
	m_shutdown.store(true, std::memory_order_release);

	// Cancel timers to wake up any waiting operations
	if (m_task_timer) { m_task_timer->cancel(); }
	if (m_pause_timer) { m_pause_timer->cancel(); }

	// CRITICAL FIX: Do NOT call close() with a callback that captures this
	// Just immediately reset the voice connection instead
	// The VoiceConnection destructor will handle cleanup
	if (m_voice_connection) {
		log<ekizu::LogLevel::Debug>("Resetting voice connection");
		m_voice_connection->request_stop();
		m_voice_connection.reset();
	}

	m_queue = nullptr;
	log<ekizu::LogLevel::Debug>("PlayerConnection shutdown complete");
}

Result<> PlayerConnection::stop_speaking(const asio::yield_context &yield) {
	if (m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}

	// Nothing to do if we never connected voice.
	if (!m_voice_connection) {
		m_speaking = false;
		return outcome::success();
	}

	SABER_TRY(m_voice_connection->silence(yield));
	SABER_TRY(m_voice_connection->speak(ekizu::SpeakerFlag::None, yield));
	m_speaking = false;
	log<ekizu::LogLevel::Info>("Stopped speaking");
	return outcome::success();
}

Result<> PlayerConnection::send_track_data(TrackData data,
										   const asio::yield_context &yield) {
	if (m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}

	if (m_queue == nullptr) { return boost::system::errc::operation_canceled; }

	if (!m_pause_timer) {
		m_pause_timer.emplace(yield.get_executor());
		m_pause_timer->expires_at(std::chrono::steady_clock::time_point::min());
	}

	if (data.data.empty() && !data.finished) { return outcome::success(); }

	// Enforce "single active track": drop any packets that don't match the
	// queue-selected current track.
	if (m_queue->current_track_id &&
		(*m_queue->current_track_id != data.track_id)) {
		return outcome::success();
	}

	boost::system::error_code ec;
	++m_tasks;

	if (m_tasks == 1) {
		if (!m_task_timer) { m_task_timer.emplace(yield.get_executor()); }
		m_task_timer->expires_at(std::chrono::steady_clock::time_point::max());
	} else {
		m_task_timer->async_wait(yield[ec]);
		// If cancelled due to shutdown, return immediately
		if (ec == asio::error::operation_aborted &&
			m_shutdown.load(std::memory_order_acquire)) {
			return boost::system::errc::operation_canceled;
		}
		if (ec != asio::error::operation_aborted) { return ec; }
	}

	return do_send(std::move(data), yield);
}

Result<> PlayerConnection::do_send(TrackData data,
								   const asio::yield_context &yield) {
	BOOST_SCOPE_EXIT_ALL(this) {
		--m_tasks;
		if (m_task_timer && !m_shutdown.load(std::memory_order_acquire)) {
			m_task_timer->cancel_one();
		}
	};

	if (m_shutdown.load(std::memory_order_acquire) || (m_queue == nullptr)) {
		return boost::system::errc::operation_canceled;
	}

	// If a different track became current since we were queued, drop the
	// packet.
	if (m_queue->current_track_id &&
		(*m_queue->current_track_id != data.track_id)) {
		return outcome::success();
	}

	// Finished packets are used only as a signal by the Player; no sequencing
	// here.
	if (data.finished) {
		log<ekizu::LogLevel::Debug>("Track {} finished", data.track_id);
		return outcome::success();
	}

	return send(data, yield);
}

Result<> PlayerConnection::send(const TrackData &data,
								const asio::yield_context &yield) {
	// CRITICAL: Check shutdown flag first, before accessing any resources
	if (m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}

	// Connect voice if not already connected
	if (!m_voice_connection) {
		log<ekizu::LogLevel::Info>("Connecting voice...");
		auto conn_res = m_config.connect(yield.get_executor(), yield);

		// Check shutdown again after the potentially long connect operation
		if (m_shutdown.load(std::memory_order_acquire)) {
			return boost::system::errc::operation_canceled;
		}

		if (!conn_res) { return conn_res.error(); }

		m_voice_connection.emplace(std::move(conn_res.value()));

		// Check shutdown one more time before calling run
		if (m_shutdown.load(std::memory_order_acquire)) {
			m_voice_connection.reset();
			return boost::system::errc::operation_canceled;
		}

		SABER_TRY(m_voice_connection->run(yield));
	}

	// Verify voice connection is still valid (might have been reset during
	// shutdown)
	if (!m_voice_connection) { return boost::system::errc::operation_canceled; }

	if (!m_speaking) {
		// Check shutdown before speaking
		if (m_shutdown.load(std::memory_order_acquire)) {
			return boost::system::errc::operation_canceled;
		}

		SABER_TRY(
			m_voice_connection->speak(ekizu::SpeakerFlag::Microphone, yield));
		log<ekizu::LogLevel::Info>("Voice connected and speaking");
		m_speaking = true;
	}

	if (m_pause_timer) {
		boost::system::error_code ec;
		m_pause_timer->async_wait(yield[ec]);

		if (ec == asio::error::operation_aborted) {
			// If cancelled due to shutdown, return immediately
			if (m_shutdown.load(std::memory_order_acquire)) {
				return boost::system::errc::operation_canceled;
			}

			// If interrupted (skip/previous while paused), do not send any
			// audio; unwind to let the Player switch tracks.
			if (m_interrupted.exchange(false, std::memory_order_acq_rel)) {
				return boost::system::errc::operation_canceled;
			}

			// Defensive: treat unexpected cancellations as a cancel signal.
			return boost::system::errc::operation_canceled;
		}

		if (ec) { return ec; }

		// Check if we were shut down while waiting
		if (m_shutdown.load(std::memory_order_acquire)) {
			return boost::system::errc::operation_canceled;
		}
	}

	// Final check before sending
	if (!m_voice_connection || m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}

	return m_voice_connection->send_opus(data.data, yield);
}

}  // namespace saber
