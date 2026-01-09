#include <saber/util.hpp>

using namespace saber;

struct Pause : Command {
	explicit Pause(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("pause")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(true)
					  .usage("pause")
					  .description("Pauses the player.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .slash_options({})  // No options needed
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, message, yield));
		SABER_TRY(bot.player().connect(
			*message.guild_id, *voice_state->channel_id, yield));
		SABER_TRY(bot.player().pause(*message.guild_id));
		SABER_TRY(bot.http()
					  .create_reaction(message.channel_id, message.id, "⏸️")
					  .send(yield));
		return outcome::success();
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		if (!interaction.guild_id) {
			return boost::system::errc::operation_not_permitted;
		}

		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, interaction, yield));
		SABER_TRY(bot.player().connect(
			*interaction.guild_id, *voice_state->channel_id, yield));
		SABER_TRY(bot.player().pause(*interaction.guild_id));

		SABER_TRY(bot.http()
					  .interaction(interaction.application_id)
					  .create_response(
						  interaction.id, interaction.token,
						  ekizu::InteractionResponseBuilder()
							  .type(ekizu::InteractionResponseType::
										ChannelMessageWithSource)
							  .content("⏸️ Paused")
							  .build())
					  .send(yield));
		return outcome::success();
	}
};

COMMAND_ALLOC(Pause)
COMMAND_FREE(Pause)
