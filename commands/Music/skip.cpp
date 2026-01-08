#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <saber/util.hpp>

using namespace saber;

struct Skip : Command {
	explicit Skip(Saber &creator)
		: Command(
			  creator,
			  CommandOptionsBuilder()
				  .name("skip")
				  .category(DIRNAME)
				  .enabled(true)
				  .guild_only(true)
				  .usage("skip [track_id]")
				  .description(
					  "Skips the current song (or jumps to a track by id).")
				  .bot_permissions(ekizu::Permissions::SendMessages |
								   ekizu::Permissions::EmbedLinks)
				  .cooldown(std::chrono::seconds(3))
				  .slash_options({ekizu::ApplicationCommandOption{
					  ekizu::ApplicationCommandOptionType::Integer,
					  "track_id",
					  {},
					  "Jump to a specific track by its queue ID",
					  {},
					  false,  // not required
				  }})
				  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, message, yield));
		SABER_TRY(bot.player().connect(
			*message.guild_id, *voice_state->channel_id, yield));

		std::optional<uint64_t> track_id;
		if (args.size() == 1) {
			uint64_t parsed = 0;
			if (boost::conversion::try_lexical_convert(args[0], parsed)) {
				track_id = parsed;
			}
		}

		auto send_embed = [&](ekizu::Embed embed) -> Result<> {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .embeds({std::move(embed)})
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		};

		return do_skip(*message.guild_id, message.author.username, track_id,
					   send_embed, yield);
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

		// Extract track_id from interaction options
		std::optional<uint64_t> track_id;
		if (interaction.data) {
			const auto *cmd_data =
				std::get_if<ekizu::ApplicationCommandData>(&*interaction.data);
			if (cmd_data) {
				for (const auto &opt : cmd_data->options) {
					if (opt.name == "track_id" && opt.value) {
						if (const auto *val =
								std::get_if<int64_t>(&*opt.value)) {
							track_id = static_cast<uint64_t>(*val);
						}
					}
				}
			}
		}

		// Get user name for footer
		std::string username = "Unknown";
		if (interaction.member) {
			username = interaction.member->user.username;
		} else if (interaction.user) {
			username = interaction.user->username;
		}

		// For slash commands, we'll just create a regular message response
		// since edit_original_response may not be available
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

		return do_skip(
			*interaction.guild_id, username, track_id, send_embed, yield);
	}

   private:
	template <typename SendEmbed>
	Result<> do_skip(ekizu::Snowflake guild_id, const std::string &username,
					 std::optional<uint64_t> track_id, SendEmbed send_embed,
					 [[maybe_unused]] const boost::asio::yield_context &yield) {
		SABER_TRY(auto queue, bot.player().queue(guild_id));

		const auto footer = fmt::format("Requested by {}", username);

		if (!queue || queue->tracks.empty() || !queue->current_track_id) {
			auto embed = util::music_action_embed(
				"Nothing to skip", util::k_color_warn,
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

		// No track_id: skip to next
		if (!track_id) {
			SABER_TRY(auto ok, bot.player().skip(guild_id));
			if (!ok) {
				auto embed = util::music_action_embed(
					"No next track", util::k_color_warn,
					"You're already at the end of the queue.",
					util::now_playing_line(
						queue->tracks, queue->current_track_id),
					util::up_next_line(queue->tracks, queue->current_track_id),
					footer);

				return send_embed(std::move(embed));
			}

			auto embed = util::music_action_embed(
				"Skipped", util::k_color_ok, "Moved to the next track.",
				util::now_playing_line(queue->tracks, queue->current_track_id),
				util::up_next_line(queue->tracks, queue->current_track_id),
				footer);

			return send_embed(std::move(embed));
		}

		// track_id provided: skip to specific track
		uint64_t tid = *track_id;

		// No-op: already current
		if (queue->current_track_id && tid == *queue->current_track_id) {
			auto embed = util::music_action_embed(
				"Already playing", util::k_color_warn,
				fmt::format("Track `{}` is already the current track.", tid),
				util::now_playing_line(queue->tracks, queue->current_track_id),
				util::up_next_line(queue->tracks, queue->current_track_id),
				footer);

			return send_embed(std::move(embed));
		}

		// Validate existence in current queue
		const auto it =
			std::find_if(queue->tracks.begin(), queue->tracks.end(),
						 [tid](const Track &t) { return t.id == tid; });

		if (it == queue->tracks.end()) {
			auto embed = util::music_action_embed(
				"Track not in queue", util::k_color_warn,
				fmt::format(
					"Track id `{}` was not found in the queue ({} tracks).",
					tid, queue->tracks.size()),
				util::now_playing_line(queue->tracks, queue->current_track_id),
				util::up_next_line(queue->tracks, queue->current_track_id),
				footer);

			return send_embed(std::move(embed));
		}

		SABER_TRY(auto ok, bot.player().skip_to(guild_id, tid));
		if (!ok) {
			auto embed = util::music_action_embed(
				"Could not skip", util::k_color_warn,
				"Skipping to that track was not possible.",
				util::now_playing_line(queue->tracks, queue->current_track_id),
				util::up_next_line(queue->tracks, queue->current_track_id),
				footer);

			return send_embed(std::move(embed));
		}

		auto embed = util::music_action_embed(
			"Skipped", util::k_color_ok,
			fmt::format("Moved to track `{}`.", tid),
			util::now_playing_line(queue->tracks, queue->current_track_id),
			util::up_next_line(queue->tracks, queue->current_track_id), footer);

		return send_embed(std::move(embed));
	}
};

COMMAND_ALLOC(Skip)
COMMAND_FREE(Skip)
