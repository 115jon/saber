#include <fmt/format.h>

#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cstddef>
#include <cstdint>
#include <ekizu/embed.hpp>
#include <saber/util.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {
using ekizu::Permissions;

const boost::unordered_flat_map<Permissions, std::string_view>
	permission_strings{
		{Permissions::CreateInstantInvite, "CREATE_INSTANT_INVITE"},
		{Permissions::KickMembers, "KICK_MEMBERS"},
		{Permissions::BanMembers, "BAN_MEMBERS"},
		{Permissions::Administrator, "ADMINISTRATOR"},
		{Permissions::ManageChannels, "MANAGE_CHANNELS"},
		{Permissions::ManageGuild, "MANAGE_GUILD"},
		{Permissions::AddReactions, "ADD_REACTIONS"},
		{Permissions::ViewAuditLog, "VIEW_AUDIT_LOG"},
		{Permissions::PrioritySpeaker, "PRIORITY_SPEAKER"},
		{Permissions::Stream, "STREAM"},
		{Permissions::ViewChannel, "VIEW_CHANNEL"},
		{Permissions::SendMessages, "SEND_MESSAGES"},
		{Permissions::SendTTSMessages, "SEND_TTS_MESSAGES"},
		{Permissions::ManageMessages, "MANAGE_MESSAGES"},
		{Permissions::EmbedLinks, "EMBED_LINKS"},
		{Permissions::AttachFiles, "ATTACH_FILES"},
		{Permissions::ReadMessageHistory, "READ_MESSAGE_HISTORY"},
		{Permissions::MentionEveryone, "MENTION_EVERYONE"},
		{Permissions::UseExternalEmojis, "USE_EXTERNAL_EMOJIS"},
		{Permissions::ViewGuildInsights, "VIEW_GUILD_INSIGHTS"},
		{Permissions::Connect, "CONNECT"},
		{Permissions::Speak, "SPEAK"},
		{Permissions::MuteMembers, "MUTE_MEMBERS"},
		{Permissions::DeafenMembers, "DEAFEN_MEMBERS"},
		{Permissions::MoveMembers, "MOVE_MEMBERS"},
		{Permissions::UseVAD, "USE_VAD"},
		{Permissions::ChangeNickname, "CHANGE_NICKNAME"},
		{Permissions::ManageNicknames, "MANAGE_NICKNAMES"},
		{Permissions::ManageRoles, "MANAGE_ROLES"},
		{Permissions::ManageWebhooks, "MANAGE_WEBHOOKS"},
		{Permissions::ManageGuildExpressions, "MANAGE_GUILD_EXPRESSIONS"},
		{Permissions::UseApplicationCommands, "USE_APPLICATION_COMMANDS"},
		{Permissions::RequestToSpeak, "REQUEST_TO_SPEAK"},
		{Permissions::ManageEvents, "MANAGE_EVENTS"},
		{Permissions::ManageThreads, "MANAGE_THREADS"},
		{Permissions::CreatePublicThreads, "CREATE_PUBLIC_THREADS"},
		{Permissions::CreatePrivateThreads, "CREATE_PRIVATE_THREADS"},
		{Permissions::UseExternalStickers, "USE_EXTERNAL_STICKERS"},
		{Permissions::SendMessagesInThreads, "SEND_MESSAGES_IN_THREADS"},
		{Permissions::UseEmbeddedActivities, "USE_EMBEDDED_ACTIVITIES"},
		{Permissions::ModerateMembers, "MODERATE_MEMBERS"},
		{Permissions::ViewCreatorMonetizationAnalytics,
		 "VIEW_CREATOR_MONETIZATION_ANALYTICS"},
		{Permissions::UseSoundboard, "USE_SOUNDBOARD"},
		{Permissions::UseExternalSounds, "USE_EXTERNAL_SOUNDS"},
		{Permissions::SendVoiceMessages, "SEND_VOICE_MESSAGES"}};
}  // namespace

namespace saber::util {
Result<> ensure_permissions(
	Saber &bot, const ekizu::Message &message, ekizu::Snowflake user_id,
	ekizu::Permissions required, const boost::asio::yield_context &yield) {
	auto user_perms = bot.get_guild_permissions(*message.guild_id, user_id);

	if (!user_perms) {
		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .content("Something went wrong. Please try again later.")
					  .reply(message.id)
					  .send(yield));
		return boost::system::error_code{};
	}

	std::vector<std::string> missing_permissions;
	auto missing = static_cast<size_t>(required) &
				   ~static_cast<size_t>(user_perms.value());

	if (missing == 0) { return outcome::success(); }

	for (const auto &[perm, name] : permission_strings) {
		if ((missing & static_cast<size_t>(perm)) != 0) {
			missing_permissions.emplace_back(name);
		}
	}

	SABER_TRY(bot.http()
				  .create_message(message.channel_id)
				  .content(fmt::format(
					  "{} need the following permissions: {}",
					  user_id == bot.bot_id() ? "I" : "You",
					  boost::algorithm::join(missing_permissions, ", ")))
				  .reply(message.id)
				  .send(yield));

	return boost::system::error_code{};
}

Result<boost::optional<ekizu::VoiceState &>> in_voice_channel(
	Saber &bot, const ekizu::Message &msg,
	const boost::asio::yield_context &yield) {
	if (!msg.guild_id) {
		return outcome::failure(boost::system::errc::invalid_argument);
	}

	auto voice_state =
		bot.voice_states().get(*msg.guild_id).flat_map([&](auto &users) {
			return users.get(msg.author.id);
		});

	if (!voice_state.map([](auto &state) { return !!state.channel_id; })
			 .value_or(false)) {
		SABER_TRY(
			bot.http()
				.create_message(msg.channel_id)
				.components(
					{ekizu::ContainerBuilder()
						 .accent_color(0xff0000)
						 .add(
							 ekizu::TextDisplayBuilder()
								 .content(
									 "You have to be connected in a voice "
									 "channel "
									 "before you can use this command!\n> [How "
									 "to "
									 "join a voice "
									 "channel?](https://support.discord.com/hc/"
									 "en-us/articles/"
									 "360045138571-Beginner-s-Guide-to-Discord#"
									 "h_"
									 "9de92bc2-3bca-459f-8efd-e1e2739ca4f4)")
								 .build())
						 .build()})
				.reply(msg.id)
				.flags(ekizu::MessageFlags::IsComponentsV2)
				.send(yield));
		return outcome::failure(boost::system::error_code{});
	}

	return outcome::success(voice_state);
}

std::string truncate(std::string_view s, size_t max_len) {
	if (s.size() <= max_len) { return std::string{s}; }
	if (max_len <= 3) { return std::string{s.substr(0, max_len)}; }
	return fmt::format("{}...", s.substr(0, max_len - 3));
}

std::optional<size_t> find_index_by_id(const std::deque<Track> &tracks,
									   std::optional<uint64_t> id) {
	if (!id) { return std::nullopt; }
	for (size_t i = 0; i < tracks.size(); ++i) {
		if (tracks[i].id == *id) { return i; }
	}
	return std::nullopt;
}

std::string format_track_line(const Track &track, bool is_current) {
	// Keep it short-ish to avoid embed limits.
	const auto title = truncate(track.title, 96);

	if (is_current) { return fmt::format("**`{}`** `{}`", track.id, title); }
	return fmt::format("`{}` `{}`", track.id, title);
}

std::string now_playing_line(const std::deque<Track> &tracks,
							 std::optional<uint64_t> current_id) {
	auto idx = find_index_by_id(tracks, current_id);
	if (!idx) { return "—"; }
	return format_track_line(tracks[*idx], /*is_current=*/true);
}

std::string up_next_line(const std::deque<Track> &tracks,
						 std::optional<uint64_t> current_id) {
	auto idx = find_index_by_id(tracks, current_id);
	if (!idx) { return "—"; }
	const auto next = *idx + 1;
	if (next >= tracks.size()) { return "—"; }
	return format_track_line(tracks[next], /*is_current=*/false);
}

ekizu::Embed music_action_embed(
	std::string title, uint16_t color, std::string description,
	std::string now_line, std::string next_line, std::string footer_text) {
	return ekizu::EmbedBuilder()
		.set_title(std::move(title))
		.set_color(color)
		.set_description(std::move(description))
		.add_field(ekizu::EmbedField{"Now playing", std::move(now_line), false})
		.add_field(ekizu::EmbedField{"Up next", std::move(next_line), false})
		.set_footer(ekizu::EmbedFooter{std::move(footer_text)})
		.build();
}

size_t page_count(size_t item_count, size_t page_size) {
	if (page_size == 0) { return 1; }
	return std::max<size_t>(1, (item_count + page_size - 1) / page_size);
}

}  // namespace saber::util
