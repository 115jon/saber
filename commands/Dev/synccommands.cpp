#include <saber/util.hpp>

using namespace saber;

struct SyncCommands : Command {
	explicit SyncCommands(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("synccommands")
					  .category(DIRNAME)
					  .enabled(true)
					  .aliases({"sync", "registercommands"})
					  .guild_only(false)
					  .usage("synccommands [global]")
					  .description(
						  "Registers slash commands with Discord. Use 'global' "
						  "flag for global registration (takes up to 1 hour).")
					  .owner_only(true)
					  .bot_permissions(ekizu::Permissions::SendMessages)
					  .cooldown(std::chrono::seconds(30))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		const bool global = !args.empty() && args[0] == "global";

		SABER_TRY(
			bot.http()
				.create_message(message.channel_id)
				.content(global ? "📝 Registering global slash commands... "
								  "(this may take up to 1 hour to propagate)"
								: "📝 Registering guild slash commands...")
				.reply(message.id)
				.send(yield));

		if (global) {
			auto result = bot.register_slash_commands(yield);
			if (!result) {
				SABER_TRY(
					bot.http()
						.create_message(message.channel_id)
						.content(fmt::format("❌ Failed to register global "
											 "commands: {}",
											 result.error().message()))
						.reply(message.id)
						.send(yield));
				return result.error();
			}

			SABER_TRY(
				bot.http()
					.create_message(message.channel_id)
					.content(fmt::format(
						"✅ Successfully registered {} global slash commands!",
						result.value().size()))
					.reply(message.id)
					.send(yield));
		} else {
			if (!message.guild_id) {
				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .content("❌ Guild commands can only be "
									   "registered from within a guild. Use "
									   "`synccommands global` for global "
									   "registration.")
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			auto result =
				bot.register_guild_slash_commands(*message.guild_id, yield);
			if (!result) {
				SABER_TRY(
					bot.http()
						.create_message(message.channel_id)
						.content(fmt::format("❌ Failed to register guild "
											 "commands: {}",
											 result.error().message()))
						.reply(message.id)
						.send(yield));
				return result.error();
			}

			SABER_TRY(
				bot.http()
					.create_message(message.channel_id)
					.content(fmt::format(
						"✅ Successfully registered {} guild slash commands! "
						"They should be available immediately.",
						result.value().size()))
					.reply(message.id)
					.send(yield));
		}

		return outcome::success();
	}
};

COMMAND_ALLOC(SyncCommands)
COMMAND_FREE(SyncCommands)
