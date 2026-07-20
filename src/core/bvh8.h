#pragma once

// Shared map-geometry types and BVH8 read/ray APIs. Callers build or load the
// data once, then the visibility worker treats it as immutable; invalid files
// and references return errors instead of becoming partially usable data.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace cs2fow
{

struct vec3
{
	float x {};
	float y {};
	float z {};
};

struct triangle
{
	vec3 v0;
	vec3 v1;
	vec3 v2;
};

struct bounds
{
	vec3 min;
	vec3 max;
};

inline constexpr uint32_t k_bvh8_version = 3;
inline constexpr uint32_t k_bvh8_recipe_version = 1;
inline constexpr uint32_t k_bvh8_flag_nested_map_vpk = 1u << 0u;
inline constexpr uint32_t k_bvh8_known_flags = k_bvh8_flag_nested_map_vpk;
inline constexpr uint32_t k_invalid_ref = 0xffffffffu;
inline constexpr uint32_t k_leaf_ref = 0x80000000u;
inline constexpr uint32_t k_leaf_index_mask = 0x0fffffffu;
inline constexpr uint32_t k_max_tree_depth = 64;

struct alignas(32) bvh8_node
{
	float min_x[8];
	float min_y[8];
	float min_z[8];
	float max_x[8];
	float max_y[8];
	float max_z[8];
	uint32_t child[8];
};

struct alignas(32) triangle_packet8
{
	float v0_x[8];
	float v0_y[8];
	float v0_z[8];
	float edge1_x[8];
	float edge1_y[8];
	float edge1_z[8];
	float edge2_x[8];
	float edge2_y[8];
	float edge2_z[8];
};

struct alignas(32) bvh8_header
{
	char magic[8];
	uint32_t version;
	uint32_t header_size;
	uint32_t flags;
	uint32_t source_crc32;
	uint64_t source_size;
	char map_name[64];
	float world_min[3];
	float world_max[3];
	uint32_t node_count;
	uint32_t packet_count;
	uint32_t triangle_count;
	uint32_t max_depth;
	uint64_t nodes_offset;
	uint64_t packets_offset;
	uint64_t file_size;
	uint32_t payload_crc32;
	uint32_t bake_recipe_version;
	uint8_t reserved[88];
};

static_assert(sizeof(bvh8_node) == 224);
static_assert(sizeof(triangle_packet8) == 288);
static_assert(sizeof(bvh8_header) == 256);

struct bvh8_data
{
	bvh8_header header {};
	std::vector<bvh8_node> nodes;
	std::vector<triangle_packet8> packets;
};

struct ray_hit
{
	bool blocked {};
	uint32_t packet_index {k_invalid_ref};
};

uint32_t crc32(std::span<const std::byte> bytes);
uint32_t crc32_extend(uint32_t previous_crc, std::span<const std::byte> bytes);
bool file_crc32(const std::filesystem::path &path, uint64_t &size, uint32_t &checksum, std::string &error);
bool cpu_supports_avx();
bool validate_bvh8(const bvh8_data &data, std::string &error);
bool load_bvh8(const std::filesystem::path &path, bvh8_data &data, std::string &error,
	const std::atomic_bool *cancel = nullptr);
bool write_bvh8(const std::filesystem::path &path, bvh8_data &data, std::string &error);
ray_hit segment_blocked(const bvh8_data &data, vec3 origin, vec3 target, uint32_t cached_packet = k_invalid_ref);
bool packet_blocks_segment(const triangle_packet8 &packet, uint32_t count, vec3 origin, vec3 target);

inline uint32_t make_leaf_ref(uint32_t packet_index, uint32_t triangle_count)
{
	return k_leaf_ref | ((triangle_count - 1u) << 28u) | packet_index;
}

inline bool is_leaf_ref(uint32_t ref)
{
	return ref != k_invalid_ref && (ref & k_leaf_ref) != 0;
}

inline uint32_t leaf_count(uint32_t ref)
{
	return ((ref >> 28u) & 7u) + 1u;
}

inline uint32_t leaf_index(uint32_t ref)
{
	return ref & k_leaf_index_mask;
}

} // namespace cs2fow
