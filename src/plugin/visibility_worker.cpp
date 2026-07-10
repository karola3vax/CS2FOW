#include "visibility_worker.h"

#include <algorithm>

namespace cs2fow
{

visibility_worker::~visibility_worker()
{
	stop();
}

void visibility_worker::start(const bvh8_data *data)
{
	stop();
	data_ = data;
	stopping_ = false;
	for (auto &observer : cached_packets_)
	{
		for (auto &target : observer)
		{
			target.fill(k_invalid_ref);
		}
	}
	for (auto &observer : revealed_until_)
	{
		observer.fill(std::chrono::steady_clock::time_point {});
	}
	{
		std::lock_guard lock(stats_mutex_);
		stats_ = {};
	}
	thread_ = std::thread(&visibility_worker::run, this);
}

void visibility_worker::stop()
{
	{
		std::lock_guard lock(mutex_);
		stopping_ = true;
		pending_.reset();
	}
	condition_.notify_one();
	if (thread_.joinable())
	{
		thread_.join();
	}
	published_.store({});
	data_ = nullptr;
}

void visibility_worker::submit(visibility_snapshot value, uint32_t hold_ms, visibility_tuning tuning)
{
	{
		std::lock_guard lock(mutex_);
		pending_ = std::move(value);
		hold_ms_ = hold_ms;
		tuning_ = tuning;
	}
	condition_.notify_one();
}

std::shared_ptr<const visibility_result> visibility_worker::result() const
{
	return published_.load();
}

worker_stats visibility_worker::stats() const
{
	std::lock_guard lock(stats_mutex_);
	return stats_;
}

visibility_player visibility_worker::sample_player(const player_state &player)
{
	return {player.eye, player.origin, player.velocity, player.mins, player.maxs, player.eye_yaw_degrees, player.rtt_seconds, player.muzzle_class};
}

void visibility_worker::run()
{
	for (;;)
	{
		visibility_snapshot current;
		uint32_t hold_ms = 0;
		visibility_tuning tuning;
		{
			std::unique_lock lock(mutex_);
			condition_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
			if (stopping_)
			{
				return;
			}
			current = std::move(*pending_);
			pending_.reset();
			hold_ms = hold_ms_;
			tuning = tuning_;
		}

		const auto started = std::chrono::steady_clock::now();
		auto result = std::make_shared<visibility_result>();
		result->sequence = current.sequence;
		std::copy(std::begin(current.players), std::end(current.players), std::begin(result->players));
		std::array<float, k_max_players> observer_lookahead {};
		std::array<std::array<vec3, k_visibility_origin_count>, k_max_players> observer_origins {};
		for (uint32_t observer = 0; observer < k_max_players; ++observer)
		{
			if (current.players[observer].valid)
			{
				observer_lookahead[observer] = visibility_effective_lookahead_seconds(current.players[observer].rtt_seconds, tuning);
				observer_origins[observer] = visibility_origins(*data_, sample_player(current.players[observer]), tuning, observer_lookahead[observer]);
			}
		}
		for (uint32_t observer = 0; observer < k_max_players; ++observer)
		{
			for (uint32_t target = 0; target < k_max_players; ++target)
			{
				result->visible[observer][target] = true;
				const player_state &from = current.players[observer];
				const player_state &to = current.players[target];
				if (!from.valid || !to.valid || observer == target || from.team == to.team)
				{
					continue;
				}
				++result->evaluated_pairs;
				bool blocked = true;
				const auto &ray_origins = observer_origins[observer];
				const auto ray_targets = visibility_targets(*data_, sample_player(to), tuning, observer_lookahead[observer]);
				uint32_t ray = 0;
				for (const vec3 &origin : ray_origins)
				{
					for (uint32_t point_index = 0; point_index < ray_targets.count; ++point_index)
					{
						const ray_hit hit = segment_blocked(*data_, origin, ray_targets.points[point_index], cached_packets_[observer][target][ray]);
						cached_packets_[observer][target][ray++] = hit.packet_index;
						if (!hit.blocked)
						{
							blocked = false;
							break;
						}
					}
					if (!blocked)
					{
						break;
					}
				}
				const auto now = std::chrono::steady_clock::now();
				if (!blocked)
				{
					revealed_until_[observer][target] = now + std::chrono::milliseconds(hold_ms);
				}
				const bool visible = !blocked || now < revealed_until_[observer][target];
				result->visible[observer][target] = visible;
				visible ? ++result->visible_pairs : ++result->hidden_pairs;
			}
		}
		result->completed = std::chrono::steady_clock::now();
		result->worker_ms = std::chrono::duration<double, std::milli>(result->completed - started).count();
		{
			std::lock_guard lock(stats_mutex_);
			stats_.latest_ms = result->worker_ms;
			stats_.maximum_ms = std::max(stats_.maximum_ms, result->worker_ms);
			stats_.average_ms = (stats_.average_ms * static_cast<double>(stats_.cycles) + result->worker_ms) / static_cast<double>(stats_.cycles + 1u);
			++stats_.cycles;
			stats_.evaluated_pairs = result->evaluated_pairs;
			stats_.visible_pairs = result->visible_pairs;
			stats_.hidden_pairs = result->hidden_pairs;
		}
		published_.store(std::move(result));
	}
}

} // namespace cs2fow
