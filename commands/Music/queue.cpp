#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <ekizu/embed_builder.hpp>
#include <saber/util.hpp>

using namespace saber;

struct Queue : Command {
	explicit Queue(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("queue")
					  .category(DIRNAME)
					  .enabled(true)
					  .aliases({"q", "list", "showqueue", "sq", "ls", "page",
								"que", "musiclist", "showall"})
					  .guild_only(true)
					  .usage("queue")
					  .description("Queues the player.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, message, yield));
		SABER_TRY(bot.player().connect(
			*message.guild_id, *voice_state->channel_id, yield));
		SABER_TRY(auto queue, bot.player().queue(*message.guild_id));

		if (!queue || queue->tracks.empty()) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("The queue is empty.")
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		auto page = 1;

		std::string description;

		for (const auto &track : queue->tracks) {
			description +=
				fmt::format("**`{}`** - `{}`\n", track.id, track.url);
		}

		auto embed = ekizu::EmbedBuilder()
						 .set_description(std::move(description))
						 .set_footer({fmt::format("Page: {}", page)})
						 .build();

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .embeds({std::move(embed)})
					  .reply(message.id)
					  .send(yield));
		return outcome::success();
	}
};

COMMAND_ALLOC(Queue)
COMMAND_FREE(Queue)
