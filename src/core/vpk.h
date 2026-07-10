#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cs2fow
{

struct vpk_entry
{
	std::string path;
	uint32_t crc32 {};
	uint64_t size {};
	uint16_t archive_index {};
	uint32_t archive_offset {};
	uint32_t archive_size {};
	uint64_t preload_offset {};
	uint16_t preload_size {};
	uint64_t embedded_data_offset {};
};

bool list_vpk_entries(const std::filesystem::path &vpk_path, std::vector<vpk_entry> &entries, std::string &error);
bool find_vpk_entry(const std::filesystem::path &vpk_path, const std::string &entry_path, vpk_entry &entry, std::string &error);
bool extract_vpk_entry(const std::filesystem::path &vpk_path, const vpk_entry &entry,
	const std::filesystem::path &output, std::string &error);

} // namespace cs2fow
