#include <algorithm>
#include <saber/util.hpp>
#include <string>

using namespace saber;

struct Limiter : Command {
	explicit Limiter(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("limiter")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(true)
					  .usage("limiter [on|off|threshold <db>|release <ms>]")
					  .description("Configures the player limiter.")
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
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("You must be in my voice channel to control "
								   "the limiter.")
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		auto show = [&]() -> Result<> {
			SABER_TRY(auto en, bot.player().limiter_enabled(*message.guild_id));
			SABER_TRY(
				auto th, bot.player().limiter_threshold_db(*message.guild_id));
			SABER_TRY(
				auto rel, bot.player().limiter_release_ms(*message.guild_id));

			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content(fmt::format(
							  "Limiter: {} | threshold: {} dB | release: {}ms",
							  en ? "on" : "off", th, rel))
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		};

		if (args.empty()) { return show(); }

		if (args[0] == "on" || args[0] == "off") {
			SABER_TRY(bot.player().set_limiter_enabled(
				*message.guild_id, args[0] == "on"));
			return show();
		}

		if (args[0] == "threshold") {
			if (args.size() < 2) {
				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .content("Usage: `limiter threshold <db>`")
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			float db = 0.0F;
			try {
				db = std::stof(args[1]);
			} catch (...) {
				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .content("Invalid dB value.")
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			SABER_TRY(
				bot.player().set_limiter_threshold_db(*message.guild_id, db));
			return show();
		}

		if (args[0] == "release") {
			if (args.size() < 2) {
				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .content("Usage: `limiter release <ms>`")
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			int ms = 0;
			try {
				ms = std::stoi(args[1]);
			} catch (...) {
				SABER_TRY(bot.http()
							  .create_message(message.channel_id)
							  .content("Invalid release value.")
							  .reply(message.id)
							  .send(yield));
				return outcome::success();
			}

			ms = std::clamp(ms, AudioSettings::k_limiter_release_min,
							AudioSettings::k_limiter_release_max);

			SABER_TRY(
				bot.player().set_limiter_release_ms(*message.guild_id, ms));
			return show();
		}

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .content("Unknown option. Usage: `limiter "
							   "[on|off|threshold <db>|release <ms>]`.")
					  .reply(message.id)
					  .send(yield));
		return outcome::success();
	}
};

COMMAND_ALLOC(Limiter)
COMMAND_FREE(Limiter)
