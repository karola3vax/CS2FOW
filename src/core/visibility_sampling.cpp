#include "visibility_sampling.h"

#include <algorithm>
#include <cmath>

namespace cs2fow
{
namespace
{

constexpr float k_max_prediction_speed = 500.0f;
constexpr float k_min_prediction_speed = 1.0f;
constexpr float k_bounds_inflate = 12.0f;
constexpr float k_shoulder_origin_offset = 24.0f;
constexpr float k_vertical_origin_offset = 24.0f;
constexpr float k_same_point_epsilon_sq = 1.0e-4f;
constexpr float k_rtt_lookahead_scale = 2.0f;
constexpr float k_degrees_to_radians = 0.017453292519943295769f;

float stepped_peek_margin(float speed, float max_margin)
{
	max_margin = std::max(0.0f, max_margin);
	if (speed < 25.0f)
	{
		return max_margin * 0.10f;
	}
	if (speed < 75.0f)
	{
		return max_margin * 0.20f;
	}
	if (speed < 150.0f)
	{
		return max_margin * 0.40f;
	}
	if (speed < 250.0f)
	{
		return max_margin * 0.60f;
	}
	if (speed < 300.0f)
	{
		return max_margin * 0.80f;
	}
	return max_margin;
}

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

bounds player_bounds(const visibility_player &player, vec3 origin)
{
	return {
		{origin.x + player.mins.x - k_bounds_inflate, origin.y + player.mins.y - k_bounds_inflate, origin.z + player.mins.z - k_bounds_inflate},
		{origin.x + player.maxs.x + k_bounds_inflate, origin.y + player.maxs.y + k_bounds_inflate, origin.z + player.maxs.z + k_bounds_inflate}
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

} // namespace

float visibility_effective_lookahead_seconds(float rtt_seconds, const visibility_tuning &tuning)
{
	if (tuning.max_lookahead_ms == 0)
	{
		return 0.0f;
	}
	const float rtt_ms = std::max(0.0f, rtt_seconds) * 1000.0f;
	const float wanted_ms = static_cast<float>(tuning.min_lookahead_ms) + rtt_ms * k_rtt_lookahead_scale;
	return std::clamp(wanted_ms, 0.0f, static_cast<float>(tuning.max_lookahead_ms)) / 1000.0f;
}

vec3 visibility_prediction_offset(vec3 velocity, float seconds, float peek_margin_units)
{
	if (seconds <= 0.0f)
	{
		return {};
	}
	const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
	if (speed <= k_min_prediction_speed)
	{
		return {};
	}
	const float capped_speed = std::min(speed, k_max_prediction_speed);
	const float distance = std::max(capped_speed * seconds, stepped_peek_margin(speed, peek_margin_units));
	const float scale = distance / speed;
	return {velocity.x * scale, velocity.y * scale, 0.0f};
}

std::array<vec3, k_visibility_origin_count> visibility_origins(const bvh8_data &data, const visibility_player &player, const visibility_tuning &tuning, float lookahead_seconds)
{
	const vec3 predicted = add(player.eye, visibility_prediction_offset(player.velocity, lookahead_seconds, tuning.peek_margin_units));
	const vec3 shoulder = scale(eye_right(player.eye_yaw_degrees), k_shoulder_origin_offset);
	const vec3 left = subtract(player.eye, shoulder);
	const vec3 right = add(player.eye, shoulder);
	const vec3 predicted_left = subtract(predicted, shoulder);
	const vec3 predicted_right = add(predicted, shoulder);
	const vec3 vertical {0.0f, 0.0f, k_vertical_origin_offset};
	const vec3 up = add(player.eye, vertical);
	const vec3 down = subtract(player.eye, vertical);
	const vec3 predicted_up = add(predicted, vertical);
	const vec3 predicted_down = subtract(predicted, vertical);
	return {
		player.eye,
		safe_origin(data, player.eye, predicted),
		safe_origin(data, player.eye, left),
		safe_origin(data, player.eye, right),
		safe_origin(data, player.eye, predicted_left),
		safe_origin(data, player.eye, predicted_right),
		safe_origin(data, player.eye, up),
		safe_origin(data, player.eye, down),
		safe_origin(data, player.eye, predicted_up),
		safe_origin(data, player.eye, predicted_down)
	};
}

std::array<vec3, k_visibility_target_count> visibility_targets(const bvh8_data &data, const visibility_player &player, const visibility_tuning &tuning, float lookahead_seconds)
{
	bounds box = player_bounds(player, player.origin);
	const vec3 offset = visibility_prediction_offset(player.velocity, lookahead_seconds, tuning.peek_margin_units);
	if (distance_sq({}, offset) > k_same_point_epsilon_sq)
	{
		const bounds future = player_bounds(player, add(player.origin, offset));
		if (!segment_blocked(data, center(box), center(future)).blocked)
		{
			box = merge(box, future);
		}
	}

	const vec3 middle = center(box);
	const vec3 upper {middle.x, middle.y, box.min.z + (box.max.z - box.min.z) * 0.75f};
	return {
		vec3 {box.min.x, box.min.y, box.min.z}, vec3 {box.max.x, box.min.y, box.min.z},
		vec3 {box.min.x, box.max.y, box.min.z}, vec3 {box.max.x, box.max.y, box.min.z},
		vec3 {box.min.x, box.min.y, box.max.z}, vec3 {box.max.x, box.min.y, box.max.z},
		vec3 {box.min.x, box.max.y, box.max.z}, vec3 {box.max.x, box.max.y, box.max.z},
		vec3 {box.min.x, middle.y, middle.z}, vec3 {box.max.x, middle.y, middle.z},
		vec3 {middle.x, box.min.y, middle.z}, vec3 {middle.x, box.max.y, middle.z},
		vec3 {middle.x, middle.y, box.min.z}, vec3 {middle.x, middle.y, box.max.z},
		middle,
		upper
	};
}

} // namespace cs2fow
