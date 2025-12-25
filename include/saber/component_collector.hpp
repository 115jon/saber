#ifndef SABER_COMPONENT_COLLECTOR_HPP
#define SABER_COMPONENT_COLLECTOR_HPP

#include <saber/export.h>

#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/steady_timer.hpp>
#include <ekizu/interaction.hpp>
#include <saber/result.hpp>

namespace saber {
struct ComponentCollector : std::enable_shared_from_this<ComponentCollector> {
	SABER_EXPORT ComponentCollector(
		ekizu::ComponentType component_type,
		std::chrono::steady_clock::duration expiry,
		std::function<bool(const ekizu::Interaction &,
						   const ekizu::MessageComponentData &)>
			filter,
		const boost::asio::yield_context &yield);

	ComponentCollector(ComponentCollector &&) noexcept = default;
	ComponentCollector &operator=(ComponentCollector &&) noexcept = default;
	ComponentCollector(const ComponentCollector &) = delete;
	ComponentCollector &operator=(const ComponentCollector &) = delete;

	SABER_EXPORT ~ComponentCollector();

	/**
	 * @brief Waits for the next interaction.
	 * @return A pair containing the Interaction and the specific component
	 * data.
	 */
	SABER_EXPORT
	Result<std::pair<ekizu::Interaction, ekizu::MessageComponentData>>
	async_receive(const boost::asio::yield_context &yield);

	SABER_EXPORT void async_send(const ekizu::Interaction &interaction,
								 const boost::asio::yield_context &yield);

	/**
	 * @brief Checks if the collector has expired or finished.
	 */
	[[nodiscard]] bool is_finished() const;

	void shutdown();

   private:
	ekizu::ComponentType m_component_type;
	std::function<bool(
		const ekizu::Interaction &, const ekizu::MessageComponentData &)>
		m_filter;

	boost::asio::experimental::channel<void(
		boost::system::error_code, ekizu::Interaction,
		ekizu::MessageComponentData)>
		m_on_collect_chan;

	boost::asio::steady_timer m_timer;
};
}  // namespace saber

#endif	// SABER_COMPONENT_COLLECTOR_HPP
