#include "plugin.h"

// Turns a fresh visibility result into primary-list clears for verified enemy
// visual groups. CheckTransmit holds the plugin transmit-state lock, skips full
// updates, allocates nothing, and fails open on stale or mismatched live state.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

namespace cs2fow
{
namespace
{

visual_group_key make_current_visual_group_key(const visual_entity_group &group)
{
	std::array<uint32_t, k_max_hidden_player_entities> values {};
	for (size_t index = 0; index < group.count; ++index)
	{
		values[index] = static_cast<uint32_t>(group.handles[index].ToInt());
	}
	return make_visual_group_key(values, group.count);
}

uint8_t hide_reason_mask(hide_reason reason)
{
	return reason == hide_reason::quarantine ? k_transmit_reason_quarantine : k_transmit_reason_current;
}

const char *transmit_member_name(transmit_member_kind member)
{
	switch (member)
	{
		case transmit_member_kind::owner_link: return "owner_link";
		case transmit_member_kind::effect_link: return "effect_link";
		case transmit_member_kind::owner_effect_link: return "owner+effect";
		default: return "direct";
	}
}

const char *transmit_reason_name(uint8_t reasons)
{
	if (reasons == (k_transmit_reason_current | k_transmit_reason_quarantine)) return "current+quarantine";
	return (reasons & k_transmit_reason_quarantine) != 0 ? "quarantine" : "current";
}

void format_recipients(uint64_t mask, char (&text)[256])
{
	text[0] = '\0';
	size_t used = 0;
	for (int slot = 0; slot < 64; ++slot)
	{
		if ((mask & (uint64_t {1} << slot)) == 0)
		{
			continue;
		}
		const int written = std::snprintf(text + used, sizeof(text) - used, "%s%d", used == 0 ? "" : ",", slot);
		if (written < 0 || static_cast<size_t>(written) >= sizeof(text) - used)
		{
			break;
		}
		used += static_cast<size_t>(written);
	}
	if (used == 0)
	{
		std::snprintf(text, sizeof(text), "-");
	}
}

} // namespace

bool plugin::group_fully_marked(CGameEntitySystem *system, CBitVec<MAX_EDICTS> *bits, const visual_entity_group &group) const
{
	if (bits == nullptr || group.count == 0)
	{
		return false;
	}
	return hidden_group_all_of(group, [&](CEntityHandle handle)
	{
		const int index = resolve_entity_index(system, handle);
		return valid_networked_edict_index(index) && bits->IsBitSet(index);
	});
}

void plugin::record_hidden_entity(CGameEntitySystem *system, size_t member_index, int edict,
	const visual_entity_group &group, int recipient_slot, hide_reason reason, std::chrono::steady_clock::time_point now)
{
	const CEntityHandle handle = group.handles[member_index];
	const CEntityHandle owner = group.link_owners[member_index];
	const CEntityHandle effect = group.link_effects[member_index];
	const bool owner_link = owner.IsValid();
	const bool effect_link = effect.IsValid();
	const transmit_member_kind member = transmit_member_from_links(owner_link, effect_link);
	char name[k_max_entity_name] {};
	copy_entity_name(system == nullptr ? nullptr : system->GetEntityInstance(handle), name);
	recent_hides_.record({
		edict,
		static_cast<uint32_t>(handle.ToInt()),
		static_cast<uint32_t>(group.source.ToInt()),
		owner_link ? static_cast<uint32_t>(owner.ToInt()) : 0u,
		effect_link ? static_cast<uint32_t>(effect.ToInt()) : 0u,
		recipient_slot,
		member,
		hide_reason_mask(reason),
		now
	}, name);
}

void plugin::clear_group(CGameEntitySystem *system, CBitVec<MAX_EDICTS> *bits,
	const visual_entity_group &group, int recipient_slot, hide_reason reason, std::chrono::steady_clock::time_point now)
{
	if (bits == nullptr)
	{
		return;
	}
	const bool debug = cs2fow_debug.Get();
	for (size_t entity = 0; entity < group.count; ++entity)
	{
		const CEntityHandle handle = group.handles[entity];
		const int index = resolve_entity_index(system, handle);
		if (!valid_networked_edict_index(index))
		{
			continue;
		}
		if (clear_transmit_bit(*bits, index, debug))
		{
			record_hidden_entity(system, entity, index, group, recipient_slot, reason, now);
		}
	}
}

void plugin::reset_transmit_state(bool clear_debug_records)
{
	std::lock_guard<std::mutex> lock(transmit_state_mutex_);
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
	aux_visual_count_ = 0;
	for (aux_visual_entity &entity : aux_visual_entities_)
	{
		entity = {};
	}
	if (clear_debug_records)
	{
		recent_hides_.clear();
	}
}

void plugin::hook_check_transmit(CCheckTransmitInfo **infos, int count, CBitVec<MAX_EDICTS> &, CBitVec<MAX_EDICTS> &, const Entity2Networkable_t **, const uint16 *, int)
{
	if (!cs2fow_enable.Get() || !disabled_reason_.empty() || infos == nullptr || count <= 0 || count > static_cast<int>(k_max_players))
	{
		return;
	}
	const std::shared_ptr<const visibility_result> result = worker_.result();
	const auto stale_after = std::chrono::milliseconds(std::max(100, 3 * cs2fow_update_interval_ms.Get()));
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(transmit_state_mutex_);
	for (int i = 0; i < count; ++i)
	{
		CCheckTransmitInfo *info = infos[i];
		if (info == nullptr || !read_checktransmit_full_update(info, transmit_offsets_.full_update_offset))
		{
			continue;
		}
		int slot = -1;
		std::memcpy(&slot, reinterpret_cast<const char *>(info) + recipient_slot_offset_, sizeof(slot));
		if (slot < 0 || slot >= static_cast<int>(k_max_players))
		{
			continue;
		}
		for (visual_entity_group &group : hidden_groups_[slot])
		{
			hidden_group_clear(group);
		}
	}
	if (!result || now - result->completed > stale_after)
	{
		return;
	}
	CGameEntitySystem *system = entity_system();
	if (system == nullptr)
	{
		return;
	}
	const auto current_player_pawn = [&](uint32_t slot, const player_state &saved)
	{
		if (!saved.valid)
		{
			return static_cast<CEntityInstance *>(nullptr);
		}
		live_player live;
		const lifecycle_key key = player_lifecycle(slot, system, &live);
		if (live.pawn == nullptr || !lifecycle_allows_hiding(lifecycle_[slot], now)
			|| lifecycle_changed(lifecycle_[slot].key, key) || key.pawn_entity != saved.pawn_entity || key.team != saved.team)
		{
			return static_cast<CEntityInstance *>(nullptr);
		}
		return live.pawn;
	};
	for (uint32_t target = 0; target < k_max_players; ++target)
	{
		target_transmit_cache &cache = transmit_target_cache_[target];
		cache.pawn = current_player_pawn(target, result->players[target]);
		cache.group_valid = cache.pawn != nullptr && collect_player_visual_group(system, cache.pawn, cache.group);
		if (cache.group_valid)
		{
			cache.group_key = make_current_visual_group_key(cache.group);
		}
	}
	for (int i = 0; i < count; ++i)
	{
		CCheckTransmitInfo *info = infos[i];
		if (info == nullptr || info->m_pTransmitEntity == nullptr)
		{
			continue;
		}
		int slot = -1;
		std::memcpy(&slot, reinterpret_cast<const char *>(info) + recipient_slot_offset_, sizeof(slot));
		if (slot < 0 || slot >= static_cast<int>(k_max_players) || read_checktransmit_full_update(info, transmit_offsets_.full_update_offset)
			|| !result->players[slot].valid)
		{
			continue;
		}
		const player_state &recipient = result->players[slot];
		if (transmit_target_cache_[slot].pawn == nullptr)
		{
			continue;
		}
		for (uint32_t target = 0; target < k_max_players; ++target)
		{
			const player_state &player = result->players[target];
			const target_transmit_cache &cache = transmit_target_cache_[target];
			visual_entity_group &stored_group = hidden_groups_[slot][target];
			if (stored_group.count != 0 && now >= stored_group.quarantine_until)
			{
				hidden_group_clear(stored_group);
			}
			if (!player.valid || player.team == recipient.team || cache.pawn == nullptr)
			{
				continue;
			}
			pair_guard &guard = pair_guards_[slot][target];
			const bool full_group_marked = cache.group_valid && group_fully_marked(system, info->m_pTransmitEntity, cache.group);
			if (cache.group_valid)
			{
				update_pair_visual_group(guard, cache.group_key, now, k_pair_baseline_warmup);
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
			if (!cache.group_valid)
			{
				if (hidden_group_quarantined(stored_group, now))
				{
					clear_group(system, info->m_pTransmitEntity, stored_group, slot, hide_reason::quarantine, now);
				}
				continue;
			}
			hidden_group_store(stored_group, cache.group, now, k_hidden_entity_quarantine);
			clear_group(system, info->m_pTransmitEntity, cache.group, slot, hide_reason::current, now);
		}
	}
}

void plugin::print_entities(int edict)
{
	if (edict >= 0 && !valid_networked_edict_index(edict))
	{
		META_CONPRINTF("[CS2FOW] invalid edict index: %d\n", edict);
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(transmit_state_mutex_);
	std::array<const recent_hide_log::record_type *, k_max_recent_hide_records> matches {};
	size_t count = 0;
	for (const recent_hide_log::record_type &record : recent_hides_.records())
	{
		if (!record.valid || (edict >= 0 && record.edict != edict))
		{
			continue;
		}
		matches[count++] = &record;
	}
	std::sort(matches.begin(), matches.begin() + count, [](const recent_hide_log::record_type *left, const recent_hide_log::record_type *right)
	{
		return left->last_seen > right->last_seen;
	});
	META_CONPRINTF("[CS2FOW] entity debug recording=%s records=%zu filter=%s\n",
		cs2fow_debug.Get() ? "on" : "off", count, edict < 0 ? "all" : "edict");
	for (size_t index = 0; index < count; ++index)
	{
		const recent_hide_log::record_type &record = *matches[index];
		char recipients[256] {};
		format_recipients(record.recipients, recipients);
		const double first_age_ms = std::chrono::duration<double, std::milli>(now - record.first_seen).count();
		const double last_age_ms = std::chrono::duration<double, std::milli>(now - record.last_seen).count();
		META_CONPRINTF("[CS2FOW] entity %d class=%s member=%s handle=0x%x source=0x%x owner=0x%x effect=0x%x recipients=%s reasons=%s clears=%llu first_age=%.0fms last_age=%.0fms\n",
			record.edict, record.name.data(), transmit_member_name(record.member), record.handle, record.source, record.owner, record.effect,
			recipients, transmit_reason_name(record.reasons), static_cast<unsigned long long>(record.clears), first_age_ms, last_age_ms);
	}
	if (count == 0)
	{
		META_CONPRINTF("[CS2FOW] no actual transmit clears recorded%s\n", edict < 0 ? "" : " for that edict");
	}
}

void plugin::clear_entity_records()
{
	std::lock_guard<std::mutex> lock(transmit_state_mutex_);
	recent_hides_.clear();
	META_CONPRINTF("[CS2FOW] entity debug records cleared\n");
}

} // namespace cs2fow
