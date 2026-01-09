#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <filesystem>
#include <limits>
#include <saber/util.hpp>
#include <type_traits>

namespace {
#ifdef _WIN32
constexpr boost::string_view LIBRARY_EXTENSION{".dll"};
#elif __linux__
constexpr boost::string_view LIBRARY_EXTENSION{".so"};
#elif __APPLE__
constexpr boost::string_view LIBRARY_EXTENSION{".dylib"};
#endif

bool has_permissions(ekizu::Permissions granted, ekizu::Permissions required) {
	using U = std::underlying_type_t<ekizu::Permissions>;
	const auto g = static_cast<U>(granted);
	const auto r = static_cast<U>(required);
	return (g & r) == r;
}

}  // namespace

namespace saber {

// ---------------------------------------------------------------------------
// CommandInvocation - common fields extracted from Message or Interaction
// ---------------------------------------------------------------------------

struct CommandInvocation {
	ekizu::Snowflake user_id;
	std::optional<ekizu::Snowflake> guild_id;
	std::optional<ekizu::Snowflake> channel_id;
	std::optional<ekizu::Snowflake> message_id;	 // for replies (message only)
	ekizu::Permissions bot_permissions{};
	ekizu::Permissions member_permissions{};
};

static CommandInvocation from_message(const ekizu::Message &msg,
									  [[maybe_unused]] Saber &bot) {
	CommandInvocation inv;
	inv.user_id = msg.author.id;
	inv.guild_id = msg.guild_id;
	inv.channel_id = msg.channel_id;
	inv.message_id = msg.id;

	// For messages, we need to look up permissions from the guild cache.
	// This is best-effort - we'll check them via util::ensure_permissions.
	inv.bot_permissions = ekizu::Permissions{};
	inv.member_permissions = ekizu::Permissions{};

	return inv;
}

static CommandInvocation from_interaction(const ekizu::Interaction &i) {
	CommandInvocation inv;

	// Extract user ID
	if (i.member) {
		inv.user_id = i.member->user.id;
	} else if (i.user) {
		inv.user_id = i.user->id;
	}

	inv.guild_id = i.guild_id;

	// Extract channel ID
	if (i.channel_id) {
		inv.channel_id = i.channel_id;
	} else if (i.channel) {
		inv.channel_id = i.channel->id;
	}

	// Discord provides bot permissions directly on interactions
	if (i.app_permissions) { inv.bot_permissions = *i.app_permissions; }

	// Parse member permissions from the interaction
	if (i.member) {
		try {
			const auto v = std::stoull(i.member->permissions);
			if (v <= std::numeric_limits<
						 std::underlying_type_t<ekizu::Permissions>>::max()) {
				inv.member_permissions = static_cast<ekizu::Permissions>(
					static_cast<std::underlying_type_t<ekizu::Permissions>>(v));
			}
		} catch (...) {}
	}

	return inv;
}

// ---------------------------------------------------------------------------
// Gate check result - used to determine how to respond to gate failures
// ---------------------------------------------------------------------------

enum class GateResult {
	Passed,
	GuildOnly,
	BotPermission,
	MemberPermission,
	Cooldown
};

struct GateCheckResult {
	GateResult result{GateResult::Passed};
	int64_t cooldown_remaining{0};	// seconds remaining if cooldown active
};

// ---------------------------------------------------------------------------
// Common gate checks - extracted to reduce duplication
// ---------------------------------------------------------------------------

static GateCheckResult check_common_gates(Saber &bot, Command &cmd,
										  const CommandInvocation &inv,
										  const std::string &command_name) {
	GateCheckResult result;

	// Owner bypasses all gates except guild-only (which is a technical
	// requirement)
	const bool is_owner = (inv.user_id == bot.owner_id());

	// Guild-only gate - even owner can't use guild commands in DMs
	if (cmd.options.guild_only && !inv.guild_id) {
		result.result = GateResult::GuildOnly;
		return result;
	}

	// Permission gates only apply in guilds - DMs don't have permission
	// concepts Owner bypasses member permission checks
	if (inv.guild_id && !is_owner) {
		// Bot permission gate (only for interactions where we have the info)
		using U = std::underlying_type_t<ekizu::Permissions>;
		if (static_cast<U>(cmd.options.bot_permissions) != 0 &&
			static_cast<U>(inv.bot_permissions) != 0) {
			if (!has_permissions(
					inv.bot_permissions, cmd.options.bot_permissions)) {
				result.result = GateResult::BotPermission;
				return result;
			}
		}

		// Member permission gate (only for interactions where we have the info)
		if (static_cast<U>(cmd.options.member_permissions) != 0 &&
			static_cast<U>(inv.member_permissions) != 0) {
			if (!has_permissions(
					inv.member_permissions, cmd.options.member_permissions)) {
				result.result = GateResult::MemberPermission;
				return result;
			}
		}
	}

	// Cooldown gate - owner bypasses cooldowns
	if (!is_owner && bot.command_cooldowns().contains(inv.user_id)) {
		auto &cooldown = bot.command_cooldowns().at(inv.user_id);

		if (cooldown.contains(command_name)) {
			auto expiry = cooldown.at(command_name);
			auto delta = std::chrono::floor<std::chrono::seconds>(
							 expiry - std::chrono::steady_clock::now())
							 .count();

			if (delta > 0) {
				result.result = GateResult::Cooldown;
				result.cooldown_remaining = delta;
				return result;
			}
		}
	}

	return result;
}

static void update_cooldown(Saber &bot, const CommandInvocation &inv,
							const std::string &command_name,
							std::chrono::steady_clock::duration cooldown) {
	bot.command_cooldowns()[inv.user_id][command_name] =
		std::chrono::steady_clock::now() + cooldown;
}

// ---------------------------------------------------------------------------
// CommandLoader implementation
// ---------------------------------------------------------------------------

CommandLoader::CommandLoader(Saber &parent) : m_parent{parent} {}

void CommandLoader::load(std::string_view path,
						 const boost::asio::yield_context &yield) {
	auto library_result = Library::create(path);

	if (!library_result) {
		m_parent.log<ekizu::LogLevel::Error>("Failed to load library {}", path);
		return;
	}

	auto &library = library_result.value();

	auto init_result = library.get<Command *(*)(Saber &)>("init_command");

	if (!init_result) {
		m_parent.log<ekizu::LogLevel::Error>(
			"Failed to find init_command in {}", path);
		return;
	}

	auto init_fn = init_result.value();
	auto free_fn_result = library.get<void (*)(Command *)>("free_command");
	auto free_fn = free_fn_result ? free_fn_result.value() : nullptr;

	const auto command_ptr =
		std::shared_ptr<Command>(init_fn(m_parent), free_fn);

	if (!command_ptr) {
		m_parent.log<ekizu::LogLevel::Error>(
			"init_command returned nullptr for {}", path);
		return;
	}

	if (!command_ptr->options.enabled) {
		m_parent.log<ekizu::LogLevel::Info>(
			"Command {} is disabled, skipping load", command_ptr->options.name);
		return;
	}

	if (command_ptr->options.init) {
		if (!command_ptr->setup(yield)) {
			m_parent.log<ekizu::LogLevel::Error>(
				"Failed to setup command {}", command_ptr->options.name);
			return;
		}
	}

	std::scoped_lock lk{m_mtx};
	std::string command_name = command_ptr->options.name;
	commands.insert_or_assign(command_name, std::move(library));
	command_map.insert_or_assign(command_name, command_ptr);

	for (const auto &alias : command_ptr->options.aliases) {
		alias_map.insert_or_assign(alias, command_ptr);
	}

	if (command_ptr->options.slash_options) {
		slash_commands.insert_or_assign(command_name, command_ptr);
	}

	if (command_ptr->options.user) {
		user_commands.insert_or_assign(command_name, command_ptr);
	}

	// Log with command type indicators
	const bool has_slash = command_ptr->options.slash_options.has_value();
	const bool has_user_ctx = command_ptr->options.user;

	m_parent.log<ekizu::LogLevel::Info>(
		"Loaded {:15} [msg: Y] [slash: {}] [user: {}]", command_name,
		has_slash ? "Y" : "N", has_user_ctx ? "Y" : "N");
}

void CommandLoader::load_all(const boost::asio::yield_context &yield) {
	namespace fs = std::filesystem;

	for (const auto &file : fs::directory_iterator(".")) {
		const auto filename = file.path().filename().string();
		boost::string_view filename_sv{filename};

		if (filename_sv.starts_with("cmd_") &&
			filename_sv.ends_with(LIBRARY_EXTENSION)) {
			load(fs::absolute(file.path()).lexically_normal().string(), yield);
		}
	}

	m_parent.log<ekizu::LogLevel::Info>("Loaded {} commands", commands.size());
}

Result<> CommandLoader::process_commands(
	const ekizu::Message &message, const boost::asio::yield_context &yield) {
	if (message.author.bot) { return outcome::success(); }

	auto content = message.content.substr(m_parent.prefix().size());
	std::vector<std::string> args;
	boost::algorithm::split(
		args, boost::algorithm::trim_copy(content), boost::is_any_of(" "));

	if (args.empty()) { return outcome::success(); }

	auto command_name = std::move(args.front());
	args.erase(args.begin());

	std::transform(
		command_name.begin(), command_name.end(), command_name.begin(),
		[](uint8_t c) { return static_cast<char>(std::tolower(c)); });

	std::unique_lock lk{m_mtx};
	std::shared_ptr<Command> cmd;

	if (commands.contains(command_name)) {
		cmd = command_map.at(command_name);
	} else if (alias_map.contains(command_name)) {
		cmd = alias_map.at(command_name);
	}

	if (!cmd) { return outcome::success(); }

	auto inv = from_message(message, m_parent);

	// For messages, use the existing permission check utilities
	// which look up permissions from the guild cache
	if (cmd->options.guild_only && !message.guild_id) {
		SABER_TRY(m_parent.http()
					  .create_message(message.channel_id)
					  .content("This command can only be used in guilds.")
					  .reply(message.id)
					  .send(yield));
		return outcome::success();
	}

	SABER_TRY(util::ensure_permissions(m_parent, message, m_parent.bot_id(),
									   cmd->options.bot_permissions, yield));
	SABER_TRY(util::ensure_permissions(m_parent, message, message.author.id,
									   cmd->options.member_permissions, yield));

	// Cooldown check
	auto gate_result = check_common_gates(m_parent, *cmd, inv, command_name);
	if (gate_result.result == GateResult::Cooldown) {
		SABER_TRY(
			m_parent.http()
				.create_message(message.channel_id)
				.content(fmt::format(
					"Please wait {} more seconds before using this command.",
					gate_result.cooldown_remaining))
				.reply(message.id)
				.send(yield));
		return outcome::success();
	}

	update_cooldown(m_parent, inv, command_name, cmd->options.cooldown);

	// NOTE: I'm seeing a case in which the commands will need the lock so it
	// should be unlocked here. i.e. an unload command or something.
	lk.unlock();

	return cmd->execute(message, args, yield);
}

Result<> CommandLoader::process_commands(
	const ekizu::Interaction &interaction,
	const boost::asio::yield_context &yield) {
	// Only handle application command interactions here. Component interactions
	// are expected to be handled via collectors/filters instead.
	if (interaction.type != ekizu::InteractionType::ApplicationCommand &&
		interaction.type !=
			ekizu::InteractionType::ApplicationCommandAutocomplete) {
		return outcome::success();
	}

	auto inv = from_interaction(interaction);
	if (inv.user_id == ekizu::Snowflake{}) { return outcome::success(); }

	if (!interaction.data) { return outcome::success(); }

	const ekizu::ApplicationCommandData *acmd = nullptr;
	std::visit(
		[&acmd](const auto &d) {
			using T = std::decay_t<decltype(d)>;
			if constexpr (std::is_same_v<T, ekizu::ApplicationCommandData>) {
				acmd = &d;
			}
		},
		*interaction.data);

	if (!acmd) { return outcome::success(); }

	auto command_name = acmd->name;
	std::transform(
		command_name.begin(), command_name.end(), command_name.begin(),
		[](uint8_t c) { return static_cast<char>(std::tolower(c)); });

	std::unique_lock lk{m_mtx};
	std::shared_ptr<Command> cmd;

	// Route based on application command type (chat input vs user context).
	if (acmd->type == ekizu::ApplicationCommandType::User) {
		if (user_commands.contains(command_name)) {
			cmd = user_commands.at(command_name);
		}
	} else {
		// ChatInput and Message context both use the "slash command" bucket in
		// this codebase for now.
		if (slash_commands.contains(command_name)) {
			cmd = slash_commands.at(command_name);
		}
	}

	if (!cmd) { return outcome::success(); }

	// Check common gates
	auto gate_result = check_common_gates(m_parent, *cmd, inv, command_name);

	// Helper to send ephemeral interaction response for gate failures
	auto send_gate_error = [&](std::string content) -> Result<> {
		SABER_TRY(m_parent.http()
					  .interaction(interaction.application_id)
					  .create_response(
						  interaction.id, interaction.token,
						  ekizu::InteractionResponseBuilder()
							  .type(ekizu::InteractionResponseType::
										ChannelMessageWithSource)
							  .content(std::move(content))
							  .flags(ekizu::MessageFlags::Ephemeral)
							  .build())
					  .send(yield));
		return outcome::success();
	};

	switch (gate_result.result) {
		case GateResult::GuildOnly:
			return send_gate_error("This command can only be used in guilds.");

		case GateResult::BotPermission:
			return send_gate_error("I don't have permission to run that here.");

		case GateResult::MemberPermission:
			return send_gate_error(
				"You don't have permission to use that command.");

		case GateResult::Cooldown:
			return send_gate_error(fmt::format(
				"Please wait {} more seconds before using this command.",
				gate_result.cooldown_remaining));

		case GateResult::Passed: break;
	}

	update_cooldown(m_parent, inv, command_name, cmd->options.cooldown);

	// Same rationale as message path: commands may need the lock (e.g. unload).
	lk.unlock();

	return cmd->execute(interaction, yield);
}

void CommandLoader::unload(const std::string &name) {
	std::scoped_lock lk{m_mtx};

	if (!commands.contains(name)) { return; }

	const auto command = std::move(command_map.at(name));

	for (const auto &alias : command->options.aliases) {
		alias_map.erase(alias);
	}

	commands.erase(name);
	command_map.erase(name);
	slash_commands.erase(name);
	user_commands.erase(name);
	m_parent.log<ekizu::LogLevel::Info>("Unloaded command {}", name);
}

void CommandLoader::get_commands(
	ekizu::FunctionView<void(const boost::unordered_flat_map<
							 std::string, std::shared_ptr<Command>> &)>
		cb) const {
	std::scoped_lock lk{m_mtx};

	cb(command_map);
}
}  // namespace saber