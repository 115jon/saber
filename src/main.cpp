#include <boost/asio/detached.hpp>
#include <boost/asio/signal_set.hpp>
#include <saber/saber.hpp>

int main() {
	boost::asio::io_context ctx;
	auto config = saber::Config::from_file("config.json");

	if (!config || config.value().token.empty()) {
		fmt::println(
			"Missing `token` in config.json. Please include or use the "
			"`SABER_TOKEN` environment variable.");
		return boost::system::errc::invalid_argument;
	}

	auto saber = saber::Saber{ctx, config.value()};

	boost::asio::spawn(
		ctx, [&saber](const auto &y) { saber.run(y); }, boost::asio::detached);

	// Handle shutdown signals
	boost::asio::signal_set signals(ctx, SIGINT, SIGTERM);
	signals.async_wait(
		[&](const boost::system::error_code &ec, int /* signal_number */) {
			if (ec) { return; }

			// Run an orderly async shutdown that can wait for shard.close() to
			// complete.
			boost::asio::spawn(
				ctx,
				[&saber](const boost::asio::yield_context &y) {
					boost::ignore_unused(saber.stop(y));
				},
				boost::asio::detached);

			// Prevent re-entry if multiple signals arrive.
			boost::system::error_code ignored;
			signals.cancel(ignored);
		});

	ctx.run();
}
