#include <saber/saber.hpp>

using namespace saber;

struct Countdown : Command {
	explicit Countdown(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("countdown")
					  .category(DIRNAME)
					  .enabled(true)
					  .usage("countdown")
					  .description("Countdown from 5.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		if (!args.empty()) { return outcome::success(); }

		const std::vector<std::string> countdown{
			"five", "four", "three", "two", "one"};

		boost::asio::steady_timer timer{yield.get_executor()};

		for (const auto &num : countdown) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content(fmt::format("**:{}:**", num))
						  .send(yield));

			boost::system::error_code ec;
			timer.expires_after(std::chrono::seconds(1));
			timer.async_wait(yield[ec]);
			if (ec) { return ec; }
		}

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .content("**:ok:** DING DING DING")
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(Countdown)
COMMAND_FREE(Countdown)
