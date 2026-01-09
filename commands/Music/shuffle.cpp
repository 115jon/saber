#include <ekizu/message_component.hpp>
#include <saber/util.hpp>

using namespace saber;

struct Shuffle : Command {
	explicit Shuffle(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("shuffle")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(true)
					  .usage("shuffle")
					  .description("Shuffles the queue.")
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

		auto send_embed = [&](ekizu::Embed embed) -> Result<> {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .embeds({std::move(embed)})
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		};

		return do_shuffle(
			*message.guild_id, message.author.username, send_embed, yield);
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

		auto username = util::get_username(interaction);

		auto send_embed = [&](ekizu::Embed embed) -> Result<> {
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
		};

		return do_shuffle(*interaction.guild_id, username, send_embed, yield);
	}

   private:
	template <typename SendEmbed>
	Result<> do_shuffle(
		ekizu::Snowflake guild_id, const std::string &username,
		SendEmbed send_embed,
		[[maybe_unused]] const boost::asio::yield_context &yield) {
		SABER_TRY(auto queue, bot.player().queue(guild_id));

		const auto footer = fmt::format("Requested by {}", username);

		if (!queue || queue->tracks.empty()) {
			auto embed = util::music_action_embed(
				"Nothing to shuffle", util::k_color_warn,
				"There are no tracks in this server's queue.",
				util::now_playing_line(
					queue ? queue->tracks : std::deque<Track>{},
					queue ? queue->current_track_id
						  : std::optional<uint64_t>{}),
				util::up_next_line(queue ? queue->tracks : std::deque<Track>{},
								   queue ? queue->current_track_id
										 : std::optional<uint64_t>{}),
				footer);
			return send_embed(std::move(embed));
		}

		SABER_TRY(auto ok, bot.player().shuffle(guild_id));
		if (!ok) {
			auto embed = util::music_action_embed(
				"Nothing to shuffle", util::k_color_warn,
				"Not enough tracks to shuffle (try adding more tracks first).",
				util::now_playing_line(queue->tracks, queue->current_track_id),
				util::up_next_line(queue->tracks, queue->current_track_id),
				footer);
			return send_embed(std::move(embed));
		}

		auto embed = util::music_action_embed(
			"Queue shuffled", util::k_color_ok,
			"Randomized the up-next tracks in the queue.",
			util::now_playing_line(queue->tracks, queue->current_track_id),
			util::up_next_line(queue->tracks, queue->current_track_id), footer);
		return send_embed(std::move(embed));
	}
};

COMMAND_ALLOC(Shuffle)
COMMAND_FREE(Shuffle)
