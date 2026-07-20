// Reads the temporary physics GLB made by ValveResourceFormat, filters groups by
// the documented bake recipe, and outputs finite static collision triangles.
// It runs only in the baker/tests and rejects malformed accessors or geometry.

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "glb_import.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>

namespace cs2fow
{
namespace
{

constexpr float k_source_units_per_meter = 39.37007874f;

std::vector<std::string> parse_interact_as(const cgltf_data *data, const cgltf_extras &extras)
{
	cgltf_size size = 0;
	if (cgltf_copy_extras_json(data, &extras, nullptr, &size) != cgltf_result_success || size == 0)
	{
		return {};
	}
	std::string json(size, '\0');
	if (cgltf_copy_extras_json(data, &extras, json.data(), &size) != cgltf_result_success)
	{
		return {};
	}
	const size_t key = json.find("\"InteractAs\"");
	const size_t begin = key == std::string::npos ? std::string::npos : json.find('[', key);
	const size_t end = begin == std::string::npos ? std::string::npos : json.find(']', begin);
	if (end == std::string::npos)
	{
		return {};
	}
	std::vector<std::string> result;
	for (size_t position = begin + 1u; position < end;)
	{
		const size_t quote = json.find('"', position);
		if (quote == std::string::npos || quote >= end)
		{
			break;
		}
		const size_t close = json.find('"', quote + 1u);
		if (close == std::string::npos || close > end)
		{
			break;
		}
		result.push_back(json.substr(quote + 1u, close - quote - 1u));
		position = close + 1u;
	}
	return result;
}

bool parse_surface_property(const cgltf_data *data, const cgltf_extras &extras, std::string &result)
{
	result.clear();
	cgltf_size size = 0;
	if (cgltf_copy_extras_json(data, &extras, nullptr, &size) != cgltf_result_success || size == 0)
	{
		return false;
	}
	std::string json(size, '\0');
	if (cgltf_copy_extras_json(data, &extras, json.data(), &size) != cgltf_result_success)
	{
		return false;
	}
	const size_t key = json.find("\"SurfaceProperty\"");
	const size_t colon = key == std::string::npos ? std::string::npos : json.find(':', key);
	const size_t quote = colon == std::string::npos ? std::string::npos : json.find('"', colon);
	const size_t close = quote == std::string::npos ? std::string::npos : json.find('"', quote + 1u);
	if (close == std::string::npos)
	{
		return false;
	}
	result = json.substr(quote + 1u, close - quote - 1u);
	return true;
}

vec3 transform_point(const cgltf_float matrix[16], const cgltf_float source[3])
{
	const vec3 gltf {
		matrix[0] * source[0] + matrix[4] * source[1] + matrix[8] * source[2] + matrix[12],
		matrix[1] * source[0] + matrix[5] * source[1] + matrix[9] * source[2] + matrix[13],
		matrix[2] * source[0] + matrix[6] * source[1] + matrix[10] * source[2] + matrix[14]
	};
	return {
		gltf.z * k_source_units_per_meter,
		gltf.x * k_source_units_per_meter,
		gltf.y * k_source_units_per_meter
	};
}

bool finite(vec3 value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool valid_triangle(const triangle &value)
{
	if (!finite(value.v0) || !finite(value.v1) || !finite(value.v2))
	{
		return false;
	}
	const vec3 a {value.v1.x - value.v0.x, value.v1.y - value.v0.y, value.v1.z - value.v0.z};
	const vec3 b {value.v2.x - value.v0.x, value.v2.y - value.v0.y, value.v2.z - value.v0.z};
	const vec3 cross {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
	return cross.x * cross.x + cross.y * cross.y + cross.z * cross.z > 1.0e-12f;
}

const cgltf_accessor *position_accessor(const cgltf_primitive &primitive)
{
	for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
	{
		if (primitive.attributes[i].type == cgltf_attribute_type_position)
		{
			return primitive.attributes[i].data;
		}
	}
	return nullptr;
}

} // namespace

bool physics_group_accepted(const std::vector<std::string> &tags, const std::string &surface_property)
{
	const bool untagged = tags.empty();
	const bool explicitly_opaque = std::find(tags.begin(), tags.end(), "solid") != tags.end()
		&& std::find(tags.begin(), tags.end(), "blocklight") != tags.end();
	if (!untagged && !explicitly_opaque)
	{
		return false;
	}

	std::string surface = surface_property;
	std::transform(surface.begin(), surface.end(), surface.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	constexpr std::string_view porous[] = {
		"glass", "chainlink", "grate", "foliage", "tree", "water", "puddle", "window", "cloth", "basket", "mesh", "netting", "wire", "screen", "vent"
	};
	for (const std::string_view value : porous)
	{
		if (surface.find(value) != std::string::npos)
		{
			return false;
		}
	}
	if (explicitly_opaque || surface.empty())
	{
		return true;
	}

	// ponytail: keep this conservative; switch to rendered-material opacity if custom maps outgrow the known surface list.
	constexpr std::string_view opaque[] = {
		"default", "concrete", "rock", "boulder", "brick", "plaster", "sheetrock", "tile", "dirt", "sand", "gravel",
		"metal", "solidmetal", "wood", "porcelain", "pottery", "clay", "carpet"
	};
	return std::any_of(std::begin(opaque), std::end(opaque), [&surface](std::string_view value) { return surface.starts_with(value); });
}

bool import_physics_glb(const std::filesystem::path &path, std::vector<triangle> &triangles, import_report &report, std::string &error,
	std::vector<std::string> *triangle_surfaces)
{
	triangles.clear();
	if (triangle_surfaces != nullptr)
	{
		triangle_surfaces->clear();
	}
	report = {};
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	if (!stream || stream.tellg() < 0)
	{
		error = "could not read GLB";
		return false;
	}
	std::vector<std::byte> file(static_cast<size_t>(stream.tellg()));
	stream.seekg(0);
	if (!stream.read(reinterpret_cast<char *>(file.data()), static_cast<std::streamsize>(file.size())))
	{
		error = "could not read GLB";
		return false;
	}
	cgltf_options options {};
	cgltf_data *data = nullptr;
	if (cgltf_parse(&options, file.data(), file.size(), &data) != cgltf_result_success || data == nullptr)
	{
		error = "could not parse GLB";
		return false;
	}
	if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success || cgltf_validate(data) != cgltf_result_success)
	{
		error = "GLB buffers or structure are invalid";
		cgltf_free(data);
		return false;
	}

	for (cgltf_size node_index = 0; node_index < data->nodes_count; ++node_index)
	{
		const cgltf_node &node = data->nodes[node_index];
		if (node.mesh == nullptr)
		{
			continue;
		}
		physics_group_report group;
		group.name = node.name == nullptr ? "unnamed" : node.name;
		const bool has_surface_property = parse_surface_property(data, node.extras, group.surface_property);
		group.tags = parse_interact_as(data, node.extras);
		group.accepted = has_surface_property && physics_group_accepted(group.tags, group.surface_property);
		cgltf_float transform[16] {};
		cgltf_node_transform_world(&node, transform);

		for (cgltf_size primitive_index = 0; primitive_index < node.mesh->primitives_count; ++primitive_index)
		{
			const cgltf_primitive &primitive = node.mesh->primitives[primitive_index];
			const cgltf_accessor *positions = position_accessor(primitive);
			if (primitive.type != cgltf_primitive_type_triangles || positions == nullptr || primitive.indices == nullptr || primitive.indices->count % 3u != 0)
			{
				error = "GLB contains a non-indexed or non-triangle physics primitive";
				cgltf_free(data);
				return false;
			}
			const uint64_t primitive_triangles = primitive.indices->count / 3u;
			group.triangles += primitive_triangles;
			report.raw_triangles += primitive_triangles;
			if (!group.accepted)
			{
				continue;
			}
			report.accepted_triangles += primitive_triangles;
			for (cgltf_size index = 0; index < primitive.indices->count; index += 3u)
			{
				triangle value;
				vec3 *vertices[3] = {&value.v0, &value.v1, &value.v2};
				bool indices_valid = true;
				for (cgltf_size corner = 0; corner < 3; ++corner)
				{
					const cgltf_size vertex_index = cgltf_accessor_read_index(primitive.indices, index + corner);
					cgltf_float source[3] {};
					if (vertex_index >= positions->count || !cgltf_accessor_read_float(positions, vertex_index, source, 3))
					{
						indices_valid = false;
						break;
					}
					*vertices[corner] = transform_point(transform, source);
				}
				if (!indices_valid || !valid_triangle(value))
				{
					++report.rejected_invalid;
					continue;
				}
				triangles.push_back(value);
				if (triangle_surfaces != nullptr)
				{
					triangle_surfaces->push_back(group.surface_property);
				}
			}
		}
		report.groups.push_back(std::move(group));
	}
	cgltf_free(data);
	if (triangles.empty())
	{
		error = "GLB contains no accepted physics triangles";
		return false;
	}
	return true;
}

} // namespace cs2fow
