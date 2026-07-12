#include "bvh8.h"

// Reads and writes baked wall files outside the transmit hook. It returns a
// complete validated BVH8 or an error, rejects headers before large allocation,
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

constexpr char k_magic[8] = {'C', 'S', '2', 'F', 'O', 'W', '8', '\0'};

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

uint32_t payload_crc(const bvh8_data &data)
{
	return crc32_extend(crc32_extend(0, bytes_of(data.nodes)), bytes_of(data.packets));
}

bool validate_header(const bvh8_header &header, uint64_t actual_size, std::string &error)
{
	if (std::memcmp(header.magic, k_magic, sizeof(k_magic)) != 0 || header.version != k_bvh8_version
		|| header.header_size != sizeof(bvh8_header) || header.bake_recipe_version != k_bvh8_recipe_version)
	{
		error = "invalid BVH8 magic, version, or bake recipe";
		return false;
	}
	if ((header.flags & ~k_bvh8_known_flags) != 0)
	{
		error = "BVH8 contains unsupported flags";
		return false;
	}
	if (std::any_of(std::begin(header.reserved), std::end(header.reserved), [](uint8_t value) { return value != 0; }))
	{
		error = "BVH8 reserved header bytes are not zero";
		return false;
	}
	if (header.map_name[0] == '\0' || std::memchr(header.map_name, '\0', sizeof(header.map_name)) == nullptr)
	{
		error = "invalid map name";
		return false;
	}
	if (header.node_count == 0 || header.packet_count == 0 || header.triangle_count == 0
		|| header.max_depth == 0 || header.max_depth > k_max_tree_depth
		|| header.triangle_count > static_cast<uint64_t>(header.packet_count) * 8u)
	{
		error = "invalid BVH8 counts or depth";
		return false;
	}
	if (!finite_bounds(header.world_min, header.world_max))
	{
		error = "invalid BVH8 world bounds";
		return false;
	}

	uint64_t node_bytes = 0;
	uint64_t packet_bytes = 0;
	uint64_t nodes_end = 0;
	uint64_t expected_size = 0;
	const uint64_t expected_nodes = align_up(sizeof(bvh8_header), 32);
	if (!checked_multiply(header.node_count, sizeof(bvh8_node), node_bytes)
		|| !checked_multiply(header.packet_count, sizeof(triangle_packet8), packet_bytes)
		|| !checked_add(expected_nodes, node_bytes, nodes_end))
	{
		error = "BVH8 payload size overflows";
		return false;
	}
	const uint64_t expected_packets = align_up(nodes_end, 32);
	if (!checked_add(expected_packets, packet_bytes, expected_size)
		|| header.nodes_offset != expected_nodes || header.packets_offset != expected_packets
		|| header.file_size != expected_size || actual_size != expected_size
		|| expected_size > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
	{
		error = "BVH8 offsets or file size are invalid";
		return false;
	}
	return true;
}

bool replace_file(const std::filesystem::path &temporary, const std::filesystem::path &destination, std::string &error)
{
#if defined(_WIN32)
	if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
	{
		error = "could not atomically install BVH8 file: "
			+ std::error_code(static_cast<int>(GetLastError()), std::system_category()).message();
		return false;
	}
#else
	if (std::rename(temporary.c_str(), destination.c_str()) != 0)
	{
		error = "could not atomically install BVH8 file: " + std::error_code(errno, std::generic_category()).message();
		return false;
	}
#endif
	return true;
}

} // namespace

uint32_t crc32(std::span<const std::byte> bytes)
{
	return crc32_extend(0, bytes);
}

uint32_t crc32_extend(uint32_t previous_crc, std::span<const std::byte> bytes)
{
	uint32_t value = ~previous_crc;
	for (const std::byte byte : bytes)
	{
		value ^= static_cast<uint8_t>(byte);
		for (int bit = 0; bit < 8; ++bit)
		{
			value = (value >> 1u) ^ (0xedb88320u & (0u - (value & 1u)));
		}
	}
	return ~value;
}

bool file_crc32(const std::filesystem::path &path, uint64_t &size, uint32_t &checksum, std::string &error)
{
	size = 0;
	checksum = 0;
	error.clear();
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
	{
		error = "could not open file for CRC32";
		return false;
	}
	std::array<std::byte, 64u * 1024u> buffer;
	while (stream)
	{
		stream.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
		const std::streamsize count = stream.gcount();
		if (count > 0)
		{
			checksum = crc32_extend(checksum, std::span<const std::byte>(buffer.data(), static_cast<size_t>(count)));
			size += static_cast<uint64_t>(count);
		}
	}
	if (!stream.eof())
	{
		error = "failed while reading file for CRC32";
		return false;
	}
	return true;
}

bool validate_bvh8(const bvh8_data &data, std::string &error)
{
	error.clear();
	const bvh8_header &header = data.header;
	if (!validate_header(header, header.file_size, error)
		|| header.node_count != data.nodes.size() || header.packet_count != data.packets.size())
	{
		if (error.empty()) error = "BVH8 header does not match payload";
		return false;
	}

	uint64_t counted_triangles = 0;
	for (uint32_t node_index = 0; node_index < header.node_count; ++node_index)
	{
		const bvh8_node &node = data.nodes[node_index];
		for (uint32_t child_index = 0; child_index < 8; ++child_index)
		{
			const uint32_t ref = node.child[child_index];
			if (ref == k_invalid_ref)
			{
				continue;
			}
			const float minimum[3] = {node.min_x[child_index], node.min_y[child_index], node.min_z[child_index]};
			const float maximum[3] = {node.max_x[child_index], node.max_y[child_index], node.max_z[child_index]};
			if (!finite_bounds(minimum, maximum))
			{
				error = "BVH8 contains invalid child bounds";
				return false;
			}
			if (is_leaf_ref(ref))
			{
				if (leaf_index(ref) >= header.packet_count)
				{
					error = "BVH8 leaf reference is out of range";
					return false;
				}
				counted_triangles += leaf_count(ref);
			}
			else if (ref >= header.node_count || ref <= node_index)
			{
				error = "BVH8 node reference is invalid";
				return false;
			}
		}
	}
	if (counted_triangles != header.triangle_count)
	{
		error = "BVH8 triangle count is inconsistent";
		return false;
	}
	return true;
}

bool load_bvh8(const std::filesystem::path &path, bvh8_data &data, std::string &error)
{
	error.clear();
	data = {};
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	if (!stream || stream.tellg() < 0)
	{
		error = "could not open BVH8 file";
		return false;
	}
	const uint64_t actual_size = static_cast<uint64_t>(stream.tellg());
	stream.seekg(0);
	bvh8_header header {};
	if (actual_size < sizeof(header) || !read_exact(stream, &header, sizeof(header)) || !validate_header(header, actual_size, error))
	{
		if (error.empty()) error = "BVH8 file is truncated";
		return false;
	}
	try
	{
		data.nodes.resize(header.node_count);
		data.packets.resize(header.packet_count);
	}
	catch (const std::bad_alloc &)
	{
		error = "not enough memory to load BVH8";
		data = {};
		return false;
	}
	catch (const std::length_error &)
	{
		error = "BVH8 payload is too large";
		data = {};
		return false;
	}
	data.header = header;
	stream.seekg(static_cast<std::streamoff>(header.nodes_offset));
	if (!read_exact(stream, data.nodes.data(), data.nodes.size() * sizeof(bvh8_node)))
	{
		error = "could not read BVH8 nodes";
		data = {};
		return false;
	}
	stream.seekg(static_cast<std::streamoff>(header.packets_offset));
	if (!read_exact(stream, data.packets.data(), data.packets.size() * sizeof(triangle_packet8)))
	{
		error = "could not read BVH8 packets";
		data = {};
		return false;
	}
	if (!validate_bvh8(data, error) || payload_crc(data) != header.payload_crc32)
	{
		if (error.empty()) error = "BVH8 payload CRC mismatch";
		data = {};
		return false;
	}
	return true;
}

bool write_bvh8(const std::filesystem::path &path, bvh8_data &data, std::string &error)
{
	error.clear();
	if (data.nodes.size() > std::numeric_limits<uint32_t>::max() || data.packets.size() > std::numeric_limits<uint32_t>::max())
	{
		error = "BVH8 payload has too many records";
		return false;
	}
	std::memcpy(data.header.magic, k_magic, sizeof(k_magic));
	data.header.version = k_bvh8_version;
	data.header.header_size = sizeof(bvh8_header);
	data.header.bake_recipe_version = k_bvh8_recipe_version;
	std::fill(std::begin(data.header.reserved), std::end(data.header.reserved), uint8_t {});
	data.header.node_count = static_cast<uint32_t>(data.nodes.size());
	data.header.packet_count = static_cast<uint32_t>(data.packets.size());
	data.header.nodes_offset = align_up(sizeof(bvh8_header), 32);
	data.header.packets_offset = align_up(data.header.nodes_offset + data.nodes.size() * sizeof(bvh8_node), 32);
	data.header.file_size = data.header.packets_offset + data.packets.size() * sizeof(triangle_packet8);
	data.header.payload_crc32 = payload_crc(data);
	if (!validate_bvh8(data, error))
	{
		return false;
	}

	std::error_code filesystem_error;
	if (!path.parent_path().empty())
	{
		std::filesystem::create_directories(path.parent_path(), filesystem_error);
		if (filesystem_error)
		{
			error = "could not create BVH8 directory: " + filesystem_error.message();
			return false;
		}
	}
	const std::filesystem::path temporary = path.string() + ".tmp";
	std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
	if (!stream)
	{
		error = "could not create temporary BVH8 file";
		return false;
	}
	stream.write(reinterpret_cast<const char *>(&data.header), sizeof(data.header));
	stream.seekp(static_cast<std::streamoff>(data.header.nodes_offset));
	stream.write(reinterpret_cast<const char *>(data.nodes.data()), static_cast<std::streamsize>(data.nodes.size() * sizeof(bvh8_node)));
	stream.seekp(static_cast<std::streamoff>(data.header.packets_offset));
	stream.write(reinterpret_cast<const char *>(data.packets.data()), static_cast<std::streamsize>(data.packets.size() * sizeof(triangle_packet8)));
	stream.flush();
	const bool written = stream.good();
	stream.close();
	if (!written)
	{
		error = "failed while writing BVH8 file";
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	bvh8_data verified;
	if (!load_bvh8(temporary, verified, error) || verified.header.payload_crc32 != data.header.payload_crc32)
	{
		if (error.empty()) error = "temporary BVH8 validation failed";
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
