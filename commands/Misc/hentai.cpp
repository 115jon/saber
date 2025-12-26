#include <fmt/ranges.h>

#include <boost/range/adaptors.hpp>
#include <ekizu/json_util.hpp>
#include <saber/reddit.hpp>
#include <saber/saber.hpp>
#include <saber/util.hpp>

using namespace saber;

struct Hentai : Command {
	explicit Hentai(Saber &creator)
		: Command(creator,
				  CommandOptionsBuilder()
					  .name("hentai")
					  .category(DIRNAME)
					  .enabled(true)
					  .init(true)
					  .bot_permissions(ekizu::Permissions::SendMessages |
									   ekizu::Permissions::EmbedLinks)
					  .cooldown(std::chrono::seconds(2))
					  .build()),
		  m_reddit{creator.http().get_executor(),
				   RedditOptions{
					   std::getenv("SABER_REDDIT_USERNAME"),
					   std::getenv("SABER_REDDIT_PASSWORD"),
					   std::getenv("SABER_REDDIT_APP_ID"),
					   std::getenv("SABER_REDDIT_APP_SECRET"),
				   }} {}

	Result<> execute(const ekizu::Message &message,
					 const std::vector<std::string> &args,
					 const boost::asio::yield_context &yield) override {
		if (args.empty()) {
			SABER_TRY(bot.http()
						  .create_message(message.channel_id)
						  .content("Please specify a query.")
						  .reply(message.id)
						  .send(yield));

			return outcome::success();
		}

		const auto query = fmt::to_string(fmt::join(args, " "));
		SABER_TRY(const auto sauce, search_hentai_subreddit(query, yield));
		auto cm =
			bot.http().create_message(message.channel_id).reply(message.id);

		if (sauce.empty()) {
			SABER_TRY(cm.content("No results found.").send(yield));
			return outcome::success();
		}

		SABER_TRY(cm.content(sauce).send(yield));

		return outcome::success();
	}

   private:
	Result<std::string> search_hentai_subreddit(
		std::string_view query, const boost::asio::yield_context &yield) {
		SABER_TRY(
			auto res,
			m_reddit.get()
				.subreddit("hentai")
				.query(query)
				.include_over_18(true)
				.restrict_sr(true)
				.limit(100)
				.count(100 *
					   std::floor(util::get_random_number<double>(0, 1) * 3))
				.time("all")
				.sort("relevance")
				.send(yield));
		SABER_TRY(const auto data, ekizu::json_util::try_parse(res.body()));

		const auto &posts = data["data"]["children"];

		std::vector<nlohmann::json> filtered_posts;
		using ekizu::json_util::not_null_all;
		using ekizu::json_util::not_null_recursive;
		std::copy_if(posts.begin(), posts.end(),
					 std::back_inserter(filtered_posts), [](const auto &post) {
						 return not_null_recursive(post, "data", "post_hint") &&
								post["data"]["post_hint"] == "image";
					 });

		// Return a random post.
		if (filtered_posts.empty()) { return ""; }

		const auto &post = filtered_posts[util::get_random_number<size_t>(
			0, filtered_posts.size() - 1)];

		// Get the post's URL.
		if (!not_null_all(post["data"], "url")) { return ""; }
		return post["data"]["url"];
	}

	Reddit m_reddit;
};

COMMAND_ALLOC(Hentai)
COMMAND_FREE(Hentai)
