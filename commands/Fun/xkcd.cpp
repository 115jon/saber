#include <nlohmann/json.hpp>
#include <saber/saber.hpp>
#include <saber/util.hpp>

using namespace saber;

static constexpr std::string_view api_url = "https://xkcd.com/info.0.json";

struct XKCD : Command {
	explicit XKCD(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("xkcd")
					  .category(DIRNAME)
					  .enabled(true)
					  .usage("xkcd")
					  .description("Shows the latest XKCD comic.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .slash_options({})
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		return fetch_xkcd(
			[&](std::string content) -> Result<> {
				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .content(std::move(content))
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

		return fetch_xkcd(
			[&](std::string content) -> Result<> {
				SABER_TRY(bot.http()
							  .interaction(interaction.application_id)
							  .edit_original_response(interaction.token)
							  .content(std::move(content))
							  .send(yield));
				return outcome::success();
			},
			yield);
	}

   private:
	template <typename SendReply>
	Result<> fetch_xkcd(SendReply send_reply,
						const boost::asio::yield_context &yield) {
		auto res = ekizu::net::HttpConnection::get(
			bot.http().get_executor(), api_url, yield);

		if (!res || res.value().result_int() != 200) {
			bot.log<ekizu::LogLevel::Error>("Error while fetching XKCD data");
			SABER_TRY(send_reply("There was an error. Please try again."));
			return outcome::success();
		}

		const auto json =
			nlohmann::json::parse(res.value().body(), nullptr, false);

		if (json.is_discarded() || !json.is_object()) {
			bot.log<ekizu::LogLevel::Error>("Error parsing XKCD data");
			SABER_TRY(send_reply("There was an error. Please try again."));
			return outcome::success();
		}

		const auto comic_url =
			fmt::format("https://xkcd.com/{}", json["num"].get<uint32_t>());
		const auto msg = fmt::format(
			"**{}**\n{}\nAlt Text:```{}```XKCD Link: <{}>",
			json["safe_title"].get<std::string>(),
			json["img"].get<std::string>(), json["alt"].get<std::string>(),
			comic_url);

		SABER_TRY(send_reply(msg));
		return outcome::success();
	}
};

COMMAND_ALLOC(XKCD)
COMMAND_FREE(XKCD)
