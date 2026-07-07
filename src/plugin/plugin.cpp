#include "bvh8.h"
#include "map_source.h"
#include "subprocess.h"
#include "vpk.h"
#include "visibility_sampling.h"

#include <ISmmPlugin.h>
#include <eiface.h>
#include <entity2/entitysystem.h>
#include <filesystem.h>
#include <inetchannelinfo.h>
#include <iserver.h>
#include <schemasystem/schemasystem.h>
#include <tier1/convar.h>
#include <tier1/utlstring.h>
#include <tier1/utlvector.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

PLUGIN_GLOBALVARS();

namespace cs2fow
{

constexpr uint32_t k_max_players = 64;
constexpr uint32_t k_max_weapons = 64;
constexpr uint32_t k_max_wearables = 32;
constexpr uint32_t k_max_hidden_player_entities = 1 + 2 + k_max_weapons + k_max_wearables + 1;
constexpr uint32_t k_max_gamedata_offset = 4096;
constexpr uint8_t k_life_alive = 0;
constexpr uint8_t k_team_t = 2;
constexpr uint8_t k_team_ct = 3;
constexpr auto k_auto_bake_timeout = std::chrono::minutes(10);
static_assert(MAX_EDICTS == 16384);

class game_resource_service
{
};

struct schema_offsets
{
	uint32_t is_hltv {};
	uint32_t player_pawn {};
	uint32_t pawn_controller {};
	uint32_t health {};
	uint32_t life_state {};
	uint32_t team {};
	uint32_t body_component {};
	uint32_t scene_node {};
	uint32_t abs_origin {};
	uint32_t abs_velocity {};
	uint32_t view_offset {};
	uint32_t view_x {};
	uint32_t view_y {};
	uint32_t view_z {};
	uint32_t collision {};
	uint32_t mins {};
	uint32_t maxs {};
	uint32_t weapon_services {};
	uint32_t weapons {};
	uint32_t active_weapon {};
	uint32_t last_weapon {};
	uint32_t wearables {};
	uint32_t hostage_services {};
	uint32_t carried_hostage_prop {};
};

struct player_state
{
	bool valid {};
	uint8_t team {};
	vec3 eye;
	vec3 origin;
	vec3 velocity;
	vec3 mins;
	vec3 maxs;
	float rtt_seconds {};
	int pawn_entity {-1};
};

struct snapshot
{
	uint64_t sequence {};
	std::chrono::steady_clock::time_point captured;
	player_state players[k_max_players];
};

struct visibility_result
{
	uint64_t sequence {};
	std::chrono::steady_clock::time_point completed;
	player_state players[k_max_players];
	bool visible[k_max_players][k_max_players] {};
	double worker_ms {};
	uint32_t evaluated_pairs {};
	uint32_t visible_pairs {};
	uint32_t hidden_pairs {};
};

struct worker_stats
{
	double latest_ms {};
	double average_ms {};
	double maximum_ms {};
	uint64_t cycles {};
	uint32_t evaluated_pairs {};
	uint32_t visible_pairs {};
	uint32_t hidden_pairs {};
};

template <typename type>
type &field(void *object, uint32_t offset)
{
	return *reinterpret_cast<type *>(reinterpret_cast<uintptr_t>(object) + offset);
}

uint32_t find_field_recursive(SchemaClassInfoData_t *class_info, const char *name)
{
	if (class_info == nullptr)
	{
		return 0;
	}
	for (int i = 0; i < class_info->m_nFieldCount; ++i)
	{
		if (std::strcmp(class_info->m_pFields[i].m_pszName, name) == 0)
		{
			return class_info->m_pFields[i].m_nSingleInheritanceOffset;
		}
	}
	for (int i = 0; i < class_info->m_nBaseClassCount; ++i)
	{
		if (const uint32_t offset = find_field_recursive(class_info->m_pBaseClasses[i].m_pClass, name); offset != 0)
		{
			return offset;
		}
	}
	return 0;
}

uint32_t resolve_field(ISchemaSystem *schema, const char *class_name, const char *field_name)
{
#if defined(_WIN32)
	CSchemaSystemTypeScope *scope = schema->FindTypeScopeForModule("server.dll");
#else
	CSchemaSystemTypeScope *scope = schema->FindTypeScopeForModule("libserver.so");
#endif
	if (scope == nullptr)
	{
		return 0;
	}
	return find_field_recursive(scope->FindDeclaredClass(class_name).Get(), field_name);
}

vec3 to_vec3(const Vector &value)
{
	return {value.x, value.y, value.z};
}

int entity_index(CEntityInstance *entity)
{
	return entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->m_EHandle.GetEntryIndex() : -1;
}

bool valid_entity_index(int index)
{
	return index > 0 && index < MAX_EDICTS;
}

class visibility_worker
{
public:
	void start(const bvh8_data *data)
	{
		stop();
		data_ = data;
		stopping_ = false;
		for (auto &observer : cached_packets_)
		{
			for (auto &target : observer)
			{
				target.fill(k_invalid_ref);
			}
		}
		for (auto &observer : revealed_until_)
		{
			observer.fill(std::chrono::steady_clock::time_point {});
		}
		thread_ = std::thread(&visibility_worker::run, this);
	}

	void stop()
	{
		{
			std::lock_guard lock(mutex_);
			stopping_ = true;
			pending_.reset();
		}
		condition_.notify_one();
		if (thread_.joinable())
		{
			thread_.join();
		}
		std::atomic_store(&published_, std::shared_ptr<const visibility_result> {});
		data_ = nullptr;
	}

	void submit(snapshot value, uint32_t hold_ms, visibility_tuning tuning)
	{
		{
			std::lock_guard lock(mutex_);
			pending_ = std::move(value);
			hold_ms_ = hold_ms;
			tuning_ = tuning;
		}
		condition_.notify_one();
	}

	std::shared_ptr<const visibility_result> result() const
	{
		return std::atomic_load(&published_);
	}

	worker_stats stats() const
	{
		std::lock_guard lock(stats_mutex_);
		return stats_;
	}

private:
	static visibility_player sample_player(const player_state &player)
	{
		return {player.eye, player.origin, player.velocity, player.mins, player.maxs, player.rtt_seconds};
	}

	void run()
	{
		for (;;)
		{
			snapshot current;
			uint32_t hold_ms = 0;
			visibility_tuning tuning;
			{
				std::unique_lock lock(mutex_);
				condition_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
				if (stopping_)
				{
					return;
				}
				current = std::move(*pending_);
				pending_.reset();
				hold_ms = hold_ms_;
				tuning = tuning_;
			}
			const auto started = std::chrono::steady_clock::now();
			auto result = std::make_shared<visibility_result>();
			result->sequence = current.sequence;
			std::copy(std::begin(current.players), std::end(current.players), std::begin(result->players));
			std::array<float, k_max_players> observer_lookahead {};
			std::array<std::array<vec3, k_visibility_origin_count>, k_max_players> observer_origins {};
			for (uint32_t observer = 0; observer < k_max_players; ++observer)
			{
				if (current.players[observer].valid)
				{
					observer_lookahead[observer] = visibility_effective_lookahead_seconds(current.players[observer].rtt_seconds, tuning);
					observer_origins[observer] = visibility_origins(*data_, sample_player(current.players[observer]), tuning, observer_lookahead[observer]);
				}
			}
			for (uint32_t observer = 0; observer < k_max_players; ++observer)
			{
				for (uint32_t target = 0; target < k_max_players; ++target)
				{
					result->visible[observer][target] = true;
					const player_state &from = current.players[observer];
					const player_state &to = current.players[target];
					if (!from.valid || !to.valid || observer == target || from.team == to.team)
					{
						continue;
					}
					++result->evaluated_pairs;
					bool blocked = true;
					const auto &ray_origins = observer_origins[observer];
					const auto ray_targets = visibility_targets(*data_, sample_player(to), tuning, observer_lookahead[observer]);
					uint32_t ray = 0;
					for (const vec3 &origin : ray_origins)
					{
						for (const vec3 &point : ray_targets)
						{
							const ray_hit hit = segment_blocked(*data_, origin, point, cached_packets_[observer][target][ray]);
							cached_packets_[observer][target][ray++] = hit.packet_index;
							if (!hit.blocked)
							{
								blocked = false;
								break;
							}
						}
						if (!blocked)
						{
							break;
						}
					}
					const auto now = std::chrono::steady_clock::now();
					if (!blocked)
					{
						revealed_until_[observer][target] = now + std::chrono::milliseconds(hold_ms);
					}
					const bool visible = !blocked || now < revealed_until_[observer][target];
					result->visible[observer][target] = visible;
					visible ? ++result->visible_pairs : ++result->hidden_pairs;
				}
			}
			result->completed = std::chrono::steady_clock::now();
			result->worker_ms = std::chrono::duration<double, std::milli>(result->completed - started).count();
			{
				std::lock_guard lock(stats_mutex_);
				stats_.latest_ms = result->worker_ms;
				stats_.maximum_ms = std::max(stats_.maximum_ms, result->worker_ms);
				stats_.average_ms = (stats_.average_ms * static_cast<double>(stats_.cycles) + result->worker_ms) / static_cast<double>(stats_.cycles + 1u);
				++stats_.cycles;
				stats_.evaluated_pairs = result->evaluated_pairs;
				stats_.visible_pairs = result->visible_pairs;
				stats_.hidden_pairs = result->hidden_pairs;
			}
			std::atomic_store(&published_, std::shared_ptr<const visibility_result>(std::move(result)));
		}
	}

	const bvh8_data *data_ {};
	mutable std::mutex mutex_;
	std::condition_variable condition_;
	std::optional<snapshot> pending_;
	bool stopping_ {true};
	uint32_t hold_ms_ {};
	visibility_tuning tuning_;
	std::thread thread_;
	std::shared_ptr<const visibility_result> published_;
	std::array<std::array<std::array<uint32_t, k_visibility_ray_count>, k_max_players>, k_max_players> cached_packets_ {};
	std::array<std::array<std::chrono::steady_clock::time_point, k_max_players>, k_max_players> revealed_until_ {};
	mutable std::mutex stats_mutex_;
	worker_stats stats_;
};

struct bake_request
{
	std::string map;
	map_source source;
	std::filesystem::path game;
	std::filesystem::path output;
	std::filesystem::path baker;
	std::filesystem::path vrf;
};

struct bake_completion
{
	bake_request request;
	bvh8_data data;
	std::string error;
	bool success {};
	bool cancelled {};
};

class automatic_baker
{
public:
	~automatic_baker()
	{
		stop();
	}

	void start(bake_request request)
	{
		stop();
		cancel_.store(false);
		{
			std::lock_guard lock(mutex_);
			running_ = true;
			map_ = request.map;
			started_ = std::chrono::steady_clock::now();
		}
		thread_ = std::thread(&automatic_baker::run, this, std::move(request));
	}

	void stop()
	{
		cancel_.store(true);
		if (thread_.joinable())
		{
			thread_.join();
		}
		std::lock_guard lock(mutex_);
		running_ = false;
		map_.clear();
		completion_.reset();
	}

	bool poll(bake_completion &completion)
	{
		std::lock_guard lock(mutex_);
		if (!completion_)
		{
			return false;
		}
		completion = std::move(*completion_);
		completion_.reset();
		return true;
	}

	bool status(std::string &map, double &elapsed_ms) const
	{
		std::lock_guard lock(mutex_);
		if (!running_)
		{
			return false;
		}
		map = map_;
		elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_).count();
		return true;
	}

private:
	void run(bake_request request)
	{
		bake_completion completion;
		completion.request = request;
		process_result process;
		std::vector<std::string> arguments {
			"--game", request.game.string(),
			"--map", request.map,
			"--vpk", request.source.vpk.string(),
			"--output", request.output.string(),
			"--vrf", request.vrf.string(),
			"--low-priority"
		};
		if (!run_process(request.baker, arguments, k_auto_bake_timeout, &cancel_, true, process, completion.error))
		{
			finish(std::move(completion));
			return;
		}
		if (process.cancelled)
		{
			completion.cancelled = true;
			finish(std::move(completion));
			return;
		}
		if (process.timed_out)
		{
			completion.error = "automatic baker timed out";
			finish(std::move(completion));
			return;
		}
		if (process.exit_code != 0)
		{
			completion.error = "automatic baker exited with code " + std::to_string(process.exit_code);
			finish(std::move(completion));
			return;
		}
		if (!load_bvh8(request.output, completion.data, completion.error))
		{
			finish(std::move(completion));
			return;
		}
		const bvh8_header &header = completion.data.header;
		if (request.map != header.map_name || header.flags != request.source.flags
			|| header.source_crc32 != request.source.metadata.crc32 || header.source_size != request.source.metadata.size)
		{
			completion.data = {};
			completion.error = "automatic bake does not match requested map source";
			finish(std::move(completion));
			return;
		}
		completion.success = true;
		finish(std::move(completion));
	}

	void finish(bake_completion completion)
	{
		std::lock_guard lock(mutex_);
		running_ = false;
		completion_ = std::move(completion);
	}

	std::atomic_bool cancel_ {false};
	mutable std::mutex mutex_;
	std::thread thread_;
	std::optional<bake_completion> completion_;
	std::chrono::steady_clock::time_point started_ {};
	std::string map_;
	bool running_ {};
};

class plugin final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI *api, char *error, size_t max_length, bool late) override;
	bool Unload(char *error, size_t max_length) override;
	void AllPluginsLoaded() override {}
	void OnLevelInit(char const *map_name, char const *, char const *, char const *, bool, bool) override;
	void OnLevelShutdown() override;
	void hook_game_frame(bool simulating, bool first_tick, bool last_tick);
	void hook_check_transmit(CCheckTransmitInfo **infos, int count, CBitVec<MAX_EDICTS> &, CBitVec<MAX_EDICTS> &, const Entity2Networkable_t **, const uint16 *, int);
	void print_status() const;

	const char *GetAuthor() override { return "karola3vax"; }
	const char *GetName() override { return "CS2FOW"; }
	const char *GetDescription() override { return "Server-side fog-of-war visibility culling for Counter-Strike 2"; }
	const char *GetURL() override { return "https://github.com/karola3vax/CS2FOW"; }
	const char *GetLicense() override { return "MIT"; }
	const char *GetVersion() override { return "0.1.2-preview"; }
	const char *GetDate() override { return __DATE__; }
	const char *GetLogTag() override { return "CS2FOW"; }

private:
	bool read_gamedata(std::string &error);
	bool resolve_schema(std::string &error);
	bool resolve_map_source(const std::string &map, map_source &source, std::string &error) const;
	bool load_map_bake(const std::filesystem::path &path, const std::string &map, const map_source &source, bvh8_data &data, std::string &error) const;
	void start_automatic_bake(const std::string &map, const map_source &source, const std::filesystem::path &output, const std::string &reason);
	void poll_automatic_bake();
	void activate(bvh8_data data);
	void change_map(const std::string &map);
	void disable(std::string reason);
	CGameEntitySystem *entity_system() const;
	CEntityInstance *controller(uint32_t slot) const;
	CEntityInstance *pawn(CEntityInstance *controller) const;
	bool capture(snapshot &value);

	ISmmAPI *api_ {};
	IServerGameDLL *server_ {};
	ISource2GameEntities *game_entities_ {};
	IVEngineServer2 *engine_ {};
	ICvar *cvar_ {};
	ISchemaSystem *schema_ {};
	IFileSystem *filesystem_ {};
	game_resource_service *game_resource_ {};
	schema_offsets fields_;
	uint32_t recipient_slot_offset_ {};
	uint32_t entity_system_offset_ {};
	int game_frame_hook_id_ {};
	int check_transmit_hook_id_ {};
	std::string map_;
	std::string disabled_reason_ {"no map loaded"};
	map_source source_;
	bvh8_data data_;
	visibility_worker worker_;
	automatic_baker automatic_baker_;
	std::chrono::steady_clock::time_point last_snapshot_ {};
	uint64_t snapshot_sequence_ {};
	bool prerequisites_valid_ {};
};

plugin g_plugin;

CConVar<bool> cs2fow_enable("cs2fow_enable", FCVAR_NONE, "Enable CS2FOW when map data is valid", true);
CConVar<int> cs2fow_update_interval_ms("cs2fow_update_interval_ms", FCVAR_NONE, "Visibility worker update interval", 10, true, 10, true, 250);
CConVar<int> cs2fow_max_lookahead_ms("cs2fow_max_lookahead_ms", FCVAR_NONE, "Maximum latency lookahead", 210, true, 0, true, 250);
CConVar<int> cs2fow_min_lookahead_ms("cs2fow_min_lookahead_ms", FCVAR_NONE, "Minimum reveal lookahead", 120, true, 0, true, 250);
CConVar<int> cs2fow_peek_margin_units("cs2fow_peek_margin_units", FCVAR_NONE, "Minimum moving peek margin in Source units", 21, true, 0, true, 64);
CConVar<int> cs2fow_visibility_hold_ms("cs2fow_visibility_hold_ms", FCVAR_NONE, "Minimum revealed duration", 150, true, 0, true, 1000);
CConVar<bool> cs2fow_debug("cs2fow_debug", FCVAR_NONE, "Enable CS2FOW diagnostic logging", false);

CON_COMMAND_F(cs2fow_status, "Report CS2FOW state", FCVAR_NONE)
{
	g_plugin.print_status();
}

SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, false, bool, bool, bool);
SH_DECL_HOOK7_void(ISource2GameEntities, CheckTransmit, SH_NOATTRIB, false, CCheckTransmitInfo **, int, CBitVec<MAX_EDICTS> &, CBitVec<MAX_EDICTS> &, const Entity2Networkable_t **, const uint16 *, int);

bool plugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	api_ = ismm;
	GET_V_IFACE_CURRENT(GetEngineFactory, engine_, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, cvar_, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, filesystem_, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, schema_, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, game_resource_, game_resource_service, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, server_, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_ANY(GetServerFactory, game_entities_, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	g_pCVar = cvar_;
	game_frame_hook_id_ = SH_ADD_HOOK(IServerGameDLL, GameFrame, server_, SH_MEMBER(this, &plugin::hook_game_frame), true);
	check_transmit_hook_id_ = SH_ADD_HOOK(ISource2GameEntities, CheckTransmit, game_entities_, SH_MEMBER(this, &plugin::hook_check_transmit), true);
	if (game_frame_hook_id_ == 0 || check_transmit_hook_id_ == 0)
	{
		if (game_frame_hook_id_ != 0) SH_REMOVE_HOOK_ID(game_frame_hook_id_);
		if (check_transmit_hook_id_ != 0) SH_REMOVE_HOOK_ID(check_transmit_hook_id_);
		game_frame_hook_id_ = 0;
		check_transmit_hook_id_ = 0;
		if (error != nullptr && maxlen != 0) ismm->Format(error, maxlen, "Could not install required SourceHook hooks");
		return false;
	}
	META_CONVAR_REGISTER(FCVAR_RELEASE | FCVAR_GAMEDLL);
	g_SMAPI->AddListener(this, this);

	std::string reason;
	const bool avx = cpu_supports_avx();
	prerequisites_valid_ = avx && read_gamedata(reason) && resolve_schema(reason);
	if (!prerequisites_valid_)
	{
		disable(avx ? reason : "AVX and OS AVX state are required");
		META_CONPRINTF("[CS2FOW] disabled: %s\n", disabled_reason_.c_str());
	}
	META_CONPRINTF("[CS2FOW] loaded; culling is fail-open until a map bake validates\n");
	return true;
}

bool plugin::Unload(char *error, size_t max_length)
{
	automatic_baker_.stop();
	worker_.stop();
	if (game_frame_hook_id_ != 0) SH_REMOVE_HOOK_ID(game_frame_hook_id_);
	if (check_transmit_hook_id_ != 0) SH_REMOVE_HOOK_ID(check_transmit_hook_id_);
	game_frame_hook_id_ = 0;
	check_transmit_hook_id_ = 0;
	ConVar_Unregister();
	return true;
}

void plugin::OnLevelInit(char const *map_name, char const *, char const *, char const *, bool, bool)
{
	if (map_name != nullptr && map_name[0] != '\0')
	{
		change_map(map_name);
	}
}

void plugin::OnLevelShutdown()
{
	automatic_baker_.stop();
	worker_.stop();
	data_ = {};
	source_ = {};
	map_.clear();
	if (prerequisites_valid_)
	{
		disabled_reason_ = "no map loaded";
	}
}

bool plugin::read_gamedata(std::string &error)
{
	const std::filesystem::path path = std::filesystem::path(api_->GetBaseDir()) / "addons" / "cs2fow" / "gamedata" / "cs2fow.games.txt";
	std::ifstream stream(path);
	if (!stream)
	{
		error = "missing gamedata: " + path.string();
		return false;
	}
	recipient_slot_offset_ = 0;
	entity_system_offset_ = 0;
#if defined(_WIN32)
	constexpr std::string_view k_recipient_key = "recipient_slot_offset_windows";
	constexpr std::string_view k_entity_system_key = "game_entity_system_offset_windows";
#else
	constexpr std::string_view k_recipient_key = "recipient_slot_offset_linux";
	constexpr std::string_view k_entity_system_key = "game_entity_system_offset_linux";
#endif
	std::string line;
	while (std::getline(stream, line))
	{
		const size_t equals = line.find('=');
		if (equals == std::string::npos || line.starts_with("//"))
		{
			continue;
		}
		const std::string key = line.substr(0, equals);
		if (key != k_recipient_key && key != k_entity_system_key)
		{
			continue;
		}
		std::string_view text(line.data() + equals + 1u, line.size() - equals - 1u);
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
		uint32_t value {};
		const auto [end, conversion_error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (text.empty() || conversion_error != std::errc {} || end != text.data() + text.size())
		{
			error = "invalid gamedata value for " + key;
			return false;
		}
		if (key == k_recipient_key) recipient_slot_offset_ = value;
		if (key == k_entity_system_key) entity_system_offset_ = value;
	}
	if (recipient_slot_offset_ == 0 || entity_system_offset_ == 0)
	{
		error = "gamedata does not contain this platform's required offsets";
		return false;
	}
	if (recipient_slot_offset_ < sizeof(CCheckTransmitInfo) || recipient_slot_offset_ > k_max_gamedata_offset || recipient_slot_offset_ % alignof(int) != 0
		|| entity_system_offset_ < sizeof(void *) || entity_system_offset_ > k_max_gamedata_offset || entity_system_offset_ % alignof(void *) != 0)
	{
		error = "gamedata contains invalid offsets for this platform";
		return false;
	}
	return true;
}

bool plugin::resolve_schema(std::string &error)
{
	auto require = [&](uint32_t &target, const char *class_name, const char *field_name)
	{
		target = resolve_field(schema_, class_name, field_name);
		if (target == 0)
		{
			if (!error.empty())
			{
				error += ", ";
			}
			error += class_name;
			error += "::";
			error += field_name;
		}
	};
	require(fields_.is_hltv, "CBasePlayerController", "m_bIsHLTV");
	require(fields_.player_pawn, "CCSPlayerController", "m_hPlayerPawn");
	require(fields_.pawn_controller, "CBasePlayerPawn", "m_hController");
	require(fields_.health, "CBaseEntity", "m_iHealth");
	require(fields_.life_state, "CBaseEntity", "m_lifeState");
	require(fields_.team, "CBaseEntity", "m_iTeamNum");
	require(fields_.body_component, "CBaseEntity", "m_CBodyComponent");
	require(fields_.scene_node, "CBodyComponent", "m_pSceneNode");
	require(fields_.abs_origin, "CGameSceneNode", "m_vecAbsOrigin");
	require(fields_.abs_velocity, "CBaseEntity", "m_vecAbsVelocity");
	require(fields_.view_offset, "CBaseModelEntity", "m_vecViewOffset");
	require(fields_.view_x, "CNetworkViewOffsetVector", "m_vecX");
	require(fields_.view_y, "CNetworkViewOffsetVector", "m_vecY");
	require(fields_.view_z, "CNetworkViewOffsetVector", "m_vecZ");
	require(fields_.collision, "CBaseEntity", "m_pCollision");
	require(fields_.mins, "CCollisionProperty", "m_vecMins");
	require(fields_.maxs, "CCollisionProperty", "m_vecMaxs");
	require(fields_.weapon_services, "CBasePlayerPawn", "m_pWeaponServices");
	require(fields_.weapons, "CPlayer_WeaponServices", "m_hMyWeapons");
	require(fields_.active_weapon, "CPlayer_WeaponServices", "m_hActiveWeapon");
	require(fields_.last_weapon, "CPlayer_WeaponServices", "m_hLastWeapon");
	require(fields_.wearables, "CBaseCombatCharacter", "m_hMyWearables");
	require(fields_.hostage_services, "CCSPlayerPawn", "m_pHostageServices");
	require(fields_.carried_hostage_prop, "CCSPlayer_HostageServices", "m_hCarriedHostageProp");
	if (!error.empty())
	{
		error = "missing schema fields: " + error;
		return false;
	}
	return true;
}

CGameEntitySystem *plugin::entity_system() const
{
	if (game_resource_ == nullptr || entity_system_offset_ == 0)
	{
		return nullptr;
	}
	return field<CGameEntitySystem *>(game_resource_, entity_system_offset_);
}

CEntityInstance *plugin::controller(uint32_t slot) const
{
	CGameEntitySystem *system = entity_system();
	return system == nullptr ? nullptr : system->GetEntityInstance(CEntityIndex(static_cast<int>(slot + 1u)));
}

CEntityInstance *plugin::pawn(CEntityInstance *controller_entity) const
{
	if (controller_entity == nullptr)
	{
		return nullptr;
	}
	const CEntityHandle handle = field<CEntityHandle>(controller_entity, fields_.player_pawn);
	CGameEntitySystem *system = entity_system();
	return handle.IsValid() && system != nullptr ? system->GetEntityInstance(handle) : nullptr;
}

void plugin::disable(std::string reason)
{
	worker_.stop();
	data_ = {};
	disabled_reason_ = std::move(reason);
}

bool plugin::resolve_map_source(const std::string &map, map_source &source, std::string &error) const
{
	if (!valid_map_name(map))
	{
		error = "map name is not a safe relative path";
		return false;
	}
	const std::string virtual_path = "maps/" + map + ".vpk";
	CUtlVector<CUtlString> paths;
	filesystem_->FindFileAbsoluteList(paths, virtual_path.c_str(), "GAME");
	std::vector<std::filesystem::path> candidates;
	auto add_candidates = [&](const std::string &path)
	{
		for (const std::filesystem::path &candidate : vpk_path_candidates(path))
		{
			if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
			{
				candidates.push_back(candidate);
			}
		}
	};
	for (int i = 0; i < paths.Count(); ++i)
	{
		add_candidates(paths[i].Get());
	}
	add_candidates((std::filesystem::path(api_->GetBaseDir()) / "maps" / (map + ".vpk")).string());

	std::vector<std::string> tried;
	for (const std::filesystem::path &candidate : candidates)
	{
		tried.push_back(candidate.filename().string());
		std::string source_error;
		if (find_map_source(candidate, map, source, source_error))
		{
			return true;
		}
	}

	std::ostringstream message;
	message << "could not resolve mounted map VPK: " << virtual_path;
	if (!tried.empty())
	{
		message << " tried=";
		for (size_t i = 0; i < tried.size(); ++i)
		{
			if (i != 0)
			{
				message << ",";
			}
			message << tried[i];
		}
	}
	error = message.str();
	return false;
}

bool plugin::load_map_bake(const std::filesystem::path &path, const std::string &map, const map_source &source,
	bvh8_data &data, std::string &error) const
{
	if (!load_bvh8(path, data, error))
	{
		return false;
	}
	if (map != data.header.map_name)
	{
		error = "bake map name does not match current map";
		data = {};
		return false;
	}
	if (data.header.flags != source.flags || data.header.source_crc32 != source.metadata.crc32 || data.header.source_size != source.metadata.size)
	{
		error = "bake source CRC or size does not match current VPK";
		data = {};
		return false;
	}
	return true;
}

void plugin::activate(bvh8_data data)
{
	worker_.stop();
	data_ = std::move(data);
	disabled_reason_.clear();
	worker_.start(&data_);
	META_CONPRINTF("[CS2FOW] active for %s: crc=0x%08x, triangles=%u, nodes=%u, packets=%u\n", map_.c_str(), data_.header.source_crc32,
		data_.header.triangle_count, data_.header.node_count, data_.header.packet_count);
}

void plugin::start_automatic_bake(const std::string &map, const map_source &source, const std::filesystem::path &output, const std::string &reason)
{
	const std::filesystem::path base = api_->GetBaseDir();
#if defined(_WIN32)
	const std::filesystem::path baker = base / "tools" / "cs2fow_baker.exe";
	const std::filesystem::path vrf = base / "tools" / "vrf" / "win64" / "Source2Viewer-CLI.exe";
#else
	const std::filesystem::path baker = base / "tools" / "cs2fow_baker";
	const std::filesystem::path vrf = base / "tools" / "vrf" / "linux64" / "Source2Viewer-CLI";
#endif
	if (!std::filesystem::is_regular_file(baker) || !std::filesystem::is_regular_file(vrf))
	{
		disable("automatic baker or VRF is missing");
		return;
	}
	disabled_reason_ = "automatic bake in progress";
	META_CONPRINTF("[CS2FOW] %s for %s; starting automatic bake\n", reason.c_str(), map.c_str());
	automatic_baker_.start({map, source, base.parent_path().parent_path(), output, baker, vrf});
}

void plugin::poll_automatic_bake()
{
	bake_completion completion;
	if (!automatic_baker_.poll(completion))
	{
		return;
	}
	if (completion.cancelled || completion.request.map != map_)
	{
		return;
	}
	if (!completion.success)
	{
		disable("automatic bake failed: " + completion.error);
		META_CONPRINTF("[CS2FOW] automatic bake failed for %s: %s\n", map_.c_str(), completion.error.c_str());
		return;
	}
	map_source current;
	std::string error;
	if (!resolve_map_source(map_, current, error) || !same_map_source(current, completion.request.source))
	{
		disable("map source changed during automatic bake");
		return;
	}
	source_ = std::move(current);
	activate(std::move(completion.data));
}

void plugin::change_map(const std::string &map)
{
	automatic_baker_.stop();
	worker_.stop();
	data_ = {};
	source_ = {};
	map_ = map;
	if (!prerequisites_valid_)
	{
		return;
	}
	disabled_reason_ = "validating map";
	const std::filesystem::path base = api_->GetBaseDir();
	const std::filesystem::path bake = base / "addons" / "cs2fow" / "data" / "maps" / (map + ".bvh8");
	std::string error;
	if (!resolve_map_source(map, source_, error))
	{
		disable(error);
		return;
	}
	bvh8_data data;
	if (!load_map_bake(bake, map, source_, data, error))
	{
		start_automatic_bake(map, source_, bake, error);
		return;
	}
	activate(std::move(data));
}

bool plugin::capture(snapshot &value)
{
	CGameEntitySystem *system = entity_system();
	if (system == nullptr)
	{
		return false;
	}
	value.sequence = ++snapshot_sequence_;
	value.captured = std::chrono::steady_clock::now();
	for (uint32_t slot = 0; slot < k_max_players; ++slot)
	{
		CEntityInstance *controller_entity = controller(slot);
		if (controller_entity == nullptr || field<bool>(controller_entity, fields_.is_hltv))
		{
			continue;
		}
		CEntityInstance *pawn_entity = pawn(controller_entity);
		CEntityInstance *pawn_controller = pawn_entity == nullptr ? nullptr : system->GetEntityInstance(field<CEntityHandle>(pawn_entity, fields_.pawn_controller));
		const int pawn_index = entity_index(pawn_entity);
		if (pawn_entity == nullptr || pawn_controller != controller_entity || !valid_entity_index(pawn_index) || field<uint8_t>(pawn_entity, fields_.life_state) != k_life_alive
			|| field<int32_t>(pawn_entity, fields_.health) <= 0)
		{
			continue;
		}
		const uint8_t team = field<uint8_t>(pawn_entity, fields_.team);
		if (team != k_team_t && team != k_team_ct)
		{
			continue;
		}
		void *body_component = field<void *>(pawn_entity, fields_.body_component);
		void *scene_node = body_component == nullptr ? nullptr : field<void *>(body_component, fields_.scene_node);
		void *collision = field<void *>(pawn_entity, fields_.collision);
		if (scene_node == nullptr || collision == nullptr)
		{
			continue;
		}
		player_state &player = value.players[slot];
		player.valid = true;
		player.team = team;
		player.pawn_entity = pawn_index;
		player.origin = to_vec3(field<Vector>(scene_node, fields_.abs_origin));
		player.velocity = to_vec3(field<Vector>(pawn_entity, fields_.abs_velocity));
		player.mins = to_vec3(field<Vector>(collision, fields_.mins));
		player.maxs = to_vec3(field<Vector>(collision, fields_.maxs));
		void *view = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(pawn_entity) + fields_.view_offset);
		player.eye = {player.origin.x + field<float>(view, fields_.view_x), player.origin.y + field<float>(view, fields_.view_y), player.origin.z + field<float>(view, fields_.view_z)};
		if (INetChannelInfo *channel = engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(slot))); channel != nullptr)
		{
			player.rtt_seconds = std::max(0.0f, channel->GetEngineLatency());
		}
	}
	return true;
}

void plugin::hook_game_frame(bool simulating, bool first_tick, bool last_tick)
{
	INetworkGameServer *network_server = g_pNetworkServerService == nullptr ? nullptr : g_pNetworkServerService->GetIGameServer();
	if (network_server == nullptr)
	{
		return;
	}
	const char *current_map = network_server->GetMapName();
	if (current_map != nullptr && map_ != current_map)
	{
		change_map(current_map);
	}
	poll_automatic_bake();
	if (!simulating || !cs2fow_enable.Get() || !disabled_reason_.empty())
	{
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - last_snapshot_ < std::chrono::milliseconds(cs2fow_update_interval_ms.Get()))
	{
		return;
	}
	snapshot value;
	if (!capture(value))
	{
		disable("game entity system is unavailable");
		return;
	}
	last_snapshot_ = now;
	worker_.submit(std::move(value), static_cast<uint32_t>(cs2fow_visibility_hold_ms.Get()), {
		static_cast<uint32_t>(cs2fow_update_interval_ms.Get()),
		static_cast<uint32_t>(cs2fow_min_lookahead_ms.Get()),
		static_cast<uint32_t>(cs2fow_max_lookahead_ms.Get()),
		static_cast<float>(cs2fow_peek_margin_units.Get())
	});
}

void plugin::hook_check_transmit(CCheckTransmitInfo **infos, int count, CBitVec<MAX_EDICTS> &, CBitVec<MAX_EDICTS> &, const Entity2Networkable_t **, const uint16 *, int)
{
	if (!cs2fow_enable.Get() || !disabled_reason_.empty() || infos == nullptr || count <= 0 || count > static_cast<int>(k_max_players))
	{
		return;
	}
	CGameEntitySystem *system = entity_system();
	if (system == nullptr)
	{
		return;
	}
	const std::shared_ptr<const visibility_result> result = worker_.result();
	const auto stale_after = std::chrono::milliseconds(std::max(100, 3 * cs2fow_update_interval_ms.Get()));
	if (!result || std::chrono::steady_clock::now() - result->completed > stale_after)
	{
		return;
	}
	const auto current_player_pawn = [&](uint32_t slot, const player_state &saved)
	{
		CEntityInstance *controller_entity = controller(slot);
		if (!saved.valid || controller_entity == nullptr || field<bool>(controller_entity, fields_.is_hltv))
		{
			return static_cast<CEntityInstance *>(nullptr);
		}
		CEntityInstance *pawn_entity = pawn(controller_entity);
		CEntityInstance *pawn_controller = pawn_entity == nullptr ? nullptr : system->GetEntityInstance(field<CEntityHandle>(pawn_entity, fields_.pawn_controller));
		if (pawn_entity == nullptr || pawn_controller != controller_entity || entity_index(pawn_entity) != saved.pawn_entity || !valid_entity_index(saved.pawn_entity)
			|| field<uint8_t>(pawn_entity, fields_.life_state) != k_life_alive || field<int32_t>(pawn_entity, fields_.health) <= 0)
		{
			return static_cast<CEntityInstance *>(nullptr);
		}
		const uint8_t team = field<uint8_t>(pawn_entity, fields_.team);
		return team == saved.team && (team == k_team_t || team == k_team_ct) ? pawn_entity : nullptr;
	};
	for (int i = 0; i < count; ++i)
	{
		CCheckTransmitInfo *info = infos[i];
		if (info == nullptr || info->m_pTransmitEntity == nullptr)
		{
			continue;
		}
		int slot = -1;
		std::memcpy(&slot, reinterpret_cast<const char *>(info) + recipient_slot_offset_, sizeof(slot));
		if (slot < 0 || slot >= static_cast<int>(k_max_players) || !result->players[slot].valid)
		{
			continue;
		}
		const player_state &observer = result->players[slot];
		if (current_player_pawn(static_cast<uint32_t>(slot), observer) == nullptr)
		{
			continue;
		}
		for (uint32_t target = 0; target < k_max_players; ++target)
		{
			const player_state &player = result->players[target];
			if (result->visible[slot][target] || !player.valid || player.team == observer.team)
			{
				continue;
			}
			CEntityInstance *current_pawn = current_player_pawn(target, player);
			if (current_pawn == nullptr)
			{
				continue;
			}
			void *services = field<void *>(current_pawn, fields_.weapon_services);
			if (services == nullptr)
			{
				continue;
			}
			std::array<int, k_max_hidden_player_entities> hidden_entities {};
			size_t hidden_count = 0;
			const auto collect_index = [&](int index)
			{
				if (!valid_entity_index(index) || hidden_count >= hidden_entities.size())
				{
					return false;
				}
				hidden_entities[hidden_count++] = index;
				return true;
			};
			const auto collect_handle = [&](CEntityHandle handle)
			{
				if (!handle.IsValid())
				{
					return true;
				}
				return collect_index(entity_index(system->GetEntityInstance(handle)));
			};
			const auto collect_vector = [&](void *base, uint32_t offset, int max_count)
			{
				auto *handles = reinterpret_cast<CUtlVector<CEntityHandle> *>(reinterpret_cast<uintptr_t>(base) + offset);
				const int count = handles->Count();
				if (count < 0 || count > max_count)
				{
					return false;
				}
				for (int item = 0; item < count; ++item)
				{
					if (!collect_handle((*handles)[item]))
					{
						return false;
					}
				}
				return true;
			};
			if (!collect_index(player.pawn_entity)
				|| !collect_handle(field<CEntityHandle>(services, fields_.active_weapon))
				|| !collect_handle(field<CEntityHandle>(services, fields_.last_weapon))
				|| !collect_vector(services, fields_.weapons, static_cast<int>(k_max_weapons))
				|| !collect_vector(current_pawn, fields_.wearables, static_cast<int>(k_max_wearables)))
			{
				continue;
			}
			void *hostage_services = field<void *>(current_pawn, fields_.hostage_services);
			if (hostage_services != nullptr && !collect_handle(field<CEntityHandle>(hostage_services, fields_.carried_hostage_prop)))
			{
				continue;
			}
			for (size_t entity = 0; entity < hidden_count; ++entity)
			{
				info->m_pTransmitEntity->Clear(hidden_entities[entity]);
			}
		}
	}
}

void plugin::print_status() const
{
	const worker_stats stats = worker_.stats();
	const std::shared_ptr<const visibility_result> result = worker_.result();
	const double age_ms = result ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - result->completed).count() : -1.0;
	META_CONPRINTF("[CS2FOW] %s; map=%s crc=0x%08x version=%u triangles=%u nodes=%u packets=%u bytes=%llu depth=%u\n",
		disabled_reason_.empty() && cs2fow_enable.Get() ? "active" : (disabled_reason_.empty() ? "disabled by convar" : disabled_reason_.c_str()), map_.c_str(),
		data_.header.source_crc32, data_.header.version, data_.header.triangle_count, data_.header.node_count, data_.header.packet_count,
		static_cast<unsigned long long>(data_.header.file_size), data_.header.max_depth);
	META_CONPRINTF("[CS2FOW] worker latest=%.3fms average=%.3fms maximum=%.3fms result_age=%.1fms pairs=%u visible=%u hidden=%u cycles=%llu\n",
		stats.latest_ms, stats.average_ms, stats.maximum_ms, age_ms, stats.evaluated_pairs, stats.visible_pairs, stats.hidden_pairs,
		static_cast<unsigned long long>(stats.cycles));
	std::string bake_map;
	double bake_elapsed_ms = 0;
	if (automatic_baker_.status(bake_map, bake_elapsed_ms))
	{
		META_CONPRINTF("[CS2FOW] auto-bake map=%s elapsed=%.1fms\n", bake_map.c_str(), bake_elapsed_ms);
	}
}

} // namespace cs2fow

CEntityIdentity *CEntitySystem::GetEntityIdentity(CEntityIndex entity_index)
{
	if (entity_index.Get() < 0 || entity_index.Get() >= MAX_TOTAL_ENTITIES - 1)
	{
		return nullptr;
	}
	CEntityIdentity *chunk = m_EntityList.m_pIdentityChunks[entity_index.Get() / MAX_ENTITIES_IN_LIST];
	if (chunk == nullptr)
	{
		return nullptr;
	}
	CEntityIdentity *identity = &chunk[entity_index.Get() % MAX_ENTITIES_IN_LIST];
	return identity->GetEntityIndex() == entity_index ? identity : nullptr;
}

CEntityIdentity *CEntitySystem::GetEntityIdentity(const CEntityHandle &handle)
{
	if (!handle.IsValid())
	{
		return nullptr;
	}
	const int index = handle.GetEntryIndex();
	if (index < 0 || index >= MAX_TOTAL_ENTITIES - 1)
	{
		return nullptr;
	}
	CEntityIdentity *chunk = m_EntityList.m_pIdentityChunks[index / MAX_ENTITIES_IN_LIST];
	if (chunk == nullptr)
	{
		return nullptr;
	}
	CEntityIdentity *identity = &chunk[index % MAX_ENTITIES_IN_LIST];
	return identity->GetRefEHandle() == handle ? identity : nullptr;
}

PLUGIN_EXPOSE(cs2fow::plugin, cs2fow::g_plugin);
