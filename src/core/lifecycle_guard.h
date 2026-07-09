#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace cs2fow
{

struct lifecycle_key
{
	bool has_controller {};
	bool hltv {};
	int pawn_entity {-1};
	uint8_t team {};
	bool alive {};
	bool spawning {};
	int32_t death_flags {};
	bool has_death_info {};
	float death_time {};
	float death_info_time {};
};

struct lifecycle_guard
{
	lifecycle_key key;
	std::chrono::steady_clock::time_point fail_open_until {};
	bool initialized {};
};

inline constexpr size_t k_pair_visual_group_key_max = 128;

struct visual_group_key
{
	std::array<uint32_t, k_pair_visual_group_key_max> values {};
	size_t count {};
};

struct pair_guard
{
	lifecycle_key observer_key;
	lifecycle_key target_key;
	visual_group_key target_visual_group;
	std::chrono::steady_clock::time_point fail_open_until {};
	uint64_t baseline_sequence {};
	bool baseline_opened {};
	bool visual_group_initialized {};
	bool initialized {};
};

template <typename handle_type, size_t max_count>
struct hidden_entity_group
{
	std::array<handle_type, max_count> handles {};
	handle_type source {};
	size_t count {};
	std::chrono::steady_clock::time_point quarantine_until {};
};

inline bool lifecycle_changed(const lifecycle_key &left, const lifecycle_key &right)
{
	return left.has_controller != right.has_controller
		|| left.hltv != right.hltv
		|| left.pawn_entity != right.pawn_entity
		|| left.team != right.team
		|| left.alive != right.alive
		|| left.spawning != right.spawning
		|| left.death_flags != right.death_flags
		|| left.has_death_info != right.has_death_info
		|| left.death_time != right.death_time
		|| left.death_info_time != right.death_info_time;
}

inline bool visual_group_changed(const visual_group_key &left, const visual_group_key &right)
{
	if (left.count != right.count)
	{
		return true;
	}
	for (size_t index = 0; index < left.count; ++index)
	{
		if (left.values[index] != right.values[index])
		{
			return true;
		}
	}
	return false;
}

template <size_t max_count>
inline visual_group_key make_visual_group_key(const std::array<uint32_t, max_count> &values, size_t count)
{
	visual_group_key key;
	key.count = std::min(count, k_pair_visual_group_key_max);
	for (size_t index = 0; index < key.count; ++index)
	{
		key.values[index] = values[index];
	}
	std::sort(key.values.begin(), key.values.begin() + key.count);
	const auto end = std::unique(key.values.begin(), key.values.begin() + key.count);
	key.count = static_cast<size_t>(end - key.values.begin());
	return key;
}

inline void pair_reset_baseline(pair_guard &guard, std::chrono::steady_clock::time_point now, std::chrono::milliseconds warmup)
{
	guard.fail_open_until = now + warmup;
	guard.baseline_sequence = 0;
	guard.baseline_opened = false;
}

inline void update_lifecycle_guard(lifecycle_guard &guard, const lifecycle_key &key, bool stable,
	std::chrono::steady_clock::time_point now, std::chrono::milliseconds grace)
{
	if (!guard.initialized || lifecycle_changed(guard.key, key) || !stable)
	{
		guard.fail_open_until = now + grace;
	}
	guard.key = key;
	guard.initialized = true;
}

inline bool lifecycle_allows_hiding(const lifecycle_guard &guard, std::chrono::steady_clock::time_point now)
{
	return guard.initialized && now >= guard.fail_open_until;
}

inline void update_pair_guard(pair_guard &guard, const lifecycle_key &observer_key, bool observer_stable,
	const lifecycle_key &target_key, bool target_stable, std::chrono::steady_clock::time_point now, std::chrono::milliseconds warmup)
{
	if (!guard.initialized || lifecycle_changed(guard.observer_key, observer_key)
		|| lifecycle_changed(guard.target_key, target_key) || !observer_stable || !target_stable)
	{
		pair_reset_baseline(guard, now, warmup);
		guard.target_visual_group = {};
		guard.visual_group_initialized = false;
	}
	guard.observer_key = observer_key;
	guard.target_key = target_key;
	guard.initialized = true;
}

inline void update_pair_visual_group(pair_guard &guard, const visual_group_key &key,
	std::chrono::steady_clock::time_point now, std::chrono::milliseconds warmup)
{
	if (!guard.visual_group_initialized || visual_group_changed(guard.target_visual_group, key))
	{
		pair_reset_baseline(guard, now, warmup);
	}
	guard.target_visual_group = key;
	guard.visual_group_initialized = true;
}

inline void pair_note_open(pair_guard &guard, std::chrono::steady_clock::time_point now, uint64_t sequence)
{
	if (guard.initialized && now >= guard.fail_open_until && !guard.baseline_opened)
	{
		guard.baseline_sequence = sequence;
		guard.baseline_opened = true;
	}
}

inline bool pair_allows_hiding(const pair_guard &guard, std::chrono::steady_clock::time_point now, uint64_t sequence)
{
	return guard.initialized && now >= guard.fail_open_until && guard.baseline_opened && guard.baseline_sequence != sequence;
}

template <typename handle_type, size_t max_count>
inline void hidden_group_clear(hidden_entity_group<handle_type, max_count> &group)
{
	group = {};
}

template <typename handle_type, size_t max_count>
inline void hidden_group_store(hidden_entity_group<handle_type, max_count> &group,
	const handle_type &source, const std::array<handle_type, max_count> &handles, size_t count,
	std::chrono::steady_clock::time_point now, std::chrono::milliseconds quarantine)
{
	group.source = source;
	group.handles = handles;
	group.count = count > max_count ? max_count : count;
	group.quarantine_until = now + quarantine;
}

template <typename handle_type, size_t max_count>
inline bool hidden_group_quarantined(hidden_entity_group<handle_type, max_count> &group, std::chrono::steady_clock::time_point now)
{
	if (group.count == 0)
	{
		return false;
	}
	if (now >= group.quarantine_until)
	{
		hidden_group_clear(group);
		return false;
	}
	return true;
}

template <typename handle_type, size_t max_count, typename predicate_type>
inline bool hidden_group_all_of(const hidden_entity_group<handle_type, max_count> &group, predicate_type predicate)
{
	if (group.count == 0)
	{
		return false;
	}
	for (size_t index = 0; index < group.count; ++index)
	{
		if (!predicate(group.handles[index]))
		{
			return false;
		}
	}
	return true;
}

template <typename handle_type, size_t max_count>
inline bool hidden_group_contains(const hidden_entity_group<handle_type, max_count> &group, const handle_type &handle, size_t count)
{
	const size_t bounded = std::min(count, group.count);
	for (size_t index = 0; index < bounded; ++index)
	{
		if (group.handles[index] == handle)
		{
			return true;
		}
	}
	return false;
}

template <typename handle_type>
struct owner_effect_link
{
	handle_type child {};
	handle_type owner {};
	handle_type effect {};
};

template <typename handle_type, size_t max_count, typename link_type, typename predicate_type>
inline bool hidden_group_append_owner_effect_links(hidden_entity_group<handle_type, max_count> &group,
	const link_type *links, size_t count, predicate_type usable_child)
{
	const size_t base_count = group.count;
	for (size_t index = 0; index < count; ++index)
	{
		const link_type &link = links[index];
		if (!usable_child(link.child) || hidden_group_contains(group, link.child, group.count))
		{
			continue;
		}
		if (!hidden_group_contains(group, link.owner, base_count) && !hidden_group_contains(group, link.effect, base_count))
		{
			continue;
		}
		if (group.count >= group.handles.size())
		{
			return false;
		}
		group.handles[group.count++] = link.child;
	}
	return true;
}

} // namespace cs2fow
