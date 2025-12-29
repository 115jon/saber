#ifndef SABER_COMMANDS_HPP
#define SABER_COMMANDS_HPP

#include <boost/unordered/unordered_flat_map.hpp>
#include <ekizu/application_command.hpp>
#include <ekizu/application_command_data.hpp>
#include <ekizu/interaction.hpp>
#include <ekizu/message.hpp>
#include <ekizu/permissions.hpp>
#include <filesystem>  // NOLINT
#include <optional>
#include <saber/library.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace saber {
struct Command;
struct Saber;

using CommandCooldown =
	boost::unordered_flat_map<std::string,
							  std::chrono::steady_clock::time_point>;

struct CommandLoader {
	explicit CommandLoader(Saber &parent);
	CommandLoader(const CommandLoader &) = delete;
	CommandLoader &operator=(const CommandLoader &) = delete;
	CommandLoader(CommandLoader &&) = delete;
	CommandLoader &operator=(CommandLoader &&) = delete;
	~CommandLoader() = default;

	SABER_EXPORT void load(std::string_view path,
						   const boost::asio::yield_context &yield);

	SABER_EXPORT void load_all(const boost::asio::yield_context &yield);

	[[nodiscard]] SABER_EXPORT Result<> process_commands(
		const ekizu::Message &message, const boost::asio::yield_context &yield);

	[[nodiscard]] SABER_EXPORT Result<> process_commands(
		const ekizu::Interaction &interaction,
		const boost::asio::yield_context &yield);

	SABER_EXPORT void unload(const std::string &name);

	SABER_EXPORT void get_commands(
		ekizu::FunctionView<void(const boost::unordered_flat_map<
								 std::string, std::shared_ptr<Command>> &)>)
		const;

   private:
	mutable std::mutex m_mtx;
	Saber &m_parent;
	boost::unordered_flat_map<std::string, Library> commands;
	boost::unordered_flat_map<std::string, std::shared_ptr<Command>>
		command_map;
	boost::unordered_flat_map<std::string, std::shared_ptr<Command>> alias_map;
	boost::unordered_flat_map<std::string, std::shared_ptr<Command>>
		slash_commands;
	boost::unordered_flat_map<std::string, std::shared_ptr<Command>>
		user_commands;
	boost::unordered_flat_map<
		std::string, boost::unordered_flat_map<
						 std::string, std::chrono::steady_clock::time_point>>
		cooldowns;
};

struct CommandOptions {
	std::string name;
	std::string category;
	bool enabled{};
	bool init{};
	bool guild_only{};
	bool user{};
	std::optional<std::vector<ekizu::ApplicationCommandOption>> slash_options;
	std::vector<std::string> aliases;
	std::string usage;
	std::string description;
	ekizu::Permissions member_permissions;
	ekizu::Permissions bot_permissions;
	bool nsfw{};
	bool owner_only{};
	std::chrono::steady_clock::duration cooldown;
	std::vector<std::string> examples;
	std::string subcommands;
	bool activity{};
	bool voice_only{};
};

struct CommandOptionsBuilder {
	[[nodiscard]] CommandOptions build() const { return m_options; }

	CommandOptionsBuilder &name(std::string_view name) {
		m_options.name = std::string{name};
		return *this;
	}

	CommandOptionsBuilder &category(std::string_view category) {
		m_options.category = std::string{category};
		return *this;
	}

	CommandOptionsBuilder &enabled(bool enabled) {
		m_options.enabled = enabled;
		return *this;
	}

	CommandOptionsBuilder &init(bool init) {
		m_options.init = init;
		return *this;
	}

	CommandOptionsBuilder &guild_only(bool guild_only) {
		m_options.guild_only = guild_only;
		return *this;
	}

	CommandOptionsBuilder &user(bool user) {
		m_options.user = user;
		return *this;
	}

	CommandOptionsBuilder &slash_options(
		std::vector<ekizu::ApplicationCommandOption> slash_options) {
		m_options.slash_options = std::move(slash_options);
		return *this;
	}

	CommandOptionsBuilder &aliases(std::vector<std::string> aliases) {
		m_options.aliases = std::move(aliases);
		return *this;
	}

	CommandOptionsBuilder &usage(std::string_view usage) {
		m_options.usage = std::string{usage};
		return *this;
	}

	CommandOptionsBuilder &description(std::string_view description) {
		m_options.description = std::string{description};
		return *this;
	}

	CommandOptionsBuilder &member_permissions(
		ekizu::Permissions member_permissions) {
		m_options.member_permissions = member_permissions;
		return *this;
	}

	CommandOptionsBuilder &bot_permissions(ekizu::Permissions bot_permissions) {
		m_options.bot_permissions = bot_permissions;
		return *this;
	}

	CommandOptionsBuilder &nsfw(bool nsfw) {
		m_options.nsfw = nsfw;
		return *this;
	}

	CommandOptionsBuilder &owner_only(bool owner_only) {
		m_options.owner_only = owner_only;
		return *this;
	}

	template <typename Rep, typename Period>
	CommandOptionsBuilder &cooldown(
		std::chrono::duration<Rep, Period> cooldown) {
		m_options.cooldown = cooldown;
		return *this;
	}

	CommandOptionsBuilder &examples(std::vector<std::string> examples) {
		m_options.examples = std::move(examples);
		return *this;
	}

	CommandOptionsBuilder &subcommands(std::string_view subcommands) {
		m_options.subcommands = std::string{subcommands};
		return *this;
	}

	CommandOptionsBuilder &activity(bool activity) {
		m_options.activity = activity;
		return *this;
	}

	CommandOptionsBuilder &voice_only(bool voice_only) {
		m_options.voice_only = voice_only;
		return *this;
	}

   private:
	CommandOptions m_options;
};

struct Command {
	Command(Saber &instigator, CommandOptions options_)
		: bot{instigator}, options{std::move(options_)} {};

	Command(const Command &) = delete;
	Command &operator=(const Command &) = delete;
	Command(Command &&) = delete;
	Command &operator=(Command &&) = delete;
	virtual ~Command() = default;

	/// Method reserved for any initial setup for the command.
	[[nodiscard]] virtual Result<> setup(
		[[maybe_unused]] const boost::asio::yield_context &yield) {
		return ekizu::outcome::success();
	}

	/// Method reserved for message commands' execution.
	[[nodiscard]] virtual Result<> execute(
		const ekizu::Message &message, const std::vector<std::string> &args,
		const boost::asio::yield_context &yield) = 0;

	/// Method reserved for interaction commands' execution
	/// (slash/context/autocomplete).
	[[nodiscard]] virtual Result<> execute(
		[[maybe_unused]] const ekizu::Interaction &interaction,
		[[maybe_unused]] const boost::asio::yield_context &yield) {
		return ekizu::outcome::success();
	}

	Saber &bot;
	CommandOptions options;
};

#if defined(__linux__) || defined(__APPLE__)
#define COMMAND_ALLOC(name)                                              \
	extern "C" {                                                         \
	__attribute__((visibility("default"))) saber::Command *init_command( \
		saber::Saber &parent) {                                          \
		return new name(parent);                                         \
	}                                                                    \
	}
#elif defined(_WIN32)
#define COMMAND_ALLOC(name)                                                    \
	extern "C" {                                                               \
	__declspec(dllexport) saber::Command *init_command(saber::Saber &parent) { \
		return new name(parent);                                               \
	}                                                                          \
	}
#endif

#if defined(__linux__) || defined(__APPLE__)
#define COMMAND_FREE(name)                                    \
	extern "C" {                                              \
	__attribute__((visibility("default"))) void free_command( \
		saber::Command *command) {                            \
		delete command;                                       \
	}                                                         \
	}
#elif defined(_WIN32)
#define COMMAND_FREE(name)                                             \
	extern "C" {                                                       \
	__declspec(dllexport) void free_command(saber::Command *command) { \
		delete command;                                                \
	}                                                                  \
	}
#endif

// I miss javascript
#define DIRNAME \
	std::filesystem::path(__FILE__).parent_path().filename().string()
}  // namespace saber

#endif	// SABER_COMMANDS_HPP
