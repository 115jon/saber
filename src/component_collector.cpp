#include <saber/component_collector.hpp>

namespace saber {
ComponentCollector::ComponentCollector(
	ekizu::ComponentType component_type,
	std::chrono::steady_clock::duration expiry,
	std::function<bool(const ekizu::Interaction &,
					   const ekizu::MessageComponentData &)>
		filter,
	const boost::asio::yield_context &yield)
	: m_component_type{component_type},
	  m_filter{std::move(filter)},
	  m_on_collect_chan{yield.get_executor(), 10},
	  m_timer{yield.get_executor(), expiry} {
	m_timer.async_wait([](boost::system::error_code) {});
}

ComponentCollector::~ComponentCollector() {
	m_timer.cancel();
	m_on_collect_chan.close();
}

Result<std::pair<ekizu::Interaction, ekizu::MessageComponentData>>
ComponentCollector::async_receive(const boost::asio::yield_context &yield) {
	boost::system::error_code ec;
	auto res = m_on_collect_chan.async_receive(yield[ec]);
	if (ec) { return ec; }
	return std::make_pair(
		std::move(std::get<0>(res)), std::move(std::get<1>(res)));
}

void ComponentCollector::async_send(const ekizu::Interaction &interaction,
									const boost::asio::yield_context &yield) {
	if (!interaction.data) { return; }

	std::visit(
		[this, &interaction, &yield](const auto &data) {
			using T = std::decay_t<decltype(data)>;

			if constexpr (std::is_same_v<T, ekizu::MessageComponentData>) {
				if (data.type != m_component_type) { return; }
				if (m_filter && !m_filter(interaction, data)) { return; }

				if (!m_on_collect_chan.try_send(
						boost::system::error_code{}, interaction, data)) {
					// Fallback to async send if buffer is full
					m_on_collect_chan.async_send(
						boost::system::error_code{}, interaction, data, yield);
				}
			}
		},
		*interaction.data);
}

bool ComponentCollector::is_finished() const {
	return !m_on_collect_chan.is_open();
}

void ComponentCollector::shutdown() {
	m_on_collect_chan.close();
	m_timer.cancel();
}
}  // namespace saber
