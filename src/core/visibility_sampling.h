#pragma once

// Plain copied player input and the line-of-sight sampling API. The background
// worker receives current movement/body/weapon values and gets bounded recipient
// and target points without touching live CS2 objects.

#include "bvh8.h"

#include <array>
#include <cstdint>

namespace cs2fow
{

inline constexpr uint32_t k_visibility_origin_count = 8;
inline constexpr uint32_t k_visibility_target_count_max = 40;
inline constexpr uint32_t k_visibility_ray_count_max = k_visibility_origin_count * k_visibility_target_count_max;

enum class weapon_muzzle_class : uint8_t
{
	none,
	pistol,
	smg,
	rifle,
	sniper
};

struct visibility_player
{
	vec3 eye;
	vec3 origin;
	vec3 velocity;
	vec3 mins;
	vec3 maxs;
	float eye_yaw_degrees {};
	float rtt_seconds {};
	weapon_muzzle_class muzzle_class {weapon_muzzle_class::none};
};

struct visibility_tuning
{
	uint32_t base_lookahead_ms {50};
	uint32_t max_lookahead_ms {150};
};

struct visibility_target_points
{
	std::array<vec3, k_visibility_target_count_max> points {};
	uint32_t count {};
};

float visibility_effective_lookahead_seconds(float rtt_seconds, const visibility_tuning &tuning);
vec3 visibility_prediction_offset(vec3 velocity, float seconds);
weapon_muzzle_class weapon_muzzle_class_from_item_definition(uint16_t item_definition);
float weapon_muzzle_length(weapon_muzzle_class value);
std::array<vec3, k_visibility_origin_count> visibility_origins(const bvh8_data &data, const visibility_player &player, float lookahead_seconds);
visibility_target_points visibility_targets(const bvh8_data &data, const visibility_player &player, float lookahead_seconds);

} // namespace cs2fow
