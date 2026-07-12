#include "builder.h"

// Command-line coordinator for map listing and baking. It turns validated VPK
// physics into a verified BVH8/report (and optional OBJ), owns temporary files,
// and exits with an error before replacing trusted output on any failed step.
#include "glb_import.h"
#include "map_source.h"
#include "subprocess.h"
#include "vpk.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace cs2fow
{
namespace
{

struct arguments
{
	std::filesystem::path game;
	std::string map;
	std::filesystem::path output;
	std::filesystem::path vrf;
	std::filesystem::path vpk;
	std::filesystem::path debug_obj;
	std::filesystem::path inspect_bvh8;
	bool low_priority {};
	bool list_maps {};
};

std::string json_escape(const std::string &value)
{
	std::string result;
	for (const char character : value)
	{
		if (character == '\\' || character == '"')
		{
			result.push_back('\\');
		}
		result.push_back(character);
	}
	return result;
}

bool parse_arguments(int argc, char **argv, arguments &result)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string option = argv[i];
		if (option == "--low-priority")
		{
			result.low_priority = true;
		}
		else if (option == "--list-maps")
		{
			result.list_maps = true;
		}
		else if (option == "--inspect-bvh8" && i + 1 < argc)
		{
			result.inspect_bvh8 = argv[++i];
		}
		else if ((option == "--game" || option == "--map" || option == "--output" || option == "--vrf" || option == "--vpk" || option == "--debug-obj") && i + 1 < argc)
		{
			const std::string value = argv[++i];
			if (option == "--game") result.game = value;
			else if (option == "--map") result.map = value;
			else if (option == "--output") result.output = value;
			else if (option == "--vrf") result.vrf = value;
			else if (option == "--vpk") result.vpk = value;
			else result.debug_obj = value;
		}
		else
		{
			return false;
		}
	}
	if (!result.inspect_bvh8.empty())
	{
		return !result.list_maps && result.game.empty() && result.map.empty() && result.output.empty()
			&& result.vrf.empty() && result.vpk.empty() && result.debug_obj.empty() && !result.low_priority;
	}
	if (result.list_maps)
	{
		return !result.vpk.empty();
	}
	if (result.game.empty() || result.map.empty())
	{
		return false;
	}
	if (result.output.empty())
	{
		result.output = result.map + ".bvh8";
	}
	return true;
}

std::filesystem::path vrf_path(const arguments &args)
{
	if (!args.vrf.empty())
	{
		return args.vrf;
	}
#if defined(_WIN32)
	return "tools/vrf/win64/Source2Viewer-CLI.exe";
#else
	return "tools/vrf/linux64/Source2Viewer-CLI";
#endif
}

bool invoke_vrf(const arguments &args, const std::vector<std::string> &arguments, std::string &error)
{
	process_result result;
	if (!run_process(vrf_path(args), arguments, std::chrono::minutes(10), nullptr, false,
		posix_process_group::inherited, result, error))
	{
		return false;
	}
	if (result.timed_out)
	{
		error = "VRF CLI timed out";
		if (!result.output_tail.empty()) error += "\n" + result.output_tail;
		return false;
	}
	if (result.exit_code != 0)
	{
		error = "VRF CLI failed with exit code " + std::to_string(result.exit_code);
		if (!result.output_tail.empty()) error += "\n" + result.output_tail;
		return false;
	}
	return true;
}

std::filesystem::path find_vrf_output(const std::filesystem::path &directory)
{
	std::error_code error;
	for (const auto &entry : std::filesystem::recursive_directory_iterator(directory, error))
	{
		if (entry.is_regular_file() && entry.path().filename().string().ends_with("_physics.glb") && entry.file_size() > 1024)
		{
			return entry.path();
		}
	}
	return {};
}

bool export_glb(const arguments &args, const std::filesystem::path &vpk, const std::filesystem::path &temporary, std::filesystem::path &glb, std::string &error)
{
	if (!std::filesystem::exists(vrf_path(args)))
	{
		error = "VRF CLI not found; pass --vrf <path>";
		return false;
	}
	const std::string resource = "maps/" + args.map + "/world_physics.vmdl_c";
	if (!invoke_vrf(args, {"-i", vpk.string(), "-o", temporary.string(), "--decompile", "--vpk_filepath", resource,
		"--gltf_export_format", "glb", "--gltf_export_extras"}, error))
	{
		return false;
	}
	glb = find_vrf_output(temporary);
	if (glb.empty())
	{
		error = "VRF did not emit a physics GLB";
		return false;
	}
	return true;
}

bool extract_nested_map(const map_source &source, const std::filesystem::path &temporary,
	std::filesystem::path &map_vpk, std::string &error)
{
	const std::filesystem::path output = temporary / "nested";
	map_vpk = output / std::filesystem::path(source.entry);
	return extract_vpk_entry(source.vpk, source.metadata, map_vpk, error);
}

bool write_obj(const std::filesystem::path &path, const std::vector<triangle> &triangles, std::string &error)
{
	std::ofstream stream(path, std::ios::trunc);
	if (!stream)
	{
		error = "could not create debug OBJ";
		return false;
	}
	for (const triangle &value : triangles)
	{
		stream << "v " << value.v0.x << ' ' << value.v0.y << ' ' << value.v0.z << '\n';
		stream << "v " << value.v1.x << ' ' << value.v1.y << ' ' << value.v1.z << '\n';
		stream << "v " << value.v2.x << ' ' << value.v2.y << ' ' << value.v2.z << '\n';
	}
	for (size_t i = 0; i < triangles.size(); ++i)
	{
		stream << "f " << i * 3u + 1u << ' ' << i * 3u + 2u << ' ' << i * 3u + 3u << '\n';
	}
	return stream.good();
}

bool write_report(const std::filesystem::path &path, const arguments &args, const map_source &source, const vpk_entry &physics,
	const import_report &report, const bvh8_data &data, std::string &error)
{
	std::ofstream stream(path, std::ios::trunc);
	if (!stream)
	{
		error = "could not create JSON report";
		return false;
	}
	stream << "{\n  \"map\": \"" << json_escape(args.map) << "\",\n"
		<< "  \"source_kind\": \"" << (source.flags == 0 ? "world_physics" : "nested_map_vpk") << "\",\n"
		<< "  \"source_entry\": \"" << json_escape(source.entry) << "\",\n"
		<< "  \"source_crc32\": \"0x" << std::hex << std::setw(8) << std::setfill('0') << source.metadata.crc32 << std::dec << "\",\n"
		<< "  \"source_size\": " << source.metadata.size << ",\n"
		<< "  \"physics_crc32\": \"0x" << std::hex << std::setw(8) << std::setfill('0') << physics.crc32 << std::dec << "\",\n"
		<< "  \"physics_size\": " << physics.size << ",\n"
		<< "  \"raw_triangles\": " << report.raw_triangles << ",\n"
		<< "  \"accepted_triangles\": " << report.accepted_triangles << ",\n"
		<< "  \"baked_triangles\": " << data.header.triangle_count << ",\n"
		<< "  \"rejected_invalid\": " << report.rejected_invalid << ",\n"
		<< "  \"nodes\": " << data.nodes.size() << ",\n"
		<< "  \"packets\": " << data.packets.size() << ",\n"
		<< "  \"max_depth\": " << data.header.max_depth << ",\n"
		<< "  \"groups\": [\n";
	for (size_t i = 0; i < report.groups.size(); ++i)
	{
		const physics_group_report &group = report.groups[i];
		stream << "    {\"name\": \"" << json_escape(group.name) << "\", \"accepted\": " << (group.accepted ? "true" : "false")
			<< ", \"triangles\": " << group.triangles << ", \"surface_property\": \"" << json_escape(group.surface_property) << "\", \"tags\": [";
		for (size_t tag = 0; tag < group.tags.size(); ++tag)
		{
			stream << (tag == 0 ? "" : ", ") << '"' << json_escape(group.tags[tag]) << '"';
		}
		stream << "]}" << (i + 1u == report.groups.size() ? "\n" : ",\n");
	}
	stream << "  ]\n}\n";
	return stream.good();
}

int run(int argc, char **argv)
{
	arguments args;
	if (!parse_arguments(argc, argv, args))
	{
		std::cerr << "usage: cs2fow_baker --game <cs2-root> --map <name> [--vpk <file>] [--low-priority] [--output <file>] [--vrf <path>] [--debug-obj <file>]\n"
			<< "       cs2fow_baker --list-maps --vpk <file>\n"
			<< "       cs2fow_baker --inspect-bvh8 <file>\n";
		return 2;
	}
	if (!args.inspect_bvh8.empty())
	{
		bvh8_data data;
		std::string error;
		if (!load_bvh8(args.inspect_bvh8, data, error))
		{
			std::cerr << "cs2fow_baker: " << error << '\n';
			return 1;
		}
		const bvh8_header &header = data.header;
		std::cout << "{\"map\":\"" << json_escape(header.map_name) << "\",\"source_kind\":\""
			<< (header.flags == 0 ? "world_physics" : "nested_map_vpk") << "\",\"source_crc32\":\"0x"
			<< std::hex << std::setw(8) << std::setfill('0') << std::nouppercase << header.source_crc32 << std::dec
			<< "\",\"source_size\":" << header.source_size << ",\"triangles\":" << header.triangle_count
			<< ",\"nodes\":" << header.node_count << ",\"packets\":" << header.packet_count
			<< ",\"max_depth\":" << header.max_depth << "}\n";
		return 0;
	}
	if (args.list_maps)
	{
		std::vector<std::string> maps;
		std::string error;
		if (!list_vpk_maps(args.vpk, maps, error))
		{
			std::cerr << "cs2fow_baker: " << error << '\n';
			return 1;
		}
		for (const std::string &map : maps)
		{
			std::cout << map << '\n';
		}
		return 0;
	}
	if (!valid_map_name(args.map))
	{
		std::cerr << "cs2fow_baker: map name is not a safe relative path\n";
		return 2;
	}
	if (args.low_priority)
	{
		std::string priority_error;
		if (!lower_process_priority(priority_error))
		{
			std::cerr << "cs2fow_baker: warning: " << priority_error << '\n';
		}
	}
	const std::filesystem::path vpk = args.vpk.empty() ? args.game / "game" / "csgo" / "maps" / (args.map + ".vpk") : args.vpk;
	const std::string resource = "maps/" + args.map + "/world_physics.vmdl_c";
	map_source source;
	std::string error;
	if (!find_map_source(vpk, args.map, source, error))
	{
		std::cerr << "cs2fow_baker: " << error << '\n';
		return 1;
	}
	const std::filesystem::path temporary = std::filesystem::temp_directory_path() / ("cs2fow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(temporary);
	std::filesystem::path map_vpk = vpk;
	if (source.flags == k_bvh8_flag_nested_map_vpk && !extract_nested_map(source, temporary, map_vpk, error))
	{
		std::cerr << "cs2fow_baker: " << error << '\n';
		std::filesystem::remove_all(temporary);
		return 1;
	}
	vpk_entry physics;
	if (!find_vpk_entry(map_vpk, resource, physics, error))
	{
		std::cerr << "cs2fow_baker: " << error << '\n';
		std::filesystem::remove_all(temporary);
		return 1;
	}
	std::filesystem::path glb;
	if (!export_glb(args, map_vpk, temporary / "physics", glb, error))
	{
		std::cerr << "cs2fow_baker: " << error << '\n';
		std::filesystem::remove_all(temporary);
		return 1;
	}
	std::vector<triangle> triangles;
	import_report report;
	if (!import_physics_glb(glb, triangles, report, error))
	{
		std::cerr << "cs2fow_baker: " << error << '\n';
		std::filesystem::remove_all(temporary);
		return 1;
	}
	std::filesystem::remove_all(temporary);
	if (args.map == "de_ancient" && physics.crc32 == 0x85c89fb4u && (report.raw_triangles != 967742u || report.accepted_triangles != 958598u))
	{
		std::cerr << "cs2fow_baker: Ancient fixture triangle counts do not match (raw=" << report.raw_triangles << ", accepted=" << report.accepted_triangles << ")\n";
		return 1;
	}
	bvh8_data data;
	if (!build_bvh8(triangles, data, error))
	{
		std::cerr << "cs2fow_baker: " << error << '\n';
		return 1;
	}
	data.header.flags = source.flags;
	data.header.source_crc32 = source.metadata.crc32;
	data.header.source_size = source.metadata.size;
	std::copy_n(args.map.c_str(), std::min(args.map.size(), sizeof(data.header.map_name) - 1u), data.header.map_name);
	if (!write_bvh8(args.output, data, error))
	{
		std::cerr << "cs2fow_baker: " << error << '\n';
		return 1;
	}
	bvh8_data verified;
	if (!load_bvh8(args.output, verified, error) || verified.header.payload_crc32 != data.header.payload_crc32)
	{
		std::cerr << "cs2fow_baker: output validation failed: " << error << '\n';
		return 1;
	}
	if (!args.debug_obj.empty() && !write_obj(args.debug_obj, triangles, error))
	{
		std::cerr << "cs2fow_baker: " << error << '\n';
		return 1;
	}
	std::filesystem::path report_path = args.output;
	report_path.replace_extension(".json");
	if (!write_report(report_path, args, source, physics, report, data, error))
	{
		std::cerr << "cs2fow_baker: " << error << '\n';
		return 1;
	}
	std::cout << args.map << ": crc=0x" << std::hex << source.metadata.crc32 << std::dec << ", raw=" << report.raw_triangles
		<< ", accepted=" << report.accepted_triangles << ", nodes=" << data.nodes.size() << ", packets=" << data.packets.size()
		<< ", depth=" << data.header.max_depth << '\n';
	return 0;
}

} // namespace
} // namespace cs2fow

int main(int argc, char **argv)
{
	return cs2fow::run(argc, argv);
}
