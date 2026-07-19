// Locates the block table inside a compiled Source 2 resource and reads the
// scalar and block constants out of VRF's --decompile KV3 text.

#include "vvis_import.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace cs2fow
{
namespace
{

constexpr uint16_t k_header_version = 12;
constexpr uint16_t k_resource_version = 7;
constexpr uint64_t k_block_row_bytes = 12;

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

uint16_t read_u16(std::span<const std::byte> bytes, uint64_t offset)
{
	return static_cast<uint16_t>(std::to_integer<uint16_t>(bytes[offset])
		| static_cast<uint16_t>(std::to_integer<uint16_t>(bytes[offset + 1]) << 8));
}

uint32_t read_u32(std::span<const std::byte> bytes, uint64_t offset)
{
	return std::to_integer<uint32_t>(bytes[offset])
		| (std::to_integer<uint32_t>(bytes[offset + 1]) << 8)
		| (std::to_integer<uint32_t>(bytes[offset + 2]) << 16)
		| (std::to_integer<uint32_t>(bytes[offset + 3]) << 24);
}

std::string_view trim(std::string_view value)
{
	constexpr std::string_view whitespace = " \t\r\n";
	const size_t begin = value.find_first_not_of(whitespace);
	if (begin == std::string_view::npos)
	{
		return {};
	}
	const size_t end = value.find_last_not_of(whitespace);
	return value.substr(begin, end - begin + 1);
}

bool parse_u32(std::string_view token, uint32_t &value)
{
	const char *first = token.data();
	const char *last = token.data() + token.size();
	const std::from_chars_result result = std::from_chars(first, last, value);
	return result.ec == std::errc() && result.ptr == last && !token.empty();
}

bool parse_float(std::string_view token, float &value)
{
	const char *first = token.data();
	const char *last = token.data() + token.size();
	const std::from_chars_result result = std::from_chars(first, last, value);
	return result.ec == std::errc() && result.ptr == last && !token.empty();
}

bool parse_vec3(std::string_view value, float out[3])
{
	std::string_view body = trim(value);
	if (body.size() < 2 || body.front() != '[' || body.back() != ']')
	{
		return false;
	}
	body = body.substr(1, body.size() - 2u);
	int count = 0;
	size_t pos = 0;
	for (;;)
	{
		const size_t comma = body.find(',', pos);
		const std::string_view token = body.substr(pos, (comma == std::string_view::npos ? body.size() : comma) - pos);
		if (count >= 3 || !parse_float(trim(token), out[count]))
		{
			return false;
		}
		++count;
		if (comma == std::string_view::npos)
		{
			break;
		}
		pos = comma + 1u;
	}
	return count == 3;
}

std::string_view next_line(std::string_view &text)
{
	while (!text.empty())
	{
		const size_t newline = text.find('\n');
		const size_t length = newline == std::string_view::npos ? text.size() : newline;
		const std::string_view line = trim(text.substr(0, length));
		text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1u);
		if (!line.empty())
		{
			return line;
		}
	}
	return {};
}

bool split_assignment(std::string_view line, std::string_view &key, std::string_view &value)
{
	const size_t equals = line.find('=');
	if (equals == std::string_view::npos)
	{
		return false;
	}
	key = trim(line.substr(0, equals));
	value = trim(line.substr(equals + 1u));
	return !key.empty();
}

struct scalar_field
{
	std::string_view key;
	uint32_t vvis_constants::*member;
};

struct vec3_field
{
	std::string_view key;
	float (vvis_constants::*member)[3];
};

struct block_field
{
	std::string_view key;
	uint32_t vvis_constants::*offset;
	uint32_t vvis_constants::*count;
};

constexpr std::array<scalar_field, 4> k_scalar_fields {{
	{"m_nBaseClusterCount", &vvis_constants::m_nBaseClusterCount},
	{"m_nPVSBytesPerCluster", &vvis_constants::m_nPVSBytesPerCluster},
	{"m_nSkyVisibilityCluster", &vvis_constants::m_nSkyVisibilityCluster},
	{"m_nSunVisibilityCluster", &vvis_constants::m_nSunVisibilityCluster},
}};

constexpr std::array<vec3_field, 2> k_vec3_fields {{
	{"m_vMinBounds", &vvis_constants::m_vMinBounds},
	{"m_vMaxBounds", &vvis_constants::m_vMaxBounds},
}};

constexpr std::array<block_field, 6> k_block_fields {{
	{"m_NodeBlock", &vvis_constants::m_NodeBlock_m_nOffset, &vvis_constants::m_NodeBlock_m_nElementCount},
	{"m_RegionBlock", &vvis_constants::m_RegionBlock_m_nOffset, &vvis_constants::m_RegionBlock_m_nElementCount},
	{"m_EnclosedClusterListBlock", &vvis_constants::m_EnclosedClusterListBlock_m_nOffset, &vvis_constants::m_EnclosedClusterListBlock_m_nElementCount},
	{"m_EnclosedClustersBlock", &vvis_constants::m_EnclosedClustersBlock_m_nOffset, &vvis_constants::m_EnclosedClustersBlock_m_nElementCount},
	{"m_MasksBlock", &vvis_constants::m_MasksBlock_m_nOffset, &vvis_constants::m_MasksBlock_m_nElementCount},
	{"m_nVisBlocks", &vvis_constants::m_nVisBlocks_m_nOffset, &vvis_constants::m_nVisBlocks_m_nElementCount},
}};

bool parse_block_body(std::string_view &text, std::string_view block_key, uint32_t &offset, uint32_t &count, std::string &error)
{
	if (next_line(text) != "{")
	{
		error = "expected '{' after vvis field: ";
		error.append(block_key);
		return false;
	}
	bool have_offset = false;
	bool have_count = false;
	for (;;)
	{
		const std::string_view line = next_line(text);
		if (line.empty())
		{
			error = "unterminated vvis block: ";
			error.append(block_key);
			return false;
		}
		if (line == "}")
		{
			break;
		}
		std::string_view key;
		std::string_view value;
		if (!split_assignment(line, key, value))
		{
			error = "line is not a field assignment in vvis block: ";
			error.append(block_key);
			return false;
		}
		uint32_t *target = nullptr;
		bool *seen = nullptr;
		if (key == "m_nOffset")
		{
			target = &offset;
			seen = &have_offset;
		}
		else if (key == "m_nElementCount")
		{
			target = &count;
			seen = &have_count;
		}
		else
		{
			continue;
		}
		if (*seen)
		{
			error = "duplicate field in vvis block: ";
			error.append(block_key);
			return false;
		}
		if (!parse_u32(value, *target))
		{
			error = "invalid value in vvis block: ";
			error.append(block_key);
			return false;
		}
		*seen = true;
	}
	if (!have_offset || !have_count)
	{
		error = "vvis block is missing m_nOffset or m_nElementCount: ";
		error.append(block_key);
		return false;
	}
	return true;
}

bool node_is_leaf(uint64_t node)
{
	return (node & 1u) != 0u;
}

uint32_t node_offset(uint64_t node)
{
	return static_cast<uint32_t>((node >> 1) & 0x7fffffffu);
}

uint32_t node_region_count(uint64_t node)
{
	return static_cast<uint32_t>((node >> 32) & 0xffu);
}

uint32_t region_mask_index(uint64_t region)
{
	return static_cast<uint32_t>(region >> 40);
}

} // namespace

bool parse_vxvs_block(std::span<const std::byte> bytes, resource_block &out, std::string &error)
{
	error.clear();

	constexpr size_t header_size = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t);
	if (bytes.size() < header_size)
	{
		error = "resource is smaller than its header";
		return false;
	}

	const uint32_t file_size = read_u32(bytes, 0);
	const uint16_t header_version = read_u16(bytes, 4);
	const uint16_t resource_version = read_u16(bytes, 6);
	const uint64_t block_offset = read_u32(bytes, 8);
	const uint32_t block_count = read_u32(bytes, 12);
	if (header_version != k_header_version)
	{
		error = "unsupported header version";
		return false;
	}
	if (resource_version != k_resource_version)
	{
		error = "unsupported resource version";
		return false;
	}
	if (block_count != 3)
	{
		error = "unsupported block count, did you pass in a vvis file?";
		return false;
	}
	if (file_size != bytes.size())
	{
		error = "resource file size does not match its header";
		return false;
	}

	uint64_t table_start = 0;
	uint64_t vxvs_row = 0;
	uint64_t table_end = 0;
	if (!checked_add(8u, block_offset, table_start)
		|| !checked_add(table_start, 2u * k_block_row_bytes, vxvs_row)
		|| !checked_add(table_start, 3u * k_block_row_bytes, table_end)
		|| table_end > bytes.size())
	{
		error = "resource block table is out of range";
		return false;
	}
	if (std::memcmp(bytes.data() + vxvs_row, "VXVS", sizeof(out.fourcc)) != 0)
	{
		error = "third resource block is not VXVS";
		return false;
	}

	uint64_t offset_field = vxvs_row + 4u;
	uint64_t block_start = 0;
	uint64_t block_end = 0;
	if (!checked_add(offset_field, read_u32(bytes, offset_field), block_start)
		|| !checked_add(block_start, read_u32(bytes, vxvs_row + 8u), block_end)
		|| block_end > bytes.size())
	{
		error = "vxvs block extends past the file";
		return false;
	}

	std::memcpy(out.fourcc, bytes.data() + vxvs_row, sizeof(out.fourcc));
	out.offset = static_cast<uint32_t>(block_start);
	out.size = static_cast<uint32_t>(block_end - block_start);
	return true;
}

bool parse_vvis_text(std::string_view text, vvis_constants &out, std::string &error)
{
	error.clear();
	out = {};

	uint32_t found_scalars = 0;
	uint32_t found_vec3s = 0;
	uint32_t found_blocks = 0;
	bool found_grid_size = false;

	for (;;)
	{
		const std::string_view line = next_line(text);
		if (line.empty())
		{
			break;
		}
		if (line == "{" || line == "}" || line.starts_with("<!--"))
		{
			continue;
		}

		std::string_view key;
		std::string_view value;
		if (!split_assignment(line, key, value))
		{
			error = "vvis text line is not a field assignment";
			return false;
		}

		if (key == "m_flGridSize")
		{
			if (found_grid_size)
			{
				error = "duplicate vvis field: m_flGridSize";
				return false;
			}
			if (!parse_float(value, out.m_flGridSize))
			{
				error = "invalid vvis field value: m_flGridSize";
				return false;
			}
			found_grid_size = true;
			continue;
		}

		bool matched = false;
		for (size_t i = 0; i < k_scalar_fields.size() && !matched; ++i)
		{
			if (key != k_scalar_fields[i].key)
			{
				continue;
			}
			if ((found_scalars & (1u << i)) != 0u)
			{
				error = "duplicate vvis field: ";
				error.append(key);
				return false;
			}
			if (!parse_u32(value, out.*k_scalar_fields[i].member))
			{
				error = "invalid vvis field value: ";
				error.append(key);
				return false;
			}
			found_scalars |= 1u << i;
			matched = true;
		}
		for (size_t i = 0; i < k_vec3_fields.size() && !matched; ++i)
		{
			if (key != k_vec3_fields[i].key)
			{
				continue;
			}
			if ((found_vec3s & (1u << i)) != 0u)
			{
				error = "duplicate vvis field: ";
				error.append(key);
				return false;
			}
			if (!parse_vec3(value, out.*k_vec3_fields[i].member))
			{
				error = "invalid vvis field value: ";
				error.append(key);
				return false;
			}
			found_vec3s |= 1u << i;
			matched = true;
		}
		for (size_t i = 0; i < k_block_fields.size() && !matched; ++i)
		{
			if (key != k_block_fields[i].key)
			{
				continue;
			}
			if ((found_blocks & (1u << i)) != 0u)
			{
				error = "duplicate vvis field: ";
				error.append(key);
				return false;
			}
			if (!value.empty())
			{
				error = "expected a block body for vvis field: ";
				error.append(key);
				return false;
			}
			if (!parse_block_body(text, key, out.*k_block_fields[i].offset, out.*k_block_fields[i].count, error))
			{
				return false;
			}
			found_blocks |= 1u << i;
			matched = true;
		}
	}

	for (size_t i = 0; i < k_scalar_fields.size(); ++i)
	{
		if ((found_scalars & (1u << i)) == 0u)
		{
			error = "missing required vvis field: ";
			error.append(k_scalar_fields[i].key);
			return false;
		}
	}
	for (size_t i = 0; i < k_vec3_fields.size(); ++i)
	{
		if ((found_vec3s & (1u << i)) == 0u)
		{
			error = "missing required vvis field: ";
			error.append(k_vec3_fields[i].key);
			return false;
		}
	}
	if (!found_grid_size)
	{
		error = "missing required vvis field: m_flGridSize";
		return false;
	}
	for (size_t i = 0; i < k_block_fields.size(); ++i)
	{
		if ((found_blocks & (1u << i)) == 0u)
		{
			error = "missing required vvis field: ";
			error.append(k_block_fields[i].key);
			return false;
		}
	}

	return true;
}

} // namespace cs2fow
