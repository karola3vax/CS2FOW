#include "test_suites.h"

// Exercises copied visibility sampling/worker behavior and fixed CheckTransmit
// lifecycle, visual-group, mask, and evidence helpers. Fake data keeps engine
// objects and live network lists out of the unit tests.

#include "builder.h"
#include "lifecycle_guard.h"
#include "smoke_occlusion.h"
#include "transmit_debug.h"
#include "transmit_masks.h"
#include "visibility_sampling.h"
#include "visibility_worker.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace cs2fow;

bvh8_data test_world(const std::vector<triangle> &triangles)
{
	bvh8_data data;
	std::string error;
	assert(build_bvh8(triangles, data, error));
	return data;
}

float max_x(const visibility_target_points &points)
{
	float result = points.points[0].x;
	for (uint32_t index = 0; index < points.count; ++index)
	{
		result = std::max(result, points.points[index].x);
	}
	return result;
}

float min_x(const visibility_target_points &points)
{
	float result = points.points[0].x;
	for (uint32_t index = 0; index < points.count; ++index)
	{
		result = std::min(result, points.points[index].x);
	}
	return result;
}

visual_group_key test_visual_key(std::initializer_list<uint32_t> handles)
{
	std::array<uint32_t, 8> values {};
	size_t count = 0;
	for (uint32_t handle : handles)
	{
		assert(count < values.size());
		values[count++] = handle;
	}
	return make_visual_group_key(values, count);
}

uint32_t test_morton(uint32_t x, uint32_t y, uint32_t z)
{
	uint32_t result = 0;
	for (uint32_t bit = 0; bit < 5; ++bit)
	{
		result |= ((x >> bit) & 1u) << (bit * 3u);
		result |= ((y >> bit) & 1u) << (bit * 3u + 1u);
		result |= ((z >> bit) & 1u) << (bit * 3u + 2u);
	}
	return result;
}

void test_visibility_pair_eligibility()
{
	player_state t;
	t.valid = true;
	t.team = 2;
	player_state ct = t;
	ct.team = 3;
	const player_state invalid;
	assert(visibility_pair_enabled(0, 1, t, ct, false));
	assert(visibility_pair_enabled(0, 1, t, ct, true));
	assert(!visibility_pair_enabled(0, 1, t, t, false));
	assert(visibility_pair_enabled(0, 1, t, t, true));
	assert(!visibility_pair_enabled(0, 0, t, ct, true));
	assert(!visibility_pair_enabled(0, 1, invalid, ct, true));
	assert(!visibility_pair_enabled(0, 1, t, invalid, true));
	player_state numbers = t;
	numbers.eye = {};
	numbers.origin = {};
	numbers.mins = {-16, -16, 0};
	numbers.maxs = {16, 16, 72};
	assert(valid_player_numbers(numbers));
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float infinity = std::numeric_limits<float>::infinity();
	numbers.eye.x = nan;
	assert(!valid_player_numbers(numbers));
	numbers.eye.x = 0;
	numbers.mins.y = infinity;
	assert(!valid_player_numbers(numbers));
	numbers.mins.y = -16;
	numbers.rtt_seconds = nan;
	assert(!valid_player_numbers(numbers));
	numbers.rtt_seconds = 0;
	numbers.mins.x = 17;
	assert(!valid_player_numbers(numbers));
}

void test_smoke_occlusion()
{
	he_clearance_history history;
	assert(!history.record({std::numeric_limits<float>::quiet_NaN(), 0, 0}, 10.0f));
	assert(!history.record({}, std::numeric_limits<float>::quiet_NaN()));
	for (uint32_t index = 0; index <= k_max_he_clearances; ++index)
	{
		assert(history.record({static_cast<float>(index), 0, 0}, 10.0f));
	}
	assert(history.count == k_max_he_clearances && history.next == 1 && history.records[0].center.x == 64.0f);
	history.clear();
	assert(history.count == 0 && history.next == 0);

	smoke_snapshot smoke;
	smoke.volumes.reserve(2);
	smoke.volumes.emplace_back();
	smoke_volume_snapshot &volume = smoke.volumes.back();
	volume.age_seconds = 2.0f;
	volume.density.fill(50.0f);
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
	assert(smoke_line_blocked(smoke, {100, 0, 0}, {-100, 0, 0}));
	assert(!smoke_line_blocked(smoke, {400, 0, 0}, {500, 0, 0}));
	volume.center = {-1000, 0, 0};
	assert(!smoke_line_blocked(smoke, {0, 0, 0}, {100, 0, 0}));
	volume.center = {1000, 0, 0};
	assert(!smoke_line_blocked(smoke, {0, 0, 0}, {100, 0, 0}));
	volume.center = {};
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float infinity = std::numeric_limits<float>::infinity();
	const float maximum = std::numeric_limits<float>::max();
	assert(!smoke_line_blocked(smoke, {nan, 0, 0}, {100, 0, 0}));
	assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {infinity, 0, 0}));
	assert(!smoke_line_blocked(smoke, {-maximum, 0, 0}, {maximum, 0, 0}));

	volume.density.fill(5.0f);
	assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
	volume.density.fill(5.5f);
	assert(!smoke_line_blocked(smoke, {0, 0, 0}, {1, 0, 0}));
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
	volume.density.fill(0.0f);
	volume.density[test_morton(16, 16, 16)] = 40.0f;
	assert(smoke_line_blocked(smoke, {0, 0, 0}, {1, 0, 0}));
	volume.density.fill(0.0f);
	volume.opaque_cells[test_morton(16, 16, 16) >> 3u] = static_cast<uint8_t>(1u << (test_morton(16, 16, 16) & 7u));
	assert(smoke_line_blocked(smoke, {0, 0, 0}, {1, 0, 0}));

	volume.opaque_cells.fill(0);
	volume.density.fill(5.5f);
	smoke.volumes.push_back(volume);
	assert(smoke_line_blocked(smoke, {0, 0, 0}, {1, 0, 0}));
	smoke.volumes.resize(1);
	volume = smoke.volumes.front();
	volume.density.fill(50.0f);
	volume.age_seconds = 0.75f;
	assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
	volume.age_seconds = 1.25f;
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
	volume.age_seconds = 19.8f;
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));
	volume.age_seconds = 20.3f;
	assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}));

	const bvh8_data open = test_world({{{10000, 10000, 10000}, {10001, 10000, 10000}, {10000, 10001, 10000}}});
	const bvh8_data blast_wall = test_world({
		{{-100, 25, -100}, {100, 25, -100}, {100, 25, 100}},
		{{-100, 25, -100}, {100, 25, 100}, {-100, 25, 100}}
	});
	volume.age_seconds = 2.0f;
	volume.center = {};
	volume.density.fill(50.0f);
	smoke.he_clear_radius_units = 100.0f;
	smoke.he_clear_seconds = 3.0f;
	smoke.he_clearance_count = 1;
	smoke.he_clearances[0] = {{0, 50, 0}, 2.999f, 2.0f};
	volume.start_time = 1.0f;
	assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
	volume.start_time = 3.0f;
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
	volume.start_time = 2.0f;
	assert(!smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &blast_wall));
	smoke.he_clearances[0].age_seconds = 3.0f;
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
	smoke.he_clearances[0].age_seconds = 2.0f;
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 1.0f, &open));
	smoke.he_clear_radius_units = 0.0f;
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
	smoke.he_clear_radius_units = 100.0f;
	smoke.he_clear_seconds = 0.0f;
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {100, 0, 0}, 0.0f, &open));
	smoke.he_clear_seconds = 3.0f;
	smoke.he_clearances[0] = {{500, 0, 0}, 1.0f, 2.0f};
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {700, 0, 0}, 0.0f, &open));
	smoke.he_clearances[0] = {{0, 0, 0}, 1.0f, 2.0f};
	smoke.volumes.push_back(volume);
	smoke.volumes.back().center = {500, 0, 0};
	assert(smoke_line_blocked(smoke, {-100, 0, 0}, {700, 0, 0}, 0.0f, &open));
	smoke.volumes.resize(1);
	smoke.he_clearance_count = 0;

	std::vector<std::byte> storage(k_smoke_storage_density_offset + 2u * k_smoke_storage_frame_stride);
	storage[k_smoke_storage_mask_offset] = std::byte {1};
	const float first = 12.5f;
	const float second = 25.0f;
	std::memcpy(storage.data() + k_smoke_storage_density_offset, &first, sizeof(first));
	std::memcpy(storage.data() + k_smoke_storage_density_offset + k_smoke_storage_frame_stride, &second, sizeof(second));
	smoke_volume_snapshot copied;
	assert(copy_smoke_frame(storage.data(), 0, {1, 2, 3}, 2.0f, copied));
	assert(copied.center.x == 1.0f && copied.opaque_cells[0] == 1 && copied.density[0] == first);
	assert(copy_smoke_frame(storage.data(), 1, {1, 2, 3}, 2.0f, copied));
	assert(copied.density[0] == second);
	assert(!copy_smoke_frame(storage.data(), 2, {}, 2.0f, copied));
	int frame_reads = 0;
	assert(copy_stable_smoke_frame(storage.data(), {}, 2.0f, copied, [&]
	{
		return frame_reads++ == 0 ? 0 : 1;
	}));
	assert(frame_reads == 4 && copied.density[0] == second);
	frame_reads = 0;
	assert(!copy_stable_smoke_frame(storage.data(), {}, 2.0f, copied, [&]
	{
		return frame_reads++ & 1;
	}));
	const float invalid = std::numeric_limits<float>::quiet_NaN();
	std::memcpy(storage.data() + k_smoke_storage_density_offset, &invalid, sizeof(invalid));
	assert(!copy_smoke_frame(storage.data(), 0, {}, 2.0f, copied));
}

void test_visibility_sampling()
{
	const bvh8_data open = test_world({{{10000, 10000, 10000}, {10001, 10000, 10000}, {10000, 10001, 10000}}});
	const visibility_tuning tuning {48.0f, 0.4f, 128.0f};
	visibility_player player {};
	player.eye = {0, 0, 64};
	player.origin = {0, 0, 0};
	player.mins = {-16, -16, 0};
	player.maxs = {16, 16, 72};

	assert(std::fabs(visibility_shoulder_offset_units(-1.0f, tuning) - 48.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.0f, tuning) - 48.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.024f, tuning) - 48.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.025f, tuning) - 58.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.049f, tuning) - 58.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.05f, tuning) - 68.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.075f, tuning) - 78.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.1f, tuning) - 88.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.125f, tuning) - 98.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.15f, tuning) - 108.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.175f, tuning) - 118.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.199f, tuning) - 118.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.2f, tuning) - 128.0f) < 0.01f);
	assert(std::fabs(visibility_shoulder_offset_units(0.3f, tuning) - 128.0f) < 0.01f);

	auto origins = visibility_origins(open, player, tuning);
	assert(k_visibility_origin_count_max == 6);
	assert(k_visibility_target_count_max == 24);
	assert(k_visibility_ray_count_max == 144);
	assert(origins.count == 5);
	assert(origins.points[0].x == 0.0f && origins.points[0].z == 64.0f);
	assert(std::fabs(origins.points[1].y - 48.0f) < 0.01f);
	assert(std::fabs(origins.points[2].y + 48.0f) < 0.01f);
	assert(std::fabs(origins.points[3].z - 80.0f) < 0.01f);
	assert(origins.points[4].x == player.origin.x && origins.points[4].y == player.origin.y
		&& origins.points[4].z == player.origin.z);
	player.rtt_seconds = 0.05f;
	origins = visibility_origins(open, player, tuning);
	assert(std::fabs(origins.points[1].y - 68.0f) < 0.01f && std::fabs(origins.points[2].y + 68.0f) < 0.01f);
	player.rtt_seconds = 0.0f;

	player.eye_yaw_degrees = 90.0f;
	origins = visibility_origins(open, player, tuning);
	assert(std::fabs(origins.points[1].x + 48.0f) < 0.01f);
	assert(std::fabs(origins.points[2].x - 48.0f) < 0.01f);
	player.eye_yaw_degrees = 0.0f;

	player.movement_buttons = k_visibility_button_forward;
	origins = visibility_origins(open, player, tuning);
	assert(origins.count == 6 && std::fabs(origins.points[5].x - 48.0f) < 0.01f);
	player.movement_buttons = k_visibility_button_back;
	origins = visibility_origins(open, player, tuning);
	assert(origins.count == 6 && std::fabs(origins.points[5].x + 48.0f) < 0.01f);
	player.movement_buttons = k_visibility_button_left;
	assert(visibility_origins(open, player, tuning).count == 5);
	player.movement_buttons = k_visibility_button_right;
	assert(visibility_origins(open, player, tuning).count == 5);

	const float diagonal = 48.0f / std::sqrt(2.0f);
	const std::array<std::pair<uint64_t, vec3>, 4> diagonals {{
		{k_visibility_button_forward | k_visibility_button_left, {diagonal, diagonal, 64}},
		{k_visibility_button_forward | k_visibility_button_right, {diagonal, -diagonal, 64}},
		{k_visibility_button_back | k_visibility_button_left, {-diagonal, diagonal, 64}},
		{k_visibility_button_back | k_visibility_button_right, {-diagonal, -diagonal, 64}}
	}};
	for (const auto &[buttons, expected] : diagonals)
	{
		player.movement_buttons = buttons;
		origins = visibility_origins(open, player, tuning);
		assert(origins.count == 6);
		assert(std::fabs(origins.points[5].x - expected.x) < 0.01f
			&& std::fabs(origins.points[5].y - expected.y) < 0.01f);
	}
	player.movement_buttons = k_visibility_button_forward | k_visibility_button_back;
	assert(visibility_origins(open, player, tuning).count == 5);
	player.movement_buttons |= k_visibility_button_left;
	assert(visibility_origins(open, player, tuning).count == 5);
	player.movement_buttons = k_visibility_button_forward | k_visibility_button_left | k_visibility_button_right;
	origins = visibility_origins(open, player, tuning);
	assert(origins.count == 6 && std::fabs(origins.points[5].x - 48.0f) < 0.01f);
	player.movement_buttons = k_visibility_button_forward;
	player.eye_yaw_degrees = 90.0f;
	origins = visibility_origins(open, player, tuning);
	assert(origins.count == 6 && std::fabs(origins.points[5].y - 48.0f) < 0.01f);
	player.eye_yaw_degrees = 0.0f;

	const bvh8_data wall = test_world({
		{{4, -100, -100}, {4, 100, -100}, {4, -100, 100}},
		{{4, 100, 100}, {4, -100, 100}, {4, 100, -100}}
	});
	origins = visibility_origins(wall, player, tuning);
	assert(origins.count == 6 && origins.points[5].x > 3.9f && origins.points[5].x < 4.1f);
	const vec3 clipped_short = visibility_clip_destination(wall, {}, {5, 0, 0});
	const vec3 clipped_long = visibility_clip_destination(wall, {}, {20, 0, 0});
	assert(clipped_short.x > 3.9f && clipped_short.x < 4.1f);
	assert(clipped_long.x >= clipped_short.x - 0.1f && clipped_long.x < 4.1f);

	const bvh8_data side_wall = test_world({
		{{-100, -8, -100}, {100, -8, -100}, {-100, -8, 100}},
		{{100, -8, 100}, {-100, -8, 100}, {100, -8, -100}}
	});
	player.movement_buttons = k_visibility_button_right;
	origins = visibility_origins(side_wall, player, tuning);
	assert(origins.count == 4);

	const bvh8_data ceiling = test_world({
		{{-100, -100, 72}, {100, -100, 72}, {-100, 100, 72}},
		{{100, 100, 72}, {-100, 100, 72}, {100, -100, 72}}
	});
	player.movement_buttons = 0;
	origins = visibility_origins(ceiling, player, tuning);
	assert(origins.count == 4);

	visibility_player target {};
	target.origin = {0, 0, 0};
	target.mins = {-16, -16, 0};
	target.maxs = {16, 16, 72};
	target.muzzle_class = weapon_muzzle_class::rifle;
	const auto current_targets = visibility_targets(target);
	assert(current_targets.count == 24);
	assert(std::fabs(min_x(current_targets) + 24.0f) < 0.01f);
	assert(std::fabs(max_x(current_targets) - 36.0f) < 0.01f);
	assert(current_targets.points[0].z == 0.0f && current_targets.points[7].z == 80.0f);

	target.muzzle_class = weapon_muzzle_class::none;
	const auto no_muzzle_targets = visibility_targets(target);
	assert(no_muzzle_targets.count == 23);
	target.muzzle_class = weapon_muzzle_class::rifle;
	target.eye_yaw_degrees = 0.0f;
	const auto yaw0_targets = visibility_targets(target);
	const vec3 yaw0_head = yaw0_targets.points[8];
	assert(std::fabs(yaw0_head.x - 5.6092f) < 0.01f);
	assert(std::fabs(yaw0_head.y + 1.4428f) < 0.01f);
	assert(std::fabs(yaw0_head.z - 64.2013f) < 0.01f);

	target.eye_yaw_degrees = 90.0f;
	const auto yaw90_targets = visibility_targets(target);
	const vec3 yaw90_head = yaw90_targets.points[8];
	assert(std::fabs(yaw90_head.x - 1.4428f) < 0.01f);
	assert(std::fabs(yaw90_head.y - 5.6092f) < 0.01f);

	target.eye_yaw_degrees = 180.0f;
	const auto yaw180_targets = visibility_targets(target);
	const vec3 yaw180_head = yaw180_targets.points[8];
	assert(std::fabs(yaw180_head.x + 5.6092f) < 0.01f);
	assert(std::fabs(yaw180_head.y - 1.4428f) < 0.01f);

	target.eye_yaw_degrees = 0.0f;
	target.maxs.z = 54.0f;
	const auto crouched_targets = visibility_targets(target);
	const vec3 crouched_head = crouched_targets.points[8];
	const vec3 crouched_left_foot = crouched_targets.points[18];
	const vec3 crouched_muzzle = crouched_targets.points[23];
	assert(crouched_head.z < yaw0_head.z - 10.0f && crouched_head.z > 45.0f);
	assert(std::fabs(crouched_left_foot.z - 4.0f) < 0.01f);
	assert(crouched_muzzle.z < 50.0f && crouched_muzzle.z > 45.0f);

	visibility_bone_transform identity {{10.0f, 20.0f, 30.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
	vec3 transformed;
	assert(visibility_transform_body_point(identity, {1.0f, 2.0f, 3.0f}, transformed));
	assert(std::fabs(transformed.x - 11.0f) < 0.001f && std::fabs(transformed.y - 22.0f) < 0.001f
		&& std::fabs(transformed.z - 33.0f) < 0.001f);
	const float half_sqrt_two = std::sqrt(0.5f);
	visibility_bone_transform quarter_turn {{}, {0.0f, 0.0f, half_sqrt_two, half_sqrt_two}};
	assert(visibility_transform_body_point(quarter_turn, {1.0f, 0.0f, 0.0f}, transformed));
	assert(std::fabs(transformed.x) < 0.001f && std::fabs(transformed.y - 1.0f) < 0.001f);
	visibility_bone_transform invalid_transform {};
	assert(!visibility_transform_body_point(invalid_transform, {}, transformed));

	target.maxs.z = 72.0f;
	for (uint32_t point = 0; point < target.body_points.size(); ++point)
	{
		target.body_points[point] = {100.0f + static_cast<float>(point), 200.0f, 300.0f};
	}
	target.body_point_count = static_cast<uint32_t>(target.body_points.size());
	const auto animated_targets = visibility_targets(target);
	assert(animated_targets.count == 24);
	for (uint32_t point = 0; point < target.body_points.size(); ++point)
	{
		assert(animated_targets.points[8 + point].x == target.body_points[point].x);
	}

	assert(weapon_muzzle_class_from_item_definition(61) == weapon_muzzle_class::pistol);
	assert(weapon_muzzle_class_from_item_definition(34) == weapon_muzzle_class::smg);
	assert(weapon_muzzle_class_from_item_definition(7) == weapon_muzzle_class::rifle);
	assert(weapon_muzzle_class_from_item_definition(9) == weapon_muzzle_class::sniper);
	assert(weapon_muzzle_class_from_item_definition(999) == weapon_muzzle_class::none);
	assert(std::fabs(weapon_muzzle_length(weapon_muzzle_class::pistol) - 18.0f) < 0.01f);
	assert(std::fabs(weapon_muzzle_length(weapon_muzzle_class::smg) - 28.0f) < 0.01f);
	assert(std::fabs(weapon_muzzle_length(weapon_muzzle_class::rifle) - 36.0f) < 0.01f);
	assert(std::fabs(weapon_muzzle_length(weapon_muzzle_class::sniper) - 52.0f) < 0.01f);
	assert(weapon_muzzle_length(weapon_muzzle_class::none) == 0.0f);

	using clock = std::chrono::steady_clock;
	const auto start = clock::time_point {} + std::chrono::seconds(1);
	assert(visibility_snapshot_fresh(start, start + std::chrono::milliseconds(100)));
	assert(!visibility_snapshot_fresh(start, start + std::chrono::milliseconds(101)));
}

void test_visibility_worker()
{
	assert(!visibility_teammate_filter_enabled(false, false));
	assert(visibility_teammate_filter_enabled(true, false));
	assert(visibility_teammate_filter_enabled(false, true));
	assert(visibility_teammate_filter_enabled(true, true));

	const bvh8_data wall = test_world({
		{{32, -1000, -1000}, {32, 1000, -1000}, {32, -1000, 1000}},
		{{32, 1000, 1000}, {32, -1000, 1000}, {32, 1000, -1000}}
	});
	visibility_snapshot value;
	value.sequence = 1;
	value.players[0] = {true, 2, {0, 0, 64}, {0, 0, 0}, {-16, -16, 0}, {16, 16, 72}};
	value.players[1] = {true, 3, {64, 0, 64}, {64, 0, 0}, {-16, -16, 0}, {16, 16, 72}};
	const visibility_tuning tuning {48.0f, 0.4f, 128.0f};

	auto worker = std::make_unique<visibility_worker>();
	assert(worker->start(&wall));
	assert(worker->stats().cycles == 0);
	const auto wait_for = [&](uint64_t sequence)
	{
		std::shared_ptr<const visibility_result> result;
		for (int attempt = 0; attempt < 2000 && (!result || result->sequence != sequence); ++attempt)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			result = worker->result();
		}
		return result;
	};
	worker->submit(value, 100, tuning);
	auto result = wait_for(1);
	assert(result && !result->visible[0][1]);

	value.sequence = 2;
	value.players[1].eye.x = 16;
	value.players[1].origin.x = 16;
	worker->submit(value, 100, tuning);
	result = wait_for(2);
	assert(result && result->visible[0][1]);

	value.sequence = 3;
	value.players[1].eye.x = 64;
	value.players[1].origin.x = 64;
	worker->submit(value, 100, tuning);
	result = wait_for(3);
	assert(result && result->visible[0][1]);

	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	value.sequence = 4;
	worker->submit(value, 100, tuning);
	result = wait_for(4);
	assert(result && !result->visible[0][1]);
	value.players[1].team = 2;
	value.sequence = 5;
	value.filter_teammates = false;
	worker->submit(value, 100, tuning);
	result = wait_for(5);
	assert(result && result->visible[0][1] && result->evaluated_pairs == 0 && !result->filter_teammates);
	value.sequence = 6;
	value.filter_teammates = true;
	worker->submit(value, 100, tuning);
	result = wait_for(6);
	assert(result && !result->visible[0][1] && result->evaluated_pairs == 2 && result->filter_teammates);
	assert(worker->stats().cycles == 6);
	assert(worker->start(&wall));
	assert(worker->stats().cycles == 0);
	worker->stop();

	const bvh8_data open = test_world({{{10000, 10000, 10000}, {10001, 10000, 10000}, {10000, 10001, 10000}}});
	assert(worker->start(&open));
	auto smokes = std::make_shared<smoke_snapshot>();
	smokes->volumes.emplace_back();
	smokes->volumes.back().age_seconds = 2.0f;
	smokes->volumes.back().density.fill(50.0f);
	value.sequence = 7;
	value.captured = std::chrono::steady_clock::now();
	value.players[0] = {true, 2, {0, 0, 64}, {0, 0, 0}, {-16, -16, 0}, {16, 16, 72}};
	value.players[1] = {true, 2, {64, 0, 64}, {64, 0, 0}, {-16, -16, 0}, {16, 16, 72}};
	value.filter_teammates = true;
	value.smoke_enabled = true;
	value.smoke_available = true;
	value.smokes = smokes;
	worker->submit(value, 0, tuning);
	result = wait_for(7);
	assert(result && !result->visible[0][1] && result->smoke_count == 1);
	smokes->volumes.back().density.fill(0.0f);
	for (uint32_t x = 16; x < 20; ++x)
	{
		const uint32_t cell = test_morton(x, 16, 19);
		smokes->volumes.back().opaque_cells[cell >> 3u] |= static_cast<uint8_t>(1u << (cell & 7u));
	}
	assert(smoke_line_blocked(*smokes, {0, 0, 64}, {64, 0, 64}));
	value.sequence = 8;
	worker->submit(value, 0, tuning);
	result = wait_for(8);
	assert(result && result->visible[0][1]);
	smokes->volumes.back().opaque_cells.fill(0);
	smokes->volumes.back().density.fill(50.0f);
	value.sequence = 9;
	value.filter_teammates = false;
	worker->submit(value, 0, tuning);
	result = wait_for(9);
	assert(result && result->visible[0][1] && result->evaluated_pairs == 0);
	value.sequence = 10;
	value.filter_teammates = true;
	value.smoke_available = false;
	worker->submit(value, 0, tuning);
	result = wait_for(10);
	assert(result && result->visible[0][1]);
	smokes->he_clear_radius_units = 100.0f;
	smokes->he_clear_seconds = 3.0f;
	smokes->he_clearance_count = 1;
	smokes->he_clearances[0] = {{32, 0, 64}, 1.0f};
	value.sequence = 11;
	value.captured = std::chrono::steady_clock::now();
	value.smoke_available = true;
	worker->submit(value, 0, tuning);
	result = wait_for(11);
	assert(result && result->visible[0][1] && result->he_clearance_count == 1);
	smokes->he_clearances[0].age_seconds = 3.0f;
	value.sequence = 12;
	value.captured = std::chrono::steady_clock::now();
	worker->submit(value, 0, tuning);
	result = wait_for(12);
	assert(result && !result->visible[0][1]);
	worker->stop();
}

void test_lifecycle_guard()
{
	using clock = std::chrono::steady_clock;
	const auto grace = std::chrono::milliseconds(3000);
	const auto start = clock::time_point {} + std::chrono::seconds(10);
	lifecycle_guard guard;
	lifecycle_key stable;
	stable.has_controller = true;
	stable.pawn_entity = 12;
	stable.team = 2;
	stable.alive = true;

	update_lifecycle_guard(guard, stable, true, start, grace);
	assert(!lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(2999)));
	assert(lifecycle_allows_hiding(guard, start + grace));

	update_lifecycle_guard(guard, stable, true, start + grace, grace);
	assert(lifecycle_allows_hiding(guard, start + grace));

	lifecycle_key changed = stable;
	changed.team = 3;
	update_lifecycle_guard(guard, changed, true, start + grace + std::chrono::milliseconds(1), grace);
	assert(!lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(5999)));
	assert(lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(6001)));

	lifecycle_key dead = changed;
	dead.alive = false;
	update_lifecycle_guard(guard, dead, false, start + std::chrono::seconds(7), grace);
	update_lifecycle_guard(guard, dead, false, start + std::chrono::milliseconds(7500), grace);
	assert(!lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(10499)));

	update_lifecycle_guard(guard, changed, true, start + std::chrono::milliseconds(10500), grace);
	assert(!lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(13499)));
	assert(lifecycle_allows_hiding(guard, start + std::chrono::milliseconds(13500)));
}

void test_visual_group_key()
{
	const visual_group_key left = test_visual_key({30, 10, 20, 10});
	const visual_group_key right = test_visual_key({20, 30, 10});
	assert(!visual_group_changed(left, right));
	assert(left.count == 3);

	assert(visual_group_changed(left, test_visual_key({10, 20})));
	assert(visual_group_changed(test_visual_key({326u | (1u << 15)}), test_visual_key({326u | (2u << 15)})));

	std::array<uint32_t, k_pair_visual_group_key_max> full {};
	for (size_t index = 0; index < full.size(); ++index)
	{
		full[index] = static_cast<uint32_t>(index + 1);
	}
	const visual_group_key full_key = make_visual_group_key(full, full.size());
	full.back() += 1000;
	assert(full_key.count == k_pair_visual_group_key_max);
	assert(visual_group_changed(full_key, make_visual_group_key(full, full.size())));
}

void test_pair_guard()
{
	lifecycle_key recipient;
	recipient.has_controller = true;
	recipient.pawn_entity = 10;
	recipient.team = 2;
	recipient.alive = true;
	lifecycle_key target = recipient;
	target.pawn_entity = 20;
	target.team = 3;

	pair_guard guard;
	assert(update_pair_guard(guard, recipient, true, target, true));
	assert(!update_pair_guard(guard, recipient, true, target, true));
	assert(!pair_allows_hiding(guard, 1));
	pair_note_open(guard, 1);
	assert(!pair_allows_hiding(guard, 1));
	assert(pair_allows_hiding(guard, 2));

	pair_guard visual_guard;
	const visual_group_key group = test_visual_key({10, 20, 30});
	const visual_group_key same_group = test_visual_key({30, 20, 10, 10});
	const visual_group_key changed_group = test_visual_key({10, 20, 31});
	update_pair_guard(visual_guard, recipient, true, target, true);
	update_pair_visual_group(visual_guard, group);
	assert(!pair_allows_hiding(visual_guard, 1));
	pair_note_open(visual_guard, 1);
	assert(pair_allows_hiding(visual_guard, 2));
	update_pair_visual_group(visual_guard, same_group);
	assert(pair_allows_hiding(visual_guard, 3));
	update_pair_visual_group(visual_guard, changed_group);
	assert(!pair_allows_hiding(visual_guard, 4));
	pair_note_open(visual_guard, 4);
	assert(!pair_allows_hiding(visual_guard, 4));
	assert(pair_allows_hiding(visual_guard, 5));

	lifecycle_key changed = target;
	changed.team = 2;
	assert(update_pair_guard(guard, recipient, true, changed, true));
	assert(!pair_allows_hiding(guard, 3));
	pair_note_open(guard, 3);
	assert(!pair_allows_hiding(guard, 3));
	assert(pair_allows_hiding(guard, 4));

	lifecycle_key dead = changed;
	dead.alive = false;
	assert(update_pair_guard(guard, recipient, true, dead, false));
	assert(update_pair_guard(guard, recipient, true, dead, false));
	assert(!pair_allows_hiding(guard, 6));

	assert(update_pair_guard(guard, recipient, true, changed, true));
	assert(!pair_allows_hiding(guard, 7));
	pair_note_open(guard, 7);
	assert(!pair_allows_hiding(guard, 7));
	assert(pair_allows_hiding(guard, 8));
}

struct test_transmit_mask
{
	bool set {};
	int sets {};
	int clears {};
	int checked_index {-1};
	int set_index {-1};
	int cleared_index {-1};
	std::vector<char> *operations {};

	bool IsBitSet(int index) { checked_index = index; return set; }
	void Set(int index) { ++sets; set_index = index; set = true; if (operations != nullptr) operations->push_back('S'); }
	void Clear(int index) { ++clears; cleared_index = index; set = false; if (operations != nullptr) operations->push_back('C'); }
};

void test_checktransmit_private_offsets()
{
	uint32_t value {};
	assert(parse_gamedata_uint32(" 580 ", value) && value == 580);
	assert(!parse_gamedata_uint32("580x", value));
	assert(valid_gamedata_offset(580, alignof(bool), 4096));
	assert(!valid_gamedata_offset(0, alignof(void *), 4096));
	struct fake_info
	{
		std::array<std::byte, 32> padding {};
		bool full_update {};
	};
	fake_info info;
	assert(!read_checktransmit_full_update(&info, 32));
	info.full_update = true;
	assert(read_checktransmit_full_update(&info, 32));

	test_transmit_mask null_primary_dont;
	assert(!withhold_transmit_bit<test_transmit_mask>(nullptr, &null_primary_dont, 10));
	assert(!null_primary_dont.set && null_primary_dont.sets == 0);
	test_transmit_mask null_dont_primary {true};
	assert(!withhold_transmit_bit(&null_dont_primary, static_cast<test_transmit_mask *>(nullptr), 10));
	assert(null_dont_primary.set && null_dont_primary.clears == 0);

	test_transmit_mask unset_primary;
	test_transmit_mask untouched_dont;
	assert(!withhold_transmit_bit(&unset_primary, &untouched_dont, 10));
	assert(unset_primary.clears == 0 && !untouched_dont.set && untouched_dont.sets == 0);

	std::vector<char> operations;
	test_transmit_mask primary {true};
	test_transmit_mask dont_transmit;
	primary.operations = &operations;
	dont_transmit.operations = &operations;
	assert(withhold_transmit_bit(&primary, &dont_transmit, 10));
	assert(!primary.set && primary.clears == 1 && dont_transmit.set && dont_transmit.sets == 1);
	assert(primary.checked_index == 10 && primary.cleared_index == 10 && dont_transmit.set_index == 10);
	assert(operations == std::vector<char>({'S', 'C'}));

	test_transmit_mask primary_again {true};
	test_transmit_mask already_dont_transmit {true};
	assert(withhold_transmit_bit(&primary_again, &already_dont_transmit, 10));
	assert(!primary_again.set && already_dont_transmit.set && already_dont_transmit.sets == 1);
}

void test_transmit_debug()
{
	using clock = std::chrono::steady_clock;
	const auto start = clock::time_point {} + std::chrono::seconds(10);
	transmit_debug_log<2, 16> log;
	transmit_debug_event first {10, 100, 1, 1, k_transmit_reason_current, start};
	test_transmit_mask primary {true};
	test_transmit_mask dont_transmit;
	const auto clear_and_record = [&](bool debug)
	{
		if (withhold_transmit_bit(&primary, &dont_transmit, 10) && debug) log.record(first, "player");
	};
	clear_and_record(true);
	clear_and_record(true);
	primary.set = true;
	clear_and_record(false);
	assert(log.records()[0].clears == 1);
	primary.set = true;
	first.recipient_slot = 3;
	first.reason = k_transmit_reason_quarantine;
	first.when += std::chrono::milliseconds(5);
	clear_and_record(true);
	const auto &records = log.records();
	assert(records[0].valid && records[0].clears == 2 && records[0].recipients == ((uint64_t {1} << 1) | (uint64_t {1} << 3)));
	assert(records[0].reasons == (k_transmit_reason_current | k_transmit_reason_quarantine));
	assert(records[0].first_seen == start && records[0].last_seen == start + std::chrono::milliseconds(5));

	transmit_debug_log<2, 16> distinct_sources;
	distinct_sources.record({10, 100, 1, 1, k_transmit_reason_current, start}, "player");
	distinct_sources.record({10, 100, 2, 1, k_transmit_reason_current, start}, "player");
	assert(distinct_sources.records()[0].valid && distinct_sources.records()[1].valid);
	assert(distinct_sources.records()[0].source != distinct_sources.records()[1].source);

	transmit_debug_event second {20, 200, 2, 2, k_transmit_reason_current, start};
	transmit_debug_event third {30, 300, 3, 3, k_transmit_reason_current, start};
	log.record(second, "weapon");
	log.record(third, "wearable");
	assert(records[0].handle == 300 && records[1].handle == 200);
	log.clear();
	assert(!records[0].valid && !records[1].valid);
}

void test_hidden_entity_group()
{
	using clock = std::chrono::steady_clock;
	const auto start = clock::time_point {} + std::chrono::seconds(30);
	hidden_entity_group<uint32_t, 3> source;
	std::array<uint32_t, 4> handles {10, 11, 12, 0};
	source.source = 10;
	std::copy_n(handles.begin(), source.handles.size(), source.handles.begin());
	source.count = 3;
	hidden_entity_group<uint32_t, 3> group;

	hidden_group_store(group, source, start, std::chrono::milliseconds(3000));
	assert(group.count == 3);
	assert(group.source == 10u);
	assert(hidden_group_quarantined(group, start + std::chrono::milliseconds(2999)));
	assert(!hidden_group_quarantined(group, start + std::chrono::milliseconds(3000)));
	assert(group.count == 0);

	hidden_group_store(group, source, start, std::chrono::milliseconds(3000));
	std::array<bool, 16> marked {};
	marked[10] = true;
	marked[11] = true;
	assert(!hidden_group_all_of(group, [&](uint32_t handle) { return marked[handle]; }));
	marked[12] = true;
	assert(hidden_group_all_of(group, [&](uint32_t handle) { return marked[handle]; }));

	hidden_group_clear(group);
	assert(hidden_group_append_unique(group, 10u));
	assert(hidden_group_append_unique(group, 10u));
	assert(hidden_group_append_unique(group, 20u));
	assert(hidden_group_append_unique(group, 30u));
	assert(group.count == 3);
	const std::array<uint32_t, 3> expected {10, 20, 30};
	assert(group.handles == expected);
	assert(hidden_group_append_unique(group, 30u));
	assert(!hidden_group_append_unique(group, 40u));
}

double benchmark_worker_loop(const bvh8_data &data, const std::string &label)
{
	constexpr uint32_t k_players = 32;
	const visibility_tuning tuning {48.0f, 0.4f, 128.0f};
	std::mt19937 random(0x51f0u);
	std::uniform_real_distribution<float> x(data.header.world_min[0], data.header.world_max[0]);
	std::uniform_real_distribution<float> y(data.header.world_min[1], data.header.world_max[1]);
	std::uniform_real_distribution<float> z(data.header.world_min[2], data.header.world_max[2]);
	std::array<visibility_player, k_players> players {};
	for (uint32_t i = 0; i < k_players; ++i)
	{
		const vec3 origin {x(random), y(random), z(random)};
		players[i] = {
			{origin.x, origin.y, origin.z + 64.0f},
			origin,
			{-16.0f, -16.0f, 0.0f},
			{16.0f, 16.0f, 72.0f},
			static_cast<float>(i % 8u) * 45.0f,
			static_cast<float>(i % 4u) * 0.025f,
			i % 3u == 0 ? k_visibility_button_forward | k_visibility_button_left : k_visibility_button_forward,
			weapon_muzzle_class::rifle
		};
	}
	smoke_snapshot smoke;
	smoke.volumes.emplace_back();
	smoke.volumes.back().center = {
		(data.header.world_min[0] + data.header.world_max[0]) * 0.5f,
		(data.header.world_min[1] + data.header.world_max[1]) * 0.5f,
		(data.header.world_min[2] + data.header.world_max[2]) * 0.5f
	};
	smoke.volumes.back().age_seconds = 2.0f;
	smoke.volumes.back().density.fill(50.0f);
	smoke.he_clear_radius_units = 100.0f;
	smoke.he_clear_seconds = 3.0f;
	smoke.he_clearance_count = 4;
	for (uint32_t index = 0; index < smoke.he_clearance_count; ++index)
	{
		smoke.he_clearances[index] = {{smoke.volumes.back().center.x + static_cast<float>(index) * 25.0f,
			smoke.volumes.back().center.y, smoke.volumes.back().center.z}, 1.0f};
	}

	std::vector<uint32_t> cache(k_players * k_players * k_visibility_ray_count_max, k_invalid_ref);
	std::array<double, 20> timings {};
	uint64_t blocked_pairs = 0;
	for (double &timing : timings)
	{
		const auto start = std::chrono::steady_clock::now();
		std::array<visibility_origin_points, k_players> origins {};
		std::array<visibility_target_points, k_players> targets {};
		for (uint32_t recipient = 0; recipient < k_players; ++recipient)
		{
			origins[recipient] = visibility_origins(data, players[recipient], tuning);
			targets[recipient] = visibility_targets(players[recipient]);
		}
		for (uint32_t recipient = 0; recipient < k_players; ++recipient)
		{
			for (uint32_t target = 0; target < k_players; ++target)
			{
				if (recipient == target)
				{
					continue;
				}
				const auto &target_points = targets[target];
				bool blocked = true;
				uint32_t ray = 0;
				for (uint32_t origin_index = 0; origin_index < origins[recipient].count; ++origin_index)
				{
					const vec3 &origin = origins[recipient].points[origin_index];
					for (uint32_t point_index = 0; point_index < target_points.count; ++point_index)
					{
						const vec3 &point = target_points.points[point_index];
						uint32_t &cached = cache[(recipient * k_players + target) * k_visibility_ray_count_max + ray];
						const ray_hit hit = segment_blocked(data, origin, point, cached);
						cached = hit.packet_index;
						++ray;
						if (!hit.blocked && !smoke_line_blocked(smoke, origin, point, 0.0f, &data))
						{
							blocked = false;
							break;
						}
					}
					if (!blocked)
					{
						break;
					}
				}
				blocked_pairs += blocked;
			}
		}
		timing = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}
	std::sort(timings.begin(), timings.end());
	double average = 0;
	for (double timing : timings) average += timing;
	average /= timings.size();
	constexpr uint32_t k_pairs = k_players * (k_players - 1u);
	std::cout << label << " worker-loop: average=" << average << "ms p99=" << timings.back() << "ms pairs=" << k_pairs
		<< " rays_max=" << k_pairs * k_visibility_ray_count_max << " blocked_pairs=" << blocked_pairs << '\n';
	return average;
}

} // namespace

void run_visibility_and_transmit_tests()
{
	test_visibility_pair_eligibility();
	test_smoke_occlusion();
	test_visibility_sampling();
	test_visibility_worker();
	test_lifecycle_guard();
	test_visual_group_key();
	test_pair_guard();
	test_checktransmit_private_offsets();
	test_transmit_debug();
	test_hidden_entity_group();
}

double run_worker_benchmark(const cs2fow::bvh8_data &data, const std::string &label)
{
	return benchmark_worker_loop(data, label);
}
