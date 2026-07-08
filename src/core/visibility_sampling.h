#pragma once

#include "bvh8.h"

#include <array>
#include <cstdint>

namespace cs2fow
{

inline constexpr uint32_t k_visibility_origin_count = 10;
inline constexpr uint32_t k_visibility_target_count = 16;
inline constexpr uint32_t k_visibility_ray_count = k_visibility_origin_count * k_visibility_target_count;

struct visibility_player
{
	vec3 eye;
	vec3 origin;
	vec3 velocity;
	vec3 mins;
	vec3 maxs;
	float eye_yaw_degrees {};
	float rtt_seconds {};
};

struct visibility_tuning
{
	uint32_t update_interval_ms {1};
	uint32_t min_lookahead_ms {200};
	uint32_t max_lookahead_ms {500};
	float peek_margin_units {160.0f};
};

float visibility_effective_lookahead_seconds(float rtt_seconds, const visibility_tuning &tuning);
vec3 visibility_prediction_offset(vec3 velocity, float seconds, float peek_margin_units);
std::array<vec3, k_visibility_origin_count> visibility_origins(const bvh8_data &data, const visibility_player &player, const visibility_tuning &tuning, float lookahead_seconds);
std::array<vec3, k_visibility_target_count> visibility_targets(const bvh8_data &data, const visibility_player &player, const visibility_tuning &tuning, float lookahead_seconds);

} // namespace cs2fow
