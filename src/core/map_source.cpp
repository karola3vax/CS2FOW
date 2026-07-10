#include "map_source.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace cs2fow
{

bool valid_map_name(std::string_view map)
{
	if (map.empty() || map.size() >= sizeof(bvh8_header::map_name) || map.front() == '/' || map.back() == '/')
	{
		return false;
	}
	std::string_view segment;
	size_t start = 0;
	while (start < map.size())
	{
		const size_t slash = map.find('/', start);
		segment = map.substr(start, slash == std::string_view::npos ? map.size() - start : slash - start);
		if (segment.empty() || segment == "." || segment == "..")
		{
			return false;
		}
		for (const unsigned char character : segment)
		{
			if (!std::isalnum(character) && character != '_' && character != '-' && character != '.')
			{
				return false;
			}
		}
		if (slash == std::string_view::npos)
		{
			break;
		}
		start = slash + 1u;
	}
	return true;
}

std::vector<std::filesystem::path> vpk_path_candidates(std::string path)
{
	std::string lower = path;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	if (lower.starts_with("vpk:"))
	{
		path.erase(0, 4);
		lower.erase(0, 4);
		const size_t extension = lower.find(".vpk");
		if (extension == std::string::npos)
		{
			return {};
		}
		path.resize(extension + 4u);
		lower.resize(extension + 4u);
	}

	std::vector<std::filesystem::path> candidates;
	if (!path.empty())
	{
		candidates.emplace_back(path);
	}
	if (lower.ends_with(".vpk") && !lower.ends_with("_dir.vpk") && !std::filesystem::exists(std::filesystem::path(path)))
	{
		candidates.emplace_back(path.substr(0, path.size() - 4u) + "_dir.vpk");
	}
	return candidates;
}

bool list_vpk_maps(const std::filesystem::path &vpk, std::vector<std::string> &maps, std::string &error)
{
	maps.clear();
	std::vector<vpk_entry> entries;
	if (!list_vpk_entries(vpk, entries, error))
	{
		return false;
	}
	constexpr std::string_view prefix = "maps/";
	constexpr std::string_view physics_suffix = "/world_physics.vmdl_c";
	constexpr std::string_view vpk_suffix = ".vpk";
	std::set<std::string> unique;
	for (const vpk_entry &entry : entries)
	{
		const std::string_view path = entry.path;
		std::string_view map;
		if (path.starts_with(prefix) && path.ends_with(physics_suffix))
		{
			map = path.substr(prefix.size(), path.size() - prefix.size() - physics_suffix.size());
		}
		else if (path.starts_with(prefix) && path.ends_with(vpk_suffix))
		{
			map = path.substr(prefix.size(), path.size() - prefix.size() - vpk_suffix.size());
		}
		if (valid_map_name(map))
		{
			unique.emplace(map);
		}
	}
	maps.assign(unique.begin(), unique.end());
	return true;
}

bool find_map_source(const std::filesystem::path &vpk, const std::string &map, map_source &source, std::string &error)
{
	source = {};
	if (!valid_map_name(map))
	{
		error = "map name is not a safe relative path";
		return false;
	}
	const std::string physics = "maps/" + map + "/world_physics.vmdl_c";
	vpk_entry entry;
	std::string direct_error;
	if (find_vpk_entry(vpk, physics, entry, direct_error))
	{
		source = {vpk, physics, entry, 0};
		return true;
	}
	const std::string nested = "maps/" + map + ".vpk";
	if (find_vpk_entry(vpk, nested, entry, error))
	{
		source = {vpk, nested, entry, k_bvh8_flag_nested_map_vpk};
		return true;
	}
	error = direct_error + "; nested map VPK not found";
	return false;
}

bool same_map_source(const map_source &left, const map_source &right)
{
	return left.flags == right.flags && left.metadata.crc32 == right.metadata.crc32 && left.metadata.size == right.metadata.size;
}

} // namespace cs2fow
