#include <ekizu/embed_builder.hpp>
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
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		return fetch_meme(message, yield);
	}

	Result<> fetch_meme(const ekizu::Message &message,
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

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .embeds({std::move(embed)})
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(Meme)
COMMAND_FREE(Meme)
