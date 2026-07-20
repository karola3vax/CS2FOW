#include "test_suites.h"
#include "subprocess.h"

// Tiny test entry point: creates a temporary workspace, runs the map/BVH and
// visibility/transmit suites, prints one benchmark, and removes test files.

#include "visibility_sampling.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <unistd.h>
#endif

using namespace cs2fow;

namespace
{

uint64_t process_id()
{
#if defined(_WIN32)
	return GetCurrentProcessId();
#else
	return static_cast<uint64_t>(getpid());
#endif
}

} // namespace

int run_tests(std::span<const std::filesystem::path> argv)
{
	if (argv.size() == 3 && argv[1] == "--process-probe")
	{
		std::cout << "probe stdout\n" << std::flush;
		std::cerr << "probe stderr\n" << std::flush;
		return argv[2] == "space [test]" ? 23 : 24;
	}
	if (argv.size() == 3 && argv[1] == "--process-empty")
	{
		return argv[2].empty() ? 26 : 27;
	}
	if (argv.size() == 3 && argv[1] == "--process-touch")
	{
		std::ofstream(argv[2]) << "touched";
		return 0;
	}
	if (argv.size() == 2 && argv[1] == "--process-flood")
	{
		std::cout << "HEAD-MARKER\n" << std::string(k_process_output_tail_bytes + 1024u, 'x') << "\nTAIL-MARKER\n";
		return 25;
	}
	if (argv.size() == 2 && argv[1] == "--process-sleep")
	{
		std::cout << "sleep probe child-pid=" << process_id() << '\n' << std::flush;
		std::this_thread::sleep_for(std::chrono::seconds(10));
		return 0;
	}
	if (argv.size() == 3 && argv[1] == "--process-sleep-file")
	{
		std::ofstream(argv[2]) << process_id();
		std::this_thread::sleep_for(std::chrono::seconds(10));
		return 0;
	}
	if (argv.size() == 3 && argv[1] == "--process-nested")
	{
		std::cout << "parent-pid=" << process_id() << '\n' << std::flush;
		process_result result;
		std::string error;
		return run_process(std::filesystem::absolute(argv[0]), {"--process-sleep-file", argv[2]}, std::chrono::seconds(10),
			nullptr, false, posix_process_group::inherited, result, error) ? result.exit_code : 1;
	}
	assert(cpu_supports_avx());
	const std::filesystem::path directory = std::filesystem::temp_directory_path()
		/ ("cs2fow-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory);
	run_map_and_bvh_tests(directory, std::filesystem::absolute(argv[0]));
	run_visibility_and_transmit_tests();
	std::filesystem::remove_all(directory);
	std::cout << "cs2fow_tests: all checks passed\n";

	if (argv.size() != 2)
	{
		return 0;
	}
	bvh8_data data;
	std::string error;
	assert(load_bvh8(argv[1], data, error));
	std::string label = argv[1].stem().string();
	if (label.empty()) label = "BVH8";
	std::mt19937 random(0xb8f8u);
	std::uniform_real_distribution<float> x(data.header.world_min[0], data.header.world_max[0]);
	std::uniform_real_distribution<float> y(data.header.world_min[1], data.header.world_max[1]);
	std::uniform_real_distribution<float> z(data.header.world_min[2], data.header.world_max[2]);
	constexpr size_t k_benchmark_rays = 512u * k_visibility_ray_count_max;
	std::vector<std::pair<vec3, vec3>> rays(k_benchmark_rays);
	std::vector<uint32_t> cache(k_benchmark_rays, k_invalid_ref);
	for (auto &ray : rays)
	{
		ray = {{x(random), y(random), z(random)}, {x(random), y(random), z(random)}};
	}
	for (size_t index = 0; index < rays.size(); ++index)
	{
		cache[index] = segment_blocked(data, rays[index].first, rays[index].second).packet_index;
	}
	std::array<double, 20> timings {};
	uint64_t blocked = 0;
	for (double &timing : timings)
	{
		const auto start = std::chrono::steady_clock::now();
		for (size_t index = 0; index < rays.size(); ++index)
		{
			const ray_hit hit = segment_blocked(data, rays[index].first, rays[index].second, cache[index]);
			cache[index] = hit.packet_index;
			blocked += hit.blocked;
		}
		timing = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}
	std::sort(timings.begin(), timings.end());
	double average = 0;
	for (double timing : timings) average += timing;
	average /= timings.size();
	std::cout << label << " traversal: average=" << average << "ms p99=" << timings.back()
		<< "ms rays=" << k_benchmark_rays << " blocked=" << blocked << '\n';
	assert(average < 25.0);
	assert(run_worker_benchmark(data, label) < 10.0);
	return 0;
}

template <typename character>
int run_main(int argc, character **argv)
{
	std::vector<std::filesystem::path> arguments;
	arguments.reserve(static_cast<size_t>(argc));
	for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
	return run_tests(arguments);
}

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv) { return run_main(argc, argv); }
#else
int main(int argc, char **argv) { return run_main(argc, argv); }
#endif
