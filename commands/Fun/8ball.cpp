#include <saber/saber.hpp>
#include <saber/util.hpp>

using namespace saber;

struct Eightball : Command {
	explicit Eightball(Saber &creator)
		: Command(
			  creator,
			  CommandOptionsBuilder()
				  .name("8ball")
				  .category(DIRNAME)
				  .enabled(true)
				  .aliases({"eight-ball", "eightball"})
				  .usage("8ball <question>")
				  .description("Asks the Magic 8-Ball for some psychic wisdom.")
				  .bot_permissions(ekizu::Permissions::SendMessages |
								   ekizu::Permissions::EmbedLinks)
				  .cooldown(std::chrono::seconds(3))
				  .slash_options(
					  {ekizu::ApplicationCommandOptionBuilder()
						   .type(ekizu::ApplicationCommandOptionType::String)
						   .name("question")
						   .description("Your question for the 8-ball")
						   .required(true)
						   .build()})
				  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		if (args.empty()) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("You need to ask a question!")
						  .send(yield));
			return outcome::success();
		}

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .content(get_response())
					  .send(yield));

		return outcome::success();
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		auto question = util::get_option<std::string>(interaction, "question");
		if (!question) {
			SABER_TRY(bot.http()
						  .interaction(interaction.application_id)
						  .create_response(
							  interaction.id, interaction.token,
							  ekizu::InteractionResponseBuilder()
								  .type(ekizu::InteractionResponseType::
											ChannelMessageWithSource)
								  .content("You need to ask a question!")
								  .build())
						  .send(yield));
			return outcome::success();
		}

		SABER_TRY(bot.http()
					  .interaction(interaction.application_id)
					  .create_response(
						  interaction.id, interaction.token,
						  ekizu::InteractionResponseBuilder()
							  .type(ekizu::InteractionResponseType::
										ChannelMessageWithSource)
							  .content(fmt::format(
								  "🎱 **{}**\n{}", *question, get_response()))
							  .build())
					  .send(yield));

		return outcome::success();
	}

   private:
	std::string get_response() {
		return responses[util::get_random_number<size_t>(
			0, responses.size() - 1)];
	}

	std::vector<std::string> responses{
		"It is certain.",
		"It is decidedly so.",
		"Without a doubt.",
		"Yes - definitely.",
		"You may rely on it.",
		"As I see it, yes.",
		"Most likely.",
		"Outlook good.",
		"Yes.",
		"Signs point to yes.",
		"Reply hazy, try again.",
		"Ask again later.",
		"Better not tell you now.",
		"Cannot predict now.",
		"Concentrate and ask again.",
		"Don't count on it.",
		"My reply is no.",
		"My sources say no.",
		"Outlook not so good.",
		"Very doubtful.",
	};
};

COMMAND_ALLOC(Eightball)
COMMAND_FREE(Eightball)
