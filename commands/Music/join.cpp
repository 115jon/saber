#include <saber/util.hpp>

using namespace saber;

struct Join : Command {
	explicit Join(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("join")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(true)
					  .usage("join")
					  .description("Joins the voice channel.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .slash_options({})  // No options needed
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		SABER_TRY(auto guild, bot.get_guild(*message.guild_id, yield));
		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, message, yield));
		SABER_TRY(auto just_connected,
				  bot.player().connect(
					  *message.guild_id, *voice_state->channel_id, yield));

		if (!just_connected) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("I am already in a voice channel!")
						  .reply(message.id)
						  .send(yield));
		}

		return outcome::success();
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		if (!interaction.guild_id) {
			return boost::system::errc::operation_not_permitted;
		}

		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, interaction, yield));
		SABER_TRY(auto just_connected,
				  bot.player().connect(
					  *interaction.guild_id, *voice_state->channel_id, yield));

		std::string content =
			just_connected ? "🎵 Joined voice channel!"
						   : "I am already in a voice channel!";

		SABER_TRY(bot.http()
					  .interaction(interaction.application_id)
					  .create_response(
						  interaction.id, interaction.token,
						  ekizu::InteractionResponseBuilder()
							  .type(ekizu::InteractionResponseType::
										ChannelMessageWithSource)
							  .content(std::move(content))
							  .build())
					  .send(yield));
		return outcome::success();
	}
};

COMMAND_ALLOC(Join)
COMMAND_FREE(Join)
