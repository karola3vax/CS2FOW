#include "automatic_baker.h"

// Starts, monitors, cancels, and joins the low-priority baker task. The task
// outputs a completion record protected by this object's mutex and never reads
// live CS2 objects or activates a bake by itself.

#include "subprocess.h"

#include <vector>

namespace cs2fow
{
namespace
{

constexpr auto k_auto_bake_timeout = std::chrono::minutes(10);

} // namespace

automatic_baker::~automatic_baker()
{
	stop();
}

void automatic_baker::start(bake_request request)
{
	stop();
	cancel_.store(false);
	{
		std::lock_guard lock(mutex_);
		running_ = true;
		map_ = request.map;
		started_ = std::chrono::steady_clock::now();
	}
	thread_ = std::thread(&automatic_baker::run, this, std::move(request));
}

void automatic_baker::stop()
{
	cancel_.store(true);
	if (thread_.joinable())
	{
		thread_.join();
	}
	std::lock_guard lock(mutex_);
	running_ = false;
	map_.clear();
	completion_.reset();
}

bool automatic_baker::poll(bake_completion &completion)
{
	std::lock_guard lock(mutex_);
	if (!completion_)
	{
		return false;
	}
	completion = std::move(*completion_);
	completion_.reset();
	return true;
}

bool automatic_baker::status(std::string &map, double &elapsed_ms) const
{
	std::lock_guard lock(mutex_);
	if (!running_)
	{
		return false;
	}
	map = map_;
	elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_).count();
	return true;
}

void automatic_baker::run(bake_request request)
{
	bake_completion completion;
	completion.request = request;
	process_result process;
	const std::vector<std::string> arguments {
		"--game", request.game.string(),
		"--map", request.map,
		"--vpk", request.source.vpk.string(),
		"--output", request.output.string(),
		"--vrf", request.vrf.string(),
		"--low-priority"
	};
	if (!run_process(request.baker, arguments, k_auto_bake_timeout, &cancel_, true, process, completion.error))
	{
		finish(std::move(completion));
		return;
	}
	if (process.cancelled)
	{
		completion.cancelled = true;
		finish(std::move(completion));
		return;
	}
	if (process.timed_out)
	{
		completion.error = "automatic baker timed out";
		if (!process.output_tail.empty()) completion.error += "\n" + process.output_tail;
		finish(std::move(completion));
		return;
	}
	if (process.exit_code != 0)
	{
		completion.error = "automatic baker exited with code " + std::to_string(process.exit_code);
		if (!process.output_tail.empty()) completion.error += "\n" + process.output_tail;
		finish(std::move(completion));
		return;
	}
	if (!load_bvh8(request.output, completion.data, completion.error))
	{
		finish(std::move(completion));
		return;
	}
	const bvh8_header &header = completion.data.header;
	if (request.map != header.map_name || header.flags != request.source.flags
		|| header.source_crc32 != request.source.metadata.crc32 || header.source_size != request.source.metadata.size)
	{
		completion.data = {};
		completion.error = "automatic bake does not match requested map source";
		finish(std::move(completion));
		return;
	}
	completion.success = true;
	finish(std::move(completion));
}

void automatic_baker::finish(bake_completion completion)
{
	std::lock_guard lock(mutex_);
	running_ = false;
	completion_ = std::move(completion);
}

} // namespace cs2fow
