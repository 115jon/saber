#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <saber/saber.hpp>

using namespace saber;

// clang-format off
boost::unordered_flat_map<std::string_view, std::string_view> CATEGORY_EMOJIS{
    {"Music", "🎵",},
    {"Fun",  "🎉",},
    {"Hentai", "🔞",},
    {"Moderation", "🛠️",},
    {"Owner", "👑",},
    {"Templates",  "📷",},
    {"Steam", "🚂",},
    {"Guild", "🏠",},
    {"Misc", "❓",},
    {"Activity", "👷"}
};
// clang-format on

struct Help : Command {
	explicit Help(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("help")
					  .category(DIRNAME)
					  .enabled(true)
					  .init(true)
					  .usage("help")
					  .examples({"help", "help ping"})
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(2))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		if (args.empty()) { return get_help(message, yield); }

		return get_command_help(message, args[0], yield);
	}

	Result<> get_help(const ekizu::Message &message,
					  const boost::asio::yield_context &yield) {
		boost::unordered_flat_map<std::string,
								  std::vector<std::shared_ptr<Command>>>
			categories;

		bot.commands().get_commands(
			[&](const boost::unordered_flat_map<
				std::string, std::shared_ptr<Command>> &commands) {
				for (const auto &[_, command] : commands) {
					categories[command->options.category].push_back(command);
				}
			});

		std::vector<ekizu::SelectOptions> opts;

		for (const auto &[category, _] : categories) {
			opts.emplace_back(
				ekizu::SelectOptionsBuilder()
					.label(category)
					.value(category)
					.description(
						fmt::format("Commands from the {} category.", category))
					.emoji(ekizu::PartialEmoji{
						{}, std::string{CATEGORY_EMOJIS.at(category)}, {}})
					.build());
		}

		SABER_TRY(
			auto msg,
			bot.http()
				.create_message(message.channel_id)
				.components(
					{ekizu::ActionRowBuilder()
						 .components(
							 {ekizu::SelectMenuBuilder()
								  .custom_id("help_menu")
								  .placeholder("Please select a category.")
								  .options(std::move(opts))
								  .build()})
						 .build()})
				.embeds({ekizu::EmbedBuilder()
							 .set_description("Please choose a category.")
							 .build()})
				.reply(message.id)
				.send(yield));

		auto filter = [author_id = message.author.id](
						  const ekizu::Interaction &interaction,
						  const ekizu::MessageComponentData &data) {
			std::optional<ekizu::Snowflake> id;

			if (interaction.member) {
				id = interaction.member->user.id;
			} else if (interaction.user) {
				id = interaction.user->id;
			}

			return data.custom_id == "help_menu" && id == author_id;
		};

		auto collector = bot.create_message_component_collector(
			message.channel_id, filter, ekizu::ComponentType::SelectMenu,
			std::chrono::seconds(30), yield);

		while (true) {
			auto res = collector->async_receive(yield);
			if (!res) { break; }

			auto &[i, data] = res.value();

			const auto &values = data.values;
			if (values.empty()) { continue; }

			const auto &category = values[0];

			auto builder =
				ekizu::EmbedBuilder()
					.set_title(fmt::format("{} commands", category))
					.set_description("Here are the commands you can use");

			for (const auto &command : categories.at(category)) {
				builder.add_field(ekizu::EmbedField{
					command->options.name,
					command->options.description,
					true,
				});
			}

			SABER_TRY(
				bot.http()
					.interaction(i.application_id)
					.create_response(
						i.id, i.token,
						ekizu::InteractionResponseBuilder()
							.embeds({builder.build()})
							.type(ekizu::InteractionResponseType::UpdateMessage)
							.build())
					.send(yield))
		}

		auto &action_row = std::get<ekizu::ActionRow>(msg.components[0]);
		auto &select_menu =
			std::get<ekizu::SelectMenu>(action_row.components[0]);
		select_menu.disabled = true;

		SABER_TRY(bot.http()
					  .edit_message(msg.channel_id, msg.id)
					  .components(std::move(msg.components))
					  .send(yield));

		return ekizu::outcome::success();
	}

	Result<> get_command_help(const ekizu::Message &message,
							  const std::string &command,
							  const boost::asio::yield_context &yield) {
		bot.commands().get_commands(
			[&, this](const boost::unordered_flat_map<
					  std::string, std::shared_ptr<Command>> &commands) {
				if (!commands.contains(command)) {
					(void)bot.http()
						.create_message(message.channel_id)
						.content("Command not found.")
						.send(yield);
					return;
				}

				const auto &cmd = commands.at(command);
				auto builder =
					ekizu::EmbedBuilder{}.set_title(cmd->options.name);

				if (!cmd->options.description.empty()) {
					builder.set_description(cmd->options.description);
				}

				if (!cmd->options.examples.empty()) {
					builder.add_field(
						{"❯ Examples",
						 boost::algorithm::join(
							 cmd->options.examples |
								 boost::adaptors::transformed(
									 [this](const auto &example) {
										 return fmt::format(
											 "`{}{}`\n", bot.prefix(), example);
									 }),
							 "")});
				}

				if (!cmd->options.usage.empty()) {
					builder.add_field(
						{"❯ Usage", fmt::format("`{}{}`", bot.prefix(),
												cmd->options.usage)});
				}

				if (!cmd->options.aliases.empty()) {
					builder.add_field(
						{"❯ Aliases",
						 boost::algorithm::join(
							 cmd->options.aliases |
								 boost::adaptors::transformed(
									 [](const auto &alias) {
										 return fmt::format("`{}`", alias);
									 }),
							 " | ")});
				}

				builder.add_field(
					{"❯ Cooldown",
					 fmt::format(
						 "{}ms",
						 std::chrono::duration_cast<std::chrono::milliseconds>(
							 cmd->options.cooldown)
							 .count())});
				builder.add_field(
					{"❯ Legend", fmt::format("`<> required, [] optional`")});
				builder.set_footer({fmt::format(
					"Prefix: {}",
					bot.prefix().empty() ? "None" : bot.prefix())});

				(void)bot.http()
					.create_message(message.channel_id)
					.embeds({builder.build()})
					.send(yield);
			});

		return outcome::success();
	}
};

COMMAND_ALLOC(Help)
COMMAND_FREE(Help)
