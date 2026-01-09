#include <ekizu/error_context.hpp>
#include <saber/util.hpp>

using namespace saber;

struct SyncCommands : Command {
	explicit SyncCommands(Saber &creator)
		: Command(
			  creator,
			  CommandOptionsBuilder()
				  .name("synccommands")
				  .category(DIRNAME)
				  .enabled(true)
				  .aliases({"sync", "registercommands"})
				  .guild_only(false)
				  .usage("synccommands [global]")
				  .description("Registers slash commands with Discord.")
				  .owner_only(true)
				  .bot_permissions(ekizu::Permissions::SendMessages)
				  .cooldown(std::chrono::seconds(30))
				  .slash_options(
					  {ekizu::ApplicationCommandOptionBuilder()
						   .type(ekizu::ApplicationCommandOptionType::Boolean)
						   .name("global")
						   .description(
							   "Register globally (takes up to 1 hour)")
						   .required(false)
						   .build()})
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

		return do_sync(
			global, message.guild_id,
			[&](std::string content) {
				return bot.http()
					.create_message(message.channel_id)
					.content(std::move(content))
					.reply(message.id)
					.send(yield);
			},
			yield);
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		const bool global =
			util::get_option<bool>(interaction, "global").value_or(false);

		// Defer the response since registration may take time
		SABER_TRY(
			bot.http()
				.interaction(interaction.application_id)
				.create_response(interaction.id, interaction.token,
								 ekizu::InteractionResponseBuilder()
									 .type(ekizu::InteractionResponseType::
											   DeferredChannelMessageWithSource)
									 .build())
				.send(yield));

		return do_sync(
			global, interaction.guild_id,
			[&](std::string content) {
				return bot.http()
					.interaction(interaction.application_id)
					.edit_original_response(interaction.token)
					.content(std::move(content))
					.send(yield);
			},
			yield);
	}

   private:
	template <typename SendReply>
	Result<> do_sync(bool global, std::optional<ekizu::Snowflake> guild_id,
					 SendReply send_reply,
					 const boost::asio::yield_context &yield) {
		if (global) {
			auto result = bot.register_slash_commands(yield);
			std::string error_ctx{ekizu::last_error_context()};

			if (!result) {
				SABER_TRY(send_reply(fmt::format(
					"❌ Failed to register global "
					"commands: {}\n\nDetails: {}",
					result.error().message(), error_ctx)));
				return result.error();
			}

			SABER_TRY(send_reply(fmt::format(
				"✅ Successfully registered {} global slash commands! "
				"(may take up to 1 hour to propagate)",
				result.value().size())));
		} else {
			if (!guild_id) {
				SABER_TRY(send_reply(
					"❌ Guild commands can only be "
					"registered from within a guild. Use "
					"`/synccommands global:true` for global "
					"registration."));
				return outcome::success();
			}

			auto result = bot.register_guild_slash_commands(*guild_id, yield);
			std::string error_ctx{ekizu::last_error_context()};

			if (!result) {
				SABER_TRY(send_reply(fmt::format(
					"❌ Failed to register guild "
					"commands: {}\n\nDetails: {}",
					result.error().message(), error_ctx)));
				return result.error();
			}

			SABER_TRY(send_reply(fmt::format(
				"✅ Successfully registered {} guild slash commands! "
				"They should be available immediately.",
				result.value().size())));
		}

		return outcome::success();
	}
};

COMMAND_ALLOC(SyncCommands)
COMMAND_FREE(SyncCommands)
