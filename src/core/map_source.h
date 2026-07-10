#pragma once

#include "bvh8.h"
#include "vpk.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cs2fow
{

struct map_source
{
	std::filesystem::path vpk;
	std::string entry;
	vpk_entry metadata;
	uint32_t flags {};
};

bool valid_map_name(std::string_view map);
std::vector<std::filesystem::path> vpk_path_candidates(std::string path);
bool list_vpk_maps(const std::filesystem::path &vpk, std::vector<std::string> &maps, std::string &error);
bool find_map_source(const std::filesystem::path &vpk, const std::string &map, map_source &source, std::string &error);
bool same_map_source(const map_source &left, const map_source &right);

} // namespace cs2fow
