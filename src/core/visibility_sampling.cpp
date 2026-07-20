#include "visibility_sampling.h"

// Builds current eye/input origins plus body, axis-aligned box, and weapon-muzzle
// points from copied player numbers. Input origins stop at baked walls; all
// returned counts remain inside fixed arrays used by the worker.

#include <algorithm>
#include <cmath>

namespace cs2fow
{
namespace
{

constexpr float k_horizontal_bounds_padding = 8.0f;
constexpr float k_top_bounds_padding = 8.0f;
constexpr float k_vertical_origin_offset = 16.0f;
constexpr float k_ping_step_ms = 25.0f;
constexpr float k_same_point_epsilon_sq = 1.0e-4f;
constexpr uint32_t k_wall_clip_steps = 8;
constexpr float k_degrees_to_radians = 0.017453292519943295769f;
constexpr float k_standing_player_height = 72.0f;
constexpr float k_pelvis_height = 38.0f;
constexpr float k_muzzle_z = 60.0f;

struct body_point
{
	vec3 local;
};

constexpr std::array<body_point, k_visibility_body_point_count> k_body_points {{
	{{5.609201635493794f, -1.4428278502142438f, 64.2012733036622f}},
	{{2.0125293444485384f, 2.7306012182339385f, 59.938710028873956f}},
	{{0.0f, 3.6606043089445834f, 54.0f}},
	{{-3.4531053226609565f, 5.946114299110735f, 38.0f}},
	{{6.649097464536467f, 9.206736663527453f, 61.50515964236403f}},
	{{-4.609436263442105f, -6.65497499510368f, 62.674399985034256f}},
	{{2.023447514568512f, 12.575529525946793f, 38.476983901746856f}},
	{{-3.5065278282472674f, -5.103061971464753f, 38.0f}},
	{{11.492137476801554f, 6.0f, 22.0f}},
	{{-4.297040890272927f, -6.0f, 22.0f}},
	{{11.870334433375513f, 10.522994945593906f, 4.0f}},
	{{-11.849791908865742f, -5.0f, 4.0f}},
	{{0.0f, -10.546890805234906f, 51.22609251649996f}},
	{{16.97650970898366f, 6.7731795517149544f, 51.74577989786342f}},
	{{-1.738377928258503f, 4.30848079861881f, 46.753597185311996f}}
}};

float distance_sq(vec3 a, vec3 b)
{
	const float x = a.x - b.x;
	const float y = a.y - b.y;
	const float z = a.z - b.z;
	return x * x + y * y + z * z;
}

vec3 add(vec3 a, vec3 b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

vec3 scale(vec3 value, float amount)
{
	return {value.x * amount, value.y * amount, value.z * amount};
}

vec3 subtract(vec3 a, vec3 b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

vec3 eye_right(float yaw_degrees)
{
	const float yaw = yaw_degrees * k_degrees_to_radians;
	return {std::sin(yaw), -std::cos(yaw), 0.0f};
}

vec3 eye_forward(float yaw_degrees)
{
	const float yaw = yaw_degrees * k_degrees_to_radians;
	return {std::cos(yaw), std::sin(yaw), 0.0f};
}

bounds player_bounds(const visibility_player &player, vec3 origin)
{
	return {
		{origin.x + player.mins.x - k_horizontal_bounds_padding,
			origin.y + player.mins.y - k_horizontal_bounds_padding,
			origin.z + player.mins.z},
		{origin.x + player.maxs.x + k_horizontal_bounds_padding,
			origin.y + player.maxs.y + k_horizontal_bounds_padding,
			origin.z + player.maxs.z + k_top_bounds_padding}
	};
}

vec3 safe_origin(const bvh8_data &data, vec3 eye, vec3 candidate)
{
	if (distance_sq(eye, candidate) <= k_same_point_epsilon_sq)
	{
		return eye;
	}
	if (segment_blocked(data, eye, candidate).blocked)
	{
		return eye;
	}
	return candidate;
}

float adjusted_local_z(const visibility_player &player, float z)
{
	const float height = std::max(1.0f, player.maxs.z - player.mins.z);
	if (z < k_pelvis_height)
	{
		return player.mins.z + z;
	}
	const float standing_upper = k_standing_player_height - k_pelvis_height;
	const float live_upper = std::max(0.0f, height - k_pelvis_height);
	return player.mins.z + k_pelvis_height + (z - k_pelvis_height) * live_upper / standing_upper;
}

vec3 local_to_world(const visibility_player &player, vec3 origin, vec3 local)
{
	const vec3 forward = eye_forward(player.eye_yaw_degrees);
	const vec3 right = eye_right(player.eye_yaw_degrees);
	return {
		origin.x + forward.x * local.x - right.x * local.y,
		origin.y + forward.y * local.x - right.y * local.y,
		origin.z + adjusted_local_z(player, local.z)
	};
}

void add_point(visibility_target_points &targets, vec3 point)
{
	if (targets.count < targets.points.size())
	{
		targets.points[targets.count++] = point;
	}
}

void add_origin(visibility_origin_points &origins, vec3 point)
{
	for (uint32_t index = 0; index < origins.count; ++index)
	{
		if (distance_sq(origins.points[index], point) <= k_same_point_epsilon_sq)
		{
			return;
		}
	}
	if (origins.count < origins.points.size())
	{
		origins.points[origins.count++] = point;
	}
}

void add_aabb_corners(visibility_target_points &targets, bounds box)
{
	add_point(targets, {box.min.x, box.min.y, box.min.z});
	add_point(targets, {box.max.x, box.min.y, box.min.z});
	add_point(targets, {box.min.x, box.max.y, box.min.z});
	add_point(targets, {box.max.x, box.max.y, box.min.z});
	add_point(targets, {box.min.x, box.min.y, box.max.z});
	add_point(targets, {box.max.x, box.min.y, box.max.z});
	add_point(targets, {box.min.x, box.max.y, box.max.z});
	add_point(targets, {box.max.x, box.max.y, box.max.z});
}

void add_body_points(visibility_target_points &targets, const visibility_player &player, vec3 origin)
{
	if (player.body_point_count == player.body_points.size())
	{
		for (vec3 point : player.body_points)
		{
			add_point(targets, point);
		}
		return;
	}
	for (const body_point &point : k_body_points)
	{
		add_point(targets, local_to_world(player, origin, point.local));
	}
}

void add_muzzle_point(visibility_target_points &targets, const visibility_player &player, vec3 origin)
{
	const float length = weapon_muzzle_length(player.muzzle_class);
	if (length <= 0.0f)
	{
		return;
	}
	add_point(targets, local_to_world(player, origin, {length, 0.0f, k_muzzle_z}));
}

} // namespace

const std::array<visibility_body_binding, k_visibility_body_point_count> k_visibility_body_bindings {{
	{"head_0", {-6.583533f, 6.974191f, -1.442826f}},
	{"neck_0", {-3.579878f, 6.462899f, 2.730605f}},
	{"spine_3", {-4.567842f, 3.748647f, 3.660606f}},
	{"pelvis", {-4.824802f, -0.599556f, 5.946114f}},
	{"arm_upper_L", {0.424835f, 9.730416f, -3.004129f}},
	{"arm_upper_R", {0.987906f, 1.396868f, 0.034221f}},
	{"leg_upper_L", {0.985176f, -6.111486f, 7.523203f}},
	{"leg_upper_R", {-1.085612f, -1.173614f, -1.729821f}},
	{"leg_lower_L", {-2.260681f, -12.971139f, -1.848936f}},
	{"leg_lower_R", {0.532510f, -2.178953f, -2.248215f}},
	{"ankle_L", {14.535840f, 7.542427f, -2.333007f}},
	{"ankle_R", {6.831867f, 4.143797f, 2.375296f}},
	{"arm_lower_R", {-0.079149f, -5.299527f, -3.384076f}},
	{"arm_lower_L", {2.545455f, 22.522351f, 3.338776f}},
	{"spine_2", {-5.793778f, -0.510446f, 4.308481f}}
}};

bool visibility_transform_body_point(const visibility_bone_transform &transform, vec3 local, vec3 &world)
{
	const float x = transform.rotation[0];
	const float y = transform.rotation[1];
	const float z = transform.rotation[2];
	const float w = transform.rotation[3];
	const float norm = x * x + y * y + z * z + w * w;
	if (!std::isfinite(transform.position.x) || !std::isfinite(transform.position.y)
		|| !std::isfinite(transform.position.z) || !std::isfinite(norm) || norm < 0.25f || norm > 4.0f)
	{
		return false;
	}
	const float inverse_length = 1.0f / std::sqrt(norm);
	const float qx = x * inverse_length;
	const float qy = y * inverse_length;
	const float qz = z * inverse_length;
	const float qw = w * inverse_length;
	const vec3 twice_cross {
		2.0f * (qy * local.z - qz * local.y),
		2.0f * (qz * local.x - qx * local.z),
		2.0f * (qx * local.y - qy * local.x)
	};
	world = {
		transform.position.x + local.x + qw * twice_cross.x + qy * twice_cross.z - qz * twice_cross.y,
		transform.position.y + local.y + qw * twice_cross.y + qz * twice_cross.x - qx * twice_cross.z,
		transform.position.z + local.z + qw * twice_cross.z + qx * twice_cross.y - qy * twice_cross.x
	};
	return std::isfinite(world.x) && std::isfinite(world.y) && std::isfinite(world.z);
}

float visibility_shoulder_offset_units(float rtt_seconds, const visibility_tuning &tuning)
{
	const float base = std::max(0.0f, tuning.shoulder_base_units);
	const float maximum = std::max(base, tuning.max_shoulder_units);
	const float rtt_ms = std::max(0.0f, rtt_seconds) * 1000.0f;
	const float stepped_ms = std::floor(rtt_ms / k_ping_step_ms) * k_ping_step_ms;
	const float wanted = base + stepped_ms * std::max(0.0f, tuning.shoulder_rtt_scale);
	return std::clamp(wanted, base, maximum);
}

vec3 visibility_clip_destination(const bvh8_data &data, vec3 origin, vec3 destination)
{
	if (distance_sq(origin, destination) <= k_same_point_epsilon_sq
		|| !segment_blocked(data, origin, destination).blocked)
	{
		return destination;
	}
	vec3 clear = origin;
	vec3 blocked = destination;
	for (uint32_t step = 0; step < k_wall_clip_steps; ++step)
	{
		const vec3 middle = scale(add(clear, blocked), 0.5f);
		if (segment_blocked(data, origin, middle).blocked)
		{
			blocked = middle;
		}
		else
		{
			clear = middle;
		}
	}
	return clear;
}

weapon_muzzle_class weapon_muzzle_class_from_item_definition(uint16_t item_definition)
{
	switch (item_definition)
	{
		case 1: // deagle
		case 2: // elite
		case 3: // fiveseven
		case 4: // glock
		case 30: // tec9
		case 32: // hkp2000
		case 36: // p250
		case 61: // usp_silencer
		case 63: // cz75a
		case 64: // revolver
			return weapon_muzzle_class::pistol;
		case 17: // mac10
		case 19: // p90
		case 23: // mp5sd
		case 24: // ump45
		case 26: // bizon
		case 33: // mp7
		case 34: // mp9
			return weapon_muzzle_class::smg;
		case 9: // awp
		case 11: // g3sg1
		case 38: // scar20
		case 40: // ssg08
			return weapon_muzzle_class::sniper;
		case 7: // ak47
		case 8: // aug
		case 10: // famas
		case 13: // galilar
		case 14: // m249
		case 16: // m4a1
		case 25: // xm1014
		case 27: // mag7
		case 28: // negev
		case 29: // sawedoff
		case 35: // nova
		case 39: // sg556
		case 60: // m4a1_silencer
			return weapon_muzzle_class::rifle;
		default:
			return weapon_muzzle_class::none;
	}
}

float weapon_muzzle_length(weapon_muzzle_class value)
{
	switch (value)
	{
		case weapon_muzzle_class::pistol: return 18.0f;
		case weapon_muzzle_class::smg: return 28.0f;
		case weapon_muzzle_class::rifle: return 36.0f;
		case weapon_muzzle_class::sniper: return 52.0f;
		default: return 0.0f;
	}
}

visibility_origin_points visibility_origins(const bvh8_data &data, const visibility_player &player,
	const visibility_tuning &tuning)
{
	visibility_origin_points origins;
	const float offset = visibility_shoulder_offset_units(player.rtt_seconds, tuning);
	const vec3 forward = eye_forward(player.eye_yaw_degrees);
	const vec3 right_axis = eye_right(player.eye_yaw_degrees);
	const vec3 shoulder = scale(right_axis, offset);
	const vec3 left = subtract(player.eye, shoulder);
	const vec3 right = add(player.eye, shoulder);
	const vec3 vertical {0.0f, 0.0f, k_vertical_origin_offset};
	const vec3 up = add(player.eye, vertical);
	add_origin(origins, player.eye);
	add_origin(origins, safe_origin(data, player.eye, left));
	add_origin(origins, safe_origin(data, player.eye, right));
	add_origin(origins, safe_origin(data, player.eye, up));
	add_origin(origins, player.origin);

	const float forward_input = static_cast<float>((player.movement_buttons & k_visibility_button_forward) != 0)
		- static_cast<float>((player.movement_buttons & k_visibility_button_back) != 0);
	const float side_input = static_cast<float>((player.movement_buttons & k_visibility_button_right) != 0)
		- static_cast<float>((player.movement_buttons & k_visibility_button_left) != 0);
	// Pure A/D already has the matching ping-scaled shoulder origin.
	if (forward_input != 0.0f)
	{
		vec3 direction = add(scale(forward, forward_input), scale(right_axis, side_input));
		const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
		direction = scale(direction, offset / length);
		add_origin(origins, visibility_clip_destination(data, player.eye, add(player.eye, direction)));
	}
	return origins;
}

visibility_target_points visibility_targets(const visibility_player &player)
{
	visibility_target_points targets;
	add_aabb_corners(targets, player_bounds(player, player.origin));
	add_body_points(targets, player, player.origin);
	add_muzzle_point(targets, player, player.origin);
	return targets;
}

} // namespace cs2fow
