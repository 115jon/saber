#include <nlohmann/json.hpp>
#include <saber/saber.hpp>

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
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		return fetchXKCD(message, args, yield);
	}

   private:
	Result<> fetchXKCD(const ekizu::Message &message,
					   [[maybe_unused]] const std::vector<std::string> &args,
					   const boost::asio::yield_context &yield) {
		auto res = ekizu::net::HttpConnection::get(
			bot.http().get_executor(), api_url, yield);

		if (!res || res.value().result_int() != 200) {
			bot.log<ekizu::LogLevel::Error>("Error while fetching XKCD data");
			(void)bot.http()
				.create_message(message.channel_id)
				.content("There was an error. Please try again.")
				.send(yield);
			return boost::system::errc::operation_not_permitted;
		}

		const auto json =
			nlohmann::json::parse(res.value().body(), nullptr, false);

		if (json.is_discarded() || !json.is_object()) {
			bot.log<ekizu::LogLevel::Error>("Error parsing XKCD data");
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("There was an error. Please try again.")
						  .send(yield));
			return boost::system::errc::invalid_argument;
		}

		const auto comic_url =
			fmt::format("https://xkcd.com/{}", json["num"].get<uint32_t>());
		const auto msg = fmt::format(
			"**{}**\n{}\nAlt Text:```{}```XKCD Link: <{}>",
			json["safe_title"].get<std::string>(),
			json["img"].get<std::string>(), json["alt"].get<std::string>(),
			comic_url);

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .content(msg)
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(XKCD)
COMMAND_FREE(XKCD)
