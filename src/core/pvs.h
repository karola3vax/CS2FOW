#pragma once

// Compact baked-visibility types and read/write APIs. Callers build or load the
// data once, then the visibility worker treats it as immutable; invalid files
// and references return errors instead of becoming partially usable data.

#include "bvh8.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cs2fow
{

inline constexpr uint32_t k_pvs_version = 1;
inline constexpr uint32_t k_pvs_known_flags = k_bvh8_flag_nested_map_vpk;

struct alignas(32) pvs_header
{
	char magic[8];
	uint32_t version;
	uint32_t header_size;
	uint32_t flags;
	uint32_t source_crc32;
	uint64_t source_size;
	char map_name[64];
	uint64_t vvis_size;
	uint32_t vvis_crc32;
	uint32_t base_cluster_count;
	uint32_t pvs_bytes_per_cluster;
	uint32_t node_count;
	uint32_t region_count;
	uint32_t mask_count;
	float world_min[3];
	float world_max[3];
	float grid_size;
	uint32_t payload_crc32;
	uint64_t nodes_offset;
	uint64_t regions_offset;
	uint64_t masks_offset;
	uint64_t vis_offset;
	uint64_t file_size;
	uint8_t reserved[56];
};

static_assert(sizeof(pvs_header) == 256);

struct pvs_data
{
	pvs_header header {};
	// note: value encoding for payload arrays
	//
	// node: 
	// 			bits0 leaf, 
	// 			bits1...31 child base / region start,
	// 			bits32...39 region count, 
	// 			bits40...63 enclosed index (unused)
	// region: 
	// 			bits0...14 cluster, 
	// 			bit15 geo flag, 
	// 			bits16...39 leaf index,
	// 			bits40...63 mask index
	// mask: 
	// 			4x4x4 subgrid occupancy, bit = x + 4y + 16z 
	// vis:     
	// 			base_cluster_count rows of pvs_bytes_per_cluster bytes
	std::vector<uint64_t> nodes;
	std::vector<uint64_t> regions;
	std::vector<uint64_t> masks;
	std::vector<uint8_t> vis;
};

bool validate_pvs(const pvs_data &data, std::string &error);
bool load_pvs(const std::filesystem::path &path, pvs_data &data, std::string &error);
bool write_pvs(const std::filesystem::path &path, pvs_data &data, std::string &error);

} // namespace cs2fow
