#include <saber/saber.hpp>

using namespace saber;

struct Praise : Command {
	explicit Praise(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("praise")
					  .category(DIRNAME)
					  .enabled(true)
					  .usage("praise")
					  .description("Praise the sun!")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .slash_options({})
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		return do_praise([&](std::string content) {
			return bot.http()
				.create_message(message.channel_id)
				.content(std::move(content))
				.send(yield);
		});
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		return do_praise([&](std::string content) {
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
	static constexpr std::string_view k_praise_gif =
		"https://i.imgur.com/K8ySn3e.gif";

	template <typename SendReply>
	Result<> do_praise(SendReply send_reply) {
		SABER_TRY(send_reply(std::string{k_praise_gif}));
		return outcome::success();
	}
};

COMMAND_ALLOC(Praise)
COMMAND_FREE(Praise)
