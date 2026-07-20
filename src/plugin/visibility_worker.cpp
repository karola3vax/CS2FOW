#include "visibility_worker.h"

// Consumes the newest copied snapshot, casts bounded BVH8 rays, applies reveal
// hold, updates worker-only caches/statistics, and publishes one complete result.
// No function in this file dereferences a live engine object.

#include <algorithm>
#include <system_error>

namespace cs2fow
{

visibility_worker::~visibility_worker()
{
	stop();
}

bool visibility_worker::start(const bvh8_data *data)
{
	stop();
	data_ = data;
	stopping_ = false;
	for (auto &recipient : cached_packets_)
	{
		for (auto &target : recipient)
		{
			target.fill(k_invalid_ref);
		}
	}
	for (auto &recipient : revealed_until_)
	{
		recipient.fill(std::chrono::steady_clock::time_point {});
	}
	{
		std::lock_guard lock(stats_mutex_);
		stats_ = {};
	}
	try
	{
		thread_ = std::thread(&visibility_worker::run, this);
	}
	catch (const std::system_error &)
	{
		stopping_.store(true);
		data_ = nullptr;
		return false;
	}
	return true;
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
	std::atomic_store(&published_, std::shared_ptr<const visibility_result> {});
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
	return std::atomic_load(&published_);
}

worker_stats visibility_worker::stats() const
{
	std::lock_guard lock(stats_mutex_);
	return stats_;
}

visibility_player visibility_worker::sample_player(const player_state &player)
{
	return {player.eye, player.origin, player.mins, player.maxs, player.eye_yaw_degrees, player.rtt_seconds,
		player.movement_buttons, player.muzzle_class, player.body_points, player.body_point_count};
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
			condition_.wait(lock, [&] { return stopping_.load() || pending_.has_value(); });
			if (stopping_.load())
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
		result->captured = current.captured;
		result->filter_teammates = current.filter_teammates;
		result->smoke_enabled = current.smoke_enabled;
		result->smoke_available = current.smoke_available;
		result->smoke_count = current.smokes == nullptr ? 0u : static_cast<uint32_t>(current.smokes->volumes.size());
		result->he_clearance_count = current.smokes == nullptr ? 0u : current.smokes->he_clearance_count;
		std::copy(std::begin(current.players), std::end(current.players), std::begin(result->players));
		const float smoke_age_advance = std::max(0.0f,
			std::chrono::duration<float>(started - current.captured).count());
		std::array<visibility_origin_points, k_max_players> recipient_origins {};
		std::array<visibility_target_points, k_max_players> target_points {};
		for (uint32_t recipient = 0; recipient < k_max_players; ++recipient)
		{
			if (stopping_.load()) return;
			if (current.players[recipient].valid)
			{
				recipient_origins[recipient] = visibility_origins(*data_, sample_player(current.players[recipient]), tuning);
				target_points[recipient] = visibility_targets(sample_player(current.players[recipient]));
			}
		}
		for (uint32_t recipient = 0; recipient < k_max_players; ++recipient)
		{
			if (stopping_.load()) return;
			for (uint32_t target = 0; target < k_max_players; ++target)
			{
				if (stopping_.load()) return;
				result->visible[recipient][target] = true;
				const player_state &from = current.players[recipient];
				const player_state &to = current.players[target];
				if (!visibility_pair_enabled(recipient, target, from, to, current.filter_teammates))
				{
					continue;
				}
				++result->evaluated_pairs;
				bool blocked = true;
				const auto &ray_origins = recipient_origins[recipient];
				const auto &ray_targets = target_points[target];
				uint32_t ray = 0;
				for (uint32_t origin_index = 0; origin_index < ray_origins.count; ++origin_index)
				{
					const vec3 &origin = ray_origins.points[origin_index];
					for (uint32_t point_index = 0; point_index < ray_targets.count; ++point_index)
					{
						const ray_hit hit = segment_blocked(*data_, origin, ray_targets.points[point_index], cached_packets_[recipient][target][ray]);
						cached_packets_[recipient][target][ray++] = hit.packet_index;
						if (!hit.blocked && (!current.smoke_enabled || !current.smoke_available || current.smokes == nullptr
							|| !smoke_line_blocked(*current.smokes, origin, ray_targets.points[point_index], smoke_age_advance, data_)))
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
					revealed_until_[recipient][target] = now + std::chrono::milliseconds(hold_ms);
				}
				const bool visible = !blocked || now < revealed_until_[recipient][target];
				result->visible[recipient][target] = visible;
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
		std::atomic_store(&published_, std::shared_ptr<const visibility_result> {std::move(result)});
	}
}

} // namespace cs2fow
