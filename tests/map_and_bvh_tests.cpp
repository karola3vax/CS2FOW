#include "test_suites.h"

// Exercises VPK/map discovery, physics import, BVH building/traversal, version 3
// file rejection/atomic replacement, subprocesses, and baker CLI fixtures.
// All inputs are local temporary fixtures; no game or network state is used.

#include "builder.h"
#include "glb_import.h"
#include "map_source.h"
#include "subprocess.h"
#include "vpk.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

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
	append<uint16_t>(tree, 0u);
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

void write_extractable_vpk(const std::filesystem::path &path, uint32_t version, uint16_t archive_index,
	const std::string &preload, const std::string &payload, uint16_t terminator = 0xffffu,
	uint32_t declared_file_data_size = std::numeric_limits<uint32_t>::max(), const std::string &footer = {})
{
	std::vector<std::byte> complete;
	complete.insert(complete.end(), reinterpret_cast<const std::byte *>(preload.data()),
		reinterpret_cast<const std::byte *>(preload.data() + preload.size()));
	complete.insert(complete.end(), reinterpret_cast<const std::byte *>(payload.data()),
		reinterpret_cast<const std::byte *>(payload.data() + payload.size()));
	std::vector<std::byte> tree;
	append_string(tree, "vpk");
	append_string(tree, "maps/workshop/123");
	append_string(tree, "de_nested");
	append<uint32_t>(tree, crc32(complete));
	append<uint16_t>(tree, static_cast<uint16_t>(preload.size()));
	append<uint16_t>(tree, archive_index);
	append<uint32_t>(tree, 0u);
	append<uint32_t>(tree, static_cast<uint32_t>(payload.size()));
	append<uint16_t>(tree, terminator);
	tree.insert(tree.end(), reinterpret_cast<const std::byte *>(preload.data()),
		reinterpret_cast<const std::byte *>(preload.data() + preload.size()));
	append_string(tree, "");
	append_string(tree, "");
	append_string(tree, "");
	std::ofstream stream(path, std::ios::binary);
	const uint32_t file_data_size = declared_file_data_size == std::numeric_limits<uint32_t>::max()
		? (archive_index == 0x7fffu ? static_cast<uint32_t>(payload.size()) : 0u) : declared_file_data_size;
	if (version == 1)
	{
		const uint32_t header[] = {0x55aa1234u, 1u, static_cast<uint32_t>(tree.size())};
		stream.write(reinterpret_cast<const char *>(header), sizeof(header));
	}
	else
	{
		const uint32_t header[] = {0x55aa1234u, 2u, static_cast<uint32_t>(tree.size()), file_data_size, 0u, 0u, 0u};
		stream.write(reinterpret_cast<const char *>(header), sizeof(header));
	}
	stream.write(reinterpret_cast<const char *>(tree.data()), static_cast<std::streamsize>(tree.size()));
	if (archive_index == 0x7fffu)
	{
		stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
	}
	stream.write(footer.data(), static_cast<std::streamsize>(footer.size()));
	stream.close();
	if (archive_index != 0x7fffu && !payload.empty())
	{
		char suffix[16] {};
		std::snprintf(suffix, sizeof(suffix), "_%03u.vpk", archive_index);
		const std::string filename = path.filename().string();
		std::ofstream archive(path.parent_path() / (filename.substr(0, filename.size() - 8u) + suffix), std::ios::binary);
		archive.write(payload.data(), static_cast<std::streamsize>(payload.size()));
	}
}

std::string read_text_file(const std::filesystem::path &path)
{
	std::ifstream stream(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

uint64_t output_pid(const std::string &output, const char *label)
{
	const size_t start = output.find(label);
	assert(start != std::string::npos);
	return std::stoull(output.substr(start + std::strlen(label)));
}

bool process_alive(uint64_t pid)
{
#if defined(_WIN32)
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (process == nullptr) return false;
	DWORD exit_code = 0;
	const bool alive = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
	CloseHandle(process);
	return alive;
#else
	return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

void assert_process_stopped(uint64_t pid)
{
	for (int attempt = 0; attempt < 200 && process_alive(pid); ++attempt)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	assert(!process_alive(pid));
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

	const std::filesystem::path embedded = directory / "embedded.vpk";
	write_extractable_vpk(embedded, 2, 0x7fffu, "pre", "payload");
	assert(find_vpk_entry(embedded, "maps/workshop/123/de_nested.vpk", entry, error));
	assert(entry.path == "maps/workshop/123/de_nested.vpk" && entry.size == 10u);
	std::vector<std::string> maps;
	assert(list_vpk_maps(embedded, maps, error));
	assert(maps == std::vector<std::string> {"workshop/123/de_nested"});
	const std::filesystem::path extracted = directory / "embedded-out.vpk";
	assert(extract_vpk_entry(embedded, entry, extracted, error));
	assert(read_text_file(extracted) == "prepayload");

	const std::filesystem::path v2_zero_data = directory / "v2-zero-data.vpk";
	write_extractable_vpk(v2_zero_data, 2, 0x7fffu, "", "footer-data", 0xffffu, 0u);
	assert(!find_vpk_entry(v2_zero_data, "maps/workshop/123/de_nested.vpk", entry, error));
	const std::filesystem::path v2_short_data = directory / "v2-short-data.vpk";
	write_extractable_vpk(v2_short_data, 2, 0x7fffu, "", "payload", 0xffffu, 3u);
	assert(!find_vpk_entry(v2_short_data, "maps/workshop/123/de_nested.vpk", entry, error));
	const std::filesystem::path v2_footer = directory / "v2-footer.vpk";
	write_extractable_vpk(v2_footer, 2, 0x7fffu, "", "payload", 0xffffu, 7u, "ignored-footer");
	assert(find_vpk_entry(v2_footer, "maps/workshop/123/de_nested.vpk", entry, error));
	assert(extract_vpk_entry(v2_footer, entry, directory / "v2-footer-out.vpk", error));
	assert(read_text_file(directory / "v2-footer-out.vpk") == "payload");

	const std::filesystem::path preload_only = directory / "preload.vpk";
	write_extractable_vpk(preload_only, 1, 0x7fffu, "only-preload", "");
	assert(find_vpk_entry(preload_only, "maps/workshop/123/de_nested.vpk", entry, error));
	assert(extract_vpk_entry(preload_only, entry, directory / "preload-out.vpk", error));
	assert(read_text_file(directory / "preload-out.vpk") == "only-preload");

	const std::filesystem::path numbered = directory / "numbered_dir.vpk";
	write_extractable_vpk(numbered, 2, 0, "head", "archive");
	assert(find_vpk_entry(numbered, "maps/workshop/123/de_nested.vpk", entry, error));
	assert(extract_vpk_entry(numbered, entry, directory / "numbered-out.vpk", error));
	assert(read_text_file(directory / "numbered-out.vpk") == "headarchive");
	std::fstream archive(directory / "numbered_000.vpk", std::ios::binary | std::ios::in | std::ios::out);
	archive.put('X');
	archive.close();
	assert(!extract_vpk_entry(numbered, entry, directory / "bad-crc.vpk", error));

	const std::filesystem::path truncated = directory / "truncated_dir.vpk";
	write_extractable_vpk(truncated, 2, 0, "", "archive");
	assert(find_vpk_entry(truncated, "maps/workshop/123/de_nested.vpk", entry, error));
	std::filesystem::resize_file(directory / "truncated_000.vpk", 2);
	assert(!extract_vpk_entry(truncated, entry, directory / "truncated-out.vpk", error));

	const std::filesystem::path invalid_terminator = directory / "invalid-terminator.vpk";
	write_extractable_vpk(invalid_terminator, 2, 0x7fffu, "", "data", 0u);
	std::vector<vpk_entry> entries;
	assert(!list_vpk_entries(invalid_terminator, entries, error));
}

void test_file_crc32(const std::filesystem::path &directory)
{
	const std::filesystem::path known = directory / "known-crc.bin";
	std::ofstream(known, std::ios::binary) << "123456789";
	uint64_t size = 0;
	uint32_t checksum = 0;
	std::string error;
	assert(file_crc32(known, size, checksum, error));
	const auto matches = [&](uint64_t expected_size, uint32_t expected_checksum)
	{
		return size == expected_size && checksum == expected_checksum;
	};
	assert(matches(9, 0xcbf43926u));
	assert(!matches(10, 0xcbf43926u));
	assert(!matches(9, 0x12345678u));

	const std::filesystem::path empty = directory / "empty.bin";
	std::ofstream(empty, std::ios::binary);
	assert(file_crc32(empty, size, checksum, error));
	assert(size == 0 && checksum == 0);
	assert(!file_crc32(directory / "missing.bin", size, checksum, error));
	assert(!file_crc32(directory, size, checksum, error));
}

void test_subprocess(const std::filesystem::path &directory, const std::filesystem::path &executable)
{
	process_result result;
	std::string error;
	assert(run_process(executable, {"--process-probe", "space [test]"}, std::chrono::seconds(5), nullptr, false,
		posix_process_group::isolated, result, error));
	assert(result.exit_code == 23 && !result.cancelled && !result.timed_out);
	assert(result.output_tail.find("probe stdout") != std::string::npos);
	assert(result.output_tail.find("probe stderr") != std::string::npos);

	assert(run_process(executable, {"--process-flood"}, std::chrono::seconds(5), nullptr, false,
		posix_process_group::isolated, result, error));
	assert(result.exit_code == 25 && result.output_tail.size() <= k_process_output_tail_bytes);
	assert(result.output_tail.find("HEAD-MARKER") == std::string::npos);
	assert(result.output_tail.find("TAIL-MARKER") != std::string::npos);

	const std::filesystem::path spaced_directory = directory / "process path";
	std::filesystem::create_directories(spaced_directory);
	const std::filesystem::path spaced_executable = spaced_directory / executable.filename();
	std::filesystem::copy_file(executable, spaced_executable, std::filesystem::copy_options::overwrite_existing);
	std::filesystem::permissions(spaced_executable, std::filesystem::status(executable).permissions());
	assert(run_process(spaced_executable, {"--process-probe", "space [test]"}, std::chrono::seconds(5), nullptr, false,
		posix_process_group::isolated, result, error));
	assert(result.exit_code == 23);

	assert(run_process(executable, {"--process-sleep"}, std::chrono::milliseconds(50), nullptr, false,
		posix_process_group::isolated, result, error));
	assert(result.timed_out);
	assert(result.output_tail.find("sleep probe") != std::string::npos);

	std::atomic_bool cancel {false};
	std::thread canceller([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		cancel.store(true);
	});
	assert(run_process(executable, {"--process-sleep"}, std::chrono::seconds(5), &cancel, false,
		posix_process_group::isolated, result, error));
	canceller.join();
	assert(result.cancelled);
	assert(result.output_tail.find("sleep probe") != std::string::npos);

	const std::filesystem::path nested_pid = directory / "nested-timeout.pid";
	assert(run_process(executable, {"--process-nested", nested_pid.string()}, std::chrono::seconds(1), nullptr, false,
		posix_process_group::isolated, result, error));
	assert(result.timed_out);
	assert_process_stopped(output_pid(result.output_tail, "parent-pid="));
	assert(std::filesystem::exists(nested_pid));
	assert_process_stopped(std::stoull(read_text_file(nested_pid)));

	cancel.store(false);
	const std::filesystem::path cancelled_pid = directory / "nested-cancel.pid";
	std::thread nested_canceller([&] {
		for (int attempt = 0; attempt < 500 && !std::filesystem::exists(cancelled_pid); ++attempt)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		cancel.store(true);
	});
	assert(run_process(executable, {"--process-nested", cancelled_pid.string()}, std::chrono::seconds(5), &cancel, false,
		posix_process_group::isolated, result, error));
	nested_canceller.join();
	assert(result.cancelled);
	assert_process_stopped(output_pid(result.output_tail, "parent-pid="));
	assert(std::filesystem::exists(cancelled_pid));
	assert_process_stopped(std::stoull(read_text_file(cancelled_pid)));
}

void write_test_glb(const std::filesystem::path &path, const std::string &surface_property, bool include_surface_property = true)
{
	const std::string surface_json = include_surface_property ? "\"SurfaceProperty\":\"" + surface_property + "\"," : "";
	std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","max":[1,1,0],"min":[0,0,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0,"extras":{)" + surface_json + R"("InteractAs":[]}}],"scenes":[{"nodes":[0]}],"scene":0})";
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
	write_test_glb(path, "concrete");
	std::vector<triangle> triangles;
	import_report report;
	std::string error;
	assert(import_physics_glb(path, triangles, report, error));
	assert(triangles.size() == 1 && report.raw_triangles == 1 && report.groups.size() == 1 && report.groups[0].accepted
		&& report.groups[0].surface_property == "concrete");
	assert(physics_group_accepted({}, "") && physics_group_accepted({}, "metal_sheetmetal"));
	assert(physics_group_accepted({"blocklight", "solid"}, "unknown"));
	assert(!physics_group_accepted({}, "glass") && !physics_group_accepted({}, "chainlink") && !physics_group_accepted({}, "metalgrate")
		&& !physics_group_accepted({}, "foliage") && !physics_group_accepted({}, "unknown"));
	assert(!physics_group_accepted({"playerclip"}, "concrete") && !physics_group_accepted({"solid"}, "concrete"));

	write_test_glb(path, "glass");
	assert(!import_physics_glb(path, triangles, report, error));
	assert(report.groups.size() == 1 && report.groups[0].surface_property == "glass" && !report.groups[0].accepted);
	write_test_glb(path, "", false);
	assert(!import_physics_glb(path, triangles, report, error) && report.groups.size() == 1 && !report.groups[0].accepted);
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

void sync_test_bvh_header(bvh8_data &data)
{
	data.header.node_count = static_cast<uint32_t>(data.nodes.size());
	data.header.packet_count = static_cast<uint32_t>(data.packets.size());
	data.header.nodes_offset = sizeof(bvh8_header);
	data.header.packets_offset = data.header.nodes_offset + data.nodes.size() * sizeof(bvh8_node);
	data.header.file_size = data.header.packets_offset + data.packets.size() * sizeof(triangle_packet8);
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
	assert(loaded.header.version == 3 && loaded.header.bake_recipe_version == 1);
	assert(loaded.header.triangle_count == triangles.size());
	loaded.header.flags = k_bvh8_flag_nested_map_vpk;
	assert(validate_bvh8(loaded, error));
	loaded.header.flags = 0x80000000u;
	assert(!validate_bvh8(loaded, error));
	loaded.header.flags = 0;

	bvh8_data invalid_tree = loaded;
	uint32_t shared_node = k_invalid_ref;
	for (uint32_t child : invalid_tree.nodes[0].child)
	{
		if (child != k_invalid_ref && !is_leaf_ref(child))
		{
			if (shared_node == k_invalid_ref) shared_node = child;
			else
			{
				for (uint32_t &candidate : invalid_tree.nodes[0].child)
				{
					if (candidate == child)
					{
						candidate = shared_node;
						break;
					}
				}
				break;
			}
		}
	}
	assert(shared_node != k_invalid_ref && !validate_bvh8(invalid_tree, error));
	assert(error == "BVH8 node has more than one parent");

	invalid_tree = loaded;
	uint32_t shared_packet = k_invalid_ref;
	bool duplicated_packet = false;
	for (bvh8_node &node : invalid_tree.nodes)
	{
		for (uint32_t &child : node.child)
		{
			if (!is_leaf_ref(child)) continue;
			if (shared_packet == k_invalid_ref) shared_packet = child;
			else
			{
				child = shared_packet;
				duplicated_packet = true;
				break;
			}
		}
		if (duplicated_packet) break;
	}
	assert(duplicated_packet && !validate_bvh8(invalid_tree, error));
	assert(error == "BVH8 packet has more than one parent");

	invalid_tree = loaded;
	invalid_tree.nodes.emplace_back();
	sync_test_bvh_header(invalid_tree);
	assert(!validate_bvh8(invalid_tree, error));
	assert(error == "BVH8 contains unreachable nodes or packets");
	invalid_tree = loaded;
	invalid_tree.packets.emplace_back();
	sync_test_bvh_header(invalid_tree);
	assert(!validate_bvh8(invalid_tree, error));
	assert(error == "BVH8 contains unreachable nodes or packets");
	invalid_tree = loaded;
	invalid_tree.header.max_depth = invalid_tree.header.max_depth < k_max_tree_depth
		? invalid_tree.header.max_depth + 1u : invalid_tree.header.max_depth - 1u;
	assert(!validate_bvh8(invalid_tree, error));
	assert(error == "BVH8 depth is inconsistent");
	invalid_tree = loaded;
	++invalid_tree.header.triangle_count;
	assert(!validate_bvh8(invalid_tree, error));
	assert(error == "BVH8 triangle count is inconsistent");

	const auto rejected_header = [&](const auto &change)
	{
		assert(write_bvh8(path, data, error));
		std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
		bvh8_header header {};
		stream.read(reinterpret_cast<char *>(&header), sizeof(header));
		change(header);
		stream.seekp(0);
		stream.write(reinterpret_cast<const char *>(&header), sizeof(header));
		stream.close();
		assert(!load_bvh8(path, loaded, error));
	};
	rejected_header([](bvh8_header &header) { header.version = 2; });
	rejected_header([](bvh8_header &header) { header.bake_recipe_version = 2; });
	rejected_header([](bvh8_header &header) { header.reserved[0] = 1; });
	rejected_header([](bvh8_header &header) { header.node_count = std::numeric_limits<uint32_t>::max(); });
	rejected_header([](bvh8_header &header) { header.world_min[0] = std::numeric_limits<float>::quiet_NaN(); });
	assert(write_bvh8(path, data, error));
	bvh8_data invalid = data;
	invalid.header.flags = 0x80000000u;
	assert(!write_bvh8(path, invalid, error));
	assert(load_bvh8(path, loaded, error));

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

void run_map_and_bvh_tests(const std::filesystem::path &directory, const std::filesystem::path &test_executable)
{
	test_vpk(directory);
	test_file_crc32(directory);
	test_subprocess(directory, test_executable);
	test_glb(directory);
	test_bvh(directory);
}
