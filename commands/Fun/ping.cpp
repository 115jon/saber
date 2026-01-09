#include <saber/saber.hpp>

using namespace saber;

struct Ping : Command {
	explicit Ping(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("ping")
					  .category(DIRNAME)
					  .enabled(true)
					  .usage("ping")
					  .description("Replies with pong.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .slash_options({})
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		return do_ping([&](std::string content) {
			return bot.http()
				.create_message(message.channel_id)
				.content(std::move(content))
				.send(yield);
		});
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		return do_ping([&](std::string content) {
			return bot.http()
				.interaction(interaction.application_id)
				.create_response(
					interaction.id, interaction.token,
					ekizu::InteractionResponseBuilder()
						.type(ekizu::InteractionResponseType::
								  ChannelMessageWithSource)
						.content(std::move(content))
						.build())
				.send(yield);
		});
	}

   private:
	template <typename SendReply>
	Result<> do_ping(SendReply send_reply) {
		SABER_TRY(send_reply("Pong!"));
		return outcome::success();
	}
};

COMMAND_ALLOC(Ping)
COMMAND_FREE(Ping)
