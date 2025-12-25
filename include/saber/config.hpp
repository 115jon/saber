#ifndef SABER_CONFIG_HPP
#define SABER_CONFIG_HPP

#include <saber/export.h>

#include <ekizu/result.hpp>
#include <ekizu/snowflake.hpp>
#include <nlohmann/json_fwd.hpp>

namespace saber {
/**
 * @brief Config parser. For now only supports fetching from JSON and also
 * filling with environment variables as backup.
 */
struct Config {
	/**
	 * @brief Fetches from a JSON file path. Will use environment variables as
	 * backup. Fails if the file does not exist or it could not be parsed.
	 *
	 * @param path The path to the JSON file
	 * @return ekizu::Result<Config> The config object.
	 */
	[[nodiscard]] SABER_EXPORT static ekizu::Result<Config> from_file(
		std::string_view path);

	std::string token;
	std::string prefix;
	ekizu::Snowflake owner_id;

   private:
	void from_env();
	void from_json(const nlohmann::json &j);

	Config() = default;
};
}  // namespace saber

#endif	// SABER_CONFIG_HPP
