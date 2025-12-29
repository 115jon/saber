#include <fmt/format.h>
#include <fmt/ranges.h>

#include <saber/util.hpp>

using namespace saber;

struct ComponentsV2Selects : Command {
	explicit ComponentsV2Selects(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("componentsv2_selects")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(true)	 // Selects need guild context
					  .description("Demo: user/role/channel select menus.")
					  .bot_permissions(ekizu::Permissions::SendMessages)
					  .cooldown(std::chrono::seconds(3))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		// Instructions
		ekizu::TextDisplay header;
		header.content = "Select menus below (click any)";

		// 4 select menus
		ekizu::UserSelectMenu user_sel;
		user_sel.custom_id = "c2_users";
		user_sel.placeholder = "Pick users...";
		user_sel.max_values = 5;

		ekizu::RoleSelectMenu role_sel;
		role_sel.custom_id = "c2_roles";
		role_sel.placeholder = "Pick roles...";

		ekizu::MentionableSelectMenu mention_sel;
		mention_sel.custom_id = "c2_mention";
		mention_sel.placeholder = "Pick users/roles...";

		ekizu::ChannelSelectMenu channel_sel;
		channel_sel.custom_id = "c2_channels";
		channel_sel.placeholder = "Pick channels...";

		// Each in own ActionRow
		auto ar1 = ekizu::ActionRowBuilder().components({user_sel}).build();
		auto ar2 = ekizu::ActionRowBuilder().components({role_sel}).build();
		auto ar3 = ekizu::ActionRowBuilder().components({mention_sel}).build();
		auto ar4 = ekizu::ActionRowBuilder().components({channel_sel}).build();

		ekizu::Container container;
		container.components = {header, ar1, ar2, ar3, ar4};

		SABER_TRY(auto msg, bot.http()
								.create_message(message.channel_id)
								.flags(ekizu::MessageFlags::IsComponentsV2)
								.components({container})
								.send(yield));

		auto filter = [author_id = message.author.id, msgid = msg.id](
						  const ekizu::Interaction &i,
						  const ekizu::MessageComponentData &data) {
			std::optional<ekizu::Snowflake> userid;
			if (i.member)
				userid = i.member->user.id;
			else if (i.user)
				userid = i.user->id;
			if (!userid || *userid != author_id) return false;

			if (!i.message || i.message->id != msgid) return false;

			// All 4 select custom_ids
			return data.custom_id == "c2_users" ||
				   data.custom_id == "c2_roles" ||
				   data.custom_id == "c2_mention" ||
				   data.custom_id == "c2_channels";
		};

		auto collector = bot.create_message_component_collector(
			message.channel_id, filter,
			{ekizu::ComponentType::UserSelect, ekizu::ComponentType::RoleSelect,
			 ekizu::ComponentType::MentionableSelect,
			 ekizu::ComponentType::ChannelSelect},
			std::chrono::minutes(5), yield);

		while (true) {
			auto res = collector.async_receive(yield);
			if (!res) break;

			auto [i, data] = res.value();

			// Ack first (never fail, play.cpp style)
			SABER_TRY(
				bot.http()
					.interaction(i.application_id)
					.create_response(i.id, i.token,
									 ekizu::InteractionResponseBuilder()
										 .type(ekizu::InteractionResponseType::
												   DeferredUpdateMessage)
										 .build())
					.send(yield));

			// Summary followup
			const auto &values =
				data.values.value_or(std::vector<std::string>{});
			std::string summary =
				fmt::format("✅ **{}** ({} items)\n`{}`", data.custom_id,
							values.size(), fmt::join(values, ", "));

			SABER_TRY(
				bot.http()
					.create_message(i.channel_id.value_or(message.channel_id))
					.content(summary)
					.send(yield));
		}

		// Cleanup
		SABER_TRY(bot.http()
					  .edit_message(msg.channel_id, msg.id)
					  .components({})  // Disable all
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(ComponentsV2Selects)
COMMAND_FREE(ComponentsV2Selects)
