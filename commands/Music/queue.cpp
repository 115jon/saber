#include <saber/util.hpp>

using namespace saber;

namespace {
constexpr size_t k_page_size = 10;
constexpr auto k_collector_lifetime = std::chrono::minutes(10);

ekizu::ActionRow build_queue_controls(bool disable_all, bool disable_prev,
									  bool disable_next) {
	return ekizu::ActionRowBuilder()
		.components(
			{ekizu::ButtonBuilder()
				 .custom_id("queue_prev")
				 .style(ekizu::ButtonStyle::Secondary)
				 .emoji(ekizu::EmojiBuilder().name("◀").build())
				 .disabled(disable_all || disable_prev)
				 .build(),
			 ekizu::ButtonBuilder()
				 .custom_id("queue_next")
				 .style(ekizu::ButtonStyle::Secondary)
				 .emoji(ekizu::EmojiBuilder().name("▶").build())
				 .disabled(disable_all || disable_next)
				 .build(),
			 ekizu::ButtonBuilder()
				 .custom_id("queue_close")
				 .style(ekizu::ButtonStyle::Danger)
				 .emoji(ekizu::EmojiBuilder().name("✖").build())
				 .disabled(disable_all)
				 .build()})
		.build();
}

std::string render_queue_page(const std::deque<Track> &tracks,
							  std::optional<uint64_t> current_id, size_t page,
							  size_t page_size) {
	const auto total = util::page_count(tracks.size(), page_size);
	page = std::max<size_t>(page, 1);
	page = std::min(page, total);

	const size_t start = (page - 1) * page_size;
	const size_t end = std::min(tracks.size(), start + page_size);

	fmt::memory_buffer out;
	for (size_t i = start; i < end; ++i) {
		const auto &t = tracks[i];
		const bool is_now = current_id && (t.id == *current_id);
		const auto title = util::truncate(t.metadata.title, 90);

		if (is_now) {
			fmt::format_to(
				std::back_inserter(out), "→ **`{}`** `{}`\n", t.id, title);
		} else {
			fmt::format_to(
				std::back_inserter(out), "  `{}` `{}`\n", t.id, title);
		}
	}

	auto s = fmt::to_string(out);
	if (s.empty()) { s = "—"; }
	return s;
}

ekizu::Embed render_queue_embed(const std::deque<Track> &tracks,
								std::optional<uint64_t> current_track_id,
								size_t page, const std::string &requester) {
	const auto pages = util::page_count(tracks.size(), k_page_size);
	page = std::max<size_t>(page, 1);
	page = std::min(page, pages);

	const auto footer = ekizu::EmbedFooter{
		fmt::format("Page {}/{} • Requested by {}", page, pages, requester)};

	return ekizu::EmbedBuilder()
		.set_title("Queue")
		.set_color(util::k_color_ok)
		.set_description(
			"Use the buttons below to browse the queue.\n\n"
			"**Legend:** `→` = currently playing")
		.add_field(ekizu::EmbedField{
			"Now playing",
			util::now_playing_line(tracks, current_track_id),
			false,
		})
		.add_field(ekizu::EmbedField{
			"Up next",
			util::up_next_line(tracks, current_track_id),
			false,
		})
		.add_field(ekizu::EmbedField{
			"Tracks",
			render_queue_page(tracks, current_track_id, page, k_page_size),
			false,
		})
		.set_footer(footer)
		.build();
}
}  // namespace

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
					  .description("Shows the current queue.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .slash_options({})  // No options needed
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
			auto embed =
				ekizu::EmbedBuilder()
					.set_title("Queue is empty")
					.set_color(util::k_color_warn)
					.set_description("Add a track with `play <query>`.")
					.set_footer(ekizu::EmbedFooter{fmt::format(
						"Requested by {}", message.author.username)})
					.build();

			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .embeds({std::move(embed)})
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		size_t page = 1;
		const auto pages = util::page_count(queue->tracks.size(), k_page_size);

		auto embed = render_queue_embed(queue->tracks, queue->current_track_id,
										page, message.author.username);

		auto controls = build_queue_controls(
			/*disable_all=*/false,
			/*disable_prev=*/(page <= 1),
			/*disable_next=*/(page >= pages));

		SABER_TRY(auto msg, bot.http()
								.create_message(message.channel_id)
								.embeds({std::move(embed)})
								.components({std::move(controls)})
								.reply(message.id)
								.send(yield));

		auto filter = [author_id = message.author.id](
						  const ekizu::Interaction &interaction,
						  const ekizu::MessageComponentData &data) {
			std::optional<ekizu::Snowflake> user_id;
			if (interaction.member) {
				user_id = interaction.member->user.id;
			} else if (interaction.user) {
				user_id = interaction.user->id;
			}

			if (!user_id || *user_id != author_id) { return false; }

			return data.custom_id == "queue_prev" ||
				   data.custom_id == "queue_next" ||
				   data.custom_id == "queue_close";
		};

		auto collector = bot.create_message_component_collector(
			message.channel_id, filter, {ekizu::ComponentType::Button},
			k_collector_lifetime, yield);

		while (true) {
			auto res = collector.async_receive(yield);
			if (!res) { break; }

			auto [i, data] = res.value();

			SABER_TRY(
				bot.http()
					.interaction(i.application_id)
					.create_response(i.id, i.token,
									 ekizu::InteractionResponseBuilder()
										 .type(ekizu::InteractionResponseType::
												   DeferredUpdateMessage)
										 .build())
					.send(yield));

			const auto max_pages =
				util::page_count(queue->tracks.size(), k_page_size);

			if (data.custom_id == "queue_close") {
				auto closed_controls = build_queue_controls(
					/*disable_all=*/true,
					/*disable_prev=*/true,
					/*disable_next=*/true);

				SABER_TRY(bot.http()
							  .edit_message(msg.channel_id, msg.id)
							  .components({std::move(closed_controls)})
							  .send(yield));
				return outcome::success();
			}

			if (data.custom_id == "queue_prev" && page > 1) { --page; }
			if (data.custom_id == "queue_next" && page < max_pages) { ++page; }

			auto updated_embed =
				render_queue_embed(queue->tracks, queue->current_track_id, page,
								   message.author.username);

			auto updated_controls = build_queue_controls(
				/*disable_all=*/false,
				/*disable_prev=*/(page <= 1),
				/*disable_next=*/(page >= max_pages));

			SABER_TRY(bot.http()
						  .edit_message(msg.channel_id, msg.id)
						  .embeds({std::move(updated_embed)})
						  .components({std::move(updated_controls)})
						  .send(yield));
		}

		auto disabled_controls = build_queue_controls(
			/*disable_all=*/true, /*disable_prev=*/true,
			/*disable_next=*/true);

		SABER_TRY(bot.http()
					  .edit_message(msg.channel_id, msg.id)
					  .components({std::move(disabled_controls)})
					  .send(yield));

		return outcome::success();
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		if (!interaction.guild_id) {
			return boost::system::errc::operation_not_permitted;
		}

		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, interaction, yield));
		SABER_TRY(bot.player().connect(
			*interaction.guild_id, *voice_state->channel_id, yield));
		SABER_TRY(auto queue, bot.player().queue(*interaction.guild_id));

		auto username = util::get_username(interaction);
		auto user_id_opt = util::get_user_id(interaction);
		if (!user_id_opt) {
			return boost::system::errc::operation_not_permitted;
		}
		auto author_id = *user_id_opt;

		if (!queue || queue->tracks.empty()) {
			auto embed =
				ekizu::EmbedBuilder()
					.set_title("Queue is empty")
					.set_color(util::k_color_warn)
					.set_description("Add a track with `/play <query>`.")
					.set_footer(ekizu::EmbedFooter{
						fmt::format("Requested by {}", username)})
					.build();

			SABER_TRY(bot.http()
						  .interaction(interaction.application_id)
						  .create_response(
							  interaction.id, interaction.token,
							  ekizu::InteractionResponseBuilder()
								  .type(ekizu::InteractionResponseType::
											ChannelMessageWithSource)
								  .embeds({std::move(embed)})
								  .build())
						  .send(yield));
			return outcome::success();
		}

		size_t page = 1;
		const auto pages = util::page_count(queue->tracks.size(), k_page_size);

		auto embed = render_queue_embed(
			queue->tracks, queue->current_track_id, page, username);

		auto controls = build_queue_controls(
			/*disable_all=*/false,
			/*disable_prev=*/(page <= 1),
			/*disable_next=*/(page >= pages));

		SABER_TRY(bot.http()
					  .interaction(interaction.application_id)
					  .create_response(
						  interaction.id, interaction.token,
						  ekizu::InteractionResponseBuilder()
							  .type(ekizu::InteractionResponseType::
										ChannelMessageWithSource)
							  .embeds({std::move(embed)})
							  .components({std::move(controls)})
							  .build())
					  .send(yield));

		// Get channel for collector
		auto channel_id = util::get_channel_id(interaction);
		if (!channel_id) { return outcome::success(); }

		auto filter = [author_id](const ekizu::Interaction &i,
								  const ekizu::MessageComponentData &data) {
			std::optional<ekizu::Snowflake> user_id;
			if (i.member) {
				user_id = i.member->user.id;
			} else if (i.user) {
				user_id = i.user->id;
			}

			if (!user_id || *user_id != author_id) { return false; }

			return data.custom_id == "queue_prev" ||
				   data.custom_id == "queue_next" ||
				   data.custom_id == "queue_close";
		};

		auto collector = bot.create_message_component_collector(
			*channel_id, filter, {ekizu::ComponentType::Button},
			k_collector_lifetime, yield);

		// Get the original response message to edit it
		SABER_TRY(auto msg,
				  bot.http()
					  .interaction(interaction.application_id)
					  .get_original_response(
						  interaction.application_id, interaction.token)
					  .send(yield));

		while (true) {
			auto res = collector.async_receive(yield);
			if (!res) { break; }

			auto [i, data] = res.value();

			SABER_TRY(
				bot.http()
					.interaction(i.application_id)
					.create_response(i.id, i.token,
									 ekizu::InteractionResponseBuilder()
										 .type(ekizu::InteractionResponseType::
												   DeferredUpdateMessage)
										 .build())
					.send(yield));

			const auto max_pages =
				util::page_count(queue->tracks.size(), k_page_size);

			if (data.custom_id == "queue_close") {
				auto closed_controls = build_queue_controls(
					/*disable_all=*/true,
					/*disable_prev=*/true,
					/*disable_next=*/true);

				SABER_TRY(bot.http()
							  .edit_message(msg.channel_id, msg.id)
							  .components({std::move(closed_controls)})
							  .send(yield));
				return outcome::success();
			}

			if (data.custom_id == "queue_prev" && page > 1) { --page; }
			if (data.custom_id == "queue_next" && page < max_pages) { ++page; }

			auto updated_embed = render_queue_embed(
				queue->tracks, queue->current_track_id, page, username);

			auto updated_controls = build_queue_controls(
				/*disable_all=*/false,
				/*disable_prev=*/(page <= 1),
				/*disable_next=*/(page >= max_pages));

			SABER_TRY(bot.http()
						  .edit_message(msg.channel_id, msg.id)
						  .embeds({std::move(updated_embed)})
						  .components({std::move(updated_controls)})
						  .send(yield));
		}

		auto disabled_controls = build_queue_controls(
			/*disable_all=*/true, /*disable_prev=*/true,
			/*disable_next=*/true);

		SABER_TRY(bot.http()
					  .edit_message(msg.channel_id, msg.id)
					  .components({std::move(disabled_controls)})
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(Queue)
COMMAND_FREE(Queue)
