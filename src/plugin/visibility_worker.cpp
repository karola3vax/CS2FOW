#include "visibility_worker.h"

// Consumes the newest copied snapshot, samples exact hitbox capsules, applies
// reveal hold, updates worker-only caches/statistics, and publishes one result.
// No function in this file dereferences a live engine object.

#include <algorithm>
#include <system_error>

namespace cs2fow
{
namespace
{

constexpr auto k_worker_budget = std::chrono::milliseconds(75);

} // namespace

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
	for (auto &recipient : cached_occluders_)
	{
		for (auto &target : recipient)
		{
			target.fill(capsule_occluder_cache {});
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
		for (auto &row : result->visible) std::fill(std::begin(row), std::end(row), true);
		const float smoke_age_advance = std::max(0.0f,
			std::chrono::duration<float>(started - current.captured).count());
		const smoke_snapshot *active_smokes = current.smoke_enabled && current.smoke_available
			&& current.smokes != nullptr && !current.smokes->volumes.empty() ? current.smokes.get() : nullptr;
		const auto deadline = started + k_worker_budget;
		std::array<visibility_origin_points, k_max_players> recipient_origins {};
		for (uint32_t recipient = 0; recipient < k_max_players; ++recipient)
		{
			if (stopping_.load()) return;
			if (current.players[recipient].valid)
			{
				recipient_origins[recipient] = visibility_origins(*data_, visibility_sample(current.players[recipient]), tuning);
			}
		}
		bool stop_evaluating = false;
		for (uint32_t recipient = 0; recipient < k_max_players; ++recipient)
		{
			if (stopping_.load()) return;
			for (uint32_t target = 0; target < k_max_players; ++target)
			{
				if (stopping_.load()) return;
				const player_state &from = current.players[recipient];
				const player_state &to = current.players[target];
				if (!visibility_pair_enabled(recipient, target, from, to, current.filter_teammates))
				{
					continue;
				}
				if (std::chrono::steady_clock::now() >= deadline)
				{
					result->budget_exhausted = true;
					stop_evaluating = true;
					break;
				}
				++result->evaluated_pairs;
				bool blocked = to.capsule_count == k_visibility_capsule_count;
				const auto &ray_origins = recipient_origins[recipient];
				const visibility_player target_sample = visibility_sample(to);
				vec3 muzzle;
				const bool has_muzzle = visibility_muzzle_point(target_sample, muzzle);
				for (uint32_t origin_index = 0; blocked && origin_index < ray_origins.count; ++origin_index)
				{
					const vec3 &origin = ray_origins.points[origin_index];
					uint32_t &cached_packet = cached_packets_[recipient][target][origin_index];
					capsule_occluder_cache &cached_occluders = cached_occluders_[recipient][target][origin_index];
					capsule_query_stats query_stats;
					const capsule_query_result capsule_result = capsule_visible_from_origin(*data_, origin,
						std::span<const visibility_capsule>(to.capsules), active_smokes, smoke_age_advance,
						deadline, &stopping_, &query_stats, &cached_occluders);
					result->sampled_pixels += query_stats.sampled_pixels;
					result->traced_rays += query_stats.traced_rays;
					result->visited_nodes += query_stats.visited_nodes;
					result->rasterized_triangles += query_stats.rasterized_triangles;
					if (stopping_.load()) return;
					if (capsule_result != capsule_query_result::blocked)
					{
						blocked = false;
						if (capsule_result == capsule_query_result::indeterminate
							&& std::chrono::steady_clock::now() >= deadline)
						{
							result->budget_exhausted = true;
							stop_evaluating = true;
						}
						break;
					}
					if (has_muzzle)
					{
						const ray_hit hit = segment_blocked(*data_, origin, muzzle, cached_packet);
						cached_packet = hit.packet_index;
						++result->traced_rays;
						if (!hit.blocked && (active_smokes == nullptr
							|| !smoke_line_blocked(*active_smokes, origin, muzzle, smoke_age_advance, data_)))
						{
							blocked = false;
							break;
						}
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
				if (stop_evaluating) break;
			}
			if (stop_evaluating) break;
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
			stats_.sampled_pixels = result->sampled_pixels;
			stats_.traced_rays = result->traced_rays;
			stats_.visited_nodes = result->visited_nodes;
			stats_.rasterized_triangles = result->rasterized_triangles;
			if (result->budget_exhausted) ++stats_.budget_exhaustions;
		}
		std::atomic_store(&published_, std::shared_ptr<const visibility_result> {std::move(result)});
	}
}

} // namespace cs2fow
