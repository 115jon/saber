// component_collector.cpp
#include <saber/component_collector.hpp>

namespace saber {

ComponentCollector::ComponentCollector(
	std::vector<ekizu::ComponentType> component_types,
	std::chrono::steady_clock::duration expiry,
	std::function<bool(const ekizu::Interaction &,
					   const CollectedComponentData &)>
		filter,
	const boost::asio::yield_context &yield)
	: m_component_types{std::move(component_types)},
	  m_filter{std::move(filter)},
	  m_on_collect_chan{yield.get_executor(), 10},
	  m_timer{yield.get_executor(), expiry} {}

ComponentCollector::~ComponentCollector() {
	m_timer.cancel();
	m_on_collect_chan.close();
}

void ComponentCollector::start() {
	auto self = shared_from_this();
	m_timer.async_wait([self](boost::system::error_code ec) {
		if (ec) { return; }
		self->shutdown();
	});
}

Result<
	std::pair<ekizu::Interaction, ComponentCollector::CollectedComponentData>>
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

			// Only collect component-related interaction payloads.
			if constexpr (std::is_same_v<T, ekizu::MessageComponentData>) {
				// Cheap pre-filter: avoid invoking user filter for unrelated
				// component types.
				if (std::find(m_component_types.begin(),
							  m_component_types.end(), data.type) ==
					m_component_types.end()) {
					return;
				}

				CollectedComponentData v = data;
				if (m_filter && !m_filter(interaction, v)) { return; }

				if (!m_on_collect_chan.try_send(boost::system::error_code{},
												interaction, std::move(v))) {
					// Fallback to async send if buffer is full
					m_on_collect_chan.async_send(
						boost::system::error_code{}, interaction, std::move(v),
						yield);
				}
			} else if constexpr (std::is_same_v<T, ekizu::ModalSubmitData>) {
				CollectedComponentData v = data;
				if (m_filter && !m_filter(interaction, v)) { return; }

				if (!m_on_collect_chan.try_send(boost::system::error_code{},
												interaction, std::move(v))) {
					// Fallback to async send if buffer is full
					m_on_collect_chan.async_send(
						boost::system::error_code{}, interaction, std::move(v),
						yield);
				}
			} else {
				// Intentionally ignore ApplicationCommandData and any other
				// InteractionData variants.
				return;
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
