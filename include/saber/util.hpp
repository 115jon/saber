#ifndef SABER_UTIL_HPP
#define SABER_UTIL_HPP

#include <random>
#include <saber/saber.hpp>

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
	Saber& bot, const ekizu::Message& message, ekizu::Snowflake user_id,
	ekizu::Permissions required, const boost::asio::yield_context& yield);

[[nodiscard]] SABER_EXPORT Result<boost::optional<ekizu::VoiceState&>>
in_voice_channel(Saber& bot, const ekizu::Message& msg,
				 const boost::asio::yield_context& yield);
}  // namespace saber::util
#endif	// SABER_UTIL_HPP
