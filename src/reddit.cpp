#include <fmt/format.h>
#include <fmt/ranges.h>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/url/encode.hpp>
#include <boost/url/rfc/unreserved_chars.hpp>
#include <ekizu/json_util.hpp>
#include <saber/reddit.hpp>

namespace {
constexpr const char *API_HOST = "oauth.reddit.com";
constexpr const char *ACCESS_TOKEN_PATH = "/api/v1/access_token";

std::string base64_encode(std::string_view str) {
	std::string ret(
		boost::beast::detail::base64::encoded_size(str.size()), '\0');
	boost::beast::detail::base64::encode(ret.data(), str.data(), str.size());
	return ret;
}

std::string to_query_string(const saber::RedditFields &fields) {
	std::vector<std::string> query_elements;

	auto add_query_param =
		[&query_elements](const std::string &key, const auto &opt_value) {
			if (opt_value) {
				query_elements.push_back(fmt::format(
					"{}={}", key,
					boost::urls::encode(fmt::to_string(opt_value.value()),
										boost::urls::unreserved_chars)));
			}
		};

	add_query_param("q", fields.query);
	add_query_param("include_over_18", fields.include_over_18);
	add_query_param("restrict_sr", fields.restrict_sr);
	add_query_param("sort", fields.sort);
	add_query_param("time", fields.time);
	add_query_param("limit", fields.limit);
	add_query_param("count", fields.count);

	return fmt::format(
		"{}/search?{}",
		fields.subreddit ? fmt::format("/r/{}", fields.subreddit.value()) : "",
		fmt::join(query_elements, "&"));
}

}  // namespace

namespace saber {

std::string GetReddit::m_build_path() const {
	return to_query_string(m_fields);
}

Reddit::Reddit(asio::any_io_executor ex, RedditOptions options)
	: m_ex{std::move(ex)}, m_options{std::move(options)} {}

GetReddit Reddit::get() {
	return GetReddit(GetReddit::MakeRequestFn{
		[this](
			std::string path,
			asio::any_completion_handler<void(Result<net::HttpResponse>)> h) {
			asio::dispatch(m_ex, [this, path = std::move(path),
								  h = std::move(h)]() mutable {
				get_token_async(asio::any_completion_handler<
								void(Result<std::string>)>{asio::bind_executor(
					m_ex, [this, path = std::move(path),
						   h = std::move(h)](Result<std::string> tok) mutable {
						if (!tok) {
							std::move(h)(tok.error());
							return;
						}

						net::HttpRequest req{net::HttpMethod::get, path, 11};
						req.set(net::http::field::authorization, tok.value());
						req.set(net::http::field::host, API_HOST);
						req.set(net::http::field::user_agent, "insomnia/8.4.5");
						req.prepare_payload();

						request_async(std::move(req), std::move(h));
					})});
			});
		}});
}

void Reddit::request_async(
	net::HttpRequest req,
	asio::any_completion_handler<void(Result<net::HttpResponse>)> handler) {
	asio::dispatch(m_ex, [this, req = std::move(req),
						  h = std::move(handler)]() mutable {
		struct Attempt : std::enable_shared_from_this<Attempt> {
			Reddit *self{};
			net::HttpRequest req;
			int attempt{};
			asio::any_completion_handler<void(Result<net::HttpResponse>)>
				handler;

			Attempt(
				Reddit *s, net::HttpRequest r, int a,
				asio::any_completion_handler<void(Result<net::HttpResponse>)> h)
				: self{s},
				  req{std::move(r)},
				  attempt{a},
				  handler{std::move(h)} {}

			void start() {
				if (self->m_http) {
					do_request();
					return;
				}

				net::HttpConnection::connect(
					self->m_ex, "https://www.reddit.com",
					asio::bind_executor(
						self->m_ex,
						[me = shared_from_this()](
							Result<net::HttpConnection> conn) mutable {
							if (!conn) {
								std::move(me->handler)(conn.error());
								return;
							}
							me->self->m_http = std::move(conn.value());
							me->do_request();
						}));
			}

			void do_request() {
				// Keep a copy for one retry (HttpRequest is copyable).
				net::HttpRequest original = req;

				self->m_http->request(
					std::move(req),
					asio::bind_executor(
						self->m_ex, [me = shared_from_this(),
									 original = std::move(original)](
										Result<net::HttpResponse> res) mutable {
							if (res) {
								std::move(me->handler)(std::move(res));
								return;
							}

							if (me->attempt >= 1) {
								std::move(me->handler)(res.error());
								return;
							}

							// Retry once: reconnect then resend the same
							// request.
							me->self->m_http.reset();
							me->attempt += 1;
							me->req = std::move(original);
							me->start();
						}));
			}
		};

		std::make_shared<Attempt>(this, std::move(req), 0, std::move(h))
			->start();
	});
}

void Reddit::get_token_async(
	asio::any_completion_handler<void(Result<std::string>)> handler) {
	asio::dispatch(m_ex, [this, h = std::move(handler)]() mutable {
		if (std::chrono::system_clock::now() < m_token_expiration &&
			!m_token.empty()) {
			std::move(h)(m_token);
			return;
		}

		net::HttpRequest req{
			net::HttpMethod::post,
			fmt::format(
				"{}?grant_type=password&username={}&password={}",
				ACCESS_TOKEN_PATH, m_options.username, m_options.password),
			11};

		const auto authorization = fmt::format(
			"Basic {}", base64_encode(fmt::format(
							"{}:{}", m_options.app_id, m_options.app_secret)));

		req.set(net::http::field::authorization, authorization);
		req.set(net::http::field::host, "www.reddit.com");
		req.set(net::http::field::user_agent, "insomnia/8.4.5");
		req.prepare_payload();

		request_async(
			std::move(req),
			asio::any_completion_handler<void(Result<net::HttpResponse>)>{
				asio::bind_executor(
					m_ex, [this, h = std::move(h)](
							  Result<net::HttpResponse> res) mutable {
						if (!res) {
							std::move(h)(res.error());
							return;
						}

						if (res.value().result() != net::http::status::ok) {
							std::move(h)(boost::system::errc::invalid_argument);
							return;
						}

						auto data =
							ekizu::json_util::try_parse(res.value().body());
						if (!data) {
							std::move(h)(data.error());
							return;
						}

						if (!ekizu::json_util::not_null_all(
								data.value(), "token_type", "access_token") ||
							!data.value().contains("expires_in")) {
							std::move(h)(boost::system::errc::invalid_argument);
							return;
						}

						m_token = fmt::format(
							"{} {}",
							data.value()["token_type"].get<std::string>(),
							data.value()["access_token"].get<std::string>());

						m_token_expiration =
							std::chrono::system_clock::now() +
							std::chrono::seconds(
								data.value()["expires_in"].get<int64_t>());

						std::move(h)(m_token);
					})});
	});
}
}  // namespace saber
