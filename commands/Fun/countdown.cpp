#include <saber/saber.hpp>
#include <saber/util.hpp>

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
					  .slash_options({})
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		return do_countdown(message.channel_id, std::nullopt, yield);
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		auto channel_id = util::get_channel_id(interaction);
		if (!channel_id) {
			return boost::system::errc::operation_not_permitted;
		}

		// For interactions, we need to respond first, then continue
		InteractionInfo info{
			interaction.application_id, interaction.id, interaction.token};
		return do_countdown(*channel_id, info, yield);
	}

   private:
	struct InteractionInfo {
		ekizu::Snowflake application_id;
		ekizu::Snowflake interaction_id;
		std::string token;
	};

	Result<> do_countdown(ekizu::Snowflake channel_id,
						  std::optional<InteractionInfo> interaction_info,
						  const boost::asio::yield_context &yield) {
		static const std::vector<std::string> k_countdown{
			"five", "four", "three", "two", "one"};

		boost::asio::steady_timer timer{yield.get_executor()};
		bool first = true;

		for (const auto &num : k_countdown) {
			auto content = fmt::format("**:{}:**", num);

			if (first && interaction_info) {
				// First message for interaction - use create_response
				SABER_TRY(bot.http()
							  .interaction(interaction_info->application_id)
							  .create_response(
								  interaction_info->interaction_id,
								  interaction_info->token,
								  ekizu::InteractionResponseBuilder()
									  .type(ekizu::InteractionResponseType::
												ChannelMessageWithSource)
									  .content(std::move(content))
									  .build())
							  .send(yield));
			} else {
				SABER_TRY(bot.http()
							  .create_message(channel_id)
							  .content(std::move(content))
							  .send(yield));
			}

			first = false;

			boost::system::error_code ec;
			timer.expires_after(std::chrono::seconds(1));
			timer.async_wait(yield[ec]);
			if (ec) { return ec; }
		}

		SABER_TRY(bot.http()
					  .create_message(channel_id)
					  .content("**:ok:** DING DING DING")
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(Countdown)
COMMAND_FREE(Countdown)
