#include "pvs.h"

// Reads and writes the compact PVS outside the transmit hook.
// It returns a complete validated PVS or an error, rejects headers before large allocation,
// and keeps an old working bake until its replacement has been written/checked.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <cstdio>
#endif

namespace cs2fow
{
namespace
{

constexpr char k_magic[8] = {'C', 'S', '2', 'F', 'O', 'W', 'P', '\0'};

uint64_t align_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1u) & ~(alignment - 1u);
}

bool checked_add(uint64_t left, uint64_t right, uint64_t &result)
{
	if (right > std::numeric_limits<uint64_t>::max() - left)
	{
		return false;
	}
	result = left + right;
	return true;
}

bool checked_multiply(uint64_t left, uint64_t right, uint64_t &result)
{
	if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)
	{
		return false;
	}
	result = left * right;
	return true;
}

bool finite_bounds(const float *minimum, const float *maximum)
{
	for (int axis = 0; axis < 3; ++axis)
	{
		if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis] > maximum[axis])
		{
			return false;
		}
	}
	return true;
}

bool read_exact(std::ifstream &stream, void *destination, size_t size)
{
	stream.read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
	return stream.good();
}

template <typename type>
std::span<const std::byte> bytes_of(const std::vector<type> &values)
{
	return {reinterpret_cast<const std::byte *>(values.data()), values.size() * sizeof(type)};
}

uint32_t payload_crc(const pvs_data &data)
{
	uint32_t crc;
	crc = crc32_extend(0, bytes_of(data.nodes));
	crc = crc32_extend(crc, bytes_of(data.regions));
	crc = crc32_extend(crc, bytes_of(data.masks));
	return crc32_extend(crc, bytes_of(data.vis));
}

bool validate_header(const pvs_header &header, uint64_t actual_size, std::string &error)
{
	if (std::memcmp(header.magic, k_magic, sizeof(k_magic)) != 0 || header.version != k_pvs_version
		|| header.header_size != sizeof(pvs_header))
	{
		error = "invalid PVS magic or version";
		return false;
	}
	if ((header.flags & ~k_pvs_known_flags) != 0)
	{
		error = "PVS contains unsupported flags";
		return false;
	}
	if (std::any_of(std::begin(header.reserved), std::end(header.reserved), [](uint8_t value) { return value != 0; }))
	{
		error = "PVS reserved header bytes are not zero";
		return false;
	}
	if (header.map_name[0] == '\0' || std::memchr(header.map_name, '\0', sizeof(header.map_name)) == nullptr)
	{
		error = "invalid PVS map name";
		return false;
	}
	if (header.base_cluster_count == 0 || header.pvs_bytes_per_cluster == 0 || header.node_count == 0
		|| header.region_count == 0 || header.mask_count == 0)
	{
		error = "invalid PVS counts";
		return false;
	}
	if (!finite_bounds(header.world_min, header.world_max) || !std::isfinite(header.grid_size) || header.grid_size <= 0.0f)
	{
		error = "invalid PVS world bounds";
		return false;
	}

	uint64_t node_bytes = 0;
	uint64_t region_bytes = 0;
	uint64_t mask_bytes = 0;
	uint64_t vis_bytes = 0;
	if (!checked_multiply(header.node_count, sizeof(uint64_t), node_bytes)
		|| !checked_multiply(header.region_count, sizeof(uint64_t), region_bytes)
		|| !checked_multiply(header.mask_count, sizeof(uint64_t), mask_bytes)
		|| !checked_multiply(header.base_cluster_count, header.pvs_bytes_per_cluster, vis_bytes))
	{
		error = "PVS payload size overflows";
		return false;
	}

	const uint64_t expected_nodes = align_up(sizeof(pvs_header), 32);
	uint64_t nodes_end = 0;
	uint64_t regions_end = 0;
	uint64_t masks_end = 0;
	uint64_t expected_size = 0;
	if (!checked_add(expected_nodes, node_bytes, nodes_end))
	{
		error = "PVS payload size overflows";
		return false;
	}
	const uint64_t expected_regions = align_up(nodes_end, 32);
	if (!checked_add(expected_regions, region_bytes, regions_end))
	{
		error = "PVS payload size overflows";
		return false;
	}
	const uint64_t expected_masks = align_up(regions_end, 32);
	if (!checked_add(expected_masks, mask_bytes, masks_end))
	{
		error = "PVS payload size overflows";
		return false;
	}
	const uint64_t expected_vis = align_up(masks_end, 32);
	if (!checked_add(expected_vis, vis_bytes, expected_size))
	{
		error = "PVS payload size overflows";
		return false;
	}
	if (header.nodes_offset != expected_nodes || header.regions_offset != expected_regions
		|| header.masks_offset != expected_masks || header.vis_offset != expected_vis
		|| header.file_size != expected_size || actual_size != expected_size
		|| expected_size > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
	{
		error = "PVS offsets or file size are invalid";
		return false;
	}
	return true;
}

bool replace_file(const std::filesystem::path &temporary, const std::filesystem::path &destination, std::string &error)
{
#if defined(_WIN32)
	if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
	{
		error = "could not atomically install PVS file: "
			+ std::error_code(static_cast<int>(GetLastError()), std::system_category()).message();
		return false;
	}
#else
	if (std::rename(temporary.c_str(), destination.c_str()) != 0)
	{
		error = "could not atomically install PVS file: " + std::error_code(errno, std::generic_category()).message();
		return false;
	}
#endif
	return true;
}

} // namespace

bool validate_pvs(const pvs_data &data, std::string &error)
{
	error.clear();
	const pvs_header &header = data.header;
	if (!validate_header(header, header.file_size, error))
	{
		return false;
	}
	uint64_t vis_bytes = 0;
	if (!checked_multiply(header.base_cluster_count, header.pvs_bytes_per_cluster, vis_bytes))
	{
		error = "PVS payload size overflows";
		return false;
	}
	if (header.node_count != data.nodes.size() || header.region_count != data.regions.size()
		|| header.mask_count != data.masks.size() || vis_bytes != data.vis.size())
	{
		error = "PVS header does not match payload";
		return false;
	}
	return true;
}

bool load_pvs(const std::filesystem::path &path, pvs_data &data, std::string &error)
{
	error.clear();
	data = {};
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	if (!stream || stream.tellg() < 0)
	{
		error = "could not open PVS file";
		return false;
	}
	const uint64_t actual_size = static_cast<uint64_t>(stream.tellg());
	stream.seekg(0);
	pvs_header header {};
	if (actual_size < sizeof(header) || !read_exact(stream, &header, sizeof(header)) || !validate_header(header, actual_size, error))
	{
		if (error.empty()) error = "PVS file is truncated";
		return false;
	}
	uint64_t vis_bytes = 0;
	checked_multiply(header.base_cluster_count, header.pvs_bytes_per_cluster, vis_bytes);
	try
	{
		data.nodes.resize(header.node_count);
		data.regions.resize(header.region_count);
		data.masks.resize(header.mask_count);
		data.vis.resize(static_cast<size_t>(vis_bytes));
	}
	catch (const std::bad_alloc &)
	{
		error = "not enough memory to load PVS";
		data = {};
		return false;
	}
	catch (const std::length_error &)
	{
		error = "PVS payload is too large";
		data = {};
		return false;
	}
	data.header = header;
	stream.seekg(static_cast<std::streamoff>(header.nodes_offset));
	if (!read_exact(stream, data.nodes.data(), data.nodes.size() * sizeof(uint64_t)))
	{
		error = "could not read PVS nodes";
		data = {};
		return false;
	}
	stream.seekg(static_cast<std::streamoff>(header.regions_offset));
	if (!read_exact(stream, data.regions.data(), data.regions.size() * sizeof(uint64_t)))
	{
		error = "could not read PVS regions";
		data = {};
		return false;
	}
	stream.seekg(static_cast<std::streamoff>(header.masks_offset));
	if (!read_exact(stream, data.masks.data(), data.masks.size() * sizeof(uint64_t)))
	{
		error = "could not read PVS masks";
		data = {};
		return false;
	}
	stream.seekg(static_cast<std::streamoff>(header.vis_offset));
	if (!read_exact(stream, data.vis.data(), data.vis.size()))
	{
		error = "could not read PVS vis rows";
		data = {};
		return false;
	}
	if (!validate_pvs(data, error) || payload_crc(data) != header.payload_crc32)
	{
		if (error.empty()) error = "PVS payload CRC mismatch";
		data = {};
		return false;
	}
	return true;
}

bool write_pvs(const std::filesystem::path &path, pvs_data &data, std::string &error)
{
	error.clear();
	if (data.nodes.size() > std::numeric_limits<uint32_t>::max() || data.regions.size() > std::numeric_limits<uint32_t>::max()
		|| data.masks.size() > std::numeric_limits<uint32_t>::max())
	{
		error = "PVS payload has too many records";
		return false;
	}
	std::memcpy(data.header.magic, k_magic, sizeof(k_magic));
	data.header.version = k_pvs_version;
	data.header.header_size = sizeof(pvs_header);
	std::fill(std::begin(data.header.reserved), std::end(data.header.reserved), uint8_t {});
	data.header.node_count = static_cast<uint32_t>(data.nodes.size());
	data.header.region_count = static_cast<uint32_t>(data.regions.size());
	data.header.mask_count = static_cast<uint32_t>(data.masks.size());
	data.header.nodes_offset = align_up(sizeof(pvs_header), 32);
	data.header.regions_offset = align_up(data.header.nodes_offset + data.nodes.size() * sizeof(uint64_t), 32);
	data.header.masks_offset = align_up(data.header.regions_offset + data.regions.size() * sizeof(uint64_t), 32);
	data.header.vis_offset = align_up(data.header.masks_offset + data.masks.size() * sizeof(uint64_t), 32);
	data.header.file_size = data.header.vis_offset + data.vis.size();
	data.header.payload_crc32 = payload_crc(data);
	if (!validate_pvs(data, error))
	{
		return false;
	}

	std::error_code filesystem_error;
	if (!path.parent_path().empty())
	{
		std::filesystem::create_directories(path.parent_path(), filesystem_error);
		if (filesystem_error)
		{
			error = "could not create PVS directory: " + filesystem_error.message();
			return false;
		}
	}
	const std::filesystem::path temporary = path.string() + ".tmp";
	std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
	if (!stream)
	{
		error = "could not create temporary PVS file";
		return false;
	}
	stream.write(reinterpret_cast<const char *>(&data.header), sizeof(data.header));
	stream.seekp(static_cast<std::streamoff>(data.header.nodes_offset));
	stream.write(reinterpret_cast<const char *>(data.nodes.data()), static_cast<std::streamsize>(data.nodes.size() * sizeof(uint64_t)));
	stream.seekp(static_cast<std::streamoff>(data.header.regions_offset));
	stream.write(reinterpret_cast<const char *>(data.regions.data()), static_cast<std::streamsize>(data.regions.size() * sizeof(uint64_t)));
	stream.seekp(static_cast<std::streamoff>(data.header.masks_offset));
	stream.write(reinterpret_cast<const char *>(data.masks.data()), static_cast<std::streamsize>(data.masks.size() * sizeof(uint64_t)));
	stream.seekp(static_cast<std::streamoff>(data.header.vis_offset));
	stream.write(reinterpret_cast<const char *>(data.vis.data()), static_cast<std::streamsize>(data.vis.size()));
	stream.flush();
	const bool written = stream.good();
	stream.close();
	if (!written)
	{
		error = "failed while writing PVS file";
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	pvs_data verified;
	if (!load_pvs(temporary, verified, error) || verified.header.payload_crc32 != data.header.payload_crc32)
	{
		if (error.empty()) error = "temporary PVS validation failed";
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	if (!replace_file(temporary, path, error))
	{
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	return true;
}

} // namespace cs2fow
