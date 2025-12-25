#include <saber/util.hpp>

using namespace saber;

struct Skip : Command {
	explicit Skip(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("skip")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(true)
					  .usage("skip")
					  .description("Skips the current song.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, message, yield));
		SABER_TRY(bot.player().connect(
			*message.guild_id, *voice_state->channel_id, yield));
		SABER_TRY(auto queue, bot.player().queue(*message.guild_id));
		queue->skip();
		return outcome::success();
	}
};

COMMAND_ALLOC(Skip)
COMMAND_FREE(Skip)
