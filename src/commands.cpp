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

std::optional<ekizu::Snowflake> interaction_user_id(
	const ekizu::Interaction &i) {
	if (i.member) { return i.member->user.id; }
	if (i.user) { return i.user->id; }
	return std::nullopt;
}

std::optional<ekizu::Snowflake> interaction_channel_id(
	const ekizu::Interaction &i) {
	if (i.channel_id) { return i.channel_id; }
	if (i.channel) { return i.channel->id; }
	return std::nullopt;
}

bool has_permissions(ekizu::Permissions granted, ekizu::Permissions required) {
	using U = std::underlying_type_t<ekizu::Permissions>;
	const auto g = static_cast<U>(granted);
	const auto r = static_cast<U>(required);
	return (g & r) == r;
}

std::optional<ekizu::Permissions> parse_member_permissions(
	const ekizu::Interaction &i) {
	if (!i.member) { return std::nullopt; }
	// Discord provides interaction member permissions as a string integer.
	try {
		const auto v = std::stoull(i.member->permissions);
		if (v > std::numeric_limits<
					std::underlying_type_t<ekizu::Permissions>>::max()) {
			return std::nullopt;
		}
		return static_cast<ekizu::Permissions>(
			static_cast<std::underlying_type_t<ekizu::Permissions>>(v));
	} catch (...) { return std::nullopt; }
}
}  // namespace

namespace saber {
CommandLoader::CommandLoader(Saber &parent) : m_parent{parent} {}

void CommandLoader::load(std::string_view path,
						 const boost::asio::yield_context &yield) {
	auto library = Library::create(path);

	if (!library) {
		m_parent.log<ekizu::LogLevel::Error>(
			"Failed to load command {}: {}", path, library.error().message());
		return;
	}

	auto init_command =
		library.value().get<Command *(*)(Saber &)>("init_command");
	auto free_command =
		library.value().get<void (*)(Command *)>("free_command");

	if (!init_command || !free_command) { return; }

	std::scoped_lock lk{m_mtx};
	auto *command_ptr = (init_command.value())(m_parent);

	if (command_ptr == nullptr) { return; }

	if (command_ptr->options.init) {
		auto res = command_ptr->setup(yield);

		if (!res) {
			m_parent.log<ekizu::LogLevel::Error>(
				"Failed to setup command {}: {}", command_ptr->options.name,
				res.error().message());
			return;
		}
	}

	const auto &command_name = command_ptr->options.name;
	const std::shared_ptr<Command> loader{
		command_ptr,
		[dealloc = free_command.value()](Command *ptr) { dealloc(ptr); }};

	commands.insert_or_assign(command_name, std::move(library.value()));
	command_map.insert_or_assign(command_name, loader);

	for (const auto &alias : command_ptr->options.aliases) {
		alias_map.insert_or_assign(alias, loader);
	}

	if (command_ptr->options.slash_options) {
		slash_commands.insert_or_assign(command_name, loader);
	}

	if (command_ptr->options.user) {
		user_commands.insert_or_assign(command_name, loader);
	}

	m_parent.log<ekizu::LogLevel::Info>("Loaded command {}", command_name);
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

	if (cmd->options.guild_only) {
		if (!message.guild_id) {
			SABER_TRY(m_parent.http()
						  .create_message(message.channel_id)
						  .content("This command can only be used in guilds.")
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}
	}

	SABER_TRY(util::ensure_permissions(m_parent, message, m_parent.bot_id(),
									   cmd->options.bot_permissions, yield));
	SABER_TRY(util::ensure_permissions(m_parent, message, message.author.id,
									   cmd->options.member_permissions, yield));

	if (m_parent.command_cooldowns().contains(message.author.id)) {
		auto cooldown = m_parent.command_cooldowns().at(message.author.id);

		if (cooldown.contains(command_name)) {
			auto expiry = cooldown.at(command_name);
			auto delta = std::chrono::floor<std::chrono::seconds>(
							 expiry - std::chrono::steady_clock::now())
							 .count();

			if (delta > 0) {
				SABER_TRY(
					m_parent.http()
						.create_message(message.channel_id)
						.content(fmt::format(
							"Please wait {} more seconds before using this "
							"command.",
							delta))
						.reply(message.id)
						.send(yield));
				return outcome::success();
			}
		}
	}

	m_parent.command_cooldowns()[message.author.id][command_name] =
		std::chrono::steady_clock::now() + cmd->options.cooldown;

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

	const auto user_id = interaction_user_id(interaction);
	if (!user_id) { return outcome::success(); }

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

	// guild_only gate
	if (cmd->options.guild_only && !interaction.guild_id) {
		if (auto ch = interaction_channel_id(interaction)) {
			SABER_TRY(m_parent.http()
						  .create_message(*ch)
						  .content("This command can only be used in guilds.")
						  .send(yield));
		}
		return outcome::success();
	}

	// bot permission gate (best-effort: only enforce if Discord provided them)
	if (static_cast<std::underlying_type_t<ekizu::Permissions>>(
			cmd->options.bot_permissions) != 0) {
		if (interaction.app_permissions &&
			!has_permissions(
				*interaction.app_permissions, cmd->options.bot_permissions)) {
			if (auto ch = interaction_channel_id(interaction)) {
				SABER_TRY(
					m_parent.http()
						.create_message(*ch)
						.content("I don't have permission to run that here.")
						.send(yield));
			}
			return outcome::success();
		}
	}

	// member permission gate (best-effort: only enforce if provided)
	if (static_cast<std::underlying_type_t<ekizu::Permissions>>(
			cmd->options.member_permissions) != 0) {
		const auto member_perms = parse_member_permissions(interaction);
		if (member_perms &&
			!has_permissions(*member_perms, cmd->options.member_permissions)) {
			if (auto ch = interaction_channel_id(interaction)) {
				SABER_TRY(
					m_parent.http()
						.create_message(*ch)
						.content(
							"You don't have permission to use that command.")
						.send(yield));
			}
			return outcome::success();
		}
	}

	// cooldown gate
	if (m_parent.command_cooldowns().contains(*user_id)) {
		auto cooldown = m_parent.command_cooldowns().at(*user_id);

		if (cooldown.contains(command_name)) {
			auto expiry = cooldown.at(command_name);
			auto delta = std::chrono::floor<std::chrono::seconds>(
							 expiry - std::chrono::steady_clock::now())
							 .count();

			if (delta > 0) {
				if (auto ch = interaction_channel_id(interaction)) {
					SABER_TRY(
						m_parent.http()
							.create_message(*ch)
							.content(fmt::format(
								"Please wait {} more seconds before using this "
								"command.",
								delta))
							.send(yield));
				}
				return outcome::success();
			}
		}
	}

	m_parent.command_cooldowns()[*user_id][command_name] =
		std::chrono::steady_clock::now() + cmd->options.cooldown;

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