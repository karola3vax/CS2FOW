#include "visibility_sampling.h"

// Builds current/predicted eye, body, axis-aligned box, and weapon-muzzle points
// from copied player numbers. Unsafe prediction stays at the current position;
// all returned counts remain inside fixed arrays used by the worker.

#include <algorithm>
#include <cmath>

namespace cs2fow
{
namespace
{

constexpr float k_max_prediction_speed = 350.0f;
constexpr float k_min_prediction_speed = 100.0f;
constexpr float k_base_bounds_padding = 4.0f;
constexpr float k_predicted_bounds_padding = 8.0f;
constexpr float k_shoulder_origin_offset = 24.0f;
constexpr float k_vertical_origin_offset = 24.0f;
constexpr float k_same_point_epsilon_sq = 1.0e-4f;
constexpr float k_rtt_lookahead_scale = 0.5f;
constexpr float k_degrees_to_radians = 0.017453292519943295769f;
constexpr float k_standing_player_height = 72.0f;
constexpr float k_pelvis_height = 38.0f;
constexpr float k_muzzle_z = 60.0f;

struct body_point
{
	vec3 local;
};

constexpr std::array<body_point, 15> k_body_points {{
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

bounds player_bounds(const visibility_player &player, vec3 origin, float padding)
{
	return {
		{origin.x + player.mins.x - padding, origin.y + player.mins.y - padding, origin.z + player.mins.z - padding},
		{origin.x + player.maxs.x + padding, origin.y + player.maxs.y + padding, origin.z + player.maxs.z + padding}
	};
}

bounds merge(bounds a, bounds b)
{
	return {
		{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
		{std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}
	};
}

vec3 center(bounds value)
{
	return {
		(value.min.x + value.max.x) * 0.5f,
		(value.min.y + value.max.y) * 0.5f,
		(value.min.z + value.max.z) * 0.5f
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

float visibility_effective_lookahead_seconds(float rtt_seconds, const visibility_tuning &tuning)
{
	if (tuning.max_lookahead_ms == 0)
	{
		return 0.0f;
	}
	const float rtt_ms = std::max(0.0f, rtt_seconds) * 1000.0f;
	const float wanted_ms = static_cast<float>(tuning.base_lookahead_ms) + rtt_ms * k_rtt_lookahead_scale;
	return std::clamp(wanted_ms, 0.0f, static_cast<float>(tuning.max_lookahead_ms)) / 1000.0f;
}

vec3 visibility_prediction_offset(vec3 velocity, float seconds)
{
	if (seconds <= 0.0f)
	{
		return {};
	}
	const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
	if (speed < k_min_prediction_speed)
	{
		return {};
	}
	const float capped_speed = std::min(speed, k_max_prediction_speed);
	const float distance = capped_speed * seconds;
	const float scale = distance / speed;
	return {velocity.x * scale, velocity.y * scale, 0.0f};
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

std::array<vec3, k_visibility_origin_count> visibility_origins(const bvh8_data &data, const visibility_player &player, float lookahead_seconds)
{
	const vec3 predicted = add(player.eye, visibility_prediction_offset(player.velocity, lookahead_seconds));
	const vec3 shoulder = scale(eye_right(player.eye_yaw_degrees), k_shoulder_origin_offset);
	const vec3 left = subtract(player.eye, shoulder);
	const vec3 right = add(player.eye, shoulder);
	const vec3 predicted_left = subtract(predicted, shoulder);
	const vec3 predicted_right = add(predicted, shoulder);
	const vec3 vertical {0.0f, 0.0f, k_vertical_origin_offset};
	const vec3 up = add(player.eye, vertical);
	const vec3 predicted_up = add(predicted, vertical);
	return {
		player.eye,
		safe_origin(data, player.eye, predicted),
		safe_origin(data, player.eye, left),
		safe_origin(data, player.eye, right),
		safe_origin(data, player.eye, predicted_left),
		safe_origin(data, player.eye, predicted_right),
		safe_origin(data, player.eye, up),
		safe_origin(data, player.eye, predicted_up)
	};
}

visibility_target_points visibility_targets(const bvh8_data &data, const visibility_player &player, float lookahead_seconds)
{
	visibility_target_points targets;
	const vec3 offset = visibility_prediction_offset(player.velocity, lookahead_seconds);
	bool predicted = false;
	if (distance_sq({}, offset) > k_same_point_epsilon_sq)
	{
		const bounds current_check = player_bounds(player, player.origin, k_base_bounds_padding);
		const bounds future_check = player_bounds(player, add(player.origin, offset), k_base_bounds_padding);
		predicted = !segment_blocked(data, center(current_check), center(future_check)).blocked;
	}

	if (predicted)
	{
		const vec3 future_origin = add(player.origin, offset);
		add_aabb_corners(targets, merge(
			player_bounds(player, player.origin, k_predicted_bounds_padding),
			player_bounds(player, future_origin, k_predicted_bounds_padding)));
		add_body_points(targets, player, player.origin);
		add_body_points(targets, player, future_origin);
		add_muzzle_point(targets, player, player.origin);
		add_muzzle_point(targets, player, future_origin);
	}
	else
	{
		add_aabb_corners(targets, player_bounds(player, player.origin, k_base_bounds_padding));
		add_body_points(targets, player, player.origin);
		add_muzzle_point(targets, player, player.origin);
	}

	return targets;
}

} // namespace cs2fow
