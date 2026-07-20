#pragma once

// Declares the offline physics-GLB importer. It outputs accepted triangles plus
// a report explaining every geometry group; invalid input stops the bake.

#include "bvh8.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cs2fow
{

struct physics_group_report
{
	std::string name;
	std::string surface_property;
	std::vector<std::string> tags;
	uint64_t triangles {};
	bool accepted {};
};

struct import_report
{
	uint64_t raw_triangles {};
	uint64_t accepted_triangles {};
	uint64_t rejected_invalid {};
	std::vector<physics_group_report> groups;
};

bool import_physics_glb(const std::filesystem::path &path, std::vector<triangle> &triangles, import_report &report, std::string &error,
	std::vector<std::string> *triangle_surfaces = nullptr);
bool physics_group_accepted(const std::vector<std::string> &tags, const std::string &surface_property);

} // namespace cs2fow
