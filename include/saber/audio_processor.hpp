#ifndef SABER_AUDIO_PROCESSOR_HPP
#define SABER_AUDIO_PROCESSOR_HPP

#include <opus/opus.h>
#include <saber/export.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/spawn.hpp>
#include <cstddef>
#include <cstdint>
#include <ekizu/snowflake.hpp>
#include <functional>
#include <memory>
#include <saber/result.hpp>
#include <ytdlpp/audio_streamer.hpp>

namespace saber {

namespace asio = boost::asio;

struct PlayerConnection;

struct AudioSettings {
	float volume{1.0F};
	int fade_ms{120};

	bool limiter_enabled{true};
	float limiter_threshold_db{-1.0F};
	int limiter_release_ms{120};

	static constexpr float k_volume_min = 0.0F;
	static constexpr float k_volume_max = 2.0F;
	static constexpr float k_volume_default = 1.0F;

	static constexpr int k_fade_min = 0;
	static constexpr int k_fade_max = 2000;
	static constexpr int k_fade_default = 120;

	static constexpr float k_limiter_threshold_min = -30.0F;
	static constexpr float k_limiter_threshold_max = 0.0F;
	static constexpr float k_limiter_threshold_default = -1.0F;

	static constexpr int k_limiter_release_min = 0;
	static constexpr int k_limiter_release_max = 2000;
	static constexpr int k_limiter_release_default = 120;

	void clamp() {
		volume = std::clamp(volume, k_volume_min, k_volume_max);
		fade_ms = std::clamp(fade_ms, k_fade_min, k_fade_max);
		limiter_threshold_db =
			std::clamp(limiter_threshold_db, k_limiter_threshold_min,
					   k_limiter_threshold_max);
		limiter_release_ms = std::clamp(
			limiter_release_ms, k_limiter_release_min, k_limiter_release_max);
	}
};

struct AudioProcessor {
	static constexpr int k_sample_rate = 48000;
	static constexpr int k_channels = 2;
	static constexpr int k_frame_ms = 20;
	static constexpr int k_frame_samples = 960;

	static constexpr size_t k_frame_bytes =
		k_frame_samples * k_channels * sizeof(int16_t);
	static constexpr size_t k_pcm_samples = k_frame_samples * k_channels;

	using Frame = std::array<int16_t, k_pcm_samples>;

	AudioProcessor();
	SABER_EXPORT ~AudioProcessor();

	// Delete copy/move to manage Opus encoder lifetime
	AudioProcessor(const AudioProcessor &) = delete;
	AudioProcessor &operator=(const AudioProcessor &) = delete;
	AudioProcessor(AudioProcessor &&) = delete;
	AudioProcessor &operator=(AudioProcessor &&) = delete;

	// Process PCM stream from pipe and send encoded frames
	Result<> process_stream(
		ytdlpp::media::AudioStream &stream,
		std::shared_ptr<PlayerConnection> conn, const AudioSettings &settings,
		std::atomic<float> &limiter_gain, std::atomic<size_t> &frames_sent,
		ekizu::Snowflake requester_id, uint64_t track_id,
		const std::function<bool()> &should_stop,
		const asio::yield_context &yield);

	// Reset encoder state between tracks to prevent audio artifacts
	void reset_encoder() {
		if (m_encoder != nullptr) {
			opus_encoder_ctl(m_encoder, OPUS_RESET_STATE);
		}
	}

   private:
	OpusEncoder *m_encoder{nullptr};

	Result<> send_frame(
		const Frame &pcm_frame, const std::shared_ptr<PlayerConnection> &conn,
		const AudioSettings &settings, std::atomic<float> &limiter_gain,
		size_t frame_index, size_t fade_frames, float extra_gain,
		ekizu::Snowflake requester_id, uint64_t track_id,
		const asio::yield_context &yield);

	static size_t calculate_fade_frames(int fade_ms) {
		return (fade_ms <= 0) ? 0U
							  : static_cast<size_t>(
									(fade_ms + k_frame_ms - 1) / k_frame_ms);
	}
};

}  // namespace saber

#endif	// SABER_AUDIO_PROCESSOR_HPP
