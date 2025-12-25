#ifndef SABER_REDDIT_HPP
#define SABER_REDDIT_HPP

#include <saber/export.h>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <chrono>
#include <ekizu/http.hpp>
#include <ekizu/json_util.hpp>
#include <functional>
#include <optional>
#include <saber/result.hpp>
#include <string>
#include <string_view>

namespace saber {
namespace asio = boost::asio;
namespace net = ekizu::net;

struct RedditOptions {
	std::string username;
	std::string password;
	std::string app_id;
	std::string app_secret;
};

struct RedditFields {
	std::optional<std::string> query;

	// NSFW posts
	std::optional<bool> include_over_18;

	// Restrict to the subreddit only.
	std::optional<bool> restrict_sr;

	std::optional<std::string> sort;
	std::optional<std::string> time;
	std::optional<uint8_t> limit;
	std::optional<uint64_t> count;
	std::optional<std::string> subreddit;
};

void to_json(nlohmann::json &j, const RedditFields &fields);

struct GetReddit {
	GetReddit &query(std::string_view query) {
		m_fields.query = std::string(query);
		return *this;
	}

	GetReddit &include_over_18(bool include_over_18) {
		m_fields.include_over_18 = include_over_18;
		return *this;
	}

	GetReddit &restrict_sr(bool restrict_sr) {
		m_fields.restrict_sr = restrict_sr;
		return *this;
	}

	GetReddit &sort(std::string_view sort) {
		m_fields.sort = std::string(sort);
		return *this;
	}

	GetReddit &time(std::string_view time) {
		m_fields.time = std::string(time);
		return *this;
	}

	GetReddit &limit(uint8_t limit) {
		m_fields.limit = limit;
		return *this;
	}

	GetReddit &count(uint64_t count) {
		m_fields.count = count;
		return *this;
	}

	GetReddit &subreddit(std::string_view subreddit) {
		m_fields.subreddit = std::string(subreddit);
		return *this;
	}

	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<net::HttpResponse>))
				  CompletionToken>
	[[nodiscard]] auto send(CompletionToken &&token) {
		return asio::async_initiate<CompletionToken,
									void(Result<net::HttpResponse>)>(
			[this](auto &&handler) mutable {
				if (!m_make_request) {
					auto ex = asio::get_associated_executor(handler);
					asio::post(ex, [h = std::forward<decltype(handler)>(
										handler)]() mutable {
						std::move(h)(
							boost::system::errc::operation_not_permitted);
					});
					return;
				}

				m_make_request(m_build_path(),
							   asio::any_completion_handler<void(
								   Result<net::HttpResponse>)>{
								   std::forward<decltype(handler)>(handler)});
			},
			token);
	}

   private:
	friend struct Reddit;

	using MakeRequestFn = std::function<void(
		std::string path,
		asio::any_completion_handler<void(Result<net::HttpResponse>)>)>;

	explicit GetReddit(MakeRequestFn make_request)
		: m_make_request{std::move(make_request)} {}

	[[nodiscard]] SABER_EXPORT std::string m_build_path() const;

	MakeRequestFn m_make_request;
	RedditFields m_fields;
};

struct Reddit {
	SABER_EXPORT explicit Reddit(asio::any_io_executor ex,
								 RedditOptions options);

	SABER_EXPORT GetReddit get();

   private:
	// Function which makes a request to the reddit API.
	void request_async(
		net::HttpRequest req,
		asio::any_completion_handler<void(Result<net::HttpResponse>)> handler);

	void get_token_async(
		asio::any_completion_handler<void(Result<std::string>)> handler);

	asio::any_io_executor m_ex;
	RedditOptions m_options;

	std::optional<net::HttpConnection> m_http;

	std::string m_token;
	std::chrono::system_clock::time_point m_token_expiration;
};

}  // namespace saber

#endif	// SABER_REDDIT_HPP
