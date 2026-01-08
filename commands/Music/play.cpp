#include <boost/algorithm/string/join.hpp>
#include <ekizu/message.hpp>
#include <saber/util.hpp>

using namespace saber;

struct Play : Command {
	explicit Play(Saber &creator)
		: Command(creator,
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

		bot.set_now_playing_channel(*message.guild_id, message.channel_id);

		SABER_TRY(bot.player().connect(
			*message.guild_id, *voice_state->channel_id, yield));
		SABER_TRY(auto track, bot.player().play(*message.guild_id, query,
												message.author.id, yield));

		SABER_TRY(
			bot.http()
				.create_message(message.channel_id)
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
