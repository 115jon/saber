#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <saber/component_collector.hpp>

namespace saber {
ComponentCollector::ComponentCollector(
	ekizu::ComponentType component_type,
	std::chrono::steady_clock::duration expiry,
	std::function<bool(const ekizu::Interaction&,
					   const ekizu::MessageComponentData&)>
		filter,
	const boost::asio::yield_context& yield)
	: m_component_type{component_type},
	  m_filter{std::move(filter)},
	  m_on_collect_chan{yield.get_executor()} {
	boost::asio::spawn(
		yield,
		[this, expiry](auto y) {
			boost::asio::steady_timer timer{y.get_executor(), expiry};
			timer.async_wait(y);
			m_on_collect_chan.close();
		},
		boost::asio::detached);
}

Result<
	std::tuple<const ekizu::Interaction*, const ekizu::MessageComponentData*>>
ComponentCollector::async_receive(const boost::asio::yield_context& yield) {
	boost::system::error_code ec;
	auto res = m_on_collect_chan.async_receive(yield[ec]);
	if (ec) { return ec; }
	return res;
}

void ComponentCollector::async_send(const ekizu::Interaction& interaction,
									const boost::asio::yield_context& yield) {
	if (!interaction.data) { return; }

	std::visit(
		[this, &interaction, &yield](const auto& data) {
			using T = std::decay_t<decltype(data)>;

			if constexpr (std::is_same_v<T, ekizu::MessageComponentData>) {
				if (data.type != m_component_type ||
					(m_filter && !m_filter(interaction, data))) {
					return;
				}

				m_on_collect_chan.async_send(
					boost::system::error_code{}, &interaction, &data, yield);
			}
		},
		*interaction.data);
}
}  // namespace saber
