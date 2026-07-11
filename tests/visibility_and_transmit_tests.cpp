#include "test_suites.h"

// Exercises copied visibility sampling/worker behavior and fixed CheckTransmit
// lifecycle, visual-group, mask, and evidence helpers. Fake data keeps engine
// objects and live network lists out of the unit tests.

#include "builder.h"
#include "lifecycle_guard.h"
#include "transmit_debug.h"
#include "transmit_masks.h"
#include "visibility_sampling.h"
#include "visibility_worker.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
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

void test_visibility_sampling()
{
	const bvh8_data open = test_world({{{10000, 10000, 10000}, {10001, 10000, 10000}, {10000, 10001, 10000}}});
	const visibility_tuning tuning {50, 150};
	visibility_player player {};
	player.eye = {0, 0, 0};
	player.origin = {0, 0, 0};
	player.mins = {-16, -16, 0};
	player.maxs = {16, 16, 72};

	assert(std::fabs(visibility_effective_lookahead_seconds(-1.0f, tuning) - 0.05f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.0f, tuning) - 0.05f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.05f, tuning) - 0.075f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.1f, tuning) - 0.1f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.2f, tuning) - 0.15f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.5f, tuning) - 0.15f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.0f, {200, 100}) - 0.1f) < 0.001f);

	assert(visibility_prediction_offset({0, 0, 0}, 0.05f).x == 0.0f);
	assert(visibility_prediction_offset({99, 0, 0}, 0.15f).x == 0.0f);
	assert(std::fabs(visibility_prediction_offset({100, 0, 0}, 0.05f).x - 5.0f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({149, 0, 0}, 0.05f).x - 7.45f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({150, 0, 0}, 0.05f).x - 7.5f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({249, 0, 0}, 0.05f).x - 12.45f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({250, 0, 0}, 0.05f).x - 12.5f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({299, 0, 0}, 0.05f).x - 14.95f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({300, 0, 0}, 0.05f).x - 15.0f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({350, 0, 0}, 0.15f).x - 52.5f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({500, 0, 0}, 0.15f).x - 52.5f) < 0.01f);
	const vec3 diagonal = visibility_prediction_offset({300, 400, 0}, 0.1f);
	assert(std::fabs(diagonal.x - 21.0f) < 0.01f && std::fabs(diagonal.y - 28.0f) < 0.01f);

	player.velocity = {100, 0, 0};
	auto origins = visibility_origins(open, player, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(k_visibility_origin_count == 8);
	assert(k_visibility_ray_count_max == 320);
	assert(std::fabs(origins[1].x - 5.0f) < 0.01f && origins[1].y == 0.0f);
	assert(std::fabs(origins[2].y - 24.0f) < 0.01f);
	assert(std::fabs(origins[3].y + 24.0f) < 0.01f);
	assert(std::fabs(origins[4].x - 5.0f) < 0.01f && std::fabs(origins[4].y - 24.0f) < 0.01f);
	assert(std::fabs(origins[5].x - 5.0f) < 0.01f && std::fabs(origins[5].y + 24.0f) < 0.01f);
	assert(std::fabs(origins[6].z - 24.0f) < 0.01f);
	assert(std::fabs(origins[7].x - 5.0f) < 0.01f && std::fabs(origins[7].z - 24.0f) < 0.01f);

	player.velocity = {};
	player.eye_yaw_degrees = 90.0f;
	origins = visibility_origins(open, player, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(std::fabs(origins[2].x + 24.0f) < 0.01f);
	assert(std::fabs(origins[3].x - 24.0f) < 0.01f);
	player.eye_yaw_degrees = 0.0f;

	player.velocity = {0, 100, 0};
	origins = visibility_origins(open, player, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(std::fabs(origins[1].y - 5.0f) < 0.01f);

	player.velocity = {1000, 0, 0};
	origins = visibility_origins(open, player, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(std::fabs(origins[1].x - 17.5f) < 0.01f);

	visibility_tuning disabled = tuning;
	disabled.max_lookahead_ms = 0;
	assert(visibility_effective_lookahead_seconds(0.2f, disabled) == 0.0f);
	origins = visibility_origins(open, player, visibility_effective_lookahead_seconds(0.0f, disabled));
	assert(origins[1].x == player.eye.x && origins[1].y == player.eye.y && origins[1].z == player.eye.z);

	const bvh8_data wall = test_world({
		{{4, -100, -100}, {4, 100, -100}, {4, -100, 100}},
		{{4, 100, 100}, {4, -100, 100}, {4, 100, -100}}
	});
	player.velocity = {100, 0, 0};
	origins = visibility_origins(wall, player, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(origins[1].x == player.eye.x && origins[1].y == player.eye.y && origins[1].z == player.eye.z);

	const bvh8_data side_wall = test_world({
		{{-100, -8, -100}, {100, -8, -100}, {-100, -8, 100}},
		{{100, -8, 100}, {-100, -8, 100}, {100, -8, -100}}
	});
	player.velocity = {};
	origins = visibility_origins(side_wall, player, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(origins[3].x == player.eye.x && origins[3].y == player.eye.y && origins[3].z == player.eye.z);

	const bvh8_data ceiling = test_world({
		{{-100, -100, 8}, {100, -100, 8}, {-100, 100, 8}},
		{{100, 100, 8}, {-100, 100, 8}, {100, -100, 8}}
	});
	origins = visibility_origins(ceiling, player, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(origins[6].x == player.eye.x && origins[6].y == player.eye.y && origins[6].z == player.eye.z);

	visibility_player target {};
	target.origin = {0, 0, 0};
	target.mins = {-16, -16, 0};
	target.maxs = {16, 16, 72};
	target.velocity = {200, 0, 0};
	target.muzzle_class = weapon_muzzle_class::rifle;
	const auto current_targets = visibility_targets(open, target, 0.0f);
	const auto no_recipient_lookahead = visibility_targets(open, target, 0.0f);
	const auto swept_targets = visibility_targets(open, target, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(current_targets.count == 24);
	assert(no_recipient_lookahead.count == 24);
	assert(swept_targets.count == 40);
	assert(std::fabs(min_x(current_targets) + 20.0f) < 0.01f);
	assert(std::fabs(max_x(current_targets) - 36.0f) < 0.01f);
	assert(std::fabs(max_x(no_recipient_lookahead) - max_x(current_targets)) < 0.01f);
	assert(max_x(swept_targets) > max_x(current_targets) + 9.0f);

	target.muzzle_class = weapon_muzzle_class::none;
	const auto no_muzzle_targets = visibility_targets(open, target, 0.0f);
	assert(no_muzzle_targets.count == 23);
	target.muzzle_class = weapon_muzzle_class::rifle;

	target.velocity = {100, 0, 0};
	const auto blocked_target_prediction = visibility_targets(wall, target, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(blocked_target_prediction.count == 24);

	target.velocity = {};
	target.eye_yaw_degrees = 0.0f;
	const auto yaw0_targets = visibility_targets(open, target, 0.0f);
	const vec3 yaw0_head = yaw0_targets.points[8];
	assert(std::fabs(yaw0_head.x - 5.6092f) < 0.01f);
	assert(std::fabs(yaw0_head.y + 1.4428f) < 0.01f);
	assert(std::fabs(yaw0_head.z - 64.2013f) < 0.01f);

	target.eye_yaw_degrees = 90.0f;
	const auto yaw90_targets = visibility_targets(open, target, 0.0f);
	const vec3 yaw90_head = yaw90_targets.points[8];
	assert(std::fabs(yaw90_head.x - 1.4428f) < 0.01f);
	assert(std::fabs(yaw90_head.y - 5.6092f) < 0.01f);

	target.eye_yaw_degrees = 180.0f;
	const auto yaw180_targets = visibility_targets(open, target, 0.0f);
	const vec3 yaw180_head = yaw180_targets.points[8];
	assert(std::fabs(yaw180_head.x + 5.6092f) < 0.01f);
	assert(std::fabs(yaw180_head.y - 1.4428f) < 0.01f);

	target.eye_yaw_degrees = 0.0f;
	target.maxs.z = 54.0f;
	const auto crouched_targets = visibility_targets(open, target, 0.0f);
	const vec3 crouched_head = crouched_targets.points[8];
	const vec3 crouched_left_foot = crouched_targets.points[18];
	const vec3 crouched_muzzle = crouched_targets.points[23];
	assert(crouched_head.z < yaw0_head.z - 10.0f && crouched_head.z > 45.0f);
	assert(std::fabs(crouched_left_foot.z - 4.0f) < 0.01f);
	assert(crouched_muzzle.z < 50.0f && crouched_muzzle.z > 45.0f);

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
}

void test_visibility_worker()
{
	const bvh8_data wall = test_world({
		{{32, -1000, -1000}, {32, 1000, -1000}, {32, -1000, 1000}},
		{{32, 1000, 1000}, {32, -1000, 1000}, {32, 1000, -1000}}
	});
	visibility_snapshot value;
	value.sequence = 1;
	value.players[0] = {true, 2, {0, 0, 64}, {0, 0, 0}, {}, {-16, -16, 0}, {16, 16, 72}};
	value.players[1] = {true, 3, {64, 0, 64}, {64, 0, 0}, {}, {-16, -16, 0}, {16, 16, 72}};

	auto worker = std::make_unique<visibility_worker>();
	worker->start(&wall);
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
	worker->submit(value, 32, {50, 150});
	auto result = wait_for(1);
	assert(result && !result->visible[0][1]);

	value.sequence = 2;
	value.players[1].eye.x = 16;
	value.players[1].origin.x = 16;
	worker->submit(value, 32, {50, 150});
	result = wait_for(2);
	assert(result && result->visible[0][1]);

	value.sequence = 3;
	value.players[1].eye.x = 64;
	value.players[1].origin.x = 64;
	worker->submit(value, 32, {50, 150});
	result = wait_for(3);
	assert(result && result->visible[0][1]);

	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	value.sequence = 4;
	worker->submit(value, 32, {50, 150});
	result = wait_for(4);
	assert(result && !result->visible[0][1]);
	assert(worker->stats().cycles == 4);
	worker->start(&wall);
	assert(worker->stats().cycles == 0);
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
	using clock = std::chrono::steady_clock;
	const auto warmup = std::chrono::milliseconds(1500);
	const auto start = clock::time_point {} + std::chrono::seconds(20);
	lifecycle_key recipient;
	recipient.has_controller = true;
	recipient.pawn_entity = 10;
	recipient.team = 2;
	recipient.alive = true;
	lifecycle_key target = recipient;
	target.pawn_entity = 20;
	target.team = 3;

	pair_guard guard;
	assert(update_pair_guard(guard, recipient, true, target, true, start, warmup));
	assert(!update_pair_guard(guard, recipient, true, target, true, start, warmup));
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(1499), 1));
	pair_note_open(guard, start + std::chrono::milliseconds(1499), 1);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(1500), 1));
	pair_note_open(guard, start + std::chrono::milliseconds(1500), 1);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(1501), 1));
	assert(pair_allows_hiding(guard, start + std::chrono::milliseconds(1501), 2));

	pair_guard visual_guard;
	const visual_group_key group = test_visual_key({10, 20, 30});
	const visual_group_key same_group = test_visual_key({30, 20, 10, 10});
	const visual_group_key changed_group = test_visual_key({10, 20, 31});
	update_pair_guard(visual_guard, recipient, true, target, true, start, warmup);
	update_pair_visual_group(visual_guard, group, start, warmup);
	assert(!pair_allows_hiding(visual_guard, start + std::chrono::milliseconds(1499), 1));
	pair_note_open(visual_guard, start + std::chrono::milliseconds(1500), 1);
	assert(pair_allows_hiding(visual_guard, start + std::chrono::milliseconds(1500), 2));
	update_pair_visual_group(visual_guard, same_group, start + std::chrono::milliseconds(1600), warmup);
	assert(pair_allows_hiding(visual_guard, start + std::chrono::milliseconds(1600), 3));
	update_pair_visual_group(visual_guard, changed_group, start + std::chrono::milliseconds(1700), warmup);
	assert(!pair_allows_hiding(visual_guard, start + std::chrono::milliseconds(3199), 4));
	pair_note_open(visual_guard, start + std::chrono::milliseconds(3200), 4);
	assert(!pair_allows_hiding(visual_guard, start + std::chrono::milliseconds(3200), 4));
	assert(pair_allows_hiding(visual_guard, start + std::chrono::milliseconds(3200), 5));

	lifecycle_key changed = target;
	changed.team = 2;
	assert(update_pair_guard(guard, recipient, true, changed, true, start + std::chrono::milliseconds(2000), warmup));
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(3499), 3));
	pair_note_open(guard, start + std::chrono::milliseconds(3500), 3);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(3500), 3));
	assert(pair_allows_hiding(guard, start + std::chrono::milliseconds(3500), 4));

	lifecycle_key dead = changed;
	dead.alive = false;
	assert(update_pair_guard(guard, recipient, true, dead, false, start + std::chrono::milliseconds(4000), warmup));
	assert(update_pair_guard(guard, recipient, true, dead, false, start + std::chrono::milliseconds(4500), warmup));
	pair_note_open(guard, start + std::chrono::milliseconds(5999), 5);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(5999), 6));

	assert(update_pair_guard(guard, recipient, true, changed, true, start + std::chrono::milliseconds(6000), warmup));
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(7499), 7));
	pair_note_open(guard, start + std::chrono::milliseconds(7500), 7);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(7500), 7));
	assert(pair_allows_hiding(guard, start + std::chrono::milliseconds(7500), 8));
}

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
}

void test_transmit_debug()
{
	assert(transmit_member_from_links(false, false) == transmit_member_kind::direct);
	assert(transmit_member_from_links(true, false) == transmit_member_kind::owner_link);
	assert(transmit_member_from_links(false, true) == transmit_member_kind::effect_link);
	assert(transmit_member_from_links(true, true) == transmit_member_kind::owner_effect_link);

	struct test_mask
	{
		bool set {};
		int checks {};
		int clears {};

		bool IsBitSet(int)
		{
			++checks;
			return set;
		}

		void Clear(int)
		{
			++clears;
			set = false;
		}
	};

	test_mask mask {true};
	assert(!clear_transmit_bit(mask, 10, false));
	assert(mask.checks == 0 && mask.clears == 1 && !mask.set);
	assert(!clear_transmit_bit(mask, 10, true));
	assert(mask.checks == 1 && mask.clears == 2);
	mask.set = true;
	assert(clear_transmit_bit(mask, 10, true));
	assert(mask.checks == 2 && mask.clears == 3 && !mask.set);

	using clock = std::chrono::steady_clock;
	const auto start = clock::time_point {} + std::chrono::seconds(10);
	transmit_debug_log<2, 16> log;
	transmit_debug_event first {10, 100, 1, 0, 0, 1, transmit_member_kind::direct, k_transmit_reason_current, start};
	log.record(first, "player");
	first.recipient_slot = 3;
	first.reason = k_transmit_reason_quarantine;
	first.when += std::chrono::milliseconds(5);
	log.record(first, "player");
	const auto &records = log.records();
	assert(records[0].valid && records[0].clears == 2 && records[0].recipients == ((uint64_t {1} << 1) | (uint64_t {1} << 3)));
	assert(records[0].reasons == (k_transmit_reason_current | k_transmit_reason_quarantine));
	assert(records[0].first_seen == start && records[0].last_seen == start + std::chrono::milliseconds(5));

	transmit_debug_event second {20, 200, 2, 10, 0, 2, transmit_member_kind::owner_link, k_transmit_reason_current, start};
	transmit_debug_event third {30, 300, 3, 0, 20, 3, transmit_member_kind::effect_link, k_transmit_reason_current, start};
	log.record(second, "weapon");
	log.record(third, "effect");
	assert(records[0].handle == 300 && records[1].handle == 200);
	log.clear();
	assert(!records[0].valid && !records[1].valid);
}

void test_hidden_entity_group()
{
	using clock = std::chrono::steady_clock;
	const auto start = clock::time_point {} + std::chrono::seconds(30);
	hidden_entity_group<uint32_t, 5> source;
	std::array<uint32_t, 4> handles {10, 11, 12, 0};
	source.source = 10;
	std::copy(handles.begin(), handles.end(), source.handles.begin());
	source.count = 3;
	hidden_entity_group<uint32_t, 5> group;

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
	group.handles[0] = 10;
	group.handles[1] = 20;
	group.count = 2;
	const std::array<owner_effect_link<uint32_t>, 5> links {{
		{30, 10, 0},
		{31, 0, 20},
		{32, 10, 20},
		{32, 99, 98},
		{20, 10, 0}
	}};
	assert(hidden_group_append_owner_effect_links(group, links.data(), links.size(), [](uint32_t child) { return child != 0; }));
	assert(group.count == 5);
	assert(hidden_group_contains(group, 30u, group.count));
	assert(hidden_group_contains(group, 31u, group.count));
	assert(hidden_group_contains(group, 32u, group.count));
	assert(group.link_owners[2] == 10 && group.link_effects[2] == 0);
	assert(group.link_owners[3] == 0 && group.link_effects[3] == 20);
	assert(group.link_owners[4] == 10 && group.link_effects[4] == 20);
	hidden_entity_group<uint32_t, 5> stored;
	hidden_group_store(stored, group, start, std::chrono::milliseconds(3000));
	assert(stored.link_owners == group.link_owners && stored.link_effects == group.link_effects);

	const std::array<owner_effect_link<uint32_t>, 1> overflow {{{33, 10, 0}}};
	assert(!hidden_group_append_owner_effect_links(group, overflow.data(), overflow.size(), [](uint32_t) { return true; }));

	hidden_group_clear(group);
	group.handles[0] = 10;
	group.count = 1;
	const std::array<owner_effect_link<uint32_t>, 1> unusable {{{30, 10, 0}}};
	assert(hidden_group_append_owner_effect_links(group, unusable.data(), unusable.size(), [](uint32_t) { return false; }));
	assert(group.count == 1);
}

double benchmark_worker_loop(const bvh8_data &data, const std::string &label)
{
	constexpr uint32_t k_players = 32;
	const visibility_tuning tuning {50, 150};
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
			{i % 2u == 0 ? 250.0f : -250.0f, 0.0f, 0.0f},
			{-16.0f, -16.0f, 0.0f},
			{16.0f, 16.0f, 72.0f},
			static_cast<float>(i % 8u) * 45.0f,
			static_cast<float>(i % 4u) * 0.025f,
			weapon_muzzle_class::rifle
		};
	}

	std::vector<uint32_t> cache(k_players * k_players * k_visibility_ray_count_max, k_invalid_ref);
	std::array<double, 20> timings {};
	uint64_t blocked_pairs = 0;
	for (double &timing : timings)
	{
		const auto start = std::chrono::steady_clock::now();
		std::array<float, k_players> lookahead {};
		std::array<std::array<vec3, k_visibility_origin_count>, k_players> origins {};
		for (uint32_t recipient = 0; recipient < k_players; ++recipient)
		{
			lookahead[recipient] = visibility_effective_lookahead_seconds(players[recipient].rtt_seconds, tuning);
			origins[recipient] = visibility_origins(data, players[recipient], lookahead[recipient]);
		}
		for (uint32_t recipient = 0; recipient < k_players; ++recipient)
		{
			for (uint32_t target = 0; target < k_players; ++target)
			{
				if ((recipient < 16u) == (target < 16u))
				{
					continue;
				}
				const auto targets = visibility_targets(data, players[target], lookahead[recipient]);
				bool blocked = true;
				uint32_t ray = 0;
				for (const vec3 &origin : origins[recipient])
				{
					for (uint32_t point_index = 0; point_index < targets.count; ++point_index)
					{
						const vec3 &point = targets.points[point_index];
						uint32_t &cached = cache[(recipient * k_players + target) * k_visibility_ray_count_max + ray];
						const ray_hit hit = segment_blocked(data, origin, point, cached);
						cached = hit.packet_index;
						++ray;
						if (!hit.blocked)
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
	std::cout << label << " worker-loop: average=" << average << "ms p99=" << timings.back() << "ms pairs=512 rays_max=" << 512u * k_visibility_ray_count_max << " blocked_pairs=" << blocked_pairs << '\n';
	return average;
}

} // namespace

void run_visibility_and_transmit_tests()
{
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
