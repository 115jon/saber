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

[[nodiscard]] SABER_EXPORT Result<boost::optional<ekizu::VoiceState &>>
in_voice_channel(Saber &bot, const ekizu::Interaction &interaction,
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

// ---------------------------------------------------------------------------
// Interaction helpers - extract common fields for slash command handlers
// ---------------------------------------------------------------------------

/// Get the username of the user who invoked the interaction.
[[nodiscard]] SABER_EXPORT std::string get_username(
	const ekizu::Interaction &interaction);

/// Get the user ID of the user who invoked the interaction.
[[nodiscard]] SABER_EXPORT std::optional<ekizu::Snowflake> get_user_id(
	const ekizu::Interaction &interaction);

/// Get the guild ID from the interaction (if it was invoked in a guild).
[[nodiscard]] inline std::optional<ekizu::Snowflake> get_guild_id(
	const ekizu::Interaction &interaction) {
	return interaction.guild_id;
}

/// Get the channel ID from the interaction.
[[nodiscard]] SABER_EXPORT std::optional<ekizu::Snowflake> get_channel_id(
	const ekizu::Interaction &interaction);

/// Get the ApplicationCommandData from an interaction (if it's an app command).
[[nodiscard]] SABER_EXPORT const ekizu::ApplicationCommandData *
get_command_data(const ekizu::Interaction &interaction);

/// Get a typed option value from an interaction by name.
/// Supports: int64_t, double, bool, std::string, ekizu::Snowflake
template <typename T>
[[nodiscard]] std::optional<T> get_option(const ekizu::Interaction &interaction,
										  std::string_view name) {
	const auto *cmd_data = get_command_data(interaction);
	if (!cmd_data) { return std::nullopt; }

	for (const auto &opt : cmd_data->options) {
		if (opt.name == name && opt.value) {
			if constexpr (std::is_same_v<T, int64_t>) {
				if (const auto *val = std::get_if<int64_t>(&*opt.value)) {
					return *val;
				}
			} else if constexpr (std::is_same_v<T, double>) {
				if (const auto *val = std::get_if<double>(&*opt.value)) {
					return *val;
				}
			} else if constexpr (std::is_same_v<T, bool>) {
				if (const auto *val = std::get_if<bool>(&*opt.value)) {
					return *val;
				}
			} else if constexpr (std::is_same_v<T, std::string>) {
				if (const auto *val = std::get_if<std::string>(&*opt.value)) {
					return *val;
				}
			} else if constexpr (std::is_same_v<T, ekizu::Snowflake>) {
				if (const auto *val =
						std::get_if<ekizu::Snowflake>(&*opt.value)) {
					return *val;
				}
			}
		}
	}
	return std::nullopt;
}

/// Convenience: get an integer option and cast to any integral type.
template <typename T>
[[nodiscard]] std::enable_if_t<
	std::is_integral_v<T> && !std::is_same_v<T, bool>, std::optional<T>>
get_int_option(const ekizu::Interaction &interaction, std::string_view name) {
	auto val = get_option<int64_t>(interaction, name);
	if (val) { return static_cast<T>(*val); }
	return std::nullopt;
}

}  // namespace saber::util

#endif	// SABER_UTIL_HPP
