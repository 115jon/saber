#include <saber/util.hpp>

using namespace saber;

struct Previous : Command {
	explicit Previous(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("previous")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(true)
					  .usage("previous")
					  .description("Go to the previous song.")
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

		return do_previous(
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

		return do_previous(*interaction.guild_id, username, send_embed, yield);
	}

   private:
	template <typename SendEmbed>
	Result<> do_previous(
		ekizu::Snowflake guild_id, const std::string &username,
		SendEmbed send_embed,
		[[maybe_unused]] const boost::asio::yield_context &yield) {
		SABER_TRY(auto queue, bot.player().queue(guild_id));

		const auto footer = fmt::format("Requested by {}", username);

		if (!queue || queue->tracks.empty() || !queue->current_track_id) {
			auto embed = util::music_action_embed(
				"Nothing to go back to", util::k_color_warn,
				"No track is currently playing in this server.",
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

		const auto before_id = queue->current_track_id;

		SABER_TRY(auto ok, bot.player().previous(guild_id));
		if (!ok) {
			auto embed = util::music_action_embed(
				"No previous track", util::k_color_warn,
				"There isn't a previous track in the queue.",
				util::now_playing_line(queue->tracks, queue->current_track_id),
				util::up_next_line(queue->tracks, queue->current_track_id),
				footer);
			return send_embed(std::move(embed));
		}

		const auto after_id = queue->current_track_id;
		const bool restarted = (before_id == after_id);

		auto embed = util::music_action_embed(
			restarted ? "Restarted track" : "Previous track", util::k_color_ok,
			restarted ? "Rewound to the beginning of the current track."
					  : "Moved to the previous track in the queue.",
			util::now_playing_line(queue->tracks, queue->current_track_id),
			util::up_next_line(queue->tracks, queue->current_track_id), footer);
		return send_embed(std::move(embed));
	}
};

COMMAND_ALLOC(Previous)
COMMAND_FREE(Previous)
