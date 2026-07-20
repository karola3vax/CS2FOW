#pragma once

// Plain copied player input and the line-of-sight sampling API. The background
// worker receives current input/body/weapon values and gets bounded recipient
// and target points without touching live CS2 objects.

#include "bvh8.h"

#include <array>
#include <cstdint>

namespace cs2fow
{

inline constexpr uint32_t k_visibility_origin_count_max = 6;
inline constexpr uint32_t k_visibility_body_point_count = 15;
inline constexpr uint32_t k_visibility_target_count_max = 24;
inline constexpr uint32_t k_visibility_ray_count_max = k_visibility_origin_count_max * k_visibility_target_count_max;

inline constexpr uint64_t k_visibility_button_forward = 0x8;
inline constexpr uint64_t k_visibility_button_back = 0x10;
inline constexpr uint64_t k_visibility_button_left = 0x200;
inline constexpr uint64_t k_visibility_button_right = 0x400;

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
	vec3 mins;
	vec3 maxs;
	float eye_yaw_degrees {};
	float rtt_seconds {};
	uint64_t movement_buttons {};
	weapon_muzzle_class muzzle_class {weapon_muzzle_class::none};
	std::array<vec3, k_visibility_body_point_count> body_points {};
	uint32_t body_point_count {};
};

struct visibility_body_binding
{
	const char *bone;
	vec3 local;
};

struct visibility_bone_transform
{
	vec3 position;
	float rotation[4] {};
};

extern const std::array<visibility_body_binding, k_visibility_body_point_count> k_visibility_body_bindings;

struct visibility_tuning
{
	float shoulder_base_units {48.0f};
	float shoulder_rtt_scale {0.4f};
	float max_shoulder_units {128.0f};
};

struct visibility_origin_points
{
	std::array<vec3, k_visibility_origin_count_max> points {};
	uint32_t count {};
};

struct visibility_target_points
{
	std::array<vec3, k_visibility_target_count_max> points {};
	uint32_t count {};
};

float visibility_shoulder_offset_units(float rtt_seconds, const visibility_tuning &tuning);
vec3 visibility_clip_destination(const bvh8_data &data, vec3 origin, vec3 destination);
weapon_muzzle_class weapon_muzzle_class_from_item_definition(uint16_t item_definition);
float weapon_muzzle_length(weapon_muzzle_class value);
bool visibility_transform_body_point(const visibility_bone_transform &transform, vec3 local, vec3 &world);
visibility_origin_points visibility_origins(const bvh8_data &data, const visibility_player &player,
	const visibility_tuning &tuning);
visibility_target_points visibility_targets(const visibility_player &player);

} // namespace cs2fow
