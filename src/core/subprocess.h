#pragma once

// Declares the small cross-platform process runner used by the baker and its
// controller. Arguments stay separate from the executable; callers receive
// timeout/cancellation/exit state and captured output without a shell string.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace cs2fow
{

inline constexpr size_t k_process_output_tail_bytes = 8u * 1024u;

struct process_result
{
	int exit_code {-1};
	bool cancelled {};
	bool timed_out {};
	std::string output_tail;
};

enum class posix_process_group
{
	isolated,
	inherited
};

bool run_process(const std::filesystem::path &executable, const std::vector<std::filesystem::path> &arguments,
	std::chrono::milliseconds timeout, const std::atomic_bool *cancel, bool low_priority,
	posix_process_group process_group, process_result &result, std::string &error);
bool lower_process_priority(std::string &error);

} // namespace cs2fow
