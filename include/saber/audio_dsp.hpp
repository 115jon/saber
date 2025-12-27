#ifndef SABER_AUDIO_DSP_HPP
#define SABER_AUDIO_DSP_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace saber::audio {

inline float db_to_linear(float db) {
	// 20*log10(x) = db  =>  x = 10^(db/20)
	return std::pow(10.0F, db / 20.0F);
}

inline float clamp_finite(float v, float fallback) {
	if (!std::isfinite(v)) { return fallback; }
	return v;
}

inline float peak_abs_i16(const int16_t *pcm, size_t n) {
	int32_t peak = 0;
	for (size_t i = 0; i < n; ++i) {
		int32_t s = static_cast<int32_t>(pcm[i]);
		s = (s < 0) ? -s : s;
		peak = std::max(peak, s);
	}
	return static_cast<float>(peak);
}

inline void apply_gain_i16(int16_t *pcm, size_t n, float gain) {
	gain = clamp_finite(gain, 1.0F);
	if (gain == 1.0F) { return; }

	for (size_t i = 0; i < n; ++i) {
		float s = static_cast<float>(pcm[i]) * gain;
		int v = static_cast<int>(std::lrintf(s));
		v = std::min(v, 32767);
		v = std::max(v, -32768);
		pcm[i] = static_cast<int16_t>(v);
	}
}

struct LimiterState {
	float gain{1.0F};
};

inline float release_alpha_from_ms(int frame_ms, int release_ms) {
	if (release_ms <= 0) { return 1.0F; }
	// Exponential smoothing: alpha = 1 - exp(-dt/tau)
	float dt = static_cast<float>(frame_ms);
	float tau = static_cast<float>(release_ms);
	return 1.0F - std::exp(-dt / tau);
}

inline void apply_peak_limiter_i16(int16_t *pcm, size_t n, LimiterState &st,
								   bool enabled, float threshold_db,
								   int frame_ms, int release_ms) {
	if (!enabled) {
		st.gain = 1.0F;
		return;
	}

	threshold_db = clamp_finite(threshold_db, -1.0F);
	float threshold_lin = db_to_linear(threshold_db);
	threshold_lin = std::min(threshold_lin, 1.0F);
	threshold_lin = std::max(threshold_lin, 0.0F);

	float peak = peak_abs_i16(pcm, n);
	if (peak <= 0.0F) {
		// Slowly recover to unity if silent.
		float a = release_alpha_from_ms(frame_ms, release_ms);
		st.gain += (1.0F - st.gain) * a;
		return;
	}

	float threshold_samples = threshold_lin * 32767.0F;
	float desired = threshold_samples / peak;
	desired = std::min(desired, 1.0F);
	desired = std::max(desired, 0.0F);

	// Instant attack, smooth release.
	if (desired < st.gain) {
		st.gain = desired;
	} else {
		float a = release_alpha_from_ms(frame_ms, release_ms);
		st.gain += (desired - st.gain) * a;
		st.gain = std::min(st.gain, 1.0F);
	}

	apply_gain_i16(pcm, n, st.gain);
}

}  // namespace saber::audio

#endif	// SABER_AUDIO_DSP_HPP
