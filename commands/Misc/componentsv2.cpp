#include <fmt/format.h>

#include <saber/util.hpp>

using namespace saber;

struct ComponentsV2 : Command {
	explicit ComponentsV2(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("componentsv2")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(false)
					  .usage("componentsv2")
					  .description(
						  "Send a Components V2 message with a button and a "
						  "select menu.")
					  .bot_permissions(ekizu::Permissions::SendMessages)
					  .cooldown(std::chrono::seconds(3))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		// Build button + select menu.
		auto button = ekizu::ButtonBuilder()
						  .custom_id("v2_demo_btn")
						  .style(ekizu::ButtonStyle::Primary)
						  .label("Click")
						  .build();

		auto select =
			ekizu::SelectMenuBuilder()
				.custom_id("v2_demo_select")
				.placeholder("Pick one")
				.options({ekizu::SelectOptionsBuilder()
							  .label("One")
							  .value("one")
							  .build(),
						  ekizu::SelectOptionsBuilder()
							  .label("Two")
							  .value("two")
							  .build(),
						  ekizu::SelectOptionsBuilder()
							  .label("Three")
							  .value("three")
							  .build()})
				.min_values(1)
				.max_values(1)
				.build();

		// NOTE: Discord enforces a per-action-row layout width limit. Select
		// menus consume the full width, so they cannot share a row with other
		// components.
		auto button_row =
			ekizu::ActionRowBuilder().components({button}).build();

		auto select_row =
			ekizu::ActionRowBuilder().components({select}).build();

		ekizu::TextDisplay header;
		header.content = "Components V2 demo";

		ekizu::TextDisplay body;
		body.content = "Use the button or the select menu below.";

		ekizu::Container container;
		container.components = {header, body, button_row, select_row};

		SABER_TRY(auto msg, bot.http()
								.create_message(message.channel_id)
								.flags(ekizu::MessageFlags::IsComponentsV2)
								.components({container})
								.send(yield));

		auto filter = [author_id = message.author.id, msg_id = msg.id](
						  const ekizu::Interaction &interaction,
						  const ekizu::MessageComponentData &data) {
			std::optional<ekizu::Snowflake> user_id;
			if (interaction.member) {
				user_id = interaction.member->user.id;
			} else if (interaction.user) {
				user_id = interaction.user->id;
			}

			if (user_id != author_id) { return false; }

			if (data.custom_id != "v2_demo_btn" &&
				data.custom_id != "v2_demo_select") {
				return false;
			}

			if (!interaction.message) { return false; }
			return interaction.message->id == msg_id;
		};

		auto collector = bot.create_message_component_collector(
			message.channel_id, filter,
			std::vector<ekizu::ComponentType>{
				ekizu::ComponentType::Button, ekizu::ComponentType::SelectMenu},
			std::chrono::minutes(5), yield);

		auto ack_interaction = [&](const ekizu::Interaction &i) {
			boost::system::error_code ec;
			(void)bot.http()
				.interaction(i.application_id)
				.create_response(i.id, i.token,
								 ekizu::InteractionResponseBuilder()
									 .type(ekizu::InteractionResponseType::
											   DeferredUpdateMessage)
									 .build())
				.send(yield[ec]);
		};

		auto send_followup = [&](std::string content) {
			boost::system::error_code ec;
			(void)bot.http()
				.create_message(message.channel_id)
				.content(std::move(content))
				.send(yield[ec]);
		};

		while (true) {
			auto res = collector.async_receive(yield);
			if (!res) { break; }

			auto &[i, data] = res.value();
			ack_interaction(i);

			if (data.custom_id == "v2_demo_btn") {
				send_followup("Button clicked.");
			} else if (data.custom_id == "v2_demo_select") {
				std::string joined;
				const auto &values =
					data.values.value_or(std::vector<std::string>{});
				for (size_t idx = 0; idx < values.size(); ++idx) {
					if (idx != 0U) { joined.append(", "); }
					joined.append(values[idx]);
				}
				send_followup(fmt::format(
					"Selected: {}", joined.empty() ? "(none)" : joined));
			}
		}

		// Cleanup: Disable components when collector ends.
		if (msg.components.empty()) { return outcome::success(); }
		auto *c = std::get_if<ekizu::Container>(msg.components.data());
		if (c == nullptr) { return outcome::success(); }

		for (auto &child : c->components) {
			auto *ar = std::get_if<ekizu::ActionRow>(&child);
			if (ar == nullptr) { continue; }
			for (auto &comp : ar->components) {
				std::visit(
					[](auto &v) {
						using T = std::decay_t<decltype(v)>;
						if constexpr (std::is_same_v<T, ekizu::Button>) {
							v.disabled = true;
						} else if constexpr (std::is_same_v<
												 T, ekizu::SelectMenu>) {
							v.disabled = true;
						}
					},
					comp);
			}
		}

		SABER_TRY(bot.http()
					  .edit_message(msg.channel_id, msg.id)
					  .components(std::move(msg.components))
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(ComponentsV2)
COMMAND_FREE(ComponentsV2)
