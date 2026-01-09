#include <ekizu/http.hpp>
#include <nlohmann/json.hpp>
#include <saber/saber.hpp>

using namespace saber;

struct Meme : Command {
	explicit Meme(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("meme")
					  .category(DIRNAME)
					  .enabled(true)
					  .usage("meme")
					  .description("Displays a random meme from the `memes`, "
								   "`dankmemes`, or `me_irl` subreddits.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .slash_options({})
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		return fetch_meme(
			[&](ekizu::Embed embed) -> Result<> {
				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .embeds({std::move(embed)})
							  .send(yield));
				return outcome::success();
			},
			yield);
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		// Defer response since API call can take time
		SABER_TRY(
			bot.http()
				.interaction(interaction.application_id)
				.create_response(interaction.id, interaction.token,
								 ekizu::InteractionResponseBuilder()
									 .type(ekizu::InteractionResponseType::
											   DeferredChannelMessageWithSource)
									 .build())
				.send(yield));

		return fetch_meme(
			[&](ekizu::Embed embed) -> Result<> {
				SABER_TRY(bot.http()
							  .interaction(interaction.application_id)
							  .edit_original_response(interaction.token)
							  .embeds({std::move(embed)})
							  .send(yield));
				return outcome::success();
			},
			yield);
	}

   private:
	template <typename SendEmbed>
	Result<> fetch_meme(SendEmbed send_embed,
						const boost::asio::yield_context &yield) const {
		auto res = ekizu::net::HttpConnection::get(
			bot.http().get_executor(), "https://meme-api.com/gimme", yield);

		if (!res) { return boost::system::errc::operation_not_permitted; }

		const auto json =
			nlohmann::json::parse(res.value().body(), nullptr, false);

		if (json.is_discarded() || !json.is_object()) {
			return boost::system::errc::invalid_argument;
		}

		auto embed =
			ekizu::EmbedBuilder{}
				.set_title(json["title"])
				.set_description(fmt::format(
					"By: **{}** | {}", json["author"].get<std::string>(),
					json["postLink"].get<std::string>()))
				.set_image({
					json["url"].get<std::string>(),
				})
				.build();

		SABER_TRY(send_embed(std::move(embed)));
		return outcome::success();
	}
};

COMMAND_ALLOC(Meme)
COMMAND_FREE(Meme)
