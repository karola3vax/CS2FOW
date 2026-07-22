#include "visibility_worker.h"

// Consumes the newest copied snapshot, distributes complete recipient rows,
// applies reveal hold, and publishes one coherent immutable result. No code in
// this file dereferences a live engine object.

#include <algorithm>
#include <system_error>

namespace cs2fow
{
namespace
{

constexpr auto k_worker_budget = std::chrono::milliseconds(75);
constexpr uint32_t k_worker_count_max = 4;
constexpr uint32_t k_visibility_probe_capsule = 4; // chest

struct job_totals
{
	double active_ms {};
	uint32_t evaluated_pairs {};
	uint32_t visible_pairs {};
	uint32_t hidden_pairs {};
	uint32_t sampled_pixels {};
	uint32_t traced_rays {};
	uint32_t visibility_probe_rays {};
	uint32_t visibility_probe_hits {};
	uint32_t hold_reuses {};
	uint32_t visited_nodes {};
	uint32_t rasterized_triangles {};
	uint32_t occluder_cache_hits {};
	uint32_t occluder_cache_misses {};
	uint32_t moc_render_calls {};
	uint32_t moc_rect_tests {};
	uint32_t rebuilt_proofs {};
	uint32_t rebuilt_proof_leaves {};
	uint32_t max_rebuilt_proof_leaves {};
	uint32_t cache_saturations {};
	uint32_t cache_compaction_trials {};
	uint32_t cache_compactions {};
	uint32_t cache_compaction_leaves_saved {};
	uint32_t uncached_blocked {};
};

vec3 capsule_midpoint(const visibility_capsule &capsule)
{
	return {(capsule.start.x + capsule.end.x) * 0.5f,
		(capsule.start.y + capsule.end.y) * 0.5f,
		(capsule.start.z + capsule.end.z) * 0.5f};
}

} // namespace

struct visibility_worker::job
{
	visibility_snapshot snapshot;
	uint32_t hold_ms {};
	visibility_tuning tuning;
	std::chrono::steady_clock::time_point started;
	std::chrono::steady_clock::time_point deadline;
	float smoke_age_advance {};
	uint32_t recipient_start {};
	uint32_t target_start {};
	std::array<visibility_origin_points, k_max_players> recipient_origins {};
	std::shared_ptr<visibility_result> result;
	std::atomic<uint32_t> next_recipient {};
	std::atomic_bool budget_exhausted {};
	std::array<job_totals, k_worker_count_max> totals {};
};

visibility_worker::~visibility_worker()
{
	stop();
}

bool visibility_worker::start(const bvh8_data *data, uint32_t thread_count)
{
	stop();
	data_ = data;
	thread_count_ = std::clamp(thread_count, 1u, k_worker_count_max);
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
		stats_.thread_count = thread_count_;
		recent_worker_ms_ = {};
		recent_worker_count_ = 0;
		recent_worker_next_ = 0;
	}
	try
	{
		threads_.reserve(thread_count_);
		threads_.emplace_back(&visibility_worker::run_coordinator, this);
		for (uint32_t index = 1; index < thread_count_; ++index)
		{
			threads_.emplace_back(&visibility_worker::run_helper, this, index);
		}
	}
	catch (const std::system_error &)
	{
		stopping_ = true;
		condition_.notify_all();
		for (std::thread &thread : threads_)
		{
			if (thread.joinable()) thread.join();
		}
		threads_.clear();
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
	condition_.notify_all();
	for (std::thread &thread : threads_)
	{
		if (thread.joinable()) thread.join();
	}
	threads_.clear();
	{
		std::lock_guard lock(mutex_);
		active_job_.reset();
		workers_done_ = 0;
	}
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
	published_.store({});
#else
	std::atomic_store(&published_, std::shared_ptr<const visibility_result> {});
#endif
	data_ = nullptr;
}

void visibility_worker::submit(visibility_snapshot value, uint32_t hold_ms, visibility_tuning tuning)
{
	{
		std::lock_guard lock(mutex_);
		if (stopping_.load()) return;
		pending_ = std::move(value);
		hold_ms_ = hold_ms;
		tuning_ = tuning;
	}
	condition_.notify_all();
}

std::shared_ptr<const visibility_result> visibility_worker::result() const
{
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
	return published_.load();
#else
	return std::atomic_load(&published_);
#endif
}

worker_stats visibility_worker::stats() const
{
	std::lock_guard lock(stats_mutex_);
	worker_stats result = stats_;
	if (recent_worker_count_ == 0) return result;
	std::array<double, 128> sorted = recent_worker_ms_;
	std::sort(sorted.begin(), sorted.begin() + recent_worker_count_);
	const auto percentile = [&](double value)
	{
		const size_t index = static_cast<size_t>(std::ceil(value * static_cast<double>(recent_worker_count_))) - 1u;
		return sorted[std::min(index, static_cast<size_t>(recent_worker_count_ - 1u))];
	};
	result.recent_p95_ms = percentile(0.95);
	result.recent_p99_ms = percentile(0.99);
	return result;
}

void visibility_worker::run_coordinator()
{
	for (;;)
	{
		visibility_snapshot snapshot;
		uint32_t hold_ms = 0;
		visibility_tuning tuning;
		{
			std::unique_lock lock(mutex_);
			condition_.wait(lock, [&] { return stopping_.load() || pending_.has_value(); });
			if (stopping_.load()) return;
			snapshot = std::move(*pending_);
			pending_.reset();
			hold_ms = hold_ms_;
			tuning = tuning_;
		}

		auto current = std::make_shared<job>();
		current->snapshot = std::move(snapshot);
		current->hold_ms = hold_ms;
		current->tuning = tuning;
		current->started = std::chrono::steady_clock::now();
		current->deadline = current->started + k_worker_budget;
		current->smoke_age_advance = std::max(0.0f,
			std::chrono::duration<float>(current->started - current->snapshot.captured).count());
		current->recipient_start = static_cast<uint32_t>(current->snapshot.sequence % k_max_players);
		current->target_start = static_cast<uint32_t>((current->snapshot.sequence * 17u) % k_max_players);
		current->result = std::make_shared<visibility_result>();
		visibility_result &result = *current->result;
		result.sequence = current->snapshot.sequence;
		result.captured = current->snapshot.captured;
		result.filter_teammates = current->snapshot.filter_teammates;
		result.smoke_enabled = current->snapshot.smoke_enabled;
		result.smoke_available = current->snapshot.smoke_available;
		result.smoke_count = current->snapshot.smokes == nullptr ? 0u
			: static_cast<uint32_t>(current->snapshot.smokes->volumes.size());
		result.he_clearance_count = current->snapshot.smokes == nullptr ? 0u
			: current->snapshot.smokes->he_clearance_count;
		std::copy(std::begin(current->snapshot.players), std::end(current->snapshot.players), std::begin(result.players));
		for (auto &row : result.visible) std::fill(std::begin(row), std::end(row), true);
		for (uint32_t recipient = 0; recipient < k_max_players; ++recipient)
		{
			if (stopping_.load()) return;
			if (current->snapshot.players[recipient].valid)
			{
				current->recipient_origins[recipient] = visibility_origins(
					*data_, visibility_sample(current->snapshot.players[recipient]), tuning);
			}
		}

		{
			std::lock_guard lock(mutex_);
			if (stopping_.load()) return;
			active_job_ = current;
			workers_done_ = 0;
			++job_generation_;
		}
		condition_.notify_all();
		process(*current, 0);
		{
			std::unique_lock lock(mutex_);
			++workers_done_;
			condition_.notify_all();
			condition_.wait(lock, [&] { return stopping_.load() || workers_done_ == thread_count_; });
			if (stopping_.load()) return;
			active_job_.reset();
		}
		publish(*current);
	}
}

void visibility_worker::run_helper(uint32_t worker_index)
{
	uint64_t observed_generation = 0;
	for (;;)
	{
		std::shared_ptr<job> current;
		{
			std::unique_lock lock(mutex_);
			condition_.wait(lock, [&]
			{
				return stopping_.load() || (active_job_ != nullptr && job_generation_ != observed_generation);
			});
			if (stopping_.load()) return;
			current = active_job_;
			observed_generation = job_generation_;
		}
		process(*current, worker_index);
		{
			std::lock_guard lock(mutex_);
			++workers_done_;
		}
		condition_.notify_all();
	}
}

void visibility_worker::process(job &current, uint32_t worker_index)
{
	const auto active_started = std::chrono::steady_clock::now();
	job_totals totals;
	const smoke_snapshot *active_smokes = current.snapshot.smoke_enabled && current.snapshot.smoke_available
		&& current.snapshot.smokes != nullptr && !current.snapshot.smokes->volumes.empty()
		? current.snapshot.smokes.get() : nullptr;
	for (;;)
	{
		if (stopping_.load()) return;
		const uint32_t ticket = current.next_recipient.fetch_add(1u);
		if (ticket >= k_max_players) break;
		const uint32_t recipient = (current.recipient_start + ticket) % k_max_players;
		const player_state &from = current.snapshot.players[recipient];
		if (!from.valid) continue;
		const visibility_origin_points &ray_origins = current.recipient_origins[recipient];
		for (uint32_t target_ticket = 0; target_ticket < k_max_players; ++target_ticket)
		{
			if (stopping_.load()) return;
			const uint32_t target = (current.target_start + target_ticket) % k_max_players;
			const player_state &to = current.snapshot.players[target];
			if (!visibility_pair_enabled(recipient, target, from, to, current.snapshot.filter_teammates)) continue;
			const auto pair_started = std::chrono::steady_clock::now();
			if (pair_started >= current.deadline)
			{
				current.budget_exhausted = true;
				break;
			}
			++totals.evaluated_pairs;
			if (pair_started < revealed_until_[recipient][target])
			{
				current.result->visible[recipient][target] = true;
				++totals.visible_pairs;
				++totals.hold_reuses;
				continue;
			}
			bool blocked = to.capsule_count == k_visibility_capsule_count;
			const visibility_player target_sample = visibility_sample(to);
			vec3 muzzle;
			const bool has_muzzle = visibility_muzzle_point(target_sample, muzzle);
			for (uint32_t origin_index = 0; blocked && origin_index < ray_origins.count; ++origin_index)
			{
				const vec3 &origin = ray_origins.points[origin_index];
				uint32_t &cached_packet = cached_packets_[recipient][target][origin_index];
				const vec3 probe = capsule_midpoint(to.capsules[k_visibility_probe_capsule]);
				const ray_hit probe_hit = segment_blocked(*data_, origin, probe, cached_packet);
				cached_packet = probe_hit.packet_index;
				++totals.traced_rays;
				++totals.visibility_probe_rays;
				if (!probe_hit.blocked && (active_smokes == nullptr || !smoke_line_blocked(
					*active_smokes, origin, probe, current.smoke_age_advance, data_)))
				{
					blocked = false;
					++totals.visibility_probe_hits;
				}
				if (!blocked) break;
				capsule_occluder_cache &cached_occluders = cached_occluders_[recipient][target][origin_index];
				capsule_query_stats query_stats;
				const capsule_query_result capsule_result = capsule_visible_from_origin(*data_, origin,
					std::span<const visibility_capsule>(to.capsules), active_smokes, current.smoke_age_advance,
					current.deadline, &stopping_, &query_stats, &cached_occluders);
				totals.sampled_pixels += query_stats.sampled_pixels;
				totals.traced_rays += query_stats.traced_rays;
				totals.visited_nodes += query_stats.visited_nodes;
				totals.rasterized_triangles += query_stats.rasterized_triangles;
				totals.occluder_cache_hits += query_stats.occluder_cache_hits;
				totals.occluder_cache_misses += query_stats.occluder_cache_misses;
				totals.moc_render_calls += query_stats.moc_render_calls;
				totals.moc_rect_tests += query_stats.moc_rect_tests;
				totals.rebuilt_proofs += query_stats.rebuilt_proofs;
				totals.rebuilt_proof_leaves += query_stats.rebuilt_proof_leaves;
				totals.max_rebuilt_proof_leaves = std::max(
					totals.max_rebuilt_proof_leaves, query_stats.max_rebuilt_proof_leaves);
				totals.cache_saturations += query_stats.cache_saturations;
				totals.cache_compaction_trials += query_stats.cache_compaction_trials;
				totals.cache_compactions += query_stats.cache_compactions;
				totals.cache_compaction_leaves_saved += query_stats.cache_compaction_leaves_saved;
				totals.uncached_blocked += query_stats.uncached_blocked;
				if (stopping_.load()) return;
				if (capsule_result != capsule_query_result::blocked)
				{
					blocked = false;
					if (capsule_result == capsule_query_result::indeterminate
						&& std::chrono::steady_clock::now() >= current.deadline)
					{
						current.budget_exhausted = true;
					}
					break;
				}
				if (has_muzzle)
				{
					const ray_hit hit = segment_blocked(*data_, origin, muzzle, cached_packet);
					cached_packet = hit.packet_index;
					++totals.traced_rays;
					if (!hit.blocked && (active_smokes == nullptr || !smoke_line_blocked(
						*active_smokes, origin, muzzle, current.smoke_age_advance, data_)))
					{
						blocked = false;
						break;
					}
				}
			}
			const auto now = std::chrono::steady_clock::now();
			if (!blocked)
			{
				revealed_until_[recipient][target] = now + std::chrono::milliseconds(current.hold_ms);
			}
			const bool visible = !blocked || now < revealed_until_[recipient][target];
			current.result->visible[recipient][target] = visible;
			visible ? ++totals.visible_pairs : ++totals.hidden_pairs;
			if (current.budget_exhausted.load()) break;
		}
		if (current.budget_exhausted.load()) break;
	}
	totals.active_ms = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - active_started).count();
	current.totals[worker_index] = totals;
}

void visibility_worker::publish(job &current)
{
	if (stopping_.load()) return;
	visibility_result &result = *current.result;
	for (uint32_t index = 0; index < thread_count_; ++index)
	{
		const job_totals &totals = current.totals[index];
		result.worker_active_ms += totals.active_ms;
		result.evaluated_pairs += totals.evaluated_pairs;
		result.visible_pairs += totals.visible_pairs;
		result.hidden_pairs += totals.hidden_pairs;
		result.sampled_pixels += totals.sampled_pixels;
		result.traced_rays += totals.traced_rays;
		result.visibility_probe_rays += totals.visibility_probe_rays;
		result.visibility_probe_hits += totals.visibility_probe_hits;
		result.hold_reuses += totals.hold_reuses;
		result.visited_nodes += totals.visited_nodes;
		result.rasterized_triangles += totals.rasterized_triangles;
		result.occluder_cache_hits += totals.occluder_cache_hits;
		result.occluder_cache_misses += totals.occluder_cache_misses;
		result.moc_render_calls += totals.moc_render_calls;
		result.moc_rect_tests += totals.moc_rect_tests;
		result.rebuilt_proofs += totals.rebuilt_proofs;
		result.rebuilt_proof_leaves += totals.rebuilt_proof_leaves;
		result.max_rebuilt_proof_leaves = std::max(
			result.max_rebuilt_proof_leaves, totals.max_rebuilt_proof_leaves);
		result.cache_saturations += totals.cache_saturations;
		result.cache_compaction_trials += totals.cache_compaction_trials;
		result.cache_compactions += totals.cache_compactions;
		result.cache_compaction_leaves_saved += totals.cache_compaction_leaves_saved;
		result.uncached_blocked += totals.uncached_blocked;
	}
	result.budget_exhausted = current.budget_exhausted.load();
	result.completed = std::chrono::steady_clock::now();
	result.worker_ms = std::chrono::duration<double, std::milli>(result.completed - current.started).count();
	{
		std::lock_guard lock(stats_mutex_);
		stats_.latest_ms = result.worker_ms;
		stats_.latest_active_ms = result.worker_active_ms;
		stats_.maximum_ms = std::max(stats_.maximum_ms, result.worker_ms);
		stats_.average_ms = (stats_.average_ms * static_cast<double>(stats_.cycles) + result.worker_ms)
			/ static_cast<double>(stats_.cycles + 1u);
		++stats_.cycles;
		stats_.evaluated_pairs = result.evaluated_pairs;
		stats_.visible_pairs = result.visible_pairs;
		stats_.hidden_pairs = result.hidden_pairs;
		stats_.sampled_pixels = result.sampled_pixels;
		stats_.traced_rays = result.traced_rays;
		stats_.visibility_probe_rays = result.visibility_probe_rays;
		stats_.visibility_probe_hits = result.visibility_probe_hits;
		stats_.hold_reuses = result.hold_reuses;
		stats_.visited_nodes = result.visited_nodes;
		stats_.rasterized_triangles = result.rasterized_triangles;
		stats_.occluder_cache_hits = result.occluder_cache_hits;
		stats_.occluder_cache_misses = result.occluder_cache_misses;
		stats_.moc_render_calls = result.moc_render_calls;
		stats_.moc_rect_tests = result.moc_rect_tests;
		stats_.rebuilt_proofs = result.rebuilt_proofs;
		stats_.rebuilt_proof_leaves = result.rebuilt_proof_leaves;
		stats_.max_rebuilt_proof_leaves = result.max_rebuilt_proof_leaves;
		stats_.cache_saturations = result.cache_saturations;
		stats_.cache_compaction_trials = result.cache_compaction_trials;
		stats_.cache_compactions = result.cache_compactions;
		stats_.cache_compaction_leaves_saved = result.cache_compaction_leaves_saved;
		stats_.uncached_blocked = result.uncached_blocked;
		if (result.budget_exhausted) ++stats_.budget_exhaustions;
		recent_worker_ms_[recent_worker_next_] = result.worker_ms;
		recent_worker_next_ = (recent_worker_next_ + 1u) % static_cast<uint32_t>(recent_worker_ms_.size());
		recent_worker_count_ = std::min(recent_worker_count_ + 1u, static_cast<uint32_t>(recent_worker_ms_.size()));
	}
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
	published_.store(std::shared_ptr<const visibility_result> {std::move(current.result)});
#else
	std::atomic_store(&published_, std::shared_ptr<const visibility_result> {std::move(current.result)});
#endif
}

} // namespace cs2fow
