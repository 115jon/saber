#include <fmt/format.h>
#include <fmt/ranges.h>

#include <boost/asio/detached.hpp>
#include <ekizu/request/interaction/create_response.hpp>
#include <saber/util.hpp>

using namespace saber;

struct ComponentsV2Modal : Command {
	explicit ComponentsV2Modal(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("componentsv2_modal")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(false)
					  .description("Demo: button opens text+file modal.")
					  .bot_permissions(ekizu::Permissions::SendMessages)
					  .cooldown(std::chrono::seconds(3))
					  .build()) {}

	// Helper method: handles modal submission asynchronously
	void handle_modal_submission(
		ekizu::Snowflake channel_id, ekizu::Snowflake author_id,
		std::string modal_custom_id, const boost::asio::yield_context &yield) {
		auto modal_filter =
			[author_id, modal_custom_id = std::move(modal_custom_id)](
				const ekizu::Interaction &i,
				const ekizu::ModalSubmitData &data) {
				auto user_id = i.member ? i.member->user.id : i.user->id;
				if (user_id != author_id) return false;
				return data.custom_id == modal_custom_id;
			};

		// Long timeout is fine now since we're non-blocking
		auto modal_collector = bot.create_modal_submit_collector(
			channel_id, modal_filter, std::chrono::minutes(5), yield);

		auto modal_res = modal_collector.async_receive(yield);

		if (!modal_res) return;	 // Timeout or cancel, just exit silently

		auto [modal_i, modal_data] = modal_res.value();

		std::string title, desc;
		size_t file_count = 0;
		std::vector<ekizu::Snowflake> file_ids;
		std::vector<ekizu::Attachment> attachments;

		// Extract data from modal submission
		for (const auto &response : modal_data.components) {
			if (const auto *label =
					std::get_if<ekizu::LabelResponse>(&response)) {
				if (!label->component) continue;

				if (const auto *ti = std::get_if<ekizu::TextInputResponse>(
						&(*label->component))) {
					if (ti->custom_id == "c2_title") {
						title = ti->value;
					} else if (ti->custom_id == "c2_description") {
						desc = ti->value;
					}
				} else if (const auto *fi =
							   std::get_if<ekizu::FileUploadResponse>(
								   &(*label->component))) {
					if (fi->custom_id == "c2_files") {
						file_count = fi->values.size();
						file_ids = fi->values;
					}
				}
			} else if (const auto *ar =
						   std::get_if<ekizu::ActionRowResponse>(&response)) {
				for (const auto &comp : ar->components) {
					if (const auto *ti =
							std::get_if<ekizu::TextInputResponse>(&comp)) {
						if (ti->custom_id == "c2_title") {
							title = ti->value;
						} else if (ti->custom_id == "c2_description") {
							desc = ti->value;
						}
					} else if (const auto *fi =
								   std::get_if<ekizu::FileUploadResponse>(
									   &comp)) {
						if (fi->custom_id == "c2_files") {
							file_count = fi->values.size();
							file_ids = fi->values;
						}
					}
				}
			}
		}

		if (modal_data.resolved) {
			for (const auto &file_id : file_ids) {
				auto it = modal_data.resolved->attachments.find(file_id);
				if (it != modal_data.resolved->attachments.end()) {
					attachments.push_back(it->second);
				}
			}
		}

		// Build summary with attachment URLs
		std::string attachment_info;
		for (const auto &att : attachments) {
			attachment_info +=
				fmt::format("\n- **{}** ({} bytes) [Download]({})",
							att.filename, att.size, att.url);
		}

		std::string summary = fmt::format(
			"✅ **Modal received!**\n**Title:** {}\n**Description:** "
			"{}\n**Files:** {}{}",
			title.empty() ? "(none)" : title,
			desc.empty() ? "(none)" : util::truncate(desc, 100), file_count,
			attachment_info);

		// Respond to close the modal
		auto result =
			bot.http()
				.interaction(modal_i.application_id)
				.create_response(modal_i.id, modal_i.token,
								 ekizu::InteractionResponseBuilder()
									 .type(ekizu::InteractionResponseType::
											   ChannelMessageWithSource)
									 .content(summary)
									 .build())
				.send(yield);

		// Silently ignore errors in spawned context
		(void)result;
	}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		// 1. Send the initial message with the Button
		ekizu::TextDisplay header;
		header.content = "Click below to open upload modal";

		ekizu::Button btn;
		btn.custom_id = "c2_modal_open";
		btn.style = ekizu::ButtonStyle::Primary;
		btn.label = "📤 Open upload modal";

		ekizu::Container container;
		container.components = {
			header, ekizu::ActionRowBuilder().components({btn}).build()};

		SABER_TRY(auto msg, bot.http()
								.create_message(message.channel_id)
								.flags(ekizu::MessageFlags::IsComponentsV2)
								.components({container})
								.send(yield));

		// 2. Button Collector (Long-lived)
		auto btn_filter = [author_id = message.author.id, msgid = msg.id](
							  const ekizu::Interaction &i,
							  const ekizu::MessageComponentData &data) {
			auto user_id = i.member ? i.member->user.id : i.user->id;
			if (user_id != author_id) return false;
			if (!i.message || i.message->id != msgid) return false;
			return data.custom_id == "c2_modal_open";
		};

		auto btn_collector = bot.create_message_component_collector(
			message.channel_id, btn_filter, {ekizu::ComponentType::Button},
			std::chrono::minutes(10), yield);

		while (true) {
			// Wait for Button Click
			auto btn_res = btn_collector.async_receive(yield);
			if (!btn_res) break;  // Timeout or error

			auto [btn_i, btn_data] = btn_res.value();

			// 3. Respond with Modal
			ekizu::TextInput title_input;
			title_input.custom_id = "c2_title";
			title_input.style = ekizu::TextInputStyle::Short;
			title_input.placeholder = "Enter a title...";
			title_input.required = true;
			title_input.max_length = 100;

			ekizu::TextInput desc_input;
			desc_input.custom_id = "c2_description";
			desc_input.style = ekizu::TextInputStyle::Paragraph;
			desc_input.placeholder = "Enter a description...";
			desc_input.required = false;
			desc_input.max_length = 1000;

			ekizu::FileUpload file_input;
			file_input.custom_id = "c2_files";
			file_input.min_values = 0;
			file_input.max_values = 3;
			file_input.required = false;

			ekizu::Label title_label;
			title_label.label = "Title";
			title_label.description = "Give your submission a title";
			title_label.component = title_input;

			ekizu::Label desc_label;
			desc_label.label = "Description";
			desc_label.description = "Describe your submission";
			desc_label.component = desc_input;

			ekizu::Label file_label;
			file_label.label = "Files";
			file_label.description = "Upload up to 3 files";
			file_label.component = file_input;

			boost::system::error_code ec;
			SABER_TRY(
				bot.http()
					.interaction(btn_i.application_id)
					.create_response(
						btn_i.id, btn_i.token,
						ekizu::InteractionResponseBuilder()
							.type(ekizu::InteractionResponseType::Modal)
							.custom_id("c2_modal_upload")
							.title("Upload Submission")
							.components({title_label, desc_label, file_label})
							.flags(ekizu::MessageFlags::IsComponentsV2)
							.build())
					.send(yield[ec]));

			if (ec) continue;

			// 4. Spawn modal handler asynchronously (NON-BLOCKING!)
			// Button collector immediately continues to next iteration
			boost::asio::spawn(
				yield.get_executor(),
				[this, channel_id = message.channel_id,
				 author_id =
					 message.author.id](boost::asio::yield_context yield) {
					handle_modal_submission(
						channel_id, author_id, "c2_modal_upload", yield);
				},
				boost::asio::detached);

			// Loop immediately continues - button stays responsive!
		}

		// Cleanup: disable button when the button collector expires
		btn.disabled = true;
		container.components = {
			header, ekizu::ActionRowBuilder().components({btn}).build()};
		SABER_TRY(bot.http()
					  .edit_message(msg.channel_id, msg.id)
					  .components({container})
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(ComponentsV2Modal)
COMMAND_FREE(ComponentsV2Modal)
