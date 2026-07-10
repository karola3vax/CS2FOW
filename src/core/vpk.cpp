#include "vpk.h"

// Parses Valve package (VPK) directory trees and copies complete entries.
// All offsets are checked before a file is created.

#include "bvh8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>

namespace cs2fow
{
namespace
{

constexpr uint32_t k_vpk_signature = 0x55aa1234u;
constexpr uint16_t k_embedded_archive = 0x7fffu;
constexpr uint16_t k_entry_terminator = 0xffffu;

struct vpk_header_info
{
	uint64_t file_size {};
	uint64_t tree_end {};
	uint64_t embedded_data_offset {};
};

template <typename type>
bool read_value(std::istream &stream, type &value)
{
	stream.read(reinterpret_cast<char *>(&value), sizeof(value));
	return stream.good();
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

bool read_header(std::ifstream &stream, vpk_header_info &info, std::string &error)
{
	stream.seekg(0, std::ios::end);
	if (stream.tellg() < 0)
	{
		error = "could not read VPK size";
		return false;
	}
	info.file_size = static_cast<uint64_t>(stream.tellg());
	stream.seekg(0);
	uint32_t signature = 0;
	uint32_t version = 0;
	uint32_t tree_size = 0;
	if (!read_value(stream, signature) || !read_value(stream, version) || !read_value(stream, tree_size)
		|| signature != k_vpk_signature || (version != 1 && version != 2))
	{
		error = "invalid or unsupported VPK header";
		return false;
	}
	uint32_t header_size = 12;
	uint64_t declared_size = 0;
	if (version == 2)
	{
		header_size = 28;
		uint32_t section_sizes[4] {};
		for (uint32_t &size : section_sizes)
		{
			if (!read_value(stream, size))
			{
				error = "truncated VPK v2 header";
				return false;
			}
		}
		declared_size = header_size;
		if (!checked_add(declared_size, tree_size, declared_size))
		{
			error = "VPK size overflows";
			return false;
		}
		for (const uint32_t size : section_sizes)
		{
			if (!checked_add(declared_size, size, declared_size))
			{
				error = "VPK size overflows";
				return false;
			}
		}
		if (declared_size > info.file_size)
		{
			error = "VPK v2 sections are truncated";
			return false;
		}
	}
	if (tree_size == 0 || !checked_add(header_size, tree_size, info.tree_end) || info.tree_end > info.file_size)
	{
		error = "VPK directory tree is invalid";
		return false;
	}
	info.embedded_data_offset = info.tree_end;
	return true;
}

bool read_string(std::ifstream &stream, uint64_t tree_end, std::string &value)
{
	value.clear();
	while (stream.tellg() >= 0 && static_cast<uint64_t>(stream.tellg()) < tree_end)
	{
		char character = '\0';
		if (!stream.get(character))
		{
			return false;
		}
		if (character == '\0')
		{
			return true;
		}
		if (value.size() >= 4096)
		{
			return false;
		}
		value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}
	return false;
}

std::string normalized(std::string value)
{
	std::replace(value.begin(), value.end(), '\\', '/');
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
	{
		return static_cast<char>(std::tolower(character));
	});
	while (!value.empty() && value.front() == '/')
	{
		value.erase(value.begin());
	}
	return value;
}

std::filesystem::path archive_path(const std::filesystem::path &directory_file, uint16_t archive_index, std::string &error)
{
	if (archive_index == k_embedded_archive)
	{
		return directory_file;
	}
	const std::string filename = directory_file.filename().string();
	const std::string lower = normalized(filename);
	if (!lower.ends_with("_dir.vpk"))
	{
		error = "multipart VPK directory is not named *_dir.vpk";
		return {};
	}
	char suffix[16] {};
	std::snprintf(suffix, sizeof(suffix), "_%03u.vpk", archive_index);
	return directory_file.parent_path() / (filename.substr(0, filename.size() - 8u) + suffix);
}

bool range_fits(const std::filesystem::path &path, uint64_t offset, uint64_t length, std::string &error)
{
	std::error_code filesystem_error;
	const uint64_t size = std::filesystem::file_size(path, filesystem_error);
	uint64_t end = 0;
	if (filesystem_error || !checked_add(offset, length, end) || end > size)
	{
		error = "VPK entry data is truncated: " + path.string();
		return false;
	}
	return true;
}

bool copy_range(std::ifstream &source, uint64_t offset, uint64_t length, std::ofstream &destination,
	uint32_t &crc, std::string &error)
{
	if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
	{
		error = "VPK entry offset is too large";
		return false;
	}
	source.seekg(static_cast<std::streamoff>(offset));
	std::array<std::byte, 64 * 1024> buffer {};
	while (length != 0)
	{
		const size_t count = static_cast<size_t>(std::min<uint64_t>(length, buffer.size()));
		source.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(count));
		if (source.gcount() != static_cast<std::streamsize>(count))
		{
			error = "VPK entry data ended early";
			return false;
		}
		destination.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(count));
		if (!destination.good())
		{
			error = "could not write extracted VPK entry";
			return false;
		}
		crc = crc32_extend(crc, std::span<const std::byte>(buffer.data(), count));
		length -= count;
	}
	return true;
}

} // namespace

bool list_vpk_entries(const std::filesystem::path &vpk_path, std::vector<vpk_entry> &entries, std::string &error)
{
	entries.clear();
	error.clear();
	std::ifstream stream(vpk_path, std::ios::binary);
	if (!stream)
	{
		error = "could not open VPK";
		return false;
	}
	vpk_header_info header;
	if (!read_header(stream, header, error))
	{
		return false;
	}

	for (;;)
	{
		std::string extension;
		if (!read_string(stream, header.tree_end, extension))
		{
			error = "malformed VPK extension tree";
			return false;
		}
		if (extension.empty())
		{
			break;
		}
		if (extension == " ") extension.clear();
		for (;;)
		{
			std::string directory;
			if (!read_string(stream, header.tree_end, directory))
			{
				error = "malformed VPK path tree";
				return false;
			}
			if (directory.empty())
			{
				break;
			}
			if (directory == " ") directory.clear();
			for (;;)
			{
				std::string name;
				if (!read_string(stream, header.tree_end, name))
				{
					error = "malformed VPK filename tree";
					return false;
				}
				if (name.empty())
				{
					break;
				}
				vpk_entry entry;
				uint32_t archive_length = 0;
				uint16_t terminator = 0;
				if (!read_value(stream, entry.crc32) || !read_value(stream, entry.preload_size)
					|| !read_value(stream, entry.archive_index) || !read_value(stream, entry.archive_offset)
					|| !read_value(stream, archive_length) || !read_value(stream, terminator)
					|| terminator != k_entry_terminator || stream.tellg() < 0)
				{
					error = "malformed VPK entry";
					return false;
				}
				entry.preload_offset = static_cast<uint64_t>(stream.tellg());
				entry.archive_size = archive_length;
				entry.size = static_cast<uint64_t>(entry.preload_size) + archive_length;
				entry.embedded_data_offset = header.embedded_data_offset;
				entry.path = (directory.empty() ? name : directory + "/" + name)
					+ (extension.empty() ? "" : "." + extension);
				uint64_t preload_end = 0;
				if (!checked_add(entry.preload_offset, entry.preload_size, preload_end) || preload_end > header.tree_end)
				{
					error = "VPK preload data is truncated";
					return false;
				}
				stream.seekg(static_cast<std::streamoff>(preload_end));
				entries.push_back(std::move(entry));
			}
		}
	}
	if (stream.tellg() < 0 || static_cast<uint64_t>(stream.tellg()) != header.tree_end)
	{
		error = "VPK directory tree has trailing or missing data";
		entries.clear();
		return false;
	}
	return true;
}

bool find_vpk_entry(const std::filesystem::path &vpk_path, const std::string &entry_path, vpk_entry &entry, std::string &error)
{
	std::vector<vpk_entry> entries;
	if (!list_vpk_entries(vpk_path, entries, error))
	{
		return false;
	}
	const std::string wanted = normalized(entry_path);
	const auto found = std::find_if(entries.begin(), entries.end(), [&](const vpk_entry &candidate)
	{
		return candidate.path == wanted;
	});
	if (found == entries.end())
	{
		error = "VPK entry not found: " + wanted;
		return false;
	}
	entry = *found;
	return true;
}

bool extract_vpk_entry(const std::filesystem::path &vpk_path, const vpk_entry &entry,
	const std::filesystem::path &output, std::string &error)
{
	error.clear();
	if (entry.path.empty() || entry.size != static_cast<uint64_t>(entry.preload_size) + entry.archive_size
		|| !range_fits(vpk_path, entry.preload_offset, entry.preload_size, error))
	{
		if (error.empty()) error = "invalid VPK entry metadata";
		return false;
	}
	std::filesystem::path archive;
	uint64_t archive_offset = 0;
	if (entry.archive_size != 0)
	{
		archive = archive_path(vpk_path, entry.archive_index, error);
		if (archive.empty())
		{
			return false;
		}
		const uint64_t archive_base = entry.archive_index == k_embedded_archive ? entry.embedded_data_offset : 0;
		if (!checked_add(archive_base, entry.archive_offset, archive_offset)
			|| !range_fits(archive, archive_offset, entry.archive_size, error))
		{
			return false;
		}
	}

	std::error_code filesystem_error;
	if (!output.parent_path().empty())
	{
		std::filesystem::create_directories(output.parent_path(), filesystem_error);
		if (filesystem_error)
		{
			error = "could not create extraction directory: " + filesystem_error.message();
			return false;
		}
	}
	const std::filesystem::path temporary = output.string() + ".tmp";
	std::ifstream directory_stream(vpk_path, std::ios::binary);
	std::ifstream archive_stream;
	if (entry.archive_size != 0) archive_stream.open(archive, std::ios::binary);
	std::ofstream destination(temporary, std::ios::binary | std::ios::trunc);
	uint32_t crc = 0;
	if (!directory_stream || (entry.archive_size != 0 && !archive_stream) || !destination
		|| !copy_range(directory_stream, entry.preload_offset, entry.preload_size, destination, crc, error)
		|| (entry.archive_size != 0 && !copy_range(archive_stream, archive_offset, entry.archive_size, destination, crc, error)))
	{
		if (error.empty()) error = "could not open VPK extraction files";
		destination.close();
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	destination.flush();
	const bool written = destination.good();
	destination.close();
	if (!written || crc != entry.crc32)
	{
		error = written ? "VPK entry CRC mismatch" : "failed while writing extracted VPK entry";
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	std::filesystem::remove(output, filesystem_error);
	filesystem_error.clear();
	std::filesystem::rename(temporary, output, filesystem_error);
	if (filesystem_error)
	{
		error = "could not install extracted VPK entry: " + filesystem_error.message();
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	return true;
}

} // namespace cs2fow
