#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <boost/asio/detached.hpp>
#include <saber/saber.hpp>
#include <unordered_set>

namespace {
template <typename... Func>
struct overload : Func... {
	using Func::operator()...;
};

template <typename... Func>
overload(Func...) -> overload<Func...>;
}  // namespace

namespace saber {
Saber::Saber(boost::asio::io_context &ctx, Config config)
	: m_commands{*this},
	  m_http{ctx.get_executor(), config.token},
	  m_shard{ctx.get_executor(), ekizu::ShardId::ONE, config.token,
			  ekizu::Intents::AllIntents},
	  m_config{std::move(config)},
	  m_player{[this](ekizu::Snowflake guild_id, ekizu::Snowflake channel_id,
					  const boost::asio::yield_context &yield) {
		  return join_voice_channel(guild_id, channel_id, yield);
	  }} {
	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	console_sink->set_pattern("%^%Y-%m-%d %H:%M:%S.%e [%L] [th#%t]%$ : %v");

	auto file_sink =
		std::make_shared<spdlog::sinks::basic_file_sink_mt>("saber.log");
	file_sink->set_level(spdlog::level::trace);

#ifdef _DEBUG
	auto level = spdlog::level::debug;
#else
	auto level = spdlog::level::info;
#endif

	if (const auto *log_level_e = std::getenv("SABER_LOG_LEVEL");
		log_level_e != nullptr) {
		std::string_view log_level{log_level_e};

		if (log_level == "info") {
			level = spdlog::level::info;
		} else if (log_level == "warn") {
			level = spdlog::level::warn;
		} else if (log_level == "error") {
			level = spdlog::level::err;
		} else if (log_level == "debug") {
			level = spdlog::level::debug;
		} else if (log_level == "trace") {
			level = spdlog::level::trace;
		} else if (log_level == "critical") {
			level = spdlog::level::critical;
		} else if (log_level == "off") {
			level = spdlog::level::off;
		}
	}

	console_sink->set_level(level);

	auto log_fn = [this](const ekizu::Log &log) {
		switch (log.level) {
			case ekizu::LogLevel::Info: m_logger->info(log.message); break;
			case ekizu::LogLevel::Warn: m_logger->warn(log.message); break;
			case ekizu::LogLevel::Error: m_logger->error(log.message); break;
			case ekizu::LogLevel::Debug: m_logger->debug(log.message); break;
			case ekizu::LogLevel::Trace: m_logger->trace(log.message); break;
			case ekizu::LogLevel::Critical:
				m_logger->critical(log.message);
				break;
		}
	};

	m_player.attach_logger(log_fn);
	m_shard.attach_logger(log_fn);

	m_logger = spdlog::logger{"saber", {console_sink, file_sink}};
	m_logger->set_level(level);
}

Result<boost::optional<ekizu::Guild &>> Saber::get_guild(
	ekizu::Snowflake guild_id, const boost::asio::yield_context &yield) {
	if (m_guild_cache.has(guild_id)) {
		return outcome::success(m_guild_cache[guild_id]);
	}

	SABER_TRY(auto guild, m_http.get_guild(guild_id).send(yield));
	m_guild_cache.put(guild_id, std::move(guild));
	log<ekizu::LogLevel::Info>("Fetched and cached guild: {}", guild_id);
	return outcome::success(m_guild_cache[guild_id]);
}

Result<ekizu::Permissions> Saber::get_guild_permissions(
	ekizu::Snowflake guild_id, ekizu::Snowflake user_id) {
	ekizu::Permissions ret{};
	auto guild = m_guild_cache[guild_id];
	if (!guild) { return ret; }

	auto members = m_guild_member_cache[guild_id];
	if (!members) { return ret; }

	auto member = members->get(user_id);
	if (!member) { return ret; }

	std::unordered_set<ekizu::Snowflake> member_roles;

	for (const auto &role : member->roles) { member_roles.insert(role); }

	for (const auto &role : guild->roles) {
		if (member_roles.find(role.id) != member_roles.end()) {
			ret = ret | static_cast<ekizu::Permissions>(role.permissions);
		}
	}

	return ret;
}

Result<ekizu::VoiceConnectionConfig *> Saber::join_voice_channel(
	ekizu::Snowflake guild_id, ekizu::Snowflake channel_id,
	const boost::asio::yield_context &yield) {
	m_voice_ready_channels.emplace(guild_id, yield.get_executor());

	auto channel = m_voice_ready_channels[guild_id];
	SABER_TRY(m_shard.join_voice_channel(guild_id, channel_id, yield));
	channel->async_receive(yield);
	m_voice_ready_channels.remove(guild_id);
	return &m_voice_configs[guild_id];
}

Result<> Saber::leave_voice_channel(ekizu::Snowflake guild_id,
									const boost::asio::yield_context &yield) {
	return m_shard.leave_voice_channel(guild_id, yield);
}

std::shared_ptr<ComponentCollector> Saber::create_message_component_collector(
	ekizu::Snowflake channel_id,
	std::function<bool(const ekizu::Interaction &,
					   const ekizu::MessageComponentData &)>
		filter,
	ekizu::ComponentType component_type,
	std::chrono::steady_clock::duration expiry,
	const boost::asio::yield_context &yield) {
	auto &list = m_collectors[channel_id];

	// LAZY CLEANUP: Remove collectors that have finished (timer expired or
	// closed) This prevents the vector from growing infinitely.
	list.erase(
		std::remove_if(list.begin(), list.end(),
					   [](const std::shared_ptr<ComponentCollector> &ptr) {
						   return ptr->is_finished();
					   }),
		list.end());

	// 1. Allocate on Heap
	// Using shared_ptr ensures the ComponentCollector object stays at a
	// fixed memory address even if the vector 'list' resizes.
	auto collector = std::make_shared<ComponentCollector>(
		component_type, expiry, std::move(filter), yield);

	// 2. Add to vector
	list.push_back(collector);

	return collector;
}

void Saber::run(const boost::asio::yield_context &yield) {
	m_commands.load_all(yield);

	boost::system::error_code ec;
	while (m_running) {
		auto res = m_shard.next_event(yield[ec]);

		if (ec == boost::asio::error::operation_aborted) {
			log<ekizu::LogLevel::Info>("Run loop cancelled by stop signal");
			break;
		}

		if (!res) {
			if (m_running && res.error().failed()) {
				fmt::println(
					"Failed to get next event: {}", res.error().message());
				return;
			}
			continue;
		}

		boost::asio::spawn(
			m_shard.get_executor(),
			[this, ev = std::move(res.value())](const auto &y) {
				handle_event(ev, y);

				bool expected = false;
				if (m_restore_started.compare_exchange_strong(expected, true)) {
					boost::asio::spawn(
						y.get_executor(),
						[this](const auto &y) {
							auto r = m_player.restore_all(y);
							if (!r) {
								log<ekizu::LogLevel::Error>(
									"Player restore_all failed: {}",
									r.error().message());
							}
						},
						boost::asio::detached);
				}
			},
			boost::asio::detached);
	}

	// Only try to close gracefully if we're not in forced shutdown
	if (m_running) {
		if (auto res = m_shard.close(ekizu::CloseFrame::NORMAL, yield); !res) {
			log<ekizu::LogLevel::Warn>(
				"Gateway close error: {}", res.error().message());
		}
	}

	log<ekizu::LogLevel::Info>("Bot run loop exited");
}

Result<> Saber::stop(const boost::asio::yield_context &yield) {
	log<ekizu::LogLevel::Info>("Initiating shutdown sequence...");

	// Step 1: Set running flag to false
	m_running = false;

	// 1) Try to close gateway gracefully and WAIT for completion.
	// This avoids returning while IOCP operations are still active.
	{
		boost::system::error_code ec;
		auto r = m_shard.close(ekizu::CloseFrame::NORMAL, yield[ec]);
		if (ec) {
			log<ekizu::LogLevel::Warn>(
				"Shard close aborted/error: {}", ec.message());
		} else if (!r) {
			log<ekizu::LogLevel::Warn>(
				"Shard close returned error: {}", r.error().message());
		}
	}

	m_http.shutdown();

	// Step 3: Shutdown collectors (cancels pending waits)
	log<ekizu::LogLevel::Debug>(
		"Shutting down {} collector groups", m_collectors.size());
	for (auto &[channel_id, collectors] : m_collectors) {
		for (auto &collector : collectors) {
			if (collector) { collector->shutdown(); }
		}
	}
	m_collectors.clear();

	// Step 4: Shutdown player (most critical - involves voice connections)
	log<ekizu::LogLevel::Debug>("Shutting down player");
	m_player.shutdown();

	log<ekizu::LogLevel::Info>("Shutdown complete");
	return outcome::success();
}

void Saber::handle_event(ekizu::Event ev,
						 const boost::asio::yield_context &yield) {
	std::visit(
		overload{
			[this](const ekizu::GuildCreate &g) {
				m_guild_cache.put(g.guild.id, g.guild);
				m_guild_member_cache.put(
					g.guild.id,
					ekizu::SnowflakeLruCache<ekizu::GuildMember>{500});

				for (const auto &member : g.guild.members) {
					m_guild_member_cache[g.guild.id]->put(
						member.user.id, member);
				}

				ekizu::SnowflakeLruCache<ekizu::VoiceState> lru{500};

				for (const auto &voice_state : g.guild.voice_states) {
					lru.put(voice_state.user_id, voice_state);
				}

				if (!m_voice_state_cache.has(g.guild.id)) {
					m_voice_state_cache.put(g.guild.id, std::move(lru));
				}
			},
			[this, &yield](const ekizu::InteractionCreate &payload) {
				const auto &interaction = payload.interaction;
				if (!interaction.channel_id) { return; }

				std::vector<std::shared_ptr<ComponentCollector>>
					local_collectors;

				// 1. Quick synchronous copy to avoid holding the map/vector
				// lock during yield
				if (auto it = m_collectors.find(*interaction.channel_id);
					it != m_collectors.end()) {
					local_collectors = it->second;	// Copy shared_ptrs (cheap)
				}

				// 2. Iterate the LOCAL copy.
				// Even if m_collectors rehashes or vector resizes,
				// 'local_collectors' is stable.
				for (const auto &collector : local_collectors) {
					if (!collector->is_finished()) {
						collector->async_send(interaction, yield);
					}
				}
			},
			[this](const ekizu::VoiceStateUpdate &v) {
				if (v.voice_state.guild_id) {
					m_voice_state_cache[*v.voice_state.guild_id]->put(
						v.voice_state.user_id, v.voice_state);
					m_voice_configs[*v.voice_state.guild_id].state =
						v.voice_state;
				}
			},
			[this](const ekizu::VoiceServerUpdate &v) {
				m_voice_configs[v.guild_id].endpoint = v.endpoint;
				m_voice_configs[v.guild_id].token = v.token;

				if (m_voice_ready_channels.has(v.guild_id)) {
					m_voice_ready_channels[v.guild_id]->async_send(
						boost::system::error_code{},
						&m_voice_configs[v.guild_id],
						[](const boost::system::error_code &) {});
				}
			},
			[this](const ekizu::Ready &r) {
				m_user = r.user;
				log<ekizu::LogLevel::Info>("Logged in as {}", m_user.username);
				m_bot_id = m_user.id;
				log<ekizu::LogLevel::Info>("API version: {}", r.v);
				log<ekizu::LogLevel::Info>("Guilds: {}", r.guilds.size());
			},
			[this, &yield](const ekizu::MessageCreate &m) {
				m_message_cache.put(m.message.id, m.message);
				m_user_cache.put(m.message.author.id, m.message.author);

				if (const auto res =
						m_commands.process_commands(m.message, yield);
					!res && res.error().failed()) {
					log<ekizu::LogLevel::Warn>(
						"Failed to process command: {}", res.error().message());
				};
			},
			[this](ekizu::Resumed) { log<ekizu::LogLevel::Info>("Resumed"); },
			[this](const auto &e) {
				log<ekizu::LogLevel::Warn>(
					"Unhandled event: {}", nlohmann::json{e}.dump());
			}},
		ev);
}
}  // namespace saber