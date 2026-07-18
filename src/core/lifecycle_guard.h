#pragma once

// Fixed-size player, pair, visual-group, and quarantine rules shared by game
// capture and CheckTransmit. Identity changes reset to a timed fail-open state;
// these helpers allocate no memory in the transmit hook.

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

inline constexpr size_t k_pair_visual_group_key_max = 132;

struct visual_group_key
{
	std::array<uint32_t, k_pair_visual_group_key_max> values {};
	size_t count {};
};

struct pair_guard
{
	lifecycle_key recipient_key;
	lifecycle_key target_key;
	visual_group_key target_visual_group;
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

inline void pair_reset_baseline(pair_guard &guard)
{
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

inline bool update_pair_guard(pair_guard &guard, const lifecycle_key &recipient_key, bool recipient_stable,
	const lifecycle_key &target_key, bool target_stable)
{
	const bool reset = !guard.initialized || lifecycle_changed(guard.recipient_key, recipient_key)
		|| lifecycle_changed(guard.target_key, target_key) || !recipient_stable || !target_stable;
	if (reset)
	{
		pair_reset_baseline(guard);
		guard.target_visual_group = {};
		guard.visual_group_initialized = false;
	}
	guard.recipient_key = recipient_key;
	guard.target_key = target_key;
	guard.initialized = true;
	return reset;
}

inline void update_pair_visual_group(pair_guard &guard, const visual_group_key &key)
{
	if (!guard.visual_group_initialized || visual_group_changed(guard.target_visual_group, key))
	{
		pair_reset_baseline(guard);
	}
	guard.target_visual_group = key;
	guard.visual_group_initialized = true;
}

inline void pair_note_open(pair_guard &guard, uint64_t sequence)
{
	if (guard.initialized && !guard.baseline_opened)
	{
		guard.baseline_sequence = sequence;
		guard.baseline_opened = true;
	}
}

inline bool pair_allows_hiding(const pair_guard &guard, uint64_t sequence)
{
	return guard.initialized && guard.baseline_opened && guard.baseline_sequence != sequence;
}

template <typename handle_type, size_t max_count>
inline void hidden_group_clear(hidden_entity_group<handle_type, max_count> &group)
{
	group = {};
}

template <typename handle_type, size_t max_count>
inline void hidden_group_store(hidden_entity_group<handle_type, max_count> &group,
	const hidden_entity_group<handle_type, max_count> &source,
	std::chrono::steady_clock::time_point now, std::chrono::milliseconds quarantine)
{
	group = source;
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
inline bool hidden_group_contains(const hidden_entity_group<handle_type, max_count> &group, const handle_type &handle)
{
	for (size_t index = 0; index < group.count; ++index)
	{
		if (group.handles[index] == handle)
		{
			return true;
		}
	}
	return false;
}

template <typename handle_type, size_t max_count>
inline bool hidden_group_append_unique(hidden_entity_group<handle_type, max_count> &group, const handle_type &handle)
{
	if (hidden_group_contains(group, handle))
	{
		return true;
	}
	if (group.count >= group.handles.size())
	{
		return false;
	}
	group.handles[group.count++] = handle;
	return true;
}

} // namespace cs2fow
