#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <saber/saber.hpp>
#include <saber/util.hpp>
#include <unordered_set>

namespace {
template <typename... Func>
struct overload : Func... {
	using Func::operator()...;
};

template <typename... Func>
overload(Func...) -> overload<Func...>;

using namespace saber;

std::shared_ptr<ComponentCollector> create_collector_impl(
	std::vector<std::shared_ptr<ComponentCollector>> &list,
	std::vector<ekizu::ComponentType> component_types,
	std::chrono::steady_clock::duration expiry,
	std::function<bool(const ekizu::Interaction &,
					   const ComponentCollector::CollectedComponentData &)>
		filter,
	const boost::asio::yield_context &yield) {
	// LAZY CLEANUP: Remove collectors that have finished
	list.erase(
		std::remove_if(list.begin(), list.end(),
					   [](const std::shared_ptr<ComponentCollector> &ptr) {
						   return ptr->is_finished();
					   }),
		list.end());

	// 1. Allocate on Heap
	auto collector = std::make_shared<ComponentCollector>(
		std::move(component_types), expiry, std::move(filter), yield);
	collector->start();

	// 2. Add to vector
	list.push_back(collector);

	return collector;
}
}  // namespace

namespace saber {
Saber::Saber(boost::asio::io_context &ctx, Config config)
	: m_commands{*this},
	  m_http{ctx.get_executor(), config.token},
	  m_shard{ctx.get_executor(), ekizu::ShardId::ONE, config.token,
			  ekizu::Intents::AllIntents},
	  m_config{std::move(config)},
	  m_player{ctx.get_executor(),
			   [this](ekizu::Snowflake guild_id, ekizu::Snowflake channel_id,
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

Result<ekizu::VoiceConnectionConfig> Saber::join_voice_channel(
	ekizu::Snowflake guild_id, ekizu::Snowflake channel_id,
	const boost::asio::yield_context &yield) {
	m_voice_ready_channels.emplace(guild_id, yield.get_executor());

	auto channel = m_voice_ready_channels[guild_id];
	SABER_TRY(m_shard.join_voice_channel(guild_id, channel_id, yield));
	auto config = channel->async_receive(yield);
	m_voice_ready_channels.remove(guild_id);
	return config;
}

Result<> Saber::leave_voice_channel(ekizu::Snowflake guild_id,
									const boost::asio::yield_context &yield) {
	return m_shard.leave_voice_channel(guild_id, yield);
}

InteractionCollector<ekizu::MessageComponentData>
Saber::create_message_component_collector(
	ekizu::Snowflake channel_id,
	std::function<bool(const ekizu::Interaction &,
					   const ekizu::MessageComponentData &)>
		filter,
	std::vector<ekizu::ComponentType> component_types,
	std::chrono::steady_clock::duration expiry,
	const boost::asio::yield_context &yield) {
	auto safe_filter =
		[fn = std::move(filter)](
			const ekizu::Interaction &interaction,
			const ComponentCollector::CollectedComponentData &data) -> bool {
		if (const auto *val = std::get_if<ekizu::MessageComponentData>(&data)) {
			return fn(interaction, *val);
		}
		return false;
	};

	auto internal = create_collector_impl(
		m_collectors[channel_id], std::move(component_types), expiry,
		std::move(safe_filter), yield);

	return InteractionCollector<ekizu::MessageComponentData>(internal);
}

InteractionCollector<ekizu::ModalSubmitData>
Saber::create_modal_submit_collector(
	ekizu::Snowflake channel_id,
	std::function<bool(const ekizu::Interaction &,
					   const ekizu::ModalSubmitData &)>
		filter,
	std::chrono::steady_clock::duration expiry,
	const boost::asio::yield_context &yield) {
	auto safe_filter =
		[fn = std::move(filter)](
			const ekizu::Interaction &interaction,
			const ComponentCollector::CollectedComponentData &data) -> bool {
		if (const auto *val = std::get_if<ekizu::ModalSubmitData>(&data)) {
			return fn(interaction, *val);
		}
		return false;
	};

	auto internal = create_collector_impl(
		m_collectors[channel_id], {}, expiry, std::move(safe_filter), yield);

	return InteractionCollector<ekizu::ModalSubmitData>(internal);
}

void Saber::run(const boost::asio::yield_context &yield) {
	m_commands.load_all(yield);

	// Playback -> "Now Playing" message bridge (single listener).
	m_player.attach_playback_event_handler(
		[this, ex = yield.get_executor()](const PlaybackEvent &ev) {
			boost::asio::spawn(
				ex, [this, ev](const auto &y) { handle_playback_event(ev, y); },
				boost::asio::detached);
		});

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

				if (const auto res =
						m_commands.process_commands(interaction, yield);
					!res && res.error().failed()) {
					log<ekizu::LogLevel::Warn>(
						"Failed to process interaction: {{message={}, "
						"context={}}}",
						res.error().message(), ekizu::last_error_context());
				};
			},
			[this](const ekizu::VoiceStateUpdate &v) {
				if (!v.voice_state.guild_id) { return; }

				auto guild_id = *v.voice_state.guild_id;
				if (!m_voice_state_cache.has(guild_id)) {
					m_voice_state_cache.put(
						guild_id,
						ekizu::SnowflakeLruCache<ekizu::VoiceState>{500});
				}

				if (v.voice_state.channel_id) {
					m_voice_state_cache[guild_id]->put(
						v.voice_state.user_id, v.voice_state);
				} else {
					m_voice_state_cache[guild_id]->remove(
						v.voice_state.user_id);
				}

				// Only the bot's voice state affects the voice connection
				// config / player
				if (v.voice_state.user_id == m_bot_id) {
					if (v.voice_state.channel_id) {
						m_voice_configs[guild_id].state = v.voice_state;
					} else {
						// Unblock any pending join and discard stale config
						if (m_voice_ready_channels.has(guild_id)) {
							m_voice_ready_channels[guild_id]->async_send(
								boost::asio::error::operation_aborted,
								ekizu::VoiceConnectionConfig{},
								[](const boost::system::error_code &) {});
						}
						m_voice_configs.erase(guild_id);
					}

					if (auto res = m_player.on_voice_state_update(
							guild_id, v.voice_state);
						!res) {
						log<ekizu::LogLevel::Error>(
							"Failed to update player state: {}",
							res.error().message());
					}
				}
			},
			[this](const ekizu::VoiceServerUpdate &v) {
				m_voice_configs[v.guild_id].endpoint = v.endpoint;
				m_voice_configs[v.guild_id].token = v.token;

				if (m_voice_ready_channels.has(v.guild_id)) {
					m_voice_ready_channels[v.guild_id]->async_send(
						boost::system::error_code{},
						m_voice_configs[v.guild_id],
						[](const boost::system::error_code &) {});
				}

				// Rebind transport without resetting playback.
				if (auto res = m_player.on_voice_server_update(
						v.guild_id, m_voice_configs[v.guild_id]);
					!res) {
					log<ekizu::LogLevel::Error>(
						"Failed to update player voice server: {}",
						res.error().message());
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
						"Failed to process command: {{message={}, context={}}}",
						res.error().message(), ekizu::last_error_context());
				};
			},
			[this](ekizu::Resumed) { log<ekizu::LogLevel::Info>("Resumed"); },
			[](const auto & /*e*/) {
				// log<ekizu::LogLevel::Warn>(
				// 	"Unhandled event: {}", nlohmann::json{e}.dump());
			}},
		ev);
}

void Saber::handle_playback_event(const PlaybackEvent &ev,
								  const boost::asio::yield_context &yield) {
	const auto guild_id = ev.guild_id;

	auto is_paused_fn = [this, guild_id] {
		auto res = m_player.is_paused(guild_id);
		return res && res.value();
	};

	auto chan_it = m_now_playing_channels.find(guild_id);
	if (chan_it == m_now_playing_channels.end()) { return; }
	const auto channel_id = chan_it->second;

	auto create_components_v2 =
		[&](std::string header_text, std::string body_text,
			std::optional<std::string> image_url, const Track *track)
		-> std::pair<ekizu::MessageFlags,
					 std::vector<ekizu::MessageComponent>> {
		const bool is_paused = is_paused_fn();

		// Header line (usually contains a markdown link)
		ekizu::TextDisplay header;
		header.content = std::move(header_text);

		ekizu::TextDisplay body;
		ekizu::TextDisplay meta;

		std::optional<ekizu::Snowflake> voice_channel_id;
		if (auto vc = m_player.voice_channel_id(guild_id); vc) {
			voice_channel_id = vc.value();
		}

		if (track) {
			const auto &md = track->metadata;
			const auto url = md.webpage_url.empty() ? std::string{"about:blank"}
													: md.webpage_url;
			body.content = fmt::format(
				"### [{}]({}) - `{}`",
				md.title.empty() ? std::string{"(unknown)"} : md.title, url,
				util::format_duration(md.duration_seconds));

			if (voice_channel_id) {
				meta.content =
					fmt::format("> Requested by <@{}>\n> Connected in <#{}>",
								track->requester_id, *voice_channel_id);
			} else {
				meta.content =
					fmt::format("> Requested by <@{}>", track->requester_id);
			}
		} else {
			body.content = std::move(body_text);
		}

		ekizu::Section section;
		if (track) {
			if (!body_text.empty()) {
				ekizu::TextDisplay extra;
				extra.content = std::move(body_text);
				section.components = {header, body, extra, meta};
			} else {
				section.components = {header, body, meta};
			}
		} else {
			section.components = {header, body};
		}

		// Add media accessory if we have album art
		if (image_url && !image_url->empty()) {
			ekizu::Thumbnail media;
			media.media.url = *image_url;
			section.accessory = media;
		}

		// Create control buttons
		auto play_button =
			ekizu::ButtonBuilder()
				.custom_id("np_pause")
				.style(is_paused ? ekizu::ButtonStyle::Success
								 : ekizu::ButtonStyle::Secondary)
				.label(is_paused ? "Resume" : "Pause")
				.emoji(is_paused
						   ? ekizu::EmojiBuilder()
								 .id(ekizu::Snowflake{1051612861769711706ULL})
								 .name("play")
								 .build()
						   : ekizu::EmojiBuilder().name("⏸️").build())
				.build();

		auto skip_button =
			ekizu::ButtonBuilder()
				.custom_id("np_skip")
				.style(ekizu::ButtonStyle::Secondary)
				.label("Skip")
				.emoji(ekizu::EmojiBuilder()
						   .id(ekizu::Snowflake{1051614199769469008ULL})
						   .name("skip_next")
						   .build())
				.build();

		auto stop_button =
			ekizu::ButtonBuilder()
				.custom_id("np_stop")
				.style(ekizu::ButtonStyle::Secondary)
				.label("Stop")
				.emoji(ekizu::EmojiBuilder()
						   .id(ekizu::Snowflake{1051615255605805127ULL})
						   .name("stop")
						   .build())
				.build();

		auto autoplay_button =
			ekizu::ButtonBuilder()
				.custom_id("np_auto_play")
				.style(ekizu::ButtonStyle::Secondary)
				.label("AutoPlay")
				.emoji(ekizu::EmojiBuilder()
						   .id(ekizu::Snowflake{1323515075839004742ULL})
						   .name("autoplay")
						   .build())
				.build();

		auto like_button =
			ekizu::ButtonBuilder()
				.custom_id(track ? fmt::format("np_like={}",
											   track->metadata.webpage_url)
								 : "np_like=")
				.style(ekizu::ButtonStyle::Secondary)
				.label("Like")
				.emoji(ekizu::EmojiBuilder().name("🤍").build())
				.build();

		ekizu::Container container;
		container.accent_color = 0x947CEA;
		container.spoiler = false;
		container.components = {section};

		auto button_row =
			ekizu::ActionRowBuilder()
				.components({play_button, skip_button, stop_button,
							 autoplay_button, like_button})
				.build();

		return {ekizu::MessageFlags::IsComponentsV2,
				std::vector<ekizu::MessageComponent>{container, button_row}};
	};

	auto start_collector = [&](ekizu::Snowflake msg_id) {
		// Stop existing collector if any
		if (m_now_playing_collectors.contains(guild_id)) {
			m_now_playing_collectors[guild_id]->shutdown();
		}

		auto filter = [msg_id](const ekizu::Interaction &interaction,
							   const ekizu::MessageComponentData &data) {
			if (!interaction.message) { return false; }
			if (interaction.message->id != msg_id) { return false; }

			return data.custom_id == "np_pause" ||
				   data.custom_id == "np_skip" || data.custom_id == "np_stop" ||
				   data.custom_id == "np_auto_play" ||
				   boost::starts_with(data.custom_id, "np_like=");
		};

		auto collector = create_message_component_collector(
			channel_id, filter, {ekizu::ComponentType::Button},
			std::chrono::hours(24), yield);	 // Long timeout

		m_now_playing_collectors[guild_id] = collector.get_internal();

		// Spawn collector loop
		boost::asio::spawn(
			yield.get_executor(),
			[this, guild_id, channel_id, collector = std::move(collector),
			 is_paused_fn](const auto &y) mutable {
				auto ack_interaction = [&](const ekizu::Interaction &i) {
					boost::system::error_code ec;
					(void)m_http.interaction(i.application_id)
						.create_response(
							i.id, i.token,
							ekizu::InteractionResponseBuilder()
								.type(ekizu::InteractionResponseType::
										  DeferredUpdateMessage)
								.build())
						.send(y[ec]);
				};

				auto send_followup = [&](std::string content) {
					boost::system::error_code ec;
					(void)m_http.create_message(channel_id)
						.content(std::move(content))
						.send(y[ec]);
				};

				bool is_paused = is_paused_fn();

				auto update_buttons = [this, &guild_id, &y, &is_paused]() {
					auto msg_it = m_now_playing_messages.find(guild_id);
					if (msg_it == m_now_playing_messages.end()) { return; }

					auto &msg = msg_it->second;

					// Find the button row and update the pause button
					for (auto &top : msg.components) {
						auto *row = std::get_if<ekizu::ActionRow>(&top);
						if (!row) { continue; }

						for (auto &comp : row->components) {
							auto *btn = std::get_if<ekizu::Button>(&comp);
							if (!btn || btn->custom_id != "np_pause") {
								continue;
							}

							btn->label = is_paused ? "Resume" : "Pause";
							btn->style =
								is_paused ? ekizu::ButtonStyle::Success
										  : ekizu::ButtonStyle::Secondary;
							btn->emoji =
								is_paused
									? ekizu::EmojiBuilder()
										  .id(ekizu::Snowflake{
											  1051612861769711706ULL})
										  .name("play")
										  .build()
									: ekizu::EmojiBuilder().name("⏸️").build();
						}
					}

					(void)m_http
						.edit_message(
							msg_it->second.channel_id, msg_it->second.id)
						.flags(ekizu::MessageFlags::IsComponentsV2)
						.components(msg.components)
						.send(y);
				};

				while (true) {
					auto res = collector.async_receive(y);
					if (!res) { break; }

					auto &[i, data] = res.value();
					ack_interaction(i);

					if (data.custom_id == "np_pause") {
						is_paused = is_paused_fn();
						if (is_paused) {
							auto r = m_player.resume(guild_id);
							if (!r) {
								send_followup(fmt::format(
									"Resume failed: {}", r.error().message()));
							} else {
								is_paused = is_paused_fn();
								update_buttons();
							}
						} else {
							auto r = m_player.pause(guild_id);
							if (!r) {
								send_followup(fmt::format(
									"Pause failed: {}", r.error().message()));
							} else {
								is_paused = is_paused_fn();
								update_buttons();
							}
						}
					} else if (data.custom_id == "np_skip") {
						auto r = m_player.skip(guild_id);
						if (!r) {
							send_followup(fmt::format(
								"Skip failed: {}", r.error().message()));
						} else if (!r.value()) {
							send_followup("There is no next track.");
						}
					} else if (data.custom_id == "np_stop") {
						auto r = m_player.disconnect(guild_id, true);
						if (!r) {
							send_followup(fmt::format(
								"Stop failed: {}", r.error().message()));
						} else {
							// Also leave the voice channel
							boost::system::error_code ec;
							(void)leave_voice_channel(guild_id, y[ec]);
						}
					} else if (data.custom_id == "np_auto_play") {
						// TODO: Implement autoplay toggle
						send_followup("AutoPlay is not yet implemented.");
					} else if (boost::starts_with(data.custom_id, "np_like=")) {
						// TODO: Implement like/save track
						send_followup("Like feature is not yet implemented.");
					}
				}

				// Cleanup collector from map when done
				m_now_playing_collectors.erase(guild_id);
			},
			boost::asio::detached);
	};

	auto upsert_now_playing = [&](std::string header, std::string body,
								  std::optional<std::string> image,
								  const Track *track) {
		auto msg_it = m_now_playing_messages.find(guild_id);

		auto [flags, components] = create_components_v2(
			std::move(header), std::move(body), std::move(image), track);

		if (msg_it != m_now_playing_messages.end() &&
			msg_it->second.channel_id == channel_id) {
			auto edit_res = m_http.edit_message(channel_id, msg_it->second.id)
								.flags(flags)
								.components(components)
								.send(yield);
			if (edit_res) {
				// Restart collector for existing message
				start_collector(msg_it->second.id);
				return;
			}
		}

		auto create_res =
			m_http.create_message(channel_id)
				.flags(flags)
				.components(components)
				.send(yield);

		if (!create_res) {
			log<ekizu::LogLevel::Warn>(
				"Now playing create_message failed: {{error={}, context={}}}",
				create_res.error().message(), ekizu::last_error_context());
			return;
		}

		m_now_playing_messages[guild_id] = create_res.value();

		// Start collector for new message
		start_collector(create_res.value().id);
	};

	auto edit_now_playing_if_exists = [&](std::string header,
										  std::string body) {
		auto msg_it = m_now_playing_messages.find(guild_id);
		if (msg_it == m_now_playing_messages.end()) { return; }

		auto [flags, components] = create_components_v2(
			std::move(header), std::move(body), std::nullopt, nullptr);

		(void)m_http.edit_message(msg_it->second.channel_id, msg_it->second.id)
			.flags(flags)
			.components(components)
			.send(yield);
	};

	switch (ev.type) {
		case PlaybackEventType::TrackStarted: {
			if (!ev.track) { return; }
			const auto &t = *ev.track;

			upsert_now_playing("**Now playing**", "",
							   t.metadata.thumbnail_url.empty()
								   ? std::nullopt
								   : std::optional{t.metadata.thumbnail_url},
							   &t);
			return;
		}
		case PlaybackEventType::TrackError: {
			if (!ev.track) { return; }
			const auto &t = *ev.track;

			upsert_now_playing(
				"**⚠️ Track error**", fmt::format("`{}`", ev.error.message()),
				t.metadata.thumbnail_url.empty()
					? std::nullopt
					: std::optional{t.metadata.thumbnail_url},
				&t);
			return;
		}
		case PlaybackEventType::QueueEnded: {
			edit_now_playing_if_exists(
				"**⏹️ Queue finished**", "Add more with `/play`.");

			// Stop collector when queue ends
			if (m_now_playing_collectors.contains(guild_id)) {
				m_now_playing_collectors[guild_id]->shutdown();
				m_now_playing_collectors.erase(guild_id);
			}
			return;
		}
		case PlaybackEventType::TrackEnqueued:
		case PlaybackEventType::TrackFinished: return;
	}
}

Result<std::vector<ekizu::ApplicationCommand>> Saber::register_slash_commands(
	const boost::asio::yield_context &yield) {
	std::vector<ekizu::ApplicationCommandCreateFields> commands;

	m_commands.get_commands([&](const auto &cmd_map) {
		for (const auto &[name, cmd] : cmd_map) {
			if (!cmd || !cmd->options.enabled) { continue; }
			if (!cmd->options.slash_options) { continue; }

			// Global registration: only register non-guild-only commands
			// Guild-only commands should be registered per-guild instead
			if (cmd->options.guild_only) { continue; }

			ekizu::ApplicationCommandCreateFields fields;
			fields.name = cmd->options.name;
			// Discord requires description to be 1-100 characters
			auto desc = cmd->options.description.empty()
							? "No description"
							: cmd->options.description;
			if (desc.size() > 100) { desc = desc.substr(0, 97) + "..."; }
			fields.description = std::move(desc);
			fields.type = ekizu::ApplicationCommandType::ChatInput;
			fields.options = *cmd->options.slash_options;

			// Allow in guilds, bot DMs, and private channels
			fields.contexts = {ekizu::InteractionContextType::Guild,
							   ekizu::InteractionContextType::BotDm,
							   ekizu::InteractionContextType::PrivateChannel};

			commands.push_back(std::move(fields));
		}
	});

	log<ekizu::LogLevel::Info>(
		"Registering {} global slash commands", commands.size());

	return m_http
		.bulk_overwrite_global_application_commands(
			m_bot_id, std::move(commands))
		.send(yield);
}

Result<std::vector<ekizu::ApplicationCommand>>
Saber::register_guild_slash_commands(ekizu::Snowflake guild_id,
									 const boost::asio::yield_context &yield) {
	std::vector<ekizu::ApplicationCommandCreateFields> commands;

	m_commands.get_commands([&](const auto &cmd_map) {
		for (const auto &[name, cmd] : cmd_map) {
			if (!cmd || !cmd->options.enabled) { continue; }
			if (!cmd->options.slash_options) { continue; }

			// Guild registration: only register guild-only commands
			// Non-guild-only commands are registered globally instead
			if (!cmd->options.guild_only) { continue; }

			ekizu::ApplicationCommandCreateFields fields;
			fields.name = cmd->options.name;
			// Discord requires description to be 1-100 characters
			auto desc = cmd->options.description.empty()
							? "No description"
							: cmd->options.description;
			if (desc.size() > 100) { desc = desc.substr(0, 97) + "..."; }
			fields.description = std::move(desc);
			fields.type = ekizu::ApplicationCommandType::ChatInput;
			fields.options = *cmd->options.slash_options;

			// Guild-only commands only work in guilds
			fields.contexts = {ekizu::InteractionContextType::Guild};

			commands.push_back(std::move(fields));
		}
	});

	log<ekizu::LogLevel::Info>("Registering {} guild slash commands for {}",
							   commands.size(), guild_id);

	return m_http
		.bulk_overwrite_guild_application_commands(
			m_bot_id, guild_id, std::move(commands))
		.send(yield);
}

}  // namespace saber