#include <saber/saber.hpp>

using namespace saber;

struct CSS : Command {
	explicit CSS(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("css")
					  .category(DIRNAME)
					  .enabled(true)
					  .usage("css")
					  .description("CSS is awesome!")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .slash_options({})
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		return do_css([&](std::string content) {
			return bot.http()
				.create_message(message.channel_id)
				.content(std::move(content))
				.send(yield);
		});
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		return do_css([&](std::string content) {
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
	static constexpr std::string_view k_css_gif =
		"https://media2.giphy.com/media/yYSSBtDgbbRzq/"
		"giphy.gif?cid="
		"ecf05e47ckbtzm84p629vw655dbua1qzaiw8tl46ejp4f0xj&ep="
		"v1_gifs_"
		"search&rid=giphy.gif&ct=g";

	template <typename SendReply>
	Result<> do_css(SendReply send_reply) {
		SABER_TRY(send_reply(std::string{k_css_gif}));
		return outcome::success();
	}
};

COMMAND_ALLOC(CSS)
COMMAND_FREE(CSS)
