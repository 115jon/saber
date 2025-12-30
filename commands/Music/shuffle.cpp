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

		auto send_v2 = [&](uint32_t accent_color, std::string title,
						   std::string desc) -> Result<> {
			const auto now_playing = util::now_playing_line(
				queue ? queue->tracks : std::deque<Track>{},
				queue ? queue->current_track_id : std::optional<uint64_t>{});

			const auto up_next = util::up_next_line(
				queue ? queue->tracks : std::deque<Track>{},
				queue ? queue->current_track_id : std::optional<uint64_t>{});

			ekizu::Container container =
				ekizu::ContainerBuilder()
					.accent_color(accent_color)
					.add(ekizu::TextDisplayBuilder()
							 .content(fmt::format("### {}", title))
							 .build())
					.add(ekizu::TextDisplayBuilder()
							 .content(std::move(desc))
							 .build())
					.add(ekizu::SeparatorBuilder()
							 .divider(true)
							 .spacing(1)
							 .build())
					.add(ekizu::TextDisplayBuilder()
							 .content(now_playing)
							 .build())
					.add(ekizu::TextDisplayBuilder().content(up_next).build())
					.add(ekizu::SeparatorBuilder()
							 .divider(true)
							 .spacing(1)
							 .build())
					.add(ekizu::TextDisplayBuilder().content(footer).build())
					.build();

			std::vector<ekizu::MessageComponent> components;
			components.emplace_back(std::move(container));

			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .flags(ekizu::MessageFlags::IsComponentsV2)
						  .components(std::move(components))
						  .reply(message.id)
						  .send(yield));

			return outcome::success();
		};

		// Allow shuffle even if nothing is currently playing; only fail when
		// there is nothing meaningful to shuffle.
		if ((queue == nullptr) || queue->tracks.empty()) {
			return send_v2(util::k_color_warn, "Nothing to shuffle",
						   "There are no tracks in this server’s queue.");
		}

		SABER_TRY(auto ok, bot.player().shuffle(*message.guild_id));
		if (!ok) {
			return send_v2(
				util::k_color_warn, "Nothing to shuffle",
				"Not enough tracks to shuffle (try adding more tracks first).");
		}

		return send_v2(util::k_color_ok, "Queue shuffled",
					   "Randomized the up-next tracks in the queue.");
	}
};

COMMAND_ALLOC(Shuffle)
COMMAND_FREE(Shuffle)
