#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <saber/util.hpp>

using namespace saber;

struct Skip : Command {
	explicit Skip(Saber &creator)
		: Command(creator,
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
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, message, yield));
		SABER_TRY(bot.player().connect(
			*message.guild_id, *voice_state->channel_id, yield));
		SABER_TRY(auto queue, bot.player().queue(*message.guild_id));

		const auto footer =
			fmt::format("Requested by {}", message.author.username);

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

			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .embeds({std::move(embed)})
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		// No arg: existing "skip to next".
		if (args.empty()) {
			SABER_TRY(auto ok, bot.player().skip(*message.guild_id));
			if (!ok) {
				auto embed = util::music_action_embed(
					"No next track", util::k_color_warn,
					"You’re already at the end of the queue.",
					util::now_playing_line(
						queue->tracks, queue->current_track_id),
					util::up_next_line(queue->tracks, queue->current_track_id),
					footer);

				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .embeds({std::move(embed)})
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			auto embed = util::music_action_embed(
				"Skipped", util::k_color_ok, "Moved to the next track.",
				util::now_playing_line(queue->tracks, queue->current_track_id),
				util::up_next_line(queue->tracks, queue->current_track_id),
				footer);

			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .embeds({std::move(embed)})
						  .reply(message.id)
						  .send(yield));

			return outcome::success();
		}

		// One arg: treat as track id.
		if (args.size() == 1) {
			uint64_t track_id = 0;
			if (!boost::conversion::try_lexical_convert(args[0], track_id)) {
				auto embed = util::music_action_embed(
					"Invalid track id", util::k_color_warn,
					fmt::format("`{}` is not a valid track id.", args[0]),
					util::now_playing_line(
						queue->tracks, queue->current_track_id),
					util::up_next_line(queue->tracks, queue->current_track_id),
					footer);

				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .embeds({std::move(embed)})
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			// No-op: already current.
			if (queue->current_track_id &&
				track_id == *queue->current_track_id) {
				auto embed = util::music_action_embed(
					"Already playing", util::k_color_warn,
					fmt::format(
						"Track `{}` is already the current track.", track_id),
					util::now_playing_line(
						queue->tracks, queue->current_track_id),
					util::up_next_line(queue->tracks, queue->current_track_id),
					footer);

				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .embeds({std::move(embed)})
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			// Validate existence in current queue snapshot.
			const auto it = std::find_if(
				queue->tracks.begin(), queue->tracks.end(),
				[track_id](const Track &t) { return t.id == track_id; });

			if (it == queue->tracks.end()) {
				auto embed = util::music_action_embed(
					"Track not in queue", util::k_color_warn,
					fmt::format(
						"Track id `{}` was not found in the queue ({} tracks).",
						track_id, queue->tracks.size()),
					util::now_playing_line(
						queue->tracks, queue->current_track_id),
					util::up_next_line(queue->tracks, queue->current_track_id),
					footer);

				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .embeds({std::move(embed)})
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			SABER_TRY(
				auto ok, bot.player().skip_to(*message.guild_id, track_id));
			if (!ok) {
				auto embed = util::music_action_embed(
					"Could not skip", util::k_color_warn,
					"Skipping to that track was not possible.",
					util::now_playing_line(
						queue->tracks, queue->current_track_id),
					util::up_next_line(queue->tracks, queue->current_track_id),
					footer);

				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .embeds({std::move(embed)})
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			auto embed = util::music_action_embed(
				"Skipped", util::k_color_ok,
				fmt::format("Moved to track `{}`.", track_id),
				util::now_playing_line(queue->tracks, queue->current_track_id),
				util::up_next_line(queue->tracks, queue->current_track_id),
				footer);

			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .embeds({std::move(embed)})
						  .reply(message.id)
						  .send(yield));

			return outcome::success();
		}

		// Too many args.
		{
			auto embed = util::music_action_embed(
				"Invalid usage", util::k_color_warn,
				"Usage: `skip` or `skip <track_id>`.",
				util::now_playing_line(queue->tracks, queue->current_track_id),
				util::up_next_line(queue->tracks, queue->current_track_id),
				footer);

			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .embeds({std::move(embed)})
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}
	}
};

COMMAND_ALLOC(Skip)
COMMAND_FREE(Skip)
