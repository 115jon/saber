#include <algorithm>
#include <saber/util.hpp>
#include <string>

using namespace saber;

struct Limiter : Command {
	explicit Limiter(Saber &creator)
		: Command(
			  creator,
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
				  .slash_options(
					  {ekizu::ApplicationCommandOptionBuilder()
						   .type(ekizu::ApplicationCommandOptionType::Boolean)
						   .name("enabled")
						   .description("Enable or disable the limiter")
						   .required(false)
						   .build(),
					   ekizu::ApplicationCommandOptionBuilder()
						   .type(ekizu::ApplicationCommandOptionType::Number)
						   .name("threshold")
						   .description("Limiter threshold in dB")
						   .required(false)
						   .build(),
					   ekizu::ApplicationCommandOptionBuilder()
						   .type(ekizu::ApplicationCommandOptionType::Integer)
						   .name("release")
						   .description("Limiter release time in ms")
						   .required(false)
						   .min_value(static_cast<int64_t>(
							   AudioSettings::k_limiter_release_min))
						   .max_value(static_cast<int64_t>(
							   AudioSettings::k_limiter_release_max))
						   .build()})
				  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		auto send_reply = [&](std::string content) -> Result<> {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content(std::move(content))
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		};

		return do_limiter(
			*message.guild_id, message.author.id, args, send_reply, yield);
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		if (!interaction.guild_id) {
			return boost::system::errc::operation_not_permitted;
		}

		auto user_id = util::get_user_id(interaction);
		if (!user_id) { return boost::system::errc::operation_not_permitted; }

		auto send_reply = [&](std::string content) -> Result<> {
			SABER_TRY(bot.http()
						  .interaction(interaction.application_id)
						  .create_response(
							  interaction.id, interaction.token,
							  ekizu::InteractionResponseBuilder()
								  .type(ekizu::InteractionResponseType::
											ChannelMessageWithSource)
								  .content(std::move(content))
								  .build())
						  .send(yield));
			return outcome::success();
		};

		// Build args from interaction options for shared logic
		std::vector<std::string> args;

		if (auto en = util::get_option<bool>(interaction, "enabled")) {
			args.push_back(*en ? "on" : "off");
		}
		if (auto th = util::get_option<double>(interaction, "threshold")) {
			args = {"threshold", std::to_string(*th)};
		}
		if (auto rel = util::get_int_option<int>(interaction, "release")) {
			args = {"release", std::to_string(*rel)};
		}

		return do_limiter(
			*interaction.guild_id, *user_id, args, send_reply, yield);
	}

   private:
	template <typename SendReply>
	Result<> do_limiter(ekizu::Snowflake guild_id, ekizu::Snowflake user_id,
						const std::vector<std::string> &args,
						SendReply send_reply,
						const boost::asio::yield_context &yield) {
		SABER_TRY(auto guild, bot.get_guild(guild_id, yield));

		if (!bot.player().has_connection(guild_id)) {
			return send_reply("I am not in a voice channel. Use `join` first.");
		}

		// Check voice channel
		auto voice_state = bot.voice_states().get(guild_id).flat_map(
			[&](auto &users) { return users.get(user_id); });

		SABER_TRY(auto bot_channel_id, bot.player().voice_channel_id(guild_id));

		if (!voice_state.map([](auto &s) { return !!s.channel_id; })
				 .value_or(false) ||
			(*voice_state->channel_id != bot_channel_id)) {
			return send_reply(
				"You must be in my voice channel to control the limiter.");
		}

		auto show = [&]() -> Result<> {
			SABER_TRY(auto en, bot.player().limiter_enabled(guild_id));
			SABER_TRY(auto th, bot.player().limiter_threshold_db(guild_id));
			SABER_TRY(auto rel, bot.player().limiter_release_ms(guild_id));

			return send_reply(fmt::format(
				"🎛️ Limiter: {} | threshold: {} dB | release: {}ms",
				en ? "on" : "off", th, rel));
		};

		if (args.empty()) { return show(); }

		if (args[0] == "on" || args[0] == "off") {
			SABER_TRY(
				bot.player().set_limiter_enabled(guild_id, args[0] == "on"));
			return show();
		}

		if (args[0] == "threshold") {
			if (args.size() < 2) {
				return send_reply("Usage: `limiter threshold <db>`");
			}

			float db = 0.0F;
			try {
				db = std::stof(args[1]);
			} catch (...) { return send_reply("Invalid dB value."); }

			SABER_TRY(bot.player().set_limiter_threshold_db(guild_id, db));
			return show();
		}

		if (args[0] == "release") {
			if (args.size() < 2) {
				return send_reply("Usage: `limiter release <ms>`");
			}

			int ms = 0;
			try {
				ms = std::stoi(args[1]);
			} catch (...) { return send_reply("Invalid release value."); }

			ms = std::clamp(ms, AudioSettings::k_limiter_release_min,
							AudioSettings::k_limiter_release_max);

			SABER_TRY(bot.player().set_limiter_release_ms(guild_id, ms));
			return show();
		}

		return send_reply(
			"Unknown option. Usage: `limiter [on|off|threshold <db>|release "
			"<ms>]`.");
	}
};

COMMAND_ALLOC(Limiter)
COMMAND_FREE(Limiter)
