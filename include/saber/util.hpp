#ifndef SABER_UTIL_HPP
#define SABER_UTIL_HPP

#include <random>
#include <saber/saber.hpp>
#include <saber/track.hpp>

namespace saber::util {
template <typename T>
T get_random_number(T begin = (std::numeric_limits<T>::min)(),
					T end = (std::numeric_limits<T>::max)()) {
	std::random_device rd;
	std::mt19937 gen(rd());

	if constexpr (std::is_floating_point_v<T>) {
		std::uniform_real_distribution<T> dis(begin, end);
		return dis(gen);
	} else {
		std::uniform_int_distribution<T> dis(begin, end);
		return dis(gen);
	}
}

[[nodiscard]] SABER_EXPORT Result<> ensure_permissions(
	Saber &bot, const ekizu::Message &message, ekizu::Snowflake user_id,
	ekizu::Permissions required, const boost::asio::yield_context &yield);

[[nodiscard]] SABER_EXPORT Result<boost::optional<ekizu::VoiceState &>>
in_voice_channel(Saber &bot, const ekizu::Message &msg,
				 const boost::asio::yield_context &yield);

constexpr uint16_t k_color_ok = 0x3A7B;
constexpr uint16_t k_color_warn = 0xB58B;
constexpr uint16_t k_color_err = 0xB000;

[[nodiscard]] SABER_EXPORT std::string truncate(std::string_view s,
												size_t max_len);

[[nodiscard]] SABER_EXPORT std::optional<size_t> find_index_by_id(
	const std::deque<Track> &tracks, std::optional<uint64_t> id);

[[nodiscard]] SABER_EXPORT std::string format_track_line(const Track &track,
														 bool is_current);

[[nodiscard]] SABER_EXPORT std::string now_playing_line(
	const std::deque<Track> &tracks, std::optional<uint64_t> current_id);

[[nodiscard]] SABER_EXPORT std::string up_next_line(
	const std::deque<Track> &tracks, std::optional<uint64_t> current_id);

[[nodiscard]] SABER_EXPORT ekizu::Embed music_action_embed(
	std::string title, uint16_t color, std::string description,
	std::string now_line, std::string next_line, std::string footer_text);

[[nodiscard]] SABER_EXPORT size_t page_count(size_t item_count,
											 size_t page_size);

[[nodiscard]] SABER_EXPORT std::string format_duration(uint64_t duration);

}  // namespace saber::util

#endif	// SABER_UTIL_HPP
