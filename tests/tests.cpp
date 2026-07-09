#include "builder.h"
#include "glb_import.h"
#include "lifecycle_guard.h"
#include "map_source.h"
#include "subprocess.h"
#include "transmit_masks.h"
#include "visibility_sampling.h"
#include "vpk.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <random>
#include <string>
#include <thread>

namespace
{

using namespace cs2fow;

template <typename type>
void append(std::vector<std::byte> &bytes, type value)
{
	const std::byte *source = reinterpret_cast<const std::byte *>(&value);
	bytes.insert(bytes.end(), source, source + sizeof(value));
}

void append_string(std::vector<std::byte> &bytes, const char *value)
{
	const size_t size = std::strlen(value) + 1u;
	const std::byte *source = reinterpret_cast<const std::byte *>(value);
	bytes.insert(bytes.end(), source, source + size);
}

void write_test_vpk(const std::filesystem::path &path, const char *extension, const char *directory,
	const char *name, uint32_t crc, uint32_t size)
{
	std::vector<std::byte> tree;
	append_string(tree, extension);
	append_string(tree, directory);
	append_string(tree, name);
	append<uint32_t>(tree, crc);
	append<uint16_t>(tree, 0u);
	append<uint16_t>(tree, 0x7fffu);
	append<uint32_t>(tree, 0u);
	append<uint32_t>(tree, size);
	append<uint16_t>(tree, 0xffffu);
	append_string(tree, "");
	append_string(tree, "");
	append_string(tree, "");
	std::ofstream stream(path, std::ios::binary);
	const uint32_t header[] = {0x55aa1234u, 2u, static_cast<uint32_t>(tree.size()), 0u, 0u, 0u, 0u};
	stream.write(reinterpret_cast<const char *>(header), sizeof(header));
	stream.write(reinterpret_cast<const char *>(tree.data()), static_cast<std::streamsize>(tree.size()));
	stream.close();
}

void test_vpk(const std::filesystem::path &directory)
{
	const std::filesystem::path path = directory / "test.vpk";
	write_test_vpk(path, "vmdl_c", "maps/de_test", "world_physics", 0x12345678u, 42u);
	vpk_entry entry;
	std::string error;
	assert(find_vpk_entry(path, "maps/de_test/world_physics.vmdl_c", entry, error));
	assert(entry.crc32 == 0x12345678u && entry.size == 42u);
	map_source source;
	assert(find_map_source(path, "de_test", source, error));
	assert(source.flags == 0 && source.entry == "maps/de_test/world_physics.vmdl_c");

	const std::filesystem::path nested = directory / "nested_dir.vpk";
	write_test_vpk(nested, "vpk", "maps", "de_nested", 0x87654321u, 100u);
	assert(find_map_source(nested, "de_nested", source, error));
	assert(source.flags == k_bvh8_flag_nested_map_vpk && source.entry == "maps/de_nested.vpk");
	assert(source.metadata.crc32 == 0x87654321u && source.metadata.size == 100u);
	map_source copy = source;
	assert(same_map_source(source, copy));
	++copy.metadata.size;
	assert(!same_map_source(source, copy));

	const std::filesystem::path workshop = directory / "3744399267";
	std::filesystem::create_directories(workshop);
	const std::filesystem::path logical = workshop / "3744399267.vpk";
	const std::filesystem::path real = workshop / "3744399267_dir.vpk";
	write_test_vpk(real, "vpk", "maps", "aim_redline", 0xa6a87540u, 19770502u);
	const std::vector<std::filesystem::path> candidates = vpk_path_candidates("vpk:" + logical.string() + ":maps\\aim_redline.vpk");
	assert(candidates.size() == 2u);
	assert(candidates[0] == logical);
	assert(candidates[1] == real);
	assert(!find_map_source(candidates[0], "aim_redline", source, error));
	assert(find_map_source(candidates[1], "aim_redline", source, error));
	assert(source.vpk == real && source.flags == k_bvh8_flag_nested_map_vpk);

	assert(valid_map_name("de_test"));
	assert(valid_map_name("workshop/123/de_test-2.0"));
	assert(!valid_map_name("../de_test"));
	assert(!valid_map_name("/de_test"));
	assert(!valid_map_name("de_test&command"));
	assert(!valid_map_name("de_test\\other"));
}

void test_subprocess(const std::filesystem::path &executable)
{
	process_result result;
	std::string error;
	assert(run_process(executable, {"--process-probe", "space [test]"}, std::chrono::seconds(5), nullptr, false, result, error));
	assert(result.exit_code == 23 && !result.cancelled && !result.timed_out);

	assert(run_process(executable, {"--process-sleep"}, std::chrono::milliseconds(50), nullptr, false, result, error));
	assert(result.timed_out);

	std::atomic_bool cancel {false};
	std::thread canceller([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		cancel.store(true);
	});
	assert(run_process(executable, {"--process-sleep"}, std::chrono::seconds(5), &cancel, false, result, error));
	canceller.join();
	assert(result.cancelled);
}

void write_test_glb(const std::filesystem::path &path)
{
	std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","max":[1,1,0],"min":[0,0,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0,"extras":{"InteractAs":[]}}],"scenes":[{"nodes":[0]}],"scene":0})";
	while (json.size() % 4u != 0) json.push_back(' ');
	std::vector<std::byte> binary;
	const float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const uint16_t indices[] = {0, 1, 2};
	binary.insert(binary.end(), reinterpret_cast<const std::byte *>(vertices), reinterpret_cast<const std::byte *>(vertices) + sizeof(vertices));
	binary.insert(binary.end(), reinterpret_cast<const std::byte *>(indices), reinterpret_cast<const std::byte *>(indices) + sizeof(indices));
	while (binary.size() % 4u != 0) binary.push_back(std::byte {0});
	std::ofstream stream(path, std::ios::binary);
	const uint32_t total = static_cast<uint32_t>(12u + 8u + json.size() + 8u + binary.size());
	const uint32_t header[] = {0x46546c67u, 2u, total, static_cast<uint32_t>(json.size()), 0x4e4f534au};
	stream.write(reinterpret_cast<const char *>(header), sizeof(header));
	stream.write(json.data(), static_cast<std::streamsize>(json.size()));
	const uint32_t bin_header[] = {static_cast<uint32_t>(binary.size()), 0x004e4942u};
	stream.write(reinterpret_cast<const char *>(bin_header), sizeof(bin_header));
	stream.write(reinterpret_cast<const char *>(binary.data()), static_cast<std::streamsize>(binary.size()));
}

void test_glb(const std::filesystem::path &directory)
{
	const std::filesystem::path path = directory / "test.glb";
	write_test_glb(path);
	std::vector<triangle> triangles;
	import_report report;
	std::string error;
	assert(import_physics_glb(path, triangles, report, error));
	assert(triangles.size() == 1 && report.raw_triangles == 1 && report.groups.size() == 1 && report.groups[0].accepted);
	assert(physics_tags_accepted({}) && physics_tags_accepted({"blocklight", "solid"}));
	assert(!physics_tags_accepted({"playerclip"}) && !physics_tags_accepted({"unknown"}));
}

bool scalar_hit(const triangle &value, vec3 origin, vec3 target)
{
	const vec3 direction {target.x - origin.x, target.y - origin.y, target.z - origin.z};
	const vec3 e1 {value.v1.x - value.v0.x, value.v1.y - value.v0.y, value.v1.z - value.v0.z};
	const vec3 e2 {value.v2.x - value.v0.x, value.v2.y - value.v0.y, value.v2.z - value.v0.z};
	const vec3 p {direction.y * e2.z - direction.z * e2.y, direction.z * e2.x - direction.x * e2.z, direction.x * e2.y - direction.y * e2.x};
	const float det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
	if (std::fabs(det) <= 1.0e-5f) return false;
	const float inverse = 1.0f / det;
	const vec3 t {origin.x - value.v0.x, origin.y - value.v0.y, origin.z - value.v0.z};
	const float u = (t.x * p.x + t.y * p.y + t.z * p.z) * inverse;
	if (u < 0 || u > 1) return false;
	const vec3 q {t.y * e1.z - t.z * e1.y, t.z * e1.x - t.x * e1.z, t.x * e1.y - t.y * e1.x};
	const float v = (direction.x * q.x + direction.y * q.y + direction.z * q.z) * inverse;
	if (v < 0 || u + v > 1) return false;
	const float distance = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inverse;
	return distance > 1.0e-5f && distance < 1.0f - 1.0e-5f;
}

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

struct test_transmit_mask
{
	std::array<bool, 64> bits {};

	void Set(int index)
	{
		bits[static_cast<size_t>(index)] = true;
	}

	void Clear(int index)
	{
		bits[static_cast<size_t>(index)] = false;
	}

	bool IsBitSet(int index) const
	{
		return bits[static_cast<size_t>(index)];
	}
};

void test_visibility_sampling()
{
	const bvh8_data open = test_world({{{10000, 10000, 10000}, {10001, 10000, 10000}, {10000, 10001, 10000}}});
	const visibility_tuning tuning {1, 200, 500, 160.0f};
	visibility_player player {};
	player.eye = {0, 0, 0};
	player.origin = {0, 0, 0};
	player.mins = {-16, -16, 0};
	player.maxs = {16, 16, 72};

	assert(std::fabs(visibility_effective_lookahead_seconds(0.0f, tuning) - 0.2f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.025f, tuning) - 0.25f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.05f, tuning) - 0.3f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.075f, tuning) - 0.35f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.1f, tuning) - 0.4f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.125f, tuning) - 0.45f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.15f, tuning) - 0.5f) < 0.001f);
	assert(std::fabs(visibility_effective_lookahead_seconds(0.2f, tuning) - 0.5f) < 0.001f);

	assert(visibility_prediction_offset({0, 0, 0}, 0.2f, 160.0f).x == 0.0f);
	assert(visibility_prediction_offset({1, 0, 0}, 0.2f, 160.0f).x == 0.0f);
	assert(std::fabs(visibility_prediction_offset({10, 0, 0}, 0.2f, 160.0f).x - 16.0f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({50, 0, 0}, 0.2f, 160.0f).x - 32.0f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({100, 0, 0}, 0.2f, 160.0f).x - 64.0f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({200, 0, 0}, 0.2f, 160.0f).x - 96.0f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({275, 0, 0}, 0.2f, 160.0f).x - 128.0f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({300, 0, 0}, 0.2f, 160.0f).x - 160.0f) < 0.01f);
	assert(std::fabs(visibility_prediction_offset({300, 0, 0}, 0.2f, 80.0f).x - 80.0f) < 0.01f);

	player.velocity = {100, 0, 0};
	auto origins = visibility_origins(open, player, tuning, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(k_visibility_origin_count == 10);
	assert(k_visibility_ray_count_max == 400);
	assert(std::fabs(origins[1].x - 64.0f) < 0.01f && origins[1].y == 0.0f);
	assert(std::fabs(origins[2].y - 24.0f) < 0.01f);
	assert(std::fabs(origins[3].y + 24.0f) < 0.01f);
	assert(std::fabs(origins[4].x - 64.0f) < 0.01f && std::fabs(origins[4].y - 24.0f) < 0.01f);
	assert(std::fabs(origins[5].x - 64.0f) < 0.01f && std::fabs(origins[5].y + 24.0f) < 0.01f);
	assert(std::fabs(origins[6].z - 24.0f) < 0.01f);
	assert(std::fabs(origins[7].z + 24.0f) < 0.01f);
	assert(std::fabs(origins[8].x - 64.0f) < 0.01f && std::fabs(origins[8].z - 24.0f) < 0.01f);
	assert(std::fabs(origins[9].x - 64.0f) < 0.01f && std::fabs(origins[9].z + 24.0f) < 0.01f);

	player.velocity = {};
	player.eye_yaw_degrees = 90.0f;
	origins = visibility_origins(open, player, tuning, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(std::fabs(origins[2].x + 24.0f) < 0.01f);
	assert(std::fabs(origins[3].x - 24.0f) < 0.01f);
	player.eye_yaw_degrees = 0.0f;

	player.velocity = {0, 10, 0};
	origins = visibility_origins(open, player, tuning, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(std::fabs(origins[1].y - 16.0f) < 0.01f);

	player.velocity = {1000, 0, 0};
	origins = visibility_origins(open, player, tuning, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(std::fabs(origins[1].x - 160.0f) < 0.01f);

	visibility_tuning disabled = tuning;
	disabled.max_lookahead_ms = 0;
	origins = visibility_origins(open, player, disabled, visibility_effective_lookahead_seconds(0.0f, disabled));
	assert(origins[1].x == player.eye.x && origins[1].y == player.eye.y && origins[1].z == player.eye.z);

	const bvh8_data wall = test_world({
		{{8, -100, -100}, {8, 100, -100}, {8, -100, 100}},
		{{8, 100, 100}, {8, -100, 100}, {8, 100, -100}}
	});
	player.velocity = {10, 0, 0};
	origins = visibility_origins(wall, player, tuning, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(origins[1].x == player.eye.x && origins[1].y == player.eye.y && origins[1].z == player.eye.z);

	const bvh8_data side_wall = test_world({
		{{-100, -8, -100}, {100, -8, -100}, {-100, -8, 100}},
		{{100, -8, 100}, {-100, -8, 100}, {100, -8, -100}}
	});
	player.velocity = {};
	origins = visibility_origins(side_wall, player, tuning, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(origins[3].x == player.eye.x && origins[3].y == player.eye.y && origins[3].z == player.eye.z);

	const bvh8_data ceiling = test_world({
		{{-100, -100, 8}, {100, -100, 8}, {-100, 100, 8}},
		{{100, 100, 8}, {-100, 100, 8}, {100, -100, 8}}
	});
	origins = visibility_origins(ceiling, player, tuning, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(origins[6].x == player.eye.x && origins[6].y == player.eye.y && origins[6].z == player.eye.z);

	visibility_player target {};
	target.origin = {0, 0, 0};
	target.mins = {-16, -16, 0};
	target.maxs = {16, 16, 72};
	target.velocity = {100, 0, 0};
	target.muzzle_class = weapon_muzzle_class::rifle;
	const auto current_targets = visibility_targets(open, target, disabled, 0.0f);
	const auto no_observer_lookahead = visibility_targets(open, target, tuning, 0.0f);
	const auto swept_targets = visibility_targets(open, target, tuning, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(current_targets.count == 24);
	assert(no_observer_lookahead.count == 24);
	assert(swept_targets.count == 40);
	assert(std::fabs(min_x(current_targets) + 20.0f) < 0.01f);
	assert(std::fabs(max_x(current_targets) - 36.0f) < 0.01f);
	assert(std::fabs(max_x(no_observer_lookahead) - max_x(current_targets)) < 0.01f);
	assert(max_x(swept_targets) > max_x(current_targets) + 30.0f);

	target.muzzle_class = weapon_muzzle_class::none;
	const auto no_muzzle_targets = visibility_targets(open, target, disabled, 0.0f);
	assert(no_muzzle_targets.count == 23);
	target.muzzle_class = weapon_muzzle_class::rifle;

	target.velocity = {100, 0, 0};
	const auto blocked_target_prediction = visibility_targets(wall, target, tuning, visibility_effective_lookahead_seconds(0.0f, tuning));
	assert(blocked_target_prediction.count == 24);

	target.velocity = {};
	target.eye_yaw_degrees = 0.0f;
	const auto yaw0_targets = visibility_targets(open, target, disabled, 0.0f);
	const vec3 yaw0_head = yaw0_targets.points[8];
	assert(std::fabs(yaw0_head.x - 5.6092f) < 0.01f);
	assert(std::fabs(yaw0_head.y + 1.4428f) < 0.01f);
	assert(std::fabs(yaw0_head.z - 64.2013f) < 0.01f);

	target.eye_yaw_degrees = 90.0f;
	const auto yaw90_targets = visibility_targets(open, target, disabled, 0.0f);
	const vec3 yaw90_head = yaw90_targets.points[8];
	assert(std::fabs(yaw90_head.x - 1.4428f) < 0.01f);
	assert(std::fabs(yaw90_head.y - 5.6092f) < 0.01f);

	target.eye_yaw_degrees = 180.0f;
	const auto yaw180_targets = visibility_targets(open, target, disabled, 0.0f);
	const vec3 yaw180_head = yaw180_targets.points[8];
	assert(std::fabs(yaw180_head.x + 5.6092f) < 0.01f);
	assert(std::fabs(yaw180_head.y - 1.4428f) < 0.01f);

	target.eye_yaw_degrees = 0.0f;
	target.maxs.z = 54.0f;
	const auto crouched_targets = visibility_targets(open, target, disabled, 0.0f);
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
}

void test_pair_guard()
{
	using clock = std::chrono::steady_clock;
	const auto warmup = std::chrono::milliseconds(1500);
	const auto start = clock::time_point {} + std::chrono::seconds(20);
	lifecycle_key observer;
	observer.has_controller = true;
	observer.pawn_entity = 10;
	observer.team = 2;
	observer.alive = true;
	lifecycle_key target = observer;
	target.pawn_entity = 20;
	target.team = 3;

	pair_guard guard;
	update_pair_guard(guard, observer, true, target, true, start, warmup);
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
	update_pair_guard(visual_guard, observer, true, target, true, start, warmup);
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
	update_pair_guard(guard, observer, true, changed, true, start + std::chrono::milliseconds(2000), warmup);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(3499), 3));
	pair_note_open(guard, start + std::chrono::milliseconds(3500), 3);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(3500), 3));
	assert(pair_allows_hiding(guard, start + std::chrono::milliseconds(3500), 4));

	lifecycle_key dead = changed;
	dead.alive = false;
	update_pair_guard(guard, observer, true, dead, false, start + std::chrono::milliseconds(4000), warmup);
	update_pair_guard(guard, observer, true, dead, false, start + std::chrono::milliseconds(4500), warmup);
	pair_note_open(guard, start + std::chrono::milliseconds(5999), 5);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(5999), 6));

	update_pair_guard(guard, observer, true, changed, true, start + std::chrono::milliseconds(6000), warmup);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(7499), 7));
	pair_note_open(guard, start + std::chrono::milliseconds(7500), 7);
	assert(!pair_allows_hiding(guard, start + std::chrono::milliseconds(7500), 7));
	assert(pair_allows_hiding(guard, start + std::chrono::milliseconds(7500), 8));
}

void test_checktransmit_private_offsets()
{
	std::array<uint32_t, k_checktransmit_aux_mask_count> offsets {};
	assert(parse_checktransmit_aux_mask_offsets("8,16,24", offsets, 4096));
	assert(offsets[0] == 8 && offsets[1] == 16 && offsets[2] == 24);
	assert(parse_checktransmit_aux_mask_offsets(" 8, 16, 24 ", offsets, 4096));
	assert(!parse_checktransmit_aux_mask_offsets("8,16", offsets, 4096));
	assert(!parse_checktransmit_aux_mask_offsets("8,16,24,32", offsets, 4096));
	assert(!parse_checktransmit_aux_mask_offsets("8,8,24", offsets, 4096));
	assert(!parse_checktransmit_aux_mask_offsets("8,18,24", offsets, 4096));
	assert(!parse_checktransmit_aux_mask_offsets("8,16,999999", offsets, 4096));
	assert(!parse_checktransmit_aux_mask_offsets("8,16,", offsets, 4096));

	uint32_t value {};
	assert(parse_gamedata_uint32(" 580 ", value) && value == 580);
	assert(!parse_gamedata_uint32("580x", value));
	assert(valid_gamedata_offset(580, alignof(bool), 4096));
	assert(!valid_gamedata_offset(0, alignof(void *), 4096));
}

void test_checktransmit_masks()
{
	struct fake_info
	{
		test_transmit_mask *primary;
		test_transmit_mask *aux1;
		test_transmit_mask *aux2;
		test_transmit_mask *aux3;
		bool full_update;
	};

	test_transmit_mask primary;
	test_transmit_mask aux1;
	test_transmit_mask aux2;
	test_transmit_mask aux3;
	fake_info info {&primary, &aux1, &aux2, &aux3, false};
	transmit_masks<test_transmit_mask> masks;
	const std::array<uint32_t, k_checktransmit_aux_mask_count> offsets {8, 16, 24};
	assert(collect_transmit_masks(&info, info.primary, offsets, masks));
	assert(masks.primary == &primary);
	assert(masks.count == 4);
	assert(!read_checktransmit_full_update(&info, 32));
	info.full_update = true;
	assert(read_checktransmit_full_update(&info, 32));
	info.full_update = false;

	for (test_transmit_mask *mask : {&primary, &aux1, &aux2, &aux3})
	{
		mask->Set(10);
		mask->Set(20);
		mask->Set(30);
	}
	const std::array<int, 3> handles {10, 20, 30};
	clear_transmit_group(masks, handles, handles.size(), [](int handle) { return handle; }, [](int index) { return index >= 0 && index < 64; });
	for (test_transmit_mask *mask : {&primary, &aux1, &aux2, &aux3})
	{
		assert(!mask->IsBitSet(10));
		assert(!mask->IsBitSet(20));
		assert(!mask->IsBitSet(30));
	}

	info.aux2 = &aux1;
	assert(collect_transmit_masks(&info, info.primary, offsets, masks));
	assert(masks.count == 3);

	info.aux2 = nullptr;
	assert(!collect_transmit_masks(&info, info.primary, offsets, masks));

	info.aux2 = reinterpret_cast<test_transmit_mask *>(reinterpret_cast<uintptr_t>(&aux2) + 1u);
	assert(!collect_transmit_masks(&info, info.primary, offsets, masks));
}

void test_hidden_entity_group()
{
	using clock = std::chrono::steady_clock;
	const auto start = clock::time_point {} + std::chrono::seconds(30);
	hidden_entity_group<uint32_t, 4> group;
	std::array<uint32_t, 4> handles {10, 11, 12, 0};

	hidden_group_store(group, 10u, handles, 3, start, std::chrono::milliseconds(3000));
	assert(group.count == 3);
	assert(group.source == 10u);
	assert(hidden_group_quarantined(group, start + std::chrono::milliseconds(2999)));
	assert(!hidden_group_quarantined(group, start + std::chrono::milliseconds(3000)));
	assert(group.count == 0);

	hidden_group_store(group, 10u, handles, 3, start, std::chrono::milliseconds(3000));
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
	const std::array<owner_effect_link<uint32_t>, 4> links {{
		{30, 10, 0},
		{31, 0, 20},
		{32, 99, 98},
		{20, 10, 0}
	}};
	assert(hidden_group_append_owner_effect_links(group, links.data(), links.size(), [](uint32_t child) { return child != 0; }));
	assert(group.count == 4);
	assert(hidden_group_contains(group, 30u, group.count));
	assert(hidden_group_contains(group, 31u, group.count));
	assert(!hidden_group_contains(group, 32u, group.count));

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
	const visibility_tuning tuning {1, 200, 500, 160.0f};
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
		for (uint32_t observer = 0; observer < k_players; ++observer)
		{
			lookahead[observer] = visibility_effective_lookahead_seconds(players[observer].rtt_seconds, tuning);
			origins[observer] = visibility_origins(data, players[observer], tuning, lookahead[observer]);
		}
		for (uint32_t observer = 0; observer < k_players; ++observer)
		{
			for (uint32_t target = 0; target < k_players; ++target)
			{
				if ((observer < 16u) == (target < 16u))
				{
					continue;
				}
				const auto targets = visibility_targets(data, players[target], tuning, lookahead[observer]);
				bool blocked = true;
				uint32_t ray = 0;
				for (const vec3 &origin : origins[observer])
				{
					for (uint32_t point_index = 0; point_index < targets.count; ++point_index)
					{
						const vec3 &point = targets.points[point_index];
						uint32_t &cached = cache[(observer * k_players + target) * k_visibility_ray_count_max + ray];
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

void test_bvh(const std::filesystem::path &directory)
{
	std::vector<triangle> triangles;
	for (int x = -4; x <= 4; ++x)
	{
		for (int y = -4; y <= 4; ++y)
		{
			triangles.push_back({{static_cast<float>(x), static_cast<float>(y), 0}, {static_cast<float>(x + 1), static_cast<float>(y), 0}, {static_cast<float>(x), static_cast<float>(y + 1), 0}});
			triangles.push_back({{static_cast<float>(x + 1), static_cast<float>(y + 1), 0}, {static_cast<float>(x), static_cast<float>(y + 1), 0}, {static_cast<float>(x + 1), static_cast<float>(y), 0}});
		}
	}
	bvh8_data data;
	std::string error;
	assert(build_bvh8(triangles, data, error));
	std::strcpy(data.header.map_name, "test");
	data.header.source_crc32 = 1;
	data.header.source_size = 2;
	const std::filesystem::path path = directory / "test.bvh8";
	assert(write_bvh8(path, data, error));
	bvh8_data loaded;
	assert(load_bvh8(path, loaded, error));
	assert(loaded.header.triangle_count == triangles.size());
	loaded.header.flags = k_bvh8_flag_nested_map_vpk;
	assert(validate_bvh8(loaded, error));
	loaded.header.flags = 0x80000000u;
	assert(!validate_bvh8(loaded, error));
	loaded.header.flags = 0;

	std::mt19937 random(0x2f0f8u);
	std::uniform_real_distribution<float> coordinate(-8.0f, 8.0f);
	for (int i = 0; i < 5000; ++i)
	{
		const vec3 origin {coordinate(random), coordinate(random), coordinate(random)};
		const vec3 target {coordinate(random), coordinate(random), coordinate(random)};
		bool expected = false;
		for (const triangle &value : triangles) expected = expected || scalar_hit(value, origin, target);
		assert(segment_blocked(loaded, origin, target).blocked == expected);
	}
	assert(segment_blocked(loaded, {0.25f, 0.25f, 1}, {0.25f, 0.25f, -1}).blocked);
	assert(!segment_blocked(loaded, {0.25f, 0.25f, 1}, {0.25f, 0.25f, 0}).blocked);

	std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
	corrupt.seekp(sizeof(bvh8_header) + 10);
	char byte = 0;
	corrupt.read(&byte, 1);
	byte ^= 0x5a;
	corrupt.seekp(sizeof(bvh8_header) + 10);
	corrupt.write(&byte, 1);
	corrupt.close();
	assert(!load_bvh8(path, loaded, error));
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 3 && std::strcmp(argv[1], "--process-probe") == 0)
	{
		return std::strcmp(argv[2], "space [test]") == 0 ? 23 : 24;
	}
	if (argc == 2 && std::strcmp(argv[1], "--process-sleep") == 0)
	{
		std::this_thread::sleep_for(std::chrono::seconds(10));
		return 0;
	}
	assert(cpu_supports_avx());
	const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("cs2fow-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory);
	test_vpk(directory);
	test_subprocess(std::filesystem::absolute(argv[0]));
	test_glb(directory);
	test_visibility_sampling();
	test_lifecycle_guard();
	test_visual_group_key();
	test_pair_guard();
	test_checktransmit_private_offsets();
	test_checktransmit_masks();
	test_hidden_entity_group();
	test_bvh(directory);
	std::filesystem::remove_all(directory);
	std::cout << "cs2fow_tests: all checks passed\n";
	if (argc == 2)
	{
		bvh8_data data;
		std::string error;
		assert(load_bvh8(argv[1], data, error));
		std::string label = std::filesystem::path(argv[1]).stem().string();
		if (label.empty()) label = "BVH8";
		std::mt19937 random(0xb8f8u);
		std::uniform_real_distribution<float> x(data.header.world_min[0], data.header.world_max[0]);
		std::uniform_real_distribution<float> y(data.header.world_min[1], data.header.world_max[1]);
		std::uniform_real_distribution<float> z(data.header.world_min[2], data.header.world_max[2]);
		constexpr size_t k_benchmark_rays = 512u * k_visibility_ray_count_max;
		std::vector<std::pair<vec3, vec3>> rays(k_benchmark_rays);
		std::vector<uint32_t> cache(k_benchmark_rays);
		std::fill(cache.begin(), cache.end(), k_invalid_ref);
		for (auto &ray : rays)
		{
			ray = {{x(random), y(random), z(random)}, {x(random), y(random), z(random)}};
		}
		for (size_t i = 0; i < rays.size(); ++i)
		{
			cache[i] = segment_blocked(data, rays[i].first, rays[i].second).packet_index;
		}
		std::array<double, 20> timings {};
		uint64_t blocked = 0;
		for (double &timing : timings)
		{
			const auto start = std::chrono::steady_clock::now();
			for (size_t i = 0; i < rays.size(); ++i)
			{
				const ray_hit hit = segment_blocked(data, rays[i].first, rays[i].second, cache[i]);
				cache[i] = hit.packet_index;
				blocked += hit.blocked;
			}
			timing = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		}
		std::sort(timings.begin(), timings.end());
		double average = 0;
		for (double timing : timings) average += timing;
		average /= timings.size();
		std::cout << label << " traversal: average=" << average << "ms p99=" << timings.back() << "ms rays=" << k_benchmark_rays << " blocked=" << blocked << '\n';
		assert(average < 25.0);
		assert(benchmark_worker_loop(data, label) < 10.0);
	}
	return 0;
}
