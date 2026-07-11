#pragma once

// Declares the one CS2FOW plugin object and the fixed runtime state shared by
// its modules. Live engine objects stay with game-thread/CheckTransmit callers;
// the worker receives copied snapshots, and uncertain state must fail open.

#include "automatic_baker.h"
#include "lifecycle_guard.h"
#include "map_source.h"
#include "transmit_debug.h"
#include "transmit_masks.h"
#include "visibility_worker.h"

#include <ISmmPlugin.h>
#include <eiface.h>
#include <entity2/entitysystem.h>
#include <filesystem.h>
#include <iserver.h>
#include <schemasystem/schemasystem.h>
#include <tier1/convar.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

PLUGIN_GLOBALVARS();

namespace cs2fow
{

inline constexpr uint32_t k_max_weapons = 64;
inline constexpr uint32_t k_max_wearables = 32;
inline constexpr uint32_t k_max_aux_visual_group_entities = 32;
inline constexpr uint32_t k_max_aux_visual_cache_entities = 2048;
inline constexpr uint32_t k_max_recent_hide_records = 256;
inline constexpr uint32_t k_entity_scan_hard_limit = MAX_TOTAL_ENTITIES;
inline constexpr uint32_t k_max_entity_name = 64;
inline constexpr uint32_t k_max_hidden_player_entities = 1 + 2 + k_max_weapons + k_max_wearables + 1 + k_max_aux_visual_group_entities;
inline constexpr uint32_t k_max_gamedata_offset = 4096;
inline constexpr uint8_t k_life_alive = 0;
inline constexpr uint8_t k_team_t = 2;
inline constexpr uint8_t k_team_ct = 3;
inline constexpr auto k_lifecycle_fail_open = std::chrono::milliseconds(3000);
inline constexpr auto k_hidden_entity_quarantine = std::chrono::milliseconds(3000);
inline constexpr auto k_pair_baseline_warmup = std::chrono::milliseconds(1500);
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
	uint32_t attribute_manager {};
	uint32_t item {};
	uint32_t item_definition_index {};
	uint32_t wearables {};
	uint32_t hostage_services {};
	uint32_t is_spawning {};
	uint32_t death_flags {};
	uint32_t has_death_info {};
	uint32_t death_info_time {};
	uint32_t carried_hostage_prop {};
	uint32_t owner_entity {};
	uint32_t effect_entity {};
};

struct live_player
{
	CEntityInstance *pawn {};
	int pawn_entity {-1};
	uint8_t team {};
};

using visual_entity_group = hidden_entity_group<CEntityHandle, k_max_hidden_player_entities>;
static_assert(k_max_hidden_player_entities <= k_pair_visual_group_key_max);

struct target_transmit_cache
{
	CEntityInstance *pawn {};
	visual_entity_group group;
	visual_group_key group_key;
	bool group_valid {};
};

enum class hide_reason : uint8_t
{
	current,
	quarantine
};

struct aux_visual_entity : owner_effect_link<CEntityHandle>
{
	int edict {-1};
	char name[k_max_entity_name] {};
};

using recent_hide_log = transmit_debug_log<k_max_recent_hide_records, k_max_entity_name>;

struct qangle
{
	float x {};
	float y {};
	float z {};
};

int entity_index(CEntityInstance *entity);
CEntityHandle entity_handle(CEntityInstance *entity);
void copy_entity_name(CEntityInstance *entity, char (&name)[k_max_entity_name]);
bool valid_networked_edict_index(int index);
int resolve_entity_index(CGameEntitySystem *system, CEntityHandle handle);

class plugin final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI *api, char *error, size_t max_length, bool late) override;
	bool Unload(char *error, size_t max_length) override;
	void AllPluginsLoaded() override {}
	void OnLevelInit(char const *map_name, char const *, char const *, char const *, bool, bool) override;
	void OnLevelShutdown() override;
	void hook_game_frame(bool simulating, bool first_tick, bool last_tick);
	void hook_check_transmit(CCheckTransmitInfo **infos, int count, CBitVec<MAX_EDICTS> &, CBitVec<MAX_EDICTS> &,
		const Entity2Networkable_t **, const uint16 *, int);
	void print_status() const;
	void print_entities(int edict);
	void clear_entity_records();
	void reset_transmit_state(bool clear_debug_records = true);

	const char *GetAuthor() override { return "karola3vax"; }
	const char *GetName() override { return "CS2FOW"; }
	const char *GetDescription() override { return "Server-side fog-of-war visibility culling for Counter-Strike 2"; }
	const char *GetURL() override { return "https://github.com/karola3vax/CS2FOW"; }
	const char *GetLicense() override { return "MIT"; }
	const char *GetVersion() override { return CS2FOW_VERSION; }
	const char *GetDate() override { return __DATE__; }
	const char *GetLogTag() override { return "CS2FOW"; }

private:
	bool read_gamedata(std::string &error);
	bool resolve_schema(std::string &error);
	bool resolve_map_source(const std::string &map, map_source &source, std::string &error) const;
	bool load_map_bake(const std::filesystem::path &path, const std::string &map, const map_source &source,
		bvh8_data &data, std::string &error) const;
	void start_automatic_bake(const std::string &map, const map_source &source, const std::filesystem::path &output,
		const std::string &reason);
	void poll_automatic_bake();
	void activate(bvh8_data data);
	void change_map(const std::string &map);
	void disable(std::string reason);
	CGameEntitySystem *entity_system() const;
	CEntityInstance *controller(uint32_t slot) const;
	CEntityInstance *pawn(CEntityInstance *controller) const;
	lifecycle_key player_lifecycle(uint32_t slot, CGameEntitySystem *system, live_player *live) const;
	weapon_muzzle_class active_weapon_muzzle_class(CGameEntitySystem *system, CEntityInstance *pawn) const;
	void refresh_aux_visual_cache(CGameEntitySystem *system);
	bool collect_player_visual_group(CGameEntitySystem *system, CEntityInstance *pawn, visual_entity_group &group) const;
	bool group_fully_marked(CGameEntitySystem *system, CBitVec<MAX_EDICTS> *bits, const visual_entity_group &group) const;
	void clear_group(CGameEntitySystem *system, CBitVec<MAX_EDICTS> *bits, const visual_entity_group &group,
		int recipient_slot, hide_reason reason, std::chrono::steady_clock::time_point now);
	void record_hidden_entity(CGameEntitySystem *system, size_t member_index, int edict, const visual_entity_group &group,
		int recipient_slot, hide_reason reason, std::chrono::steady_clock::time_point now);
	bool capture(visibility_snapshot &value);

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
	bool weapon_item_schema_available_ {};
	bool owner_effect_schema_available_ {};
	std::array<aux_visual_entity, k_max_aux_visual_cache_entities> aux_visual_entities_;
	size_t aux_visual_count_ {};
	recent_hide_log recent_hides_;
	std::array<lifecycle_guard, k_max_players> lifecycle_;
	std::array<std::array<pair_guard, k_max_players>, k_max_players> pair_guards_;
	std::array<std::array<visual_entity_group, k_max_players>, k_max_players> hidden_groups_;
	std::array<target_transmit_cache, k_max_players> transmit_target_cache_;
	std::mutex transmit_state_mutex_;
	std::chrono::steady_clock::time_point last_snapshot_ {};
	uint64_t snapshot_sequence_ {};
	bool prerequisites_valid_ {};
};

extern plugin g_plugin;
extern CConVar<bool> cs2fow_enable;
extern CConVar<int> cs2fow_update_interval_ms;
extern CConVar<int> cs2fow_base_lookahead_ms;
extern CConVar<int> cs2fow_max_lookahead_ms;
extern CConVar<int> cs2fow_visibility_hold_ms;
extern CConVar<bool> cs2fow_debug;

} // namespace cs2fow
