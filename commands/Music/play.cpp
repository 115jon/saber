#include <boost/algorithm/string/join.hpp>
#include <ekizu/embed_builder.hpp>
#include <saber/util.hpp>

using namespace saber;

struct Play : Command {
	explicit Play(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("play")
					  .category(DIRNAME)
					  .enabled(true)
					  .aliases({"p"})
					  .guild_only(true)
					  .usage("play <query>")
					  .description("Play a youtube video in the voice channel.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, message, yield));

		if (args.empty()) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("Please specify a query.")
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		auto query = boost::algorithm::join(args, " ");

		SABER_TRY(bot.player().connect(
			*message.guild_id, *voice_state->channel_id, yield));
		SABER_TRY(auto track, bot.player().play(*message.guild_id, query,
												message.author.id, yield));
		SABER_TRY(auto queue, bot.player().queue(*message.guild_id));

		auto description =
			queue->current_track_id != track.id
				? fmt::format(
					  "**✅ Added to queue\n** `{}` - `{}`", query, track.id)
				: fmt::format("**▶️ Started playing\n** `{}`", query);

		auto embed = ekizu::EmbedBuilder().set_description(description).build();

		// Define buttons
		auto controls =
			ekizu::ActionRowBuilder()
				.components(
					{ekizu::ButtonBuilder()
						 .custom_id("track_previous")
						 .style(ekizu::ButtonStyle::Secondary)
						 .emoji(ekizu::EmojiBuilder().name("⏮️").build())
						 .build(),
					 ekizu::ButtonBuilder()
						 .custom_id("track_toggle_pause")
						 .style(ekizu::ButtonStyle::Primary)
						 .emoji(ekizu::EmojiBuilder().name("⏯️").build())
						 .build(),
					 ekizu::ButtonBuilder()
						 .custom_id("track_skip")
						 .style(ekizu::ButtonStyle::Secondary)
						 .emoji(ekizu::EmojiBuilder().name("⏭️").build())
						 .build()})
				.build();

		// Send the message and capture the result to get the message ID
		SABER_TRY(auto msg, bot.http()
								.create_message(message.channel_id)
								.embeds({std::move(embed)})
								.components({controls})
								.send(yield));

		// --- Collector Implementation ---

		// Filter: Only allow the command author to use the buttons
		auto filter = [author_id = message.author.id](
						  const ekizu::Interaction &interaction,
						  const ekizu::MessageComponentData &data) {
			std::optional<ekizu::Snowflake> user_id;
			if (interaction.member) {
				user_id = interaction.member->user.id;
			} else if (interaction.user) {
				user_id = interaction.user->id;
			}

			return user_id == author_id &&
				   (data.custom_id == "track_previous" ||
					data.custom_id == "track_toggle_pause" ||
					data.custom_id == "track_skip");
		};

		// Create collector (Active for 10 minutes)
		auto collector = bot.create_message_component_collector(
			message.channel_id, filter, ekizu::ComponentType::Button,
			std::chrono::minutes(10), yield);

		bool is_paused = false;

		while (true) {
			auto res = collector->async_receive(yield);
			if (!res) { break; }  // Timeout or error

			auto &[i, data] = res.value();

			// Handle Actions
			if (data.custom_id == "track_skip") {
				if (auto q = bot.player().queue(*message.guild_id)) {
					q.value()->skip();
				}
			} else if (data.custom_id == "track_previous") {
				if (auto q = bot.player().queue(*message.guild_id)) {
					q.value()->previous();
				}
			} else if (data.custom_id == "track_toggle_pause") {
				if (is_paused) {
					SABER_TRY(bot.player().resume(*message.guild_id));
					is_paused = false;
				} else {
					SABER_TRY(bot.player().pause(*message.guild_id));
					is_paused = true;
				}
			}

			// Acknowledge interaction to prevent "Interaction Failed" error
			// We use DeferredUpdateMessage so the UI doesn't flicker
			SABER_TRY(
				bot.http()
					.interaction(i.application_id)
					.create_response(i.id, i.token,
									 ekizu::InteractionResponseBuilder()
										 .type(ekizu::InteractionResponseType::
												   DeferredUpdateMessage)
										 .build())
					.send(yield));
		}

		// Cleanup: Disable buttons when collector times out
		auto &action_row = std::get<ekizu::ActionRow>(msg.components[0]);
		for (auto &component : action_row.components) {
			std::visit([](auto &c) { c.disabled = true; }, component);
		}

		SABER_TRY(bot.http()
					  .edit_message(msg.channel_id, msg.id)
					  .components(std::move(msg.components))
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(Play)
COMMAND_FREE(Play)
