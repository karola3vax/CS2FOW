#include "bvh8.h"
#include "lifecycle_guard.h"
#include "map_source.h"
#include "subprocess.h"
#include "transmit_masks.h"
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
constexpr auto k_lifecycle_fail_open = std::chrono::milliseconds(3000);
constexpr auto k_hidden_entity_quarantine = std::chrono::milliseconds(3000);
constexpr auto k_pair_baseline_warmup = std::chrono::milliseconds(1500);
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
	uint32_t death_time {};
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
	uint32_t eye_angles {};
	uint32_t collision {};
	uint32_t mins {};
	uint32_t maxs {};
	uint32_t weapon_services {};
	uint32_t weapons {};
	uint32_t active_weapon {};
	uint32_t last_weapon {};
	uint32_t wearables {};
	uint32_t hostage_services {};
	uint32_t is_spawning {};
	uint32_t death_flags {};
	uint32_t has_death_info {};
	uint32_t death_info_time {};
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
	float eye_yaw_degrees {};
	float rtt_seconds {};
	int pawn_entity {-1};
};

struct snapshot
{
	uint64_t sequence {};
	std::chrono::steady_clock::time_point captured;
	player_state players[k_max_players];
};

struct live_player
{
	CEntityInstance *pawn {};
	int pawn_entity {-1};
	uint8_t team {};
};

using visual_entity_group = hidden_entity_group<CEntityHandle, k_max_hidden_player_entities>;

visual_group_key make_current_visual_group_key(const visual_entity_group &group)
{
	std::array<uint32_t, k_max_hidden_player_entities> values {};
	for (size_t index = 0; index < group.count; ++index)
	{
		values[index] = static_cast<uint32_t>(group.handles[index].ToInt());
	}
	return make_visual_group_key(values, group.count);
}

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

struct qangle
{
	float x {};
	float y {};
	float z {};
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

CEntityHandle entity_handle(CEntityInstance *entity)
{
	return entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->GetRefEHandle() : CEntityHandle {};
}

bool valid_entity_index(int index)
{
	return index > 0 && index < MAX_EDICTS;
}

int resolve_entity_index(CGameEntitySystem *system, CEntityHandle handle)
{
	if (system == nullptr || !handle.IsValid())
	{
		return -1;
	}
	return entity_index(system->GetEntityInstance(handle));
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
		return {player.eye, player.origin, player.velocity, player.mins, player.maxs, player.eye_yaw_degrees, player.rtt_seconds};
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
	lifecycle_key player_lifecycle(uint32_t slot, CGameEntitySystem *system, live_player *live) const;
	bool collect_player_visual_group(CGameEntitySystem *system, CEntityInstance *pawn, visual_entity_group &group) const;
	bool group_fully_marked(CGameEntitySystem *system, CBitVec<MAX_EDICTS> *bits, const visual_entity_group &group) const;
	void clear_group(CGameEntitySystem *system, const transmit_masks<CBitVec<MAX_EDICTS>> &masks, const visual_entity_group &group) const;
	void reset_transmit_state();
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
	checktransmit_private_offsets transmit_offsets_;
	int game_frame_hook_id_ {};
	int check_transmit_hook_id_ {};
	std::string map_;
	std::string disabled_reason_ {"no map loaded"};
	map_source source_;
	bvh8_data data_;
	visibility_worker worker_;
	automatic_baker automatic_baker_;
	std::array<lifecycle_guard, k_max_players> lifecycle_;
	std::array<std::array<pair_guard, k_max_players>, k_max_players> pair_guards_;
	std::array<std::array<visual_entity_group, k_max_players>, k_max_players> hidden_groups_;
	std::chrono::steady_clock::time_point last_snapshot_ {};
	uint64_t snapshot_sequence_ {};
	bool prerequisites_valid_ {};
};

plugin g_plugin;

CConVar<bool> cs2fow_enable("cs2fow_enable", FCVAR_NONE, "Enable CS2FOW when map data is valid", true);
CConVar<int> cs2fow_update_interval_ms("cs2fow_update_interval_ms", FCVAR_NONE, "Visibility worker update interval", 1, true, 1, true, 250);
CConVar<int> cs2fow_max_lookahead_ms("cs2fow_max_lookahead_ms", FCVAR_NONE, "Maximum latency lookahead", 500, true, 0, true, 500);
CConVar<int> cs2fow_min_lookahead_ms("cs2fow_min_lookahead_ms", FCVAR_NONE, "Minimum reveal lookahead", 200, true, 0, true, 500);
CConVar<int> cs2fow_peek_margin_units("cs2fow_peek_margin_units", FCVAR_NONE, "Maximum speed-stepped peek margin in Source units", 160, true, 0, true, 256);
CConVar<int> cs2fow_visibility_hold_ms("cs2fow_visibility_hold_ms", FCVAR_NONE, "Minimum revealed duration", 50, true, 0, true, 1000);
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
	reset_transmit_state();
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
	transmit_offsets_ = {};
#if defined(_WIN32)
	constexpr std::string_view k_recipient_key = "recipient_slot_offset_windows";
	constexpr std::string_view k_entity_system_key = "game_entity_system_offset_windows";
	constexpr std::string_view k_aux_masks_key = "checktransmit_aux_mask_offsets_windows";
	constexpr std::string_view k_full_update_key = "checktransmit_full_update_offset_windows";
#else
	constexpr std::string_view k_recipient_key = "recipient_slot_offset_linux";
	constexpr std::string_view k_entity_system_key = "game_entity_system_offset_linux";
	constexpr std::string_view k_aux_masks_key = "checktransmit_aux_mask_offsets_linux";
	constexpr std::string_view k_full_update_key = "checktransmit_full_update_offset_linux";
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
		if (key != k_recipient_key && key != k_entity_system_key && key != k_aux_masks_key && key != k_full_update_key)
		{
			continue;
		}
		std::string_view text(line.data() + equals + 1u, line.size() - equals - 1u);
		if (key == k_aux_masks_key)
		{
			if (!parse_checktransmit_aux_mask_offsets(text, transmit_offsets_.aux_mask_offsets, k_max_gamedata_offset))
			{
				error = "invalid gamedata value for " + key;
				return false;
			}
			continue;
		}
		uint32_t value {};
		if (!parse_gamedata_uint32(text, value))
		{
			error = "invalid gamedata value for " + key;
			return false;
		}
		if (key == k_recipient_key) recipient_slot_offset_ = value;
		if (key == k_entity_system_key) entity_system_offset_ = value;
		if (key == k_full_update_key) transmit_offsets_.full_update_offset = value;
	}
	const bool aux_offsets_valid = std::all_of(transmit_offsets_.aux_mask_offsets.begin(), transmit_offsets_.aux_mask_offsets.end(),
		[](uint32_t offset) { return offset != 0; });
	if (recipient_slot_offset_ == 0 || entity_system_offset_ == 0 || transmit_offsets_.full_update_offset == 0 || !aux_offsets_valid)
	{
		error = "gamedata does not contain this platform's required offsets";
		return false;
	}
	if (recipient_slot_offset_ < sizeof(CCheckTransmitInfo) || recipient_slot_offset_ > k_max_gamedata_offset || recipient_slot_offset_ % alignof(int) != 0
		|| entity_system_offset_ < sizeof(void *) || entity_system_offset_ > k_max_gamedata_offset || entity_system_offset_ % alignof(void *) != 0
		|| !valid_gamedata_offset(transmit_offsets_.full_update_offset, static_cast<uint32_t>(alignof(bool)), k_max_gamedata_offset))
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
	require(fields_.death_time, "CBasePlayerPawn", "m_flDeathTime");
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
	require(fields_.eye_angles, "CCSPlayerPawn", "m_angEyeAngles");
	require(fields_.collision, "CBaseEntity", "m_pCollision");
	require(fields_.mins, "CCollisionProperty", "m_vecMins");
	require(fields_.maxs, "CCollisionProperty", "m_vecMaxs");
	require(fields_.weapon_services, "CBasePlayerPawn", "m_pWeaponServices");
	require(fields_.weapons, "CPlayer_WeaponServices", "m_hMyWeapons");
	require(fields_.active_weapon, "CPlayer_WeaponServices", "m_hActiveWeapon");
	require(fields_.last_weapon, "CPlayer_WeaponServices", "m_hLastWeapon");
	require(fields_.wearables, "CBaseCombatCharacter", "m_hMyWearables");
	require(fields_.hostage_services, "CCSPlayerPawn", "m_pHostageServices");
	require(fields_.is_spawning, "CCSPlayerPawn", "m_bIsSpawning");
	require(fields_.death_flags, "CCSPlayerPawn", "m_iDeathFlags");
	require(fields_.has_death_info, "CCSPlayerPawn", "m_bHasDeathInfo");
	require(fields_.death_info_time, "CCSPlayerPawn", "m_flDeathInfoTime");
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

lifecycle_key plugin::player_lifecycle(uint32_t slot, CGameEntitySystem *system, live_player *live) const
{
	if (live != nullptr)
	{
		*live = {};
	}
	lifecycle_key key;
	CEntityInstance *controller_entity = system == nullptr ? nullptr : system->GetEntityInstance(CEntityIndex(static_cast<int>(slot + 1u)));
	key.has_controller = controller_entity != nullptr;
	if (controller_entity == nullptr)
	{
		return key;
	}
	key.hltv = field<bool>(controller_entity, fields_.is_hltv);
	if (key.hltv)
	{
		return key;
	}
	CEntityInstance *pawn_entity = pawn(controller_entity);
	CEntityInstance *pawn_controller = pawn_entity == nullptr ? nullptr : system->GetEntityInstance(field<CEntityHandle>(pawn_entity, fields_.pawn_controller));
	key.pawn_entity = entity_index(pawn_entity);
	if (pawn_entity == nullptr || pawn_controller != controller_entity || !valid_entity_index(key.pawn_entity))
	{
		return key;
	}
	key.team = field<uint8_t>(pawn_entity, fields_.team);
	key.alive = field<uint8_t>(pawn_entity, fields_.life_state) == k_life_alive && field<int32_t>(pawn_entity, fields_.health) > 0;
	key.spawning = field<bool>(pawn_entity, fields_.is_spawning);
	key.death_flags = field<int32_t>(pawn_entity, fields_.death_flags);
	key.has_death_info = field<bool>(pawn_entity, fields_.has_death_info);
	key.death_time = field<float>(pawn_entity, fields_.death_time);
	key.death_info_time = field<float>(pawn_entity, fields_.death_info_time);
	if (live != nullptr && key.alive && !key.spawning && (key.team == k_team_t || key.team == k_team_ct))
	{
		live->pawn = pawn_entity;
		live->pawn_entity = key.pawn_entity;
		live->team = key.team;
	}
	return key;
}

bool plugin::collect_player_visual_group(CGameEntitySystem *system, CEntityInstance *pawn_entity, visual_entity_group &group) const
{
	hidden_group_clear(group);
	if (system == nullptr || pawn_entity == nullptr)
	{
		return false;
	}
	void *services = field<void *>(pawn_entity, fields_.weapon_services);
	if (services == nullptr)
	{
		return false;
	}
	const auto collect_handle = [&](CEntityHandle handle)
	{
		if (!handle.IsValid())
		{
			return true;
		}
		if (!valid_entity_index(resolve_entity_index(system, handle)) || group.count >= group.handles.size())
		{
			return false;
		}
		group.handles[group.count++] = handle;
		return true;
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
	group.source = entity_handle(pawn_entity);
	if (!group.source.IsValid()
		|| !collect_handle(group.source)
		|| !collect_handle(field<CEntityHandle>(services, fields_.active_weapon))
		|| !collect_handle(field<CEntityHandle>(services, fields_.last_weapon))
		|| !collect_vector(services, fields_.weapons, static_cast<int>(k_max_weapons))
		|| !collect_vector(pawn_entity, fields_.wearables, static_cast<int>(k_max_wearables)))
	{
		hidden_group_clear(group);
		return false;
	}
	void *hostage_services = field<void *>(pawn_entity, fields_.hostage_services);
	if (hostage_services != nullptr && !collect_handle(field<CEntityHandle>(hostage_services, fields_.carried_hostage_prop)))
	{
		hidden_group_clear(group);
		return false;
	}
	return group.count != 0;
}

bool plugin::group_fully_marked(CGameEntitySystem *system, CBitVec<MAX_EDICTS> *bits, const visual_entity_group &group) const
{
	if (bits == nullptr || group.count == 0)
	{
		return false;
	}
	return hidden_group_all_of(group, [&](CEntityHandle handle)
	{
		const int index = resolve_entity_index(system, handle);
		return valid_entity_index(index) && bits->IsBitSet(index);
	});
}

void plugin::clear_group(CGameEntitySystem *system, const transmit_masks<CBitVec<MAX_EDICTS>> &masks, const visual_entity_group &group) const
{
	if (masks.count == 0)
	{
		return;
	}
	clear_transmit_group(masks, group.handles, group.count,
		[&](CEntityHandle handle) { return resolve_entity_index(system, handle); },
		[](int index) { return valid_entity_index(index); });
}

void plugin::disable(std::string reason)
{
	worker_.stop();
	data_ = {};
	reset_transmit_state();
	disabled_reason_ = std::move(reason);
}

void plugin::reset_transmit_state()
{
	for (lifecycle_guard &guard : lifecycle_)
	{
		guard = {};
	}
	for (auto &row : pair_guards_)
	{
		for (pair_guard &guard : row)
		{
			guard = {};
		}
	}
	for (auto &row : hidden_groups_)
	{
		for (visual_entity_group &group : row)
		{
			hidden_group_clear(group);
		}
	}
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
	reset_transmit_state();
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
	reset_transmit_state();
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
	const auto now = value.captured;
	std::array<lifecycle_key, k_max_players> keys;
	std::array<bool, k_max_players> stable_slots {};
	for (uint32_t slot = 0; slot < k_max_players; ++slot)
	{
		live_player live;
		const lifecycle_key key = player_lifecycle(slot, system, &live);
		keys[slot] = key;
		const bool stable = live.pawn != nullptr;
		stable_slots[slot] = stable;
		update_lifecycle_guard(lifecycle_[slot], key, stable, now, k_lifecycle_fail_open);
		if (!stable || !lifecycle_allows_hiding(lifecycle_[slot], now))
		{
			continue;
		}
		CEntityInstance *pawn_entity = live.pawn;
		void *body_component = field<void *>(pawn_entity, fields_.body_component);
		void *scene_node = body_component == nullptr ? nullptr : field<void *>(body_component, fields_.scene_node);
		void *collision = field<void *>(pawn_entity, fields_.collision);
		if (scene_node == nullptr || collision == nullptr)
		{
			continue;
		}
		player_state &player = value.players[slot];
		player.valid = true;
		player.team = live.team;
		player.pawn_entity = live.pawn_entity;
		player.origin = to_vec3(field<Vector>(scene_node, fields_.abs_origin));
		player.velocity = to_vec3(field<Vector>(pawn_entity, fields_.abs_velocity));
		player.mins = to_vec3(field<Vector>(collision, fields_.mins));
		player.maxs = to_vec3(field<Vector>(collision, fields_.maxs));
		void *view = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(pawn_entity) + fields_.view_offset);
		player.eye = {player.origin.x + field<float>(view, fields_.view_x), player.origin.y + field<float>(view, fields_.view_y), player.origin.z + field<float>(view, fields_.view_z)};
		player.eye_yaw_degrees = field<qangle>(pawn_entity, fields_.eye_angles).y;
		if (INetChannelInfo *channel = engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(slot))); channel != nullptr)
		{
			player.rtt_seconds = std::max(0.0f, channel->GetEngineLatency());
		}
	}
	for (uint32_t observer = 0; observer < k_max_players; ++observer)
	{
		for (uint32_t target = 0; target < k_max_players; ++target)
		{
			update_pair_guard(pair_guards_[observer][target], keys[observer], stable_slots[observer],
				keys[target], stable_slots[target], now, k_pair_baseline_warmup);
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
	CGameEntitySystem *system = entity_system();
	if (system == nullptr)
	{
		disable("game entity system is unavailable");
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
	const auto now = std::chrono::steady_clock::now();
	if (!result || now - result->completed > stale_after)
	{
		return;
	}
	const auto current_player_pawn = [&](uint32_t slot, const player_state &saved)
	{
		live_player live;
		const lifecycle_key key = player_lifecycle(slot, system, &live);
		if (!saved.valid || live.pawn == nullptr || !lifecycle_allows_hiding(lifecycle_[slot], now)
			|| lifecycle_changed(lifecycle_[slot].key, key) || key.pawn_entity != saved.pawn_entity || key.team != saved.team)
		{
			return static_cast<CEntityInstance *>(nullptr);
		}
		return live.pawn;
	};
	for (int i = 0; i < count; ++i)
	{
		CCheckTransmitInfo *info = infos[i];
		if (info == nullptr || info->m_pTransmitEntity == nullptr)
		{
			continue;
		}
		if (read_checktransmit_full_update(info, transmit_offsets_.full_update_offset))
		{
			continue;
		}
		transmit_masks<CBitVec<MAX_EDICTS>> masks;
		if (!collect_transmit_masks(info, info->m_pTransmitEntity, transmit_offsets_.aux_mask_offsets, masks))
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
			visual_entity_group &stored_group = hidden_groups_[slot][target];
			if (stored_group.count != 0 && now >= stored_group.quarantine_until)
			{
				hidden_group_clear(stored_group);
			}
			if (!player.valid || player.team == observer.team)
			{
				if (hidden_group_quarantined(stored_group, now))
				{
					clear_group(system, masks, stored_group);
				}
				continue;
			}
			CEntityInstance *current_pawn = current_player_pawn(target, player);
			if (current_pawn == nullptr)
			{
				if (hidden_group_quarantined(stored_group, now))
				{
					clear_group(system, masks, stored_group);
				}
				continue;
			}
			pair_guard &guard = pair_guards_[slot][target];
			visual_entity_group current_group;
			const bool current_group_valid = collect_player_visual_group(system, current_pawn, current_group);
			const bool full_group_marked = current_group_valid && group_fully_marked(system, masks.primary, current_group);
			if (current_group_valid)
			{
				update_pair_visual_group(guard, make_current_visual_group_key(current_group), now, k_pair_baseline_warmup);
			}
			if (result->visible[slot][target])
			{
				if (full_group_marked)
				{
					pair_note_open(guard, now, result->sequence);
					hidden_group_clear(stored_group);
				}
				continue;
			}
			if (!pair_allows_hiding(guard, now, result->sequence))
			{
				if (full_group_marked)
				{
					pair_note_open(guard, now, result->sequence);
					hidden_group_clear(stored_group);
				}
				continue;
			}
			if (!current_group_valid)
			{
				if (hidden_group_quarantined(stored_group, now))
				{
					clear_group(system, masks, stored_group);
				}
				continue;
			}
			hidden_group_store(stored_group, current_group.source, current_group.handles, current_group.count, now, k_hidden_entity_quarantine);
			clear_group(system, masks, current_group);
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
