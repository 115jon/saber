#include <boost/scope_exit.hpp>
#include <saber/audio_dsp.hpp>
#include <saber/audio_processor.hpp>
#include <saber/player_connection.hpp>
#include <saber/ring_buffer.hpp>
#include <saber/track.hpp>

namespace saber {

namespace {
constexpr size_t k_shutdown_check_interval = 10;
}  // namespace

AudioProcessor::AudioProcessor() {
	int err = 0;
	m_encoder = opus_encoder_create(
		k_sample_rate, k_channels, OPUS_APPLICATION_AUDIO, &err);
	if (err != OPUS_OK || (m_encoder == nullptr)) {
		throw std::runtime_error("Failed to create Opus encoder");
	}
	opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(128000));
}

AudioProcessor::~AudioProcessor() {
	if (m_encoder != nullptr) { opus_encoder_destroy(m_encoder); }
}

Result<> AudioProcessor::send_frame(
	const Frame &pcm_frame, PlayerConnection *conn,
	const AudioSettings &settings, std::atomic<float> &limiter_gain,
	size_t frame_index, size_t fade_frames, float extra_gain,
	ekizu::Snowflake requester_id, uint64_t track_id,
	const asio::yield_context &yield) {
	// Calculate fade-in factor
	const float fade_in =
		(fade_frames > 0 && frame_index < fade_frames)
			? std::min(static_cast<float>(frame_index + 1) / fade_frames, 1.0F)
			: 1.0F;

	const float total_gain = settings.volume * fade_in * extra_gain;

	// Apply gain (use SIMD on x86_64)
	Frame processed = pcm_frame;
	auto *samples = reinterpret_cast<int16_t *>(processed.data());

#ifdef __x86_64__
	audio::apply_gain_i16_simd(samples, k_pcm_samples, total_gain);
#else
	audio::apply_gain_i16(samples, k_pcm_samples, total_gain);
#endif

	// Apply peak limiter if enabled
	if (settings.limiter_enabled) {
		audio::LimiterState lim{limiter_gain.load(std::memory_order_relaxed)};
		audio::apply_peak_limiter_i16(
			samples, k_pcm_samples, lim, true, settings.limiter_threshold_db,
			k_frame_ms, settings.limiter_release_ms);
		limiter_gain.store(lim.gain, std::memory_order_relaxed);
	}

	// Encode with Opus
	std::array<uint8_t, 4096> opus_buf{};
	const int encoded =
		opus_encode(m_encoder, processed.data(), k_frame_samples,
					opus_buf.data(), opus_buf.size());

	if (encoded < 0) { return boost::system::errc::operation_not_permitted; }

	// Send to connection
	TrackData td{std::vector<std::byte>(
					 reinterpret_cast<std::byte *>(opus_buf.data()),
					 reinterpret_cast<std::byte *>(opus_buf.data() + encoded)),
				 false, requester_id, track_id};

	return conn->send_track_data(std::move(td), yield);
}

Result<> AudioProcessor::process_stream(
	asio::readable_pipe &rp, PlayerConnection *conn,
	const AudioSettings &settings, std::atomic<float> &limiter_gain,
	std::atomic<size_t> &frames_sent, ekizu::Snowflake requester_id,
	uint64_t track_id, const std::function<bool()> &should_stop,
	const asio::yield_context &yield) {
	const size_t fade_frames = calculate_fade_frames(settings.fade_ms);
	std::deque<Frame> fade_buffer;
	RingBuffer pcm_buffer(65536);

	auto read_buf = std::make_shared<std::vector<uint8_t>>(8192);
	Frame pcm_frame;
	size_t frames_since_check = 0;

	boost::system::error_code ec;

	auto send_with_fade = [&](float extra_gain) -> Result<> {
		const size_t idx = frames_sent.load(std::memory_order_relaxed);
		auto res =
			send_frame(pcm_frame, conn, settings, limiter_gain, idx,
					   fade_frames, extra_gain, requester_id, track_id, yield);
		if (!res.has_error()) {
			frames_sent.fetch_add(1, std::memory_order_relaxed);
		}
		return res;
	};

	// Main processing loop
	while (true) {
		// Check for shutdown periodically
		if (++frames_since_check >= k_shutdown_check_interval) {
			frames_since_check = 0;
			if (should_stop()) {
				return boost::system::errc::operation_canceled;
			}
		}

		// Read from pipe
		read_buf->resize(8192);
		const size_t n = rp.async_read_some(asio::buffer(*read_buf), yield[ec]);

		if (ec == asio::error::operation_aborted ||
			ec == asio::error::bad_descriptor || !rp.is_open()) {
			return boost::system::errc::operation_canceled;
		}

		if (ec && ec != asio::error::eof && ec != asio::error::broken_pipe) {
			return ec;
		}

		if (n == 0 || ec) { break; }

		pcm_buffer.write(read_buf->data(), n);

		// Process complete frames
		while (pcm_buffer.size() >= k_frame_bytes) {
			pcm_buffer.read(
				reinterpret_cast<uint8_t *>(pcm_frame.data()), k_frame_bytes);

			if (fade_frames > 0) {
				fade_buffer.push_back(pcm_frame);
				if (fade_buffer.size() <= fade_frames) { continue; }

				pcm_frame = fade_buffer.front();
				fade_buffer.pop_front();
			}

			SABER_TRY(send_with_fade(1.0F));
		}
	}

	// Fade-out remaining frames
	if (fade_frames > 0 && !fade_buffer.empty()) {
		const size_t remaining = fade_buffer.size();
		const float step = (remaining > 1) ? 1.0F / (remaining - 1) : 0.0F;

		for (size_t i = 0; i < remaining; ++i) {
			pcm_frame = fade_buffer.front();
			fade_buffer.pop_front();

			const float fade_out =
				(remaining > 1)
					? std::clamp((remaining - i - 1) * step, 0.0F, 1.0F)
					: 0.0F;

			SABER_TRY(send_with_fade(fade_out));
		}
	}

	return outcome::success();
}

}  // namespace saber