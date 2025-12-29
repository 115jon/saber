#ifndef SABER_COMPONENT_COLLECTOR_HPP
#define SABER_COMPONENT_COLLECTOR_HPP

#include <saber/export.h>

#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <ekizu/interaction.hpp>
#include <ekizu/message_component.hpp>
#include <ekizu/snowflake.hpp>
#include <memory>
#include <saber/result.hpp>
#include <variant>
#include <vector>

namespace saber {

// The internal "engine" that handles mixed types.
// Saber stores this in its list.
struct ComponentCollector : std::enable_shared_from_this<ComponentCollector> {
	using CollectedComponentData =
		std::variant<ekizu::MessageComponentData, ekizu::ModalSubmitData>;

	SABER_EXPORT ComponentCollector(
		std::vector<ekizu::ComponentType> component_types,
		std::chrono::steady_clock::duration expiry,
		std::function<bool(const ekizu::Interaction &,
						   const CollectedComponentData &)>
			filter,
		const boost::asio::yield_context &yield);

	ComponentCollector(ComponentCollector &&) noexcept = default;
	ComponentCollector &operator=(ComponentCollector &&) noexcept = default;
	ComponentCollector(const ComponentCollector &) = delete;
	ComponentCollector &operator=(const ComponentCollector &) = delete;
	SABER_EXPORT ~ComponentCollector();

	/**
	 * @brief Waits for the next interaction (Variant type).
	 */
	SABER_EXPORT Result<std::pair<ekizu::Interaction, CollectedComponentData>>
	async_receive(const boost::asio::yield_context &yield);

	SABER_EXPORT void async_send(const ekizu::Interaction &interaction,
								 const boost::asio::yield_context &yield);
	SABER_EXPORT void start();
	[[nodiscard]] bool is_finished() const;
	void shutdown();

   private:
	std::vector<ekizu::ComponentType> m_component_types;
	std::function<bool(
		const ekizu::Interaction &, const CollectedComponentData &)>
		m_filter;
	boost::asio::experimental::channel<void(
		boost::system::error_code, ekizu::Interaction, CollectedComponentData)>
		m_on_collect_chan;
	boost::asio::steady_timer m_timer;
};

// ============================================================================
// Typed Wrapper
// ============================================================================
// This is what the user interacts with. It guarantees the type T.
template <typename T>
class InteractionCollector {
   public:
	explicit InteractionCollector(std::shared_ptr<ComponentCollector> collector)
		: m_collector(std::move(collector)) {}

	// The key fix: Returns the specific type T, not a variant.
	Result<std::pair<ekizu::Interaction, T>> async_receive(
		const boost::asio::yield_context &yield) {
		auto res = m_collector->async_receive(yield);
		if (!res) { return res.error(); }

		auto &pair = res.value();
		// SAFE: The factory filter guarantees the variant holds T.
		return std::make_pair(
			std::move(pair.first), std::move(std::get<T>(pair.second)));
	}

	void start() { m_collector->start(); }
	void shutdown() { m_collector->shutdown(); }
	[[nodiscard]] bool is_finished() const {
		return m_collector->is_finished();
	}

   private:
	std::shared_ptr<ComponentCollector> m_collector;
};

}  // namespace saber

#endif	// SABER_COMPONENT_COLLECTOR_HPP
