#include <algorithm>
#include <saber/util.hpp>
#include <string>

using namespace saber;

struct Fade : Command {
	explicit Fade(Saber &creator)
		: Command(
			  creator,
			  CommandOptionsBuilder()
				  .name("fade")
				  .category(DIRNAME)
				  .enabled(true)
				  .guild_only(true)
				  .usage(fmt::format("fade [{}-{}]", AudioSettings::k_fade_min,
									 AudioSettings::k_fade_max))
				  .description("Shows or sets the track fade duration (ms).")
				  .bot_permissions(ekizu::Permissions::SendMessages |
								   ekizu::Permissions::EmbedLinks)
				  .cooldown(std::chrono::seconds(3))
				  .slash_options(
					  {ekizu::ApplicationCommandOptionBuilder()
						   .type(ekizu::ApplicationCommandOptionType::Integer)
						   .name("milliseconds")
						   .description(fmt::format("Fade duration ({}-{}ms)",
													AudioSettings::k_fade_min,
													AudioSettings::k_fade_max))
						   .required(false)
						   .min_value(
							   static_cast<int64_t>(AudioSettings::k_fade_min))
						   .max_value(
							   static_cast<int64_t>(AudioSettings::k_fade_max))
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

		std::optional<int> ms;
		if (!args.empty()) {
			try {
				ms = std::stoi(args[0]);
			} catch (...) {
				return send_reply(fmt::format(
					"Invalid fade. Usage: `fade [{}-{}]`.",
					AudioSettings::k_fade_min, AudioSettings::k_fade_max));
			}
		}

		return do_fade(
			*message.guild_id, message.author.id, ms, send_reply, yield);
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		if (!interaction.guild_id) {
			return boost::system::errc::operation_not_permitted;
		}

		auto ms = util::get_int_option<int>(interaction, "milliseconds");
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

		return do_fade(*interaction.guild_id, *user_id, ms, send_reply, yield);
	}

   private:
	template <typename SendReply>
	Result<> do_fade(ekizu::Snowflake guild_id, ekizu::Snowflake user_id,
					 std::optional<int> ms, SendReply send_reply,
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
				"You must be in my voice channel to control fade.");
		}

		if (!ms) {
			SABER_TRY(auto current, bot.player().fade_ms(guild_id));
			return send_reply(fmt::format("🎚️ Current fade: {}ms", current));
		}

		int val = std::clamp(
			*ms, AudioSettings::k_fade_min, AudioSettings::k_fade_max);
		SABER_TRY(bot.player().set_fade_ms(guild_id, val));
		return send_reply(fmt::format("🎚️ Fade set to {}ms.", val));
	}
};

COMMAND_ALLOC(Fade)
COMMAND_FREE(Fade)
