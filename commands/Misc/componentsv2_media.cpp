#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <ekizu/http.hpp>
#include <ekizu/request/upload_attachment.hpp>
#include <saber/util.hpp>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace saber;

namespace {

std::string basename_from_url(std::string_view url) {
	// Strip query string.
	auto qpos = url.find('?');
	if (qpos != std::string_view::npos) { url = url.substr(0, qpos); }

	// Take everything after the last '/'.
	auto slash = url.find_last_of('/');
	std::string name = (slash == std::string_view::npos)
						   ? std::string(url)
						   : std::string(url.substr(slash + 1));

	// Fallback if empty.
	if (name.empty()) { name = "file"; }

	// Basic sanitization for multipart headers.
	name.erase(std::remove(name.begin(), name.end(), '\r'), name.end());
	name.erase(std::remove(name.begin(), name.end(), '\n'), name.end());
	name.erase(std::remove(name.begin(), name.end(), '"'), name.end());

	if (name.empty()) { name = "file"; }
	return name;
}

std::string guess_content_type(std::string_view filename) {
	auto dot = filename.find_last_of('.');
	if (dot == std::string_view::npos) { return "application/octet-stream"; }

	std::string ext(filename.substr(dot + 1));
	for (auto &c : ext) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

	if (ext == "png") { return "image/png"; }
	if (ext == "jpg" || ext == "jpeg") { return "image/jpeg"; }
	if (ext == "gif") { return "image/gif"; }
	if (ext == "webp") { return "image/webp"; }

	return "application/octet-stream";
}

std::string make_unique_filename(std::string base,
								 std::unordered_set<std::string> &used) {
	if (used.insert(base).second) { return base; }

	// Insert suffix before extension if possible.
	auto dot = base.find_last_of('.');
	std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
	std::string ext = (dot == std::string::npos) ? "" : base.substr(dot);

	for (int i = 2;; ++i) {
		std::string candidate = fmt::format("{}_{}{}", stem, i, ext);
		if (used.insert(candidate).second) { return candidate; }
	}
}

}  // namespace

struct ComponentsV2Media : Command {
	explicit ComponentsV2Media(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("componentsv2_media")
					  .category(DIRNAME)
					  .enabled(true)
					  .guild_only(false)
					  .usage("componentsv2_media [url1 url2 url3]")
					  .description(
						  "Downloads remote images, re-uploads them as "
						  "attachments, and displays them using Components V2 "
						  "Media/File components.")
					  .bot_permissions(ekizu::Permissions::SendMessages)
					  .cooldown(std::chrono::seconds(3))
					  .build()) {}

	Result<> execute(const ekizu::Message &message,
					 const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		auto urls = args;

		// Keep it small and deterministic for a demo.
		if (urls.size() > 3) { urls.resize(3); }

		std::vector<ekizu::UploadAttachment> uploads;
		uploads.reserve(urls.size());

		std::unordered_set<std::string> used_names;

		for (const auto &url : urls) {
			SABER_TRY(auto resp, ekizu::net::HttpConnection::get(
									 yield.get_executor(), url, yield));

			// Conservative success check: accept any 2xx.
			if (resp.result_int() < 200 || resp.result_int() >= 300) {
				SABER_TRY(
					bot.http()
						.create_message(message.channel_id)
						.content(fmt::format("Download failed (HTTP {}): {}",
											 resp.result_int(), url))
						.reply(message.id)
						.send(yield));
				return outcome::success();
			}

			std::string filename = basename_from_url(url);
			filename = make_unique_filename(std::move(filename), used_names);

			ekizu::UploadAttachment up;
			up.filename = filename;
			up.data = resp.body();
			up.content_type = guess_content_type(filename);
			up.description = fmt::format("Downloaded from {}", url);

			uploads.push_back(std::move(up));
		}

		if (uploads.empty()) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("No URLs to download.")
						  .reply(message.id)
						  .send(yield));
			return outcome::success();
		}

		// Build Components V2 layout that references the *uploaded*
		// attachments. For attachments in Components V2, reference as:
		// attachment://filename
		const auto attachment_url = [](const std::string &fn) {
			return fmt::format("attachment://{}", fn);
		};

		ekizu::TextDisplay header;
		header.content = "Components V2 Media/File demo";

		ekizu::TextDisplay body;
		body.content =
			"These images were downloaded from URLs, re-uploaded as "
			"attachments, "
			"and displayed via V2 components.";

		ekizu::Section section;
		{
			ekizu::TextDisplay line1;
			line1.content = "Section + Thumbnail accessory";
			ekizu::TextDisplay line2;
			line2.content = fmt::format("Uploaded items: {}", uploads.size());

			section.components = {line1, line2};

			ekizu::Thumbnail thumb;
			thumb.media.url = attachment_url(uploads.front().filename);
			thumb.description = uploads.front().filename;
			section.accessory = thumb;
		}

		ekizu::Separator sep;
		sep.divider = true;

		ekizu::MediaGallery gallery;
		gallery.items.reserve(uploads.size());
		for (const auto &u : uploads) {
			ekizu::MediaGalleryItem item;
			item.media.url = attachment_url(u.filename);
			item.description = u.filename;
			gallery.items.push_back(std::move(item));
		}

		ekizu::File file_display;
		file_display.file.url = attachment_url(uploads.back().filename);

		ekizu::Container container;
		container.components = {
			header, body, section, sep, gallery, file_display};

		SABER_TRY(bot.http()
					  .create_message(message.channel_id)
					  .flags(ekizu::MessageFlags::IsComponentsV2)
					  .components({container})
					  .attachments(std::move(uploads))
					  .send(yield));

		return outcome::success();
	}
};

COMMAND_ALLOC(ComponentsV2Media)
COMMAND_FREE(ComponentsV2Media)
