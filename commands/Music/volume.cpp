#include <algorithm>
#include <cmath>
#include <saber/util.hpp>
#include <string>

using namespace saber;

struct Volume : Command {
	explicit Volume(Saber &creator)
		: Command(
			  creator,
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
				  .slash_options(
					  {ekizu::ApplicationCommandOptionBuilder()
						   .type(ekizu::ApplicationCommandOptionType::Integer)
						   .name("level")
						   .description("Volume level (0-200%)")
						   .required(false)
						   .min_value(static_cast<int64_t>(0))
						   .max_value(static_cast<int64_t>(200))
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

		std::optional<int> level;
		if (!args.empty()) {
			try {
				level = std::stoi(args[0]);
			} catch (...) {
				return send_reply("Invalid volume. Usage: `volume [0-200]`.");
			}
		}

		return do_volume(
			*message.guild_id, message.author.id, level, send_reply, yield);
	}

	Result<> execute(const ekizu::Interaction &interaction,
					 const boost::asio::yield_context &yield) override {
		if (!interaction.guild_id) {
			return boost::system::errc::operation_not_permitted;
		}

		auto level = util::get_int_option<int>(interaction, "level");
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

		return do_volume(
			*interaction.guild_id, *user_id, level, send_reply, yield);
	}

   private:
	template <typename SendReply>
	Result<> do_volume(ekizu::Snowflake guild_id, ekizu::Snowflake user_id,
					   std::optional<int> level, SendReply send_reply,
					   const boost::asio::yield_context &yield) {
		SABER_TRY(auto guild, bot.get_guild(guild_id, yield));

		if (!bot.player().has_connection(guild_id)) {
			return send_reply("I am not in a voice channel. Use `join` first.");
		}

		// Check if user is in same voice channel as bot
		auto voice_state = bot.voice_states().get(guild_id).flat_map(
			[&](auto &users) { return users.get(user_id); });

		SABER_TRY(auto bot_channel_id, bot.player().voice_channel_id(guild_id));

		if (!voice_state.map([](auto &s) { return !!s.channel_id; })
				 .value_or(false) ||
			(*voice_state->channel_id != bot_channel_id)) {
			return send_reply(
				"You must be in my voice channel to control volume.");
		}

		if (!level) {
			// Show current volume
			SABER_TRY(auto v, bot.player().volume(guild_id));
			int pct = static_cast<int>(std::lround(v * 100.0F));
			return send_reply(fmt::format("🔊 Current volume: {}%", pct));
		}

		// Set volume
		int pct = std::clamp(*level, 0, 200);
		const float scalar = static_cast<float>(pct) / 100.0F;
		SABER_TRY(bot.player().set_volume(guild_id, scalar));

		return send_reply(fmt::format("🔊 Volume set to {}%.", pct));
	}
};

COMMAND_ALLOC(Volume)
COMMAND_FREE(Volume)
