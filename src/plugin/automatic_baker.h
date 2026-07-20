#pragma once

// Runs one external map baker away from the game thread. It receives copied
// map paths/metadata and reports one validated completion; stop/cancel joins the
// task before its request state can be replaced.

#include "bvh8.h"
#include "map_source.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace cs2fow
{

struct bake_request
{
	std::string map;
	map_source source;
	std::filesystem::path game;
	std::filesystem::path output;
	std::filesystem::path baker;
	std::filesystem::path vrf;
};

struct bake_completion
{
	bake_request request;
	bvh8_data data;
	std::string error;
	bool success {};
	bool cancelled {};
};

class automatic_baker
{
public:
	~automatic_baker();
	bool start(bake_request request);
	void stop();
	bool poll(bake_completion &completion);
	bool status(std::string &map, double &elapsed_ms) const;

private:
	void run(bake_request request);
	void finish(bake_completion completion);

	std::atomic_bool cancel_ {false};
	mutable std::mutex mutex_;
	std::thread thread_;
	std::optional<bake_completion> completion_;
	std::chrono::steady_clock::time_point started_ {};
	std::string map_;
	bool running_ {};
};

} // namespace cs2fow
