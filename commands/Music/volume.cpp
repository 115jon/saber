#include <algorithm>
#include <cmath>
#include <saber/util.hpp>
#include <string>

using namespace saber;

struct Volume : Command {
	explicit Volume(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("volume")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(true)
					  .usage("volume [0-200]")
					  .description("Shows or sets the player volume (0-200%).")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		SABER_TRY(auto guild, bot.get_guild(*message.guild_id, yield));

		if (!bot.player().has_connection(*message.guild_id)) {
			SABER_TRY(
				bot.http()
					.create_message(message.channel_id)
					.content("I am not in a voice channel. Use `join` first.")
					.reply(message.id)
					.send(yield));
			return outcome::success();
		}

		SABER_TRY(
			auto voice_state, util::in_voice_channel(bot, message, yield));
		SABER_TRY(auto bot_channel_id,
				  bot.player().voice_channel_id(*message.guild_id));

		if (!voice_state->channel_id ||
			(*voice_state->channel_id != bot_channel_id)) {
			SABER_TRY(
				bot.http()
					.create_message(message.channel_id)
					.content(
						"You must be in my voice channel to control volume.")
					.reply(message.id)
					.send(yield));
			return outcome::success();
		}

		if (args.empty()) {
			SABER_TRY(auto v, bot.player().volume(*message.guild_id));
			int pct = static_cast<int>(std::lround(v * 100.0F));
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content(fmt::format("Current volume: {}%", pct))
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		int pct = 0;
		try {
			pct = std::stoi(args[0]);
		} catch (...) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("Invalid volume. Usage: `volume [0-200]`.")
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		pct = std::max(pct, 0);
		pct = std::min(pct, 200);

		const float scalar = static_cast<float>(pct) / 100.0F;
		SABER_TRY(bot.player().set_volume(*message.guild_id, scalar));

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .content(fmt::format("Volume set to {}%.", pct))
					  .reply(message.id)
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(Volume)
COMMAND_FREE(Volume)
