#include "plugin.h"

// Reads live controllers, pawns, weapons, bounds, and visual groups on the game
// thread, then outputs plain copied visibility snapshots. Broken handles,
// lifecycle changes, or incomplete groups reset toward fail-open behavior.

#include <inetchannelinfo.h>
#include <mathlib/transform.h>
#include <tier1/utlvector.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace cs2fow
{
namespace
{

template <typename type>
type &field(void *object, uint32_t offset)
{
	return *reinterpret_cast<type *>(reinterpret_cast<uintptr_t>(object) + offset);
}

bool find_declared_field(SchemaClassInfoData_t *class_info, const char *name, uint32_t &offset)
{
	if (class_info == nullptr)
	{
		return false;
	}
	for (int i = 0; i < class_info->m_nFieldCount; ++i)
	{
		if (std::strcmp(class_info->m_pFields[i].m_pszName, name) == 0)
		{
			offset = class_info->m_pFields[i].m_nSingleInheritanceOffset;
			return true;
		}
	}
	return false;
}

bool resolve_field(ISchemaSystem *schema, const char *class_name, const char *field_name, uint32_t &offset)
{
#if defined(_WIN32)
	CSchemaSystemTypeScope *scope = schema->FindTypeScopeForModule("server.dll");
#else
	CSchemaSystemTypeScope *scope = schema->FindTypeScopeForModule("libserver.so");
#endif
	if (scope == nullptr)
	{
		return false;
	}
	return find_declared_field(scope->FindDeclaredClass(class_name).Get(), field_name, offset);
}

bool resolve_global_field(ISchemaSystem *schema, const char *class_name, const char *field_name, uint32_t &offset)
{
	CSchemaSystemTypeScope *scope = schema->GlobalTypeScope();
	return scope != nullptr && find_declared_field(scope->FindDeclaredClass(class_name).Get(), field_name, offset);
}

vec3 to_vec3(const Vector &value)
{
	return {value.x, value.y, value.z};
}

} // namespace

int entity_index(CEntityInstance *entity)
{
	return entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->m_EHandle.GetEntryIndex() : -1;
}

CEntityHandle entity_handle(CEntityInstance *entity)
{
	return entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->GetRefEHandle() : CEntityHandle {};
}

void copy_entity_name(CEntityInstance *entity, char (&name)[k_max_entity_name])
{
	const char *source = entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->GetClassname() : nullptr;
	if (source == nullptr || source[0] == '\0')
	{
		source = "<unknown>";
	}
	std::snprintf(name, sizeof(name), "%s", source);
}

bool valid_networked_edict_index(int index)
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

bool plugin::resolve_schema(std::string &error)
{
	auto require = [&](uint32_t &target, const char *class_name, const char *field_name)
	{
		if (!resolve_field(schema_, class_name, field_name, target))
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
	auto optional = [&](uint32_t &target, const char *class_name, const char *field_name)
	{
		return resolve_field(schema_, class_name, field_name, target);
	};
	auto require_global = [&](uint32_t &target, const char *class_name, const char *field_name)
	{
		if (!resolve_global_field(schema_, class_name, field_name, target))
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
	require(fields_.movement_services, "CBasePlayerPawn", "m_pMovementServices");
	require(fields_.movement_buttons, "CPlayer_MovementServices", "m_nButtons");
	require_global(fields_.button_states, "CInButtonState", "m_pButtonStates");
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
	const bool weapon_item_schema_available =
		optional(fields_.attribute_manager, "CEconEntity", "m_AttributeManager")
		&& optional(fields_.item, "CAttributeContainer", "m_Item")
		&& optional(fields_.item_definition_index, "CEconItemView", "m_iItemDefinitionIndex");
	require(fields_.wearables, "CBaseCombatCharacter", "m_hMyWearables");
	require(fields_.hostage_services, "CCSPlayerPawn", "m_pHostageServices");
	require(fields_.is_spawning, "CCSPlayerPawn", "m_bIsSpawning");
	require(fields_.death_flags, "CCSPlayerPawn", "m_iDeathFlags");
	require(fields_.has_death_info, "CCSPlayerPawn", "m_bHasDeathInfo");
	require(fields_.death_info_time, "CCSPlayerPawn", "m_flDeathInfoTime");
	require(fields_.carried_hostage_prop, "CCSPlayer_HostageServices", "m_hCarriedHostageProp");
	smoke_schema_available_ = optional(fields_.did_smoke_effect, "CSmokeGrenadeProjectile", "m_bDidSmokeEffect");
	if (!error.empty())
	{
		error = "missing schema fields: " + error;
		return false;
	}
	weapon_item_schema_available_ = weapon_item_schema_available;
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
	if (pawn_entity == nullptr || pawn_controller != controller_entity || !valid_networked_edict_index(key.pawn_entity))
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

weapon_muzzle_class plugin::active_weapon_muzzle_class(CGameEntitySystem *system, CEntityInstance *pawn_entity) const
{
	if (system == nullptr || pawn_entity == nullptr || !weapon_item_schema_available_)
	{
		return weapon_muzzle_class::none;
	}
	void *services = field<void *>(pawn_entity, fields_.weapon_services);
	if (services == nullptr)
	{
		return weapon_muzzle_class::none;
	}
	const CEntityHandle active_weapon = field<CEntityHandle>(services, fields_.active_weapon);
	CEntityInstance *weapon = active_weapon.IsValid() ? system->GetEntityInstance(active_weapon) : nullptr;
	if (weapon == nullptr)
	{
		return weapon_muzzle_class::none;
	}
	void *attribute_manager = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(weapon) + fields_.attribute_manager);
	void *item = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(attribute_manager) + fields_.item);
	const uint16_t definition = field<uint16_t>(item, fields_.item_definition_index);
	return weapon_muzzle_class_from_item_definition(definition);
}

void plugin::collect_smoke_entities(CGameEntitySystem *system,
	std::array<CEntityInstance *, k_max_smoke_volumes> &smokes, size_t &smoke_count, bool &smoke_overflow)
{
	smoke_count = 0;
	smoke_overflow = false;
	if (system == nullptr)
	{
		return;
	}
	CEntityIdentity *identity = system->m_EntityList.m_pFirstActiveEntity;
	for (uint32_t scanned = 0; identity != nullptr && scanned < k_entity_scan_hard_limit; identity = identity->m_pNext, ++scanned)
	{
		CEntityInstance *entity = identity->m_pInstance;
		const int edict = entity_index(entity);
		if (!valid_networked_edict_index(edict))
		{
			continue;
		}
		const char *classname = entity != nullptr && entity->m_pEntity != nullptr ? entity->m_pEntity->GetClassname() : nullptr;
		if (smoke_schema_available_ && smoke_gamedata_available_ && classname != nullptr
			&& std::strcmp(classname, "smokegrenade_projectile") == 0
			&& field<bool>(entity, fields_.did_smoke_effect))
		{
			if (smoke_count < smokes.size())
			{
				smokes[smoke_count++] = entity;
			}
			else
			{
				smoke_overflow = true;
			}
		}
	}
}

bool plugin::capture_smokes(const std::array<CEntityInstance *, k_max_smoke_volumes> &entities, size_t count,
	bool overflow, float game_time, visibility_snapshot &value)
{
	if (overflow || !std::isfinite(game_time))
	{
		return false;
	}
	auto snapshot = std::make_shared<smoke_snapshot>();
	snapshot->he_clear_radius_units = cs2fow_he_clear_radius_units.Get();
	snapshot->he_clear_seconds = cs2fow_he_clear_seconds.Get();
	if (snapshot->he_clear_radius_units > 0.0f && snapshot->he_clear_seconds > 0.0f)
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		for (uint32_t index = 0; index < he_clearance_history_.count; ++index)
		{
			const live_he_clearance &clearance = he_clearance_history_.records[index];
			const float age = game_time - clearance.detonation_time;
			if (age >= 0.0f && age < snapshot->he_clear_seconds)
			{
				snapshot->he_clearances[snapshot->he_clearance_count++] = {clearance.center, age, clearance.detonation_time};
			}
		}
	}
	snapshot->volumes.reserve(count);
	for (size_t index = 0; index < count; ++index)
	{
		CEntityInstance *entity = entities[index];
		if (entity == nullptr)
		{
			return false;
		}
		auto *volume = reinterpret_cast<std::byte *>(entity) + smoke_layout_.volume;
		const vec3 center = to_vec3(field<Vector>(volume, smoke_layout_.center));
		const float start_time = field<float>(volume, smoke_layout_.start_time);
		const auto *storage = field<std::byte *>(volume, smoke_layout_.storage);
		snapshot->volumes.emplace_back();
		if (!copy_stable_smoke_frame(storage, center, game_time - start_time, snapshot->volumes.back(),
			[&] { return field<int32_t>(volume, smoke_layout_.frame); }))
		{
			return false;
		}
		snapshot->volumes.back().start_time = start_time;
	}
	value.smokes = std::move(snapshot);
	return true;
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
		if (!valid_networked_edict_index(resolve_entity_index(system, handle)))
		{
			return false;
		}
		return hidden_group_append_unique(group, handle);
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

bool plugin::capture_animated_body_points(CEntityInstance *pawn, uint32_t slot, player_state &player,
	std::chrono::steady_clock::time_point now)
{
	if (pawn == nullptr || slot >= player_bone_cache_.size() || lookup_bone_ == nullptr || get_bone_transform_ == nullptr)
	{
		return false;
	}
	player_bone_cache &cache = player_bone_cache_[slot];
	if (cache.pawn != pawn || (!cache.valid && now >= cache.retry_after))
	{
		cache = {};
		cache.pawn = pawn;
		cache.retry_after = now + std::chrono::seconds(1);
		cache.valid = true;
		const auto lookup = reinterpret_cast<int32_t (*)(void *, const char *)>(lookup_bone_);
		for (size_t point = 0; point < cache.indices.size(); ++point)
		{
			cache.indices[point] = lookup(pawn, k_visibility_body_bindings[point].bone);
			if (cache.indices[point] < 0)
			{
				cache.valid = false;
			}
		}
	}
	if (!cache.valid)
	{
		return false;
	}

	std::array<vec3, k_visibility_body_point_count> points;
	constexpr float k_max_body_point_distance_sq = 128.0f * 128.0f;
	for (size_t point = 0; point < points.size(); ++point)
	{
		CTransform transform;
		const float invalid = std::numeric_limits<float>::quiet_NaN();
		transform.m_vPosition.Init(invalid, invalid, invalid);
		transform.m_orientation.Init(invalid, invalid, invalid, invalid);
#if defined(_WIN32)
		reinterpret_cast<void (*)(void *, CTransform *, int32_t)>(get_bone_transform_)(pawn, &transform, cache.indices[point]);
#else
		reinterpret_cast<void (*)(CTransform *, void *, int32_t)>(get_bone_transform_)(&transform, pawn, cache.indices[point]);
#endif
		const visibility_bone_transform copied {
			to_vec3(transform.m_vPosition),
			{transform.m_orientation.x, transform.m_orientation.y, transform.m_orientation.z, transform.m_orientation.w}
		};
		if (!visibility_transform_body_point(copied, k_visibility_body_bindings[point].local, points[point]))
		{
			return false;
		}
		const float x = points[point].x - player.origin.x;
		const float y = points[point].y - player.origin.y;
		const float z = points[point].z - player.origin.z;
		if (x * x + y * y + z * z > k_max_body_point_distance_sq)
		{
			return false;
		}
	}
	player.body_points = points;
	player.body_point_count = static_cast<uint32_t>(points.size());
	return true;
}

bool plugin::capture(visibility_snapshot &value, float game_time)
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
	std::array<CEntityInstance *, k_max_players> animated_pawns {};
	value.filter_teammates = visibility_teammate_filter_enabled(
		cs2fow_filter_teammates.Get(), teammates_are_enemies());
	value.smoke_enabled = cs2fow_smoke_occlusion.Get();
	value.smoke_available = smoke_schema_available_ && smoke_gamedata_available_;
	if (value.smoke_enabled && value.smoke_available)
	{
		std::array<CEntityInstance *, k_max_smoke_volumes> smoke_entities {};
		size_t smoke_count = 0;
		bool smoke_overflow = false;
		collect_smoke_entities(system, smoke_entities, smoke_count, smoke_overflow);
		if (!capture_smokes(smoke_entities, smoke_count, smoke_overflow, game_time, value))
		{
			value.smoke_available = false;
			value.smokes.reset();
		}
	}
	std::unique_lock<std::mutex> lock(transmit_state_mutex_);
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
		player_state player;
		player.team = live.team;
		player.pawn_entity = live.pawn_entity;
		player.origin = to_vec3(field<Vector>(scene_node, fields_.abs_origin));
		player.mins = to_vec3(field<Vector>(collision, fields_.mins));
		player.maxs = to_vec3(field<Vector>(collision, fields_.maxs));
		void *view = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(pawn_entity) + fields_.view_offset);
		player.eye = {player.origin.x + field<float>(view, fields_.view_x), player.origin.y + field<float>(view, fields_.view_y), player.origin.z + field<float>(view, fields_.view_z)};
		player.eye_yaw_degrees = field<qangle>(pawn_entity, fields_.eye_angles).y;
		if (void *movement = field<void *>(pawn_entity, fields_.movement_services); movement != nullptr)
		{
			void *buttons = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(movement) + fields_.movement_buttons);
			player.movement_buttons = field<uint64_t>(buttons, fields_.button_states);
		}
		player.muzzle_class = active_weapon_muzzle_class(system, pawn_entity);
		if (INetChannelInfo *channel = engine_->GetPlayerNetInfo(CPlayerSlot(static_cast<int>(slot))); channel != nullptr)
		{
			player.rtt_seconds = channel->GetEngineLatency();
		}
		if (!valid_player_numbers(player))
		{
			continue;
		}
		player.rtt_seconds = std::max(0.0f, player.rtt_seconds);
		player.valid = true;
		value.players[slot] = player;
		animated_pawns[slot] = pawn_entity;
	}
	const auto bones_started = std::chrono::steady_clock::now();
	uint32_t animated_players = 0;
	uint32_t static_fallback_players = 0;
	for (uint32_t slot = 0; slot < k_max_players; ++slot)
	{
		if (!value.players[slot].valid)
		{
			player_bone_cache_[slot] = {};
			continue;
		}
		capture_animated_body_points(animated_pawns[slot], slot, value.players[slot], now)
			? ++animated_players : ++static_fallback_players;
	}
	bone_timing_.record(std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - bones_started).count());
	animated_players_ = animated_players;
	static_fallback_players_ = static_fallback_players;
	for (uint32_t recipient = 0; recipient < k_max_players; ++recipient)
	{
		for (uint32_t target = 0; target < k_max_players; ++target)
		{
			if (update_pair_guard(pair_guards_[recipient][target], keys[recipient], stable_slots[recipient],
				keys[target], stable_slots[target])
				&& hidden_groups_[recipient][target].count != 0)
			{
				hidden_group_clear(hidden_groups_[recipient][target]);
			}
		}
	}
	return true;
}

} // namespace cs2fow
