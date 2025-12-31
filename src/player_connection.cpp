#include <spdlog/spdlog.h>

#include <boost/asio/error.hpp>
#include <boost/scope_exit.hpp>
#include <boost/system/errc.hpp>
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

	m_paused.store(true, std::memory_order_release);
	if (m_pause_timer) {
		m_pause_timer->expires_at(std::chrono::steady_clock::time_point::max());
		// Wake any in-flight wait so send() can observe the new paused state.
		m_pause_timer->cancel();
	}

	log<ekizu::LogLevel::Info>("Playback paused");
	return outcome::success();
}

Result<> PlayerConnection::resume() {
	if (m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}

	// During transport rebinding, resume() is a no-op until send() confirms
	// the new transport is live.
	if (m_transport_rebinding.load(std::memory_order_acquire)) {
		log<ekizu::LogLevel::Debug>(
			"Resume ignored during transport rebinding");
		return outcome::success();
	}

	m_paused.store(false, std::memory_order_release);
	if (m_pause_timer) {
		m_pause_timer->expires_at(std::chrono::steady_clock::time_point::min());
		// Wake any in-flight wait so send() can observe the new paused state.
		m_pause_timer->cancel();
	}

	log<ekizu::LogLevel::Info>("Playback resumed");
	return outcome::success();
}

void PlayerConnection::interrupt_playback() {
	if (m_shutdown.load(std::memory_order_acquire)) { return; }
	m_interrupted.store(true, std::memory_order_release);
	// Only intended to wake send() if it's currently blocked on the pause wait.
	if (m_pause_timer) { m_pause_timer->cancel(); }
}

Result<> PlayerConnection::rebind_transport(
	ekizu::VoiceConnectionConfig config) {
	if (m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}

	// Wake any waiters WITHOUT setting m_interrupted
	if (m_pause_timer) { m_pause_timer->cancel(); }

	// Replace config first
	m_config = std::move(config);

	// Reset ONLY the voice transport
	if (m_voice_connection) {
		log<ekizu::LogLevel::Debug>(
			"Rebinding voice transport (resetting voice connection)");
		m_voice_connection->request_stop();
		m_voice_connection.reset();
	} else {
		log<ekizu::LogLevel::Debug>(
			"Rebinding voice transport (no active voice connection)");
	}

	// CRITICAL: Unpause the sender so process_stream() can continue sending
	// and trigger m_transport_rebinding=false on first successful send_opus()
	m_paused.store(false, std::memory_order_release);
	m_transport_rebinding.store(true, std::memory_order_release);
	m_speaking = false;

	log<ekizu::LogLevel::Debug>(
		"Transport rebinding complete (unpaused, waiting for send success)");
	return outcome::success();
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

	if (!m_pause_timer) { m_pause_timer.emplace(yield.get_executor()); }
	if (m_paused.load(std::memory_order_acquire)) {
		m_pause_timer->expires_at(std::chrono::steady_clock::time_point::max());
	} else {
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
	if (m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}

	// Connect voice if not already connected
	if (!m_voice_connection) {
		log<ekizu::LogLevel::Info>("Connecting voice...");

		auto conn_res = m_config.connect(yield.get_executor(), yield);

		if (m_shutdown.load(std::memory_order_acquire)) {
			return boost::system::errc::operation_canceled;
		}

		if (!conn_res) { return conn_res.error(); }

		m_voice_connection.emplace(std::move(conn_res.value()));
		m_voice_connection->attach_logger([log = m_on_log](ekizu::Log l) {
			if (log) { log(std::move(l)); }
		});

		if (m_shutdown.load(std::memory_order_acquire)) {
			m_voice_connection.reset();
			return boost::system::errc::operation_canceled;
		}

		SABER_TRY(m_voice_connection->run(yield));
	}

	// Verify voice connection is still valid (might have been reset during
	// rebind)
	if (!m_voice_connection) { return boost::system::errc::operation_canceled; }

	if (!m_speaking) {
		if (m_shutdown.load(std::memory_order_acquire)) {
			return boost::system::errc::operation_canceled;
		}

		SABER_TRY(
			m_voice_connection->speak(ekizu::SpeakerFlag::Microphone, yield));
		log<ekizu::LogLevel::Info>("Voice connected and speaking");
		m_speaking = true;
	}

	if (m_pause_timer) {
		while (m_paused.load(std::memory_order_acquire)) {
			boost::system::error_code ec;
			m_pause_timer->async_wait(yield[ec]);

			if (ec == asio::error::operation_aborted) {
				// If cancelled due to shutdown, return immediately
				if (m_shutdown.load(std::memory_order_acquire)) {
					return boost::system::errc::operation_canceled;
				}

				// Only skip/previous should set this.
				if (m_interrupted.exchange(false, std::memory_order_acq_rel)) {
					return boost::system::errc::operation_canceled;
				}

				continue;
			}

			if (ec) { return ec; }

			if (m_shutdown.load(std::memory_order_acquire)) {
				return boost::system::errc::operation_canceled;
			}
		}
	}

	if (!m_voice_connection || m_shutdown.load(std::memory_order_acquire)) {
		return boost::system::errc::operation_canceled;
	}

	auto is_transient_transport_error =
		[](const boost::system::error_code &ec) {
			if (!ec) { return false; }
			if (ec == asio::error::operation_aborted) { return true; }
			if (ec == asio::error::connection_reset) { return true; }
			if (ec == asio::error::network_reset) { return true; }
			if (ec == asio::error::not_connected) { return true; }
			if (ec == asio::error::broken_pipe) { return true; }
			if (ec == asio::error::eof) { return true; }
			if (ec == boost::system::errc::operation_canceled) { return true; }
			return false;
		};

	auto send_res = m_voice_connection->send_opus(data.data, yield);
	if (!send_res) {
		const auto ec = send_res.error();

		// During channel moves, transport may close. Drop packet, reset, retry
		// next.
		if (is_transient_transport_error(ec)) {
			log<ekizu::LogLevel::Debug>(
				"send_opus transient failure ({}); dropping packet",
				ec.message());
			if (m_voice_connection) {
				m_voice_connection->request_stop();
				m_voice_connection.reset();
			}
			m_speaking = false;
			return outcome::success();
		}
		return ec;
	}

	// SUCCESS: Transport confirmed live. Unblock resume() calls.
	m_transport_rebinding.store(false, std::memory_order_release);
	return outcome::success();
}

}  // namespace saber
