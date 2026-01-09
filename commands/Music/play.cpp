#include <boost/algorithm/string/join.hpp>
#include <ekizu/message.hpp>
#include <saber/util.hpp>

using namespace saber;

struct Play : Command {
	explicit Play(Saber &creator)
		: Command(
			  creator,
			  CommandOptionsBuilder()
				  .name("play")
				  .category(DIRNAME)
				  .enabled(true)
				  .aliases({"p"})
				  .guild_only(true)
				  .usage("play <query>")
				  .description("Play a youtube video in the voice channel.")
				  .bot_permissions(ekizu::Permissions::SendMessages |
								   ekizu::Permissions::EmbedLinks)
				  .cooldown(std::chrono::seconds(3))
				  .slash_options(
					  {ekizu::ApplicationCommandOptionBuilder()
						   .type(ekizu::ApplicationCommandOptionType::String)
						   .name("query")
						   .description("YouTube URL or search query")
						   .required(true)
						   .build()})
				  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, message, yield));

		if (args.empty()) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("Please specify a query.")
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		auto query = boost::algorithm::join(args, " ");

		return do_play(*message.guild_id, *voice_state->channel_id,
					   message.channel_id, message.author.id, query, yield);
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		if (!interaction.guild_id) {
			return boost::system::errc::operation_not_permitted;
		}

		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, interaction, yield));

		auto query = util::get_option<std::string>(interaction, "query");
		if (!query) {
			SABER_TRY(bot.http()
						  .interaction(interaction.application_id)
						  .create_response(
							  interaction.id, interaction.token,
							  ekizu::InteractionResponseBuilder()
								  .type(ekizu::InteractionResponseType::
											ChannelMessageWithSource)
								  .content("Please specify a query.")
								  .build())
						  .send(yield));
			return outcome::success();
		}

		auto user_id = util::get_user_id(interaction);
		auto channel_id = util::get_channel_id(interaction);
		if (!user_id || !channel_id) {
			return boost::system::errc::operation_not_permitted;
		}

		// Defer the response since play might take a while
		SABER_TRY(
			bot.http()
				.interaction(interaction.application_id)
				.create_response(interaction.id, interaction.token,
								 ekizu::InteractionResponseBuilder()
									 .type(ekizu::InteractionResponseType::
											   DeferredChannelMessageWithSource)
									 .build())
				.send(yield));

		bot.set_now_playing_channel(*interaction.guild_id, *channel_id);

		SABER_TRY(bot.player().connect(
			*interaction.guild_id, *voice_state->channel_id, yield));
		SABER_TRY(auto track, bot.player().play(*interaction.guild_id, *query,
												*user_id, yield));

		// Edit the deferred response with the final content
		SABER_TRY(
			bot.http()
				.interaction(interaction.application_id)
				.edit_original_response(interaction.token)
				.flags(ekizu::MessageFlags::IsComponentsV2)
				.components(
					{ekizu::ContainerBuilder()
						 .accent_color(0x947CEA)
						 .components(
							 {ekizu::TextDisplayBuilder()
								  .content(fmt::format(
									  "<:white_check_mark:1360660615571570800> "
									  "Added **[{}]({})** - `{}` to the queue.",
									  track.metadata.title,
									  track.metadata.webpage_url,
									  util::format_duration(
										  track.metadata.duration_seconds)))
								  .build()})
						 .build()})
				.send(yield));

		return outcome::success();
	}

   private:
	Result<> do_play(ekizu::Snowflake guild_id,
					 ekizu::Snowflake voice_channel_id,
					 ekizu::Snowflake text_channel_id, ekizu::Snowflake user_id,
					 const std::string &query,
					 const boost::asio::yield_context &yield) {
		bot.set_now_playing_channel(guild_id, text_channel_id);

		SABER_TRY(bot.player().connect(guild_id, voice_channel_id, yield));
		SABER_TRY(
			auto track, bot.player().play(guild_id, query, user_id, yield));

		SABER_TRY(
			bot.http()
				.create_message(text_channel_id)
				.flags(ekizu::MessageFlags::IsComponentsV2)
				.components(
					{ekizu::ContainerBuilder()
						 .accent_color(0x947CEA)
						 .components(
							 {ekizu::TextDisplayBuilder()
								  .content(fmt::format(
									  "<:white_check_mark:1360660615571570800> "
									  "Added **[{}]({})** - `{}` to the queue.",
									  track.metadata.title,
									  track.metadata.webpage_url,
									  util::format_duration(
										  track.metadata.duration_seconds)))
								  .build()})
						 .build()})
				.send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(Play)
COMMAND_FREE(Play)
