#include <boost/algorithm/string/join.hpp>
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

		const std::string display =
			!track.title.empty()
				? fmt::format("[{}]({})", track.title,
							  track.webpage_url.empty()
								  ? std::string{"about:blank"}
								  : track.webpage_url)
				: (!track.webpage_url.empty() ? track.webpage_url
											  : std::string{"(unknown)"});

		auto description =
			queue->current_track_id != track.id
				? fmt::format(
					  "**✅ Added to queue**\n{}\n`{}`", display, track.id)
				: fmt::format("**▶️ Started playing**\n{}", display);

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

		// Filter: Only allow the command author to use the buttons AND only for
		// this specific message (prevents multiple collectors in the same
		// channel from racing / double-responding).
		auto filter = [author_id = message.author.id, msg_id = msg.id](
						  const ekizu::Interaction &interaction,
						  const ekizu::MessageComponentData &data) {
			std::optional<ekizu::Snowflake> user_id;
			if (interaction.member) {
				user_id = interaction.member->user.id;
			} else if (interaction.user) {
				user_id = interaction.user->id;
			}

			if (user_id != author_id) { return false; }

			if (data.custom_id != "track_previous" &&
				data.custom_id != "track_toggle_pause" &&
				data.custom_id != "track_skip") {
				return false;
			}

			// Buttons always include the source message; ensure it's ours.
			if (!interaction.message) { return false; }
			return interaction.message->id == msg_id;
		};

		// Create collector (Active for 10 minutes)
		auto collector = bot.create_message_component_collector(
			message.channel_id, filter, ekizu::ComponentType::Button,
			std::chrono::minutes(10), yield);

		bool is_paused = false;

		auto ack_interaction = [&](const ekizu::Interaction &i) {
			// Never let ack failures abort the /play command coroutine (that’s
			// what causes "Failed to process command: ...").
			boost::system::error_code ec;
			(void)bot.http()
				.interaction(i.application_id)
				.create_response(i.id, i.token,
								 ekizu::InteractionResponseBuilder()
									 .type(ekizu::InteractionResponseType::
											   DeferredUpdateMessage)
									 .build())
				.send(yield[ec]);
		};

		auto send_followup = [&](std::string content) {
			// Same: do not fail /play if the follow-up message fails.
			boost::system::error_code ec;
			(void)bot.http()
				.create_message(message.channel_id)
				.content(std::move(content))
				.send(yield[ec]);
		};

		while (true) {
			auto res = collector->async_receive(yield);
			if (!res) { break; }  // Timeout or error

			auto &[i, data] = res.value();

			// Ack first so Discord doesn't show "Interaction Failed" even if
			// the player operation is slow.
			ack_interaction(i);

			if (data.custom_id == "track_skip") {
				auto r = bot.player().skip(*message.guild_id);
				if (!r) {
					send_followup(
						fmt::format("Skip failed: {}", r.error().message()));
				} else if (!r.value()) {
					send_followup("There is no next track.");
				}
			} else if (data.custom_id == "track_previous") {
				auto r = bot.player().previous(*message.guild_id);
				if (!r) {
					send_followup(fmt::format(
						"Previous failed: {}", r.error().message()));
				} else if (!r.value()) {
					send_followup("There is no previous track.");
				}
			} else if (data.custom_id == "track_toggle_pause") {
				if (is_paused) {
					// Keep SABER_TRY here: pause/resume failure should be
					// visible.
					SABER_TRY(bot.player().resume(*message.guild_id));
					is_paused = false;
				} else {
					SABER_TRY(bot.player().pause(*message.guild_id));
					is_paused = true;
				}
			}
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
