#pragma once

// Owns the background ray-casting thread. The game thread gives it copied
// player data; it publishes immutable visibility results and never reads live
// CS2 objects. New pending work replaces old pending work instead of queuing.

#include "bvh8.h"
#include "capsule_visibility.h"
#include "smoke_occlusion.h"
#include "visibility_sampling.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace cs2fow
{

inline constexpr uint32_t k_max_players = 64;

struct player_state
{
	bool valid {};
	uint8_t team {};
	vec3 eye;
	vec3 origin;
	vec3 mins;
	vec3 maxs;
	float eye_yaw_degrees {};
	float rtt_seconds {};
	uint64_t movement_buttons {};
	weapon_muzzle_class muzzle_class {weapon_muzzle_class::none};
	std::array<visibility_capsule, k_visibility_capsule_count> capsules {};
	uint32_t capsule_count {};
	int pawn_entity {-1};
};

inline visibility_player visibility_sample(const player_state &player)
{
	return {player.eye, player.origin, player.mins, player.maxs, player.eye_yaw_degrees, player.rtt_seconds,
		player.movement_buttons, player.muzzle_class, player.capsules, player.capsule_count};
}

inline bool valid_player_numbers(const player_state &player)
{
	const auto finite = [](vec3 value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	};
	return finite(player.origin) && finite(player.eye) && finite(player.mins) && finite(player.maxs)
		&& std::isfinite(player.eye_yaw_degrees) && std::isfinite(player.rtt_seconds)
		&& player.mins.x <= player.maxs.x && player.mins.y <= player.maxs.y && player.mins.z <= player.maxs.z
		&& (player.capsule_count == 0 || (player.capsule_count == player.capsules.size()
			&& std::all_of(player.capsules.begin(), player.capsules.end(), valid_visibility_capsule)));
}

inline bool visibility_pair_enabled(uint32_t recipient, uint32_t target, const player_state &from,
	const player_state &to, bool filter_teammates)
{
	return from.valid && to.valid && recipient != target && (filter_teammates || from.team != to.team);
}

inline bool visibility_teammate_filter_enabled(bool configured, bool teammates_are_enemies)
{
	return configured || teammates_are_enemies;
}

struct visibility_snapshot
{
	uint64_t sequence {};
	std::chrono::steady_clock::time_point captured;
	bool filter_teammates {};
	bool smoke_enabled {};
	bool smoke_available {};
	std::shared_ptr<const smoke_snapshot> smokes;
	player_state players[k_max_players];
};

struct visibility_result
{
	uint64_t sequence {};
	std::chrono::steady_clock::time_point captured;
	std::chrono::steady_clock::time_point completed;
	player_state players[k_max_players];
	bool filter_teammates {};
	bool smoke_enabled {};
	bool smoke_available {};
	uint32_t smoke_count {};
	uint32_t he_clearance_count {};
	bool visible[k_max_players][k_max_players] {};
	double worker_ms {};
	uint32_t evaluated_pairs {};
	uint32_t visible_pairs {};
	uint32_t hidden_pairs {};
	uint32_t sampled_pixels {};
	uint32_t traced_rays {};
	uint32_t visited_nodes {};
	uint32_t rasterized_triangles {};
	bool budget_exhausted {};
};

inline bool visibility_snapshot_fresh(std::chrono::steady_clock::time_point captured,
	std::chrono::steady_clock::time_point now)
{
	return now - captured <= std::chrono::milliseconds(100);
}

struct worker_stats
{
	double latest_ms {};
	double average_ms {};
	double maximum_ms {};
	uint64_t cycles {};
	uint32_t evaluated_pairs {};
	uint32_t visible_pairs {};
	uint32_t hidden_pairs {};
	uint32_t sampled_pixels {};
	uint32_t traced_rays {};
	uint32_t visited_nodes {};
	uint32_t rasterized_triangles {};
	uint64_t budget_exhaustions {};
};

class visibility_worker
{
public:
	~visibility_worker();
	bool start(const bvh8_data *data);
	void stop();
	void submit(visibility_snapshot value, uint32_t hold_ms, visibility_tuning tuning);
	std::shared_ptr<const visibility_result> result() const;
	worker_stats stats() const;

private:
	void run();

	const bvh8_data *data_ {};
	mutable std::mutex mutex_;
	std::condition_variable condition_;
	std::optional<visibility_snapshot> pending_;
	std::atomic_bool stopping_ {true};
	uint32_t hold_ms_ {};
	visibility_tuning tuning_;
	std::thread thread_;
	std::shared_ptr<const visibility_result> published_;
	std::array<std::array<std::array<uint32_t, k_visibility_origin_count_max>, k_max_players>, k_max_players> cached_packets_ {};
	std::array<std::array<std::array<capsule_occluder_cache,
		k_visibility_origin_count_max>, k_max_players>, k_max_players> cached_occluders_ {};
	std::array<std::array<std::chrono::steady_clock::time_point, k_max_players>, k_max_players> revealed_until_ {};
	mutable std::mutex stats_mutex_;
	worker_stats stats_;
};

} // namespace cs2fow
