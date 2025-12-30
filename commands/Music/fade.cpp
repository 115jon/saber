#include <algorithm>
#include <saber/util.hpp>
#include <string>

using namespace saber;

const std::string k_usage_str = fmt::format(
	"fade [{}-{}]", AudioSettings::k_fade_min, AudioSettings::k_fade_max);

struct Fade : Command {
	explicit Fade(Saber &creator)
		: Command(
			  creator,
			  CommandOptionsBuilder()
				  .name("fade")
				  .category(DIRNAME)
				  .enabled(true)
				  .guild_only(true)
				  .usage(k_usage_str)
				  .description(
					  "Shows or sets the track fade duration in milliseconds.")
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
					.content("You must be in my voice channel to control fade.")
					.reply(message.id)
					.send(yield));
			return outcome::success();
		}

		if (args.empty()) {
			SABER_TRY(auto ms, bot.player().fade_ms(*message.guild_id));
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content(fmt::format("Current fade: {}ms", ms))
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		int ms = 0;
		try {
			ms = std::stoi(args[0]);
		} catch (...) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content(fmt::format(
							  "Invalid fade. Usage: `{}`.", k_usage_str))
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		ms = std::clamp(
			ms, AudioSettings::k_fade_min, AudioSettings::k_fade_max);

		SABER_TRY(bot.player().set_fade_ms(*message.guild_id, ms));

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .content(fmt::format("Fade set to {}ms.", ms))
					  .reply(message.id)
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(Fade)
COMMAND_FREE(Fade)
