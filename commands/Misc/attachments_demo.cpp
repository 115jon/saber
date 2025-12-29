#include <fmt/core.h>

#include <nlohmann/json.hpp>
#include <saber/util.hpp>

using namespace saber;

namespace {
std::string format_attachment_list(const ekizu::Message &m) {
	if (m.attachments.empty()) {
		return "(no attachments on returned Message object)";
	}

	std::string out;
	for (const auto &a : m.attachments) {
		out += fmt::format("- {} (id={})\n", a.filename, a.id);
	}
	return out;
}
}  // namespace

struct AttachmentsDemo : Command {
	explicit AttachmentsDemo(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("attachmentsdemo")
					  .category(DIRNAME)
					  .enabled(true)
					  .aliases({"attachdemo"})
					  .guild_only(false)
					  .usage("attachmentsdemo")
					  .description("Demonstrate Ekizu message upload + "
								   "keep_attachment_ids + "
								   "payload_json override behavior.")
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(3))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 [[maybe_unused]] const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		// NOTE: Uploading files requires the bot to have the Attach Files
		// permission in the channel. If missing, the HTTP request will fail.
		ekizu::UploadAttachment a1;
		a1.filename = "ekizu-demo-1.txt";
		a1.data =
			"Ekizu attachments demo\n"
			"Step A: create_message with multipart upload\n";
		a1.content_type = "text/plain";
		a1.description = "Demo file 1";

		SABER_TRY(
			auto demo_msg,
			bot.http()
				.create_message(message.channel_id)
				.reply(message.id)
				.content("Attachments demo: Step A (created message with 1 "
						 "uploaded file).")
				.attachments({a1})
				.send(yield));

		std::optional<ekizu::Snowflake> keep_id;
		if (!demo_msg.attachments.empty()) {
			keep_id = demo_msg.attachments.front().id;
		}

		const std::string step_a_list = format_attachment_list(demo_msg);

		ekizu::UploadAttachment a2;
		a2.filename = "ekizu-demo-2.txt";
		a2.data =
			"Ekizu attachments demo\n"
			"Step B: edit_message keep_attachment_ids + upload new file\n";
		a2.content_type = "text/plain";
		a2.description = "Demo file 2";

		std::vector<ekizu::Snowflake> keep_ids;
		if (keep_id) { keep_ids.push_back(*keep_id); }

		SABER_TRY(
			auto demo_msg_b,
			bot.http()
				.edit_message(demo_msg.channel_id, demo_msg.id)
				.content(
					"Attachments demo: Step B (kept prior attachment id(s) "
					"and uploaded a second file).")
				.keep_attachment_ids(std::move(keep_ids))
				.attachments({a2})
				.send(yield));

		demo_msg = std::move(demo_msg_b);
		const std::string step_b_list = format_attachment_list(demo_msg);

		ekizu::UploadAttachment a3;
		a3.filename = "ekizu-demo-3.txt";
		a3.data =
			"Ekizu attachments demo\n"
			"Step C: payload_json override + upload\n";
		a3.content_type = "text/plain";
		a3.description = "Demo file 3";

		nlohmann::json payload;
		payload["content"] =
			"Attachments demo: Step C (payload_json override). If you see this "
			"text, payload_json won over builder fields.";

		SABER_TRY(auto demo_msg_c,
				  bot.http()
					  .edit_message(demo_msg.channel_id, demo_msg.id)
					  .content("THIS SHOULD BE IGNORED (builder field)")
					  .payload_json(payload.dump())
					  .attachments({a3})
					  .send(yield));

		demo_msg = std::move(demo_msg_c);
		const std::string step_c_list = format_attachment_list(demo_msg);

		std::string summary;
		summary += "Attachments demo complete.\n\n";
		summary += fmt::format("Step A attachments:\n{}\n", step_a_list);
		summary += fmt::format("Step B attachments:\n{}\n", step_b_list);
		summary += fmt::format("Step C attachments:\n{}\n", step_c_list);
		summary += "\nNote: Step C uses payload_json override.";

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .content(std::move(summary))
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(AttachmentsDemo)
COMMAND_FREE(AttachmentsDemo)
