#include "capsule_visibility.h"
#include "masked_occlusion_culling/MaskedOcclusionCulling.h"

// A target-fitted CPU occlusion buffer plus analytic ray/capsule intersections
// treats the animated hitbox volume as a silhouette instead of discrete dots.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace cs2fow
{
namespace
{

constexpr float k_epsilon = 1.0e-5f;
constexpr float k_near_depth = 0.125f;
constexpr float k_view_margin = 1.02f;
constexpr float k_depth_epsilon = 1.0e-5f;

struct camera_view
{
	vec3 origin;
	vec3 forward;
	vec3 right;
	vec3 up;
	float horizontal {};
	float vertical {};
	float far_depth {};
};

struct camera_vertex
{
	float x {};
	float y {};
	float depth {};
};

struct projected_bounds
{
	int first_x {};
	int last_x {};
	int first_y {};
	int last_y {};
	float minimum_x {};
	float maximum_x {};
	float minimum_y {};
	float maximum_y {};
	float minimum_depth {};
};

struct traversal_entry
{
	uint32_t ref {};
	float near_depth {};
};

enum class map_render_result
{
	complete,
	target_occluded,
	failed
};

bool project_bounds(const camera_view &view, bounds box, projected_bounds &result);

struct moc_deleter
{
	void operator()(MaskedOcclusionCulling *value) const
	{
		if (value != nullptr) MaskedOcclusionCulling::Destroy(value);
	}
};

struct occlusion_scratch
{
	std::unique_ptr<MaskedOcclusionCulling, moc_deleter> moc {
		MaskedOcclusionCulling::Create(MaskedOcclusionCulling::SSE41)};
	std::vector<float> vertices;
	std::vector<unsigned int> indices;
	std::vector<traversal_entry> traversal;
	std::array<float, k_visibility_pixel_count> pixel_depth {};

	occlusion_scratch()
	{
		if (moc != nullptr)
		{
			moc->SetResolution(k_visibility_pixel_grid_size, k_visibility_pixel_grid_size);
			moc->SetNearClipPlane(k_near_depth);
		}
		traversal.reserve(512);
	}
};

vec3 add(vec3 a, vec3 b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

vec3 subtract(vec3 a, vec3 b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

vec3 scale(vec3 value, float amount)
{
	return {value.x * amount, value.y * amount, value.z * amount};
}

float dot(vec3 a, vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

vec3 cross(vec3 a, vec3 b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float length_sq(vec3 value)
{
	return dot(value, value);
}

bool normalize(vec3 value, vec3 &result)
{
	const float squared = length_sq(value);
	if (!std::isfinite(squared) || squared <= k_epsilon)
	{
		return false;
	}
	result = scale(value, 1.0f / std::sqrt(squared));
	return true;
}

float point_segment_distance_sq(vec3 point, vec3 start, vec3 end)
{
	const vec3 segment = subtract(end, start);
	const float denominator = length_sq(segment);
	const float amount = denominator <= k_epsilon ? 0.0f
		: std::clamp(dot(subtract(point, start), segment) / denominator, 0.0f, 1.0f);
	return length_sq(subtract(point, add(start, scale(segment, amount))));
}

bool ray_sphere(vec3 origin, vec3 direction, vec3 center, float radius, float &distance)
{
	const vec3 offset = subtract(origin, center);
	const float a = length_sq(direction);
	const float b = dot(offset, direction);
	const float c = length_sq(offset) - radius * radius;
	const float discriminant = b * b - a * c;
	if (a <= k_epsilon || discriminant < 0.0f)
	{
		return false;
	}
	const float root = std::sqrt(std::max(0.0f, discriminant));
	distance = (-b - root) / a;
	if (distance <= k_epsilon)
	{
		distance = (-b + root) / a;
	}
	return std::isfinite(distance) && distance > k_epsilon;
}

bool ray_capsule(vec3 origin, vec3 direction, const visibility_capsule &capsule, float &distance)
{
	const vec3 axis = subtract(capsule.end, capsule.start);
	const float axis_length_sq = length_sq(axis);
	if (axis_length_sq <= k_epsilon)
	{
		return ray_sphere(origin, direction, capsule.start, capsule.radius, distance);
	}

	const vec3 offset = subtract(origin, capsule.start);
	const float direction_length_sq = length_sq(direction);
	const float axis_direction = dot(axis, direction);
	const float axis_offset = dot(axis, offset);
	const float direction_offset = dot(direction, offset);
	const float a = axis_length_sq * direction_length_sq - axis_direction * axis_direction;
	const float b = axis_length_sq * direction_offset - axis_offset * axis_direction;
	const float c = axis_length_sq * (length_sq(offset) - capsule.radius * capsule.radius)
		- axis_offset * axis_offset;
	if (a > k_epsilon)
	{
		const float discriminant = b * b - a * c;
		if (discriminant >= 0.0f)
		{
			const float candidate = (-b - std::sqrt(std::max(0.0f, discriminant))) / a;
			const float along_axis = axis_offset + candidate * axis_direction;
			if (candidate > k_epsilon && along_axis > 0.0f && along_axis < axis_length_sq)
			{
				distance = candidate;
				return true;
			}
		}
	}

	float start_distance = 0.0f;
	float end_distance = 0.0f;
	const bool hit_start = ray_sphere(origin, direction, capsule.start, capsule.radius, start_distance);
	const bool hit_end = ray_sphere(origin, direction, capsule.end, capsule.radius, end_distance);
	if (!hit_start && !hit_end)
	{
		return false;
	}
	distance = hit_start && hit_end ? std::min(start_distance, end_distance)
		: (hit_start ? start_distance : end_distance);
	return true;
}

uint32_t center_out(uint32_t value)
{
	constexpr uint32_t half = k_visibility_pixel_grid_size / 2u;
	return (value & 1u) == 0u ? half - 1u - value / 2u : half + value / 2u;
}

camera_vertex to_camera(const camera_view &view, vec3 point)
{
	const vec3 offset = subtract(point, view.origin);
	return {dot(offset, view.right), dot(offset, view.up), dot(offset, view.forward)};
}

bool build_view(vec3 origin, bounds box, camera_view &view)
{
	view.origin = origin;
	const vec3 center = scale(add(box.min, box.max), 0.5f);
	if (!normalize(subtract(center, origin), view.forward)
		|| (!normalize(cross(view.forward, {0.0f, 0.0f, 1.0f}), view.right)
			&& !normalize(cross(view.forward, {0.0f, 1.0f, 0.0f}), view.right)))
	{
		return false;
	}
	view.up = cross(view.right, view.forward);
	view.horizontal = k_epsilon;
	view.vertical = k_epsilon;
	for (uint32_t corner = 0; corner < 8; ++corner)
	{
		const vec3 point {
			(corner & 1u) != 0u ? box.max.x : box.min.x,
			(corner & 2u) != 0u ? box.max.y : box.min.y,
			(corner & 4u) != 0u ? box.max.z : box.min.z
		};
		const camera_vertex camera = to_camera(view, point);
		if (!std::isfinite(camera.depth) || camera.depth <= k_near_depth)
		{
			return false;
		}
		view.horizontal = std::max(view.horizontal, std::fabs(camera.x / camera.depth));
		view.vertical = std::max(view.vertical, std::fabs(camera.y / camera.depth));
		view.far_depth = std::max(view.far_depth, camera.depth);
	}
	view.horizontal *= k_view_margin;
	view.vertical *= k_view_margin;
	return std::isfinite(view.horizontal) && std::isfinite(view.vertical)
		&& std::isfinite(view.far_depth) && view.far_depth > k_near_depth;
}

bool box_intersects_view(const camera_view &view, bounds box, float &near_depth)
{
	const vec3 center = scale(add(box.min, box.max), 0.5f);
	const vec3 extent = scale(subtract(box.max, box.min), 0.5f);
	const camera_vertex camera = to_camera(view, center);
	const auto radius = [&](vec3 axis)
	{
		return std::fabs(axis.x) * extent.x + std::fabs(axis.y) * extent.y + std::fabs(axis.z) * extent.z;
	};
	const float depth_radius = radius(view.forward);
	const float right_radius = radius(view.right);
	const float up_radius = radius(view.up);
	near_depth = camera.depth - depth_radius;
	return camera.depth + depth_radius >= k_near_depth
		&& camera.depth - depth_radius <= view.far_depth
		&& view.horizontal * camera.depth + camera.x + view.horizontal * depth_radius + right_radius >= 0.0f
		&& view.horizontal * camera.depth - camera.x + view.horizontal * depth_radius + right_radius >= 0.0f
		&& view.vertical * camera.depth + camera.y + view.vertical * depth_radius + up_radius >= 0.0f
		&& view.vertical * camera.depth - camera.y + view.vertical * depth_radius + up_radius >= 0.0f;
}

bool append_clip_triangle(occlusion_scratch &scratch, const camera_view &view,
	vec3 first, vec3 second, vec3 third)
{
	for (vec3 point : {first, second, third})
	{
		const camera_vertex camera = to_camera(view, point);
		const float x = camera.x / view.horizontal;
		const float y = camera.y / view.vertical;
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(camera.depth))
		{
			return false;
		}
		scratch.indices.push_back(static_cast<unsigned int>(scratch.vertices.size() / 4u));
		scratch.vertices.push_back(x);
		scratch.vertices.push_back(y);
		scratch.vertices.push_back(0.0f);
		scratch.vertices.push_back(camera.depth);
	}
	return true;
}

map_render_result render_map_moc(const bvh8_data &geometry, const camera_view &view,
	std::span<const projected_bounds> targets, occlusion_scratch &scratch,
	std::chrono::steady_clock::time_point deadline, const std::atomic_bool *stopping,
	capsule_query_stats *stats, capsule_occluder_cache *occluder_cache)
{
	if (scratch.moc == nullptr || geometry.nodes.empty())
	{
		return map_render_result::failed;
	}
	const auto interrupted = [&]
	{
		return (stopping != nullptr && stopping->load()) || std::chrono::steady_clock::now() >= deadline;
	};
	projected_bounds combined_target;
	if (!targets.empty())
	{
		combined_target = targets.front();
		for (const projected_bounds &target : targets.subspan(1))
		{
			combined_target.minimum_x = std::min(combined_target.minimum_x, target.minimum_x);
			combined_target.maximum_x = std::max(combined_target.maximum_x, target.maximum_x);
			combined_target.minimum_y = std::min(combined_target.minimum_y, target.minimum_y);
			combined_target.maximum_y = std::max(combined_target.maximum_y, target.maximum_y);
			combined_target.minimum_depth = std::min(combined_target.minimum_depth, target.minimum_depth);
		}
	}
	const auto all_targets_occluded = [&]
	{
		if (targets.empty()) return false;
		constexpr float margin = 1.0e-4f;
		if (scratch.moc->TestRect(combined_target.minimum_x - margin, combined_target.minimum_y - margin,
			combined_target.maximum_x + margin, combined_target.maximum_y + margin,
			combined_target.minimum_depth * (1.0f - margin)) == MaskedOcclusionCulling::OCCLUDED)
		{
			return true;
		}
		for (const projected_bounds &target : targets)
		{
			if (scratch.moc->TestRect(target.minimum_x - margin, target.minimum_y - margin,
				target.maximum_x + margin, target.maximum_y + margin,
				target.minimum_depth * (1.0f - margin)) != MaskedOcclusionCulling::OCCLUDED)
			{
				return false;
			}
		}
		return true;
	};
	const auto append_leaf = [&](uint32_t ref)
	{
		if (!is_leaf_ref(ref) || leaf_index(ref) >= geometry.packets.size()) return false;
		const triangle_packet8 &packet = geometry.packets[leaf_index(ref)];
		for (uint32_t triangle_index = 0; triangle_index < leaf_count(ref); ++triangle_index)
		{
			const vec3 first {packet.v0_x[triangle_index], packet.v0_y[triangle_index], packet.v0_z[triangle_index]};
			const vec3 second {first.x + packet.edge1_x[triangle_index], first.y + packet.edge1_y[triangle_index],
				first.z + packet.edge1_z[triangle_index]};
			const vec3 third {first.x + packet.edge2_x[triangle_index], first.y + packet.edge2_y[triangle_index],
				first.z + packet.edge2_z[triangle_index]};
			if (!append_clip_triangle(scratch, view, first, second, third)) return false;
		}
		return true;
	};
	const auto render_leaves = [&]
	{
		if (scratch.indices.empty()) return;
		const int triangles = static_cast<int>(scratch.indices.size() / 3u);
		scratch.moc->RenderTriangles(scratch.vertices.data(), scratch.indices.data(), triangles, nullptr,
			MaskedOcclusionCulling::BACKFACE_NONE, MaskedOcclusionCulling::CLIP_PLANE_ALL);
		if (stats != nullptr) stats->rasterized_triangles += static_cast<uint32_t>(triangles);
		scratch.vertices.clear();
		scratch.indices.clear();
	};

	scratch.moc->ClearBuffer();
	scratch.vertices.clear();
	scratch.indices.clear();
	if (occluder_cache != nullptr && occluder_cache->count != 0)
	{
		for (uint32_t index = 0; index < occluder_cache->count; ++index)
		{
			if (!append_leaf(occluder_cache->leaves[index]))
			{
				occluder_cache->count = 0;
				break;
			}
		}
		render_leaves();
		if (interrupted()) return map_render_result::failed;
		if (all_targets_occluded()) return map_render_result::target_occluded;
		scratch.moc->ClearBuffer();
	}

	capsule_occluder_cache candidate_cache;
	scratch.traversal.clear();
	scratch.traversal.push_back({0u, -std::numeric_limits<float>::infinity()});
	const auto farther_first = [](const traversal_entry &left, const traversal_entry &right)
	{
		return left.near_depth > right.near_depth;
	};
	uint32_t checked_nodes = 0;
	while (!scratch.traversal.empty())
	{
		if ((checked_nodes++ & 63u) == 0u
			&& interrupted())
		{
			return map_render_result::failed;
		}
		std::pop_heap(scratch.traversal.begin(), scratch.traversal.end(), farther_first);
		const traversal_entry entry = scratch.traversal.back();
		scratch.traversal.pop_back();
		if (is_leaf_ref(entry.ref))
		{
			if (!append_leaf(entry.ref)) return map_render_result::failed;
			render_leaves();
			if (candidate_cache.count < candidate_cache.leaves.size())
			{
				candidate_cache.leaves[candidate_cache.count++] = entry.ref;
			}
			if (all_targets_occluded())
			{
				if (occluder_cache != nullptr) *occluder_cache = candidate_cache;
				return map_render_result::target_occluded;
			}
			continue;
		}
		const bvh8_node &node = geometry.nodes[entry.ref];
		if (stats != nullptr) ++stats->visited_nodes;
		for (uint32_t lane = 0; lane < 8; ++lane)
		{
			const uint32_t ref = node.child[lane];
			if (ref == k_invalid_ref) continue;
			const bounds box {{node.min_x[lane], node.min_y[lane], node.min_z[lane]},
				{node.max_x[lane], node.max_y[lane], node.max_z[lane]}};
			float near_depth = 0.0f;
			if (!box_intersects_view(view, box, near_depth)) continue;
			scratch.traversal.push_back({ref, near_depth});
			std::push_heap(scratch.traversal.begin(), scratch.traversal.end(), farther_first);
		}
	}
	if (occluder_cache != nullptr) occluder_cache->count = 0;
	return map_render_result::complete;
}

bool append_capsule_mesh(occlusion_scratch &scratch, const camera_view &view,
	const visibility_capsule &capsule)
{
	constexpr uint32_t sides = 12;
	constexpr uint32_t hemisphere_segments = 4;
	constexpr float pi = 3.14159265358979323846f;
	// Circumscribed, not inscribed: 1.06*cos(15deg)*cos(11.25deg) > 1.
	// Therefore mesh occlusion cannot hide a sub-pixel piece of the true capsule.
	constexpr float radial_scale = 1.06f;
	constexpr uint32_t ring_count = hemisphere_segments * 2u;
	constexpr uint32_t vertex_count = 2u + ring_count * sides;
	std::array<vec3, vertex_count> points {};
	vec3 axis;
	if (!normalize(subtract(capsule.end, capsule.start), axis)) axis = {0.0f, 0.0f, 1.0f};
	vec3 side;
	if (!normalize(cross(axis, {0.0f, 0.0f, 1.0f}), side)
		&& !normalize(cross(axis, {0.0f, 1.0f, 0.0f}), side))
	{
		return false;
	}
	const vec3 up = cross(side, axis);
	const float radius = capsule.radius * radial_scale;
	points[0] = add(capsule.start, scale(axis, -radius));
	uint32_t output = 1;
	for (uint32_t ring = 1; ring <= hemisphere_segments; ++ring)
	{
		const float latitude = -0.5f * pi + 0.5f * pi * static_cast<float>(ring)
			/ static_cast<float>(hemisphere_segments);
		for (uint32_t lane = 0; lane < sides; ++lane)
		{
			const float longitude = 2.0f * pi * static_cast<float>(lane) / static_cast<float>(sides);
			const vec3 radial = add(scale(side, std::cos(longitude)), scale(up, std::sin(longitude)));
			points[output++] = add(capsule.start, add(scale(axis, std::sin(latitude) * radius),
				scale(radial, std::cos(latitude) * radius)));
		}
	}
	for (uint32_t ring = 0; ring < hemisphere_segments; ++ring)
	{
		const float latitude = 0.5f * pi * static_cast<float>(ring)
			/ static_cast<float>(hemisphere_segments);
		for (uint32_t lane = 0; lane < sides; ++lane)
		{
			const float longitude = 2.0f * pi * static_cast<float>(lane) / static_cast<float>(sides);
			const vec3 radial = add(scale(side, std::cos(longitude)), scale(up, std::sin(longitude)));
			points[output++] = add(capsule.end, add(scale(axis, std::sin(latitude) * radius),
				scale(radial, std::cos(latitude) * radius)));
		}
	}
	points[output] = add(capsule.end, scale(axis, radius));
	const uint32_t top_pole = vertex_count - 1u;
	const auto ring_start = [&](uint32_t ring) { return 1u + ring * sides; };
	const unsigned int first_vertex = static_cast<unsigned int>(scratch.vertices.size() / 4u);
	for (vec3 point : points)
	{
		const camera_vertex camera = to_camera(view, point);
		const float x = camera.x / view.horizontal;
		const float y = camera.y / view.vertical;
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(camera.depth))
		{
			return false;
		}
		scratch.vertices.push_back(x);
		scratch.vertices.push_back(y);
		scratch.vertices.push_back(0.0f);
		scratch.vertices.push_back(camera.depth);
	}
	const auto add_triangle = [&](uint32_t a, uint32_t b, uint32_t c)
	{
		scratch.indices.push_back(first_vertex + a);
		scratch.indices.push_back(first_vertex + b);
		scratch.indices.push_back(first_vertex + c);
		return true;
	};
	for (uint32_t lane = 0; lane < sides; ++lane)
	{
		const uint32_t next = (lane + 1u) % sides;
		if (!add_triangle(0, ring_start(0) + next, ring_start(0) + lane)) return false;
		for (uint32_t ring = 0; ring + 1u < ring_count; ++ring)
		{
			const uint32_t lower = ring_start(ring);
			const uint32_t upper = ring_start(ring + 1u);
			if (!add_triangle(lower + lane, lower + next, upper + lane)
				|| !add_triangle(lower + next, upper + next, upper + lane)) return false;
		}
		if (!add_triangle(ring_start(ring_count - 1u) + lane,
			ring_start(ring_count - 1u) + next, top_pole)) return false;
	}
	return true;
}

MaskedOcclusionCulling::CullingResult test_capsule_moc(occlusion_scratch &scratch,
	const camera_view &view, const visibility_capsule &capsule)
{
	scratch.vertices.clear();
	scratch.indices.clear();
	if (!append_capsule_mesh(scratch, view, capsule)) return MaskedOcclusionCulling::VIEW_CULLED;
	return scratch.moc->TestTriangles(scratch.vertices.data(),
		scratch.indices.data(), static_cast<int>(scratch.indices.size() / 3u), nullptr,
		MaskedOcclusionCulling::BACKFACE_NONE, MaskedOcclusionCulling::CLIP_PLANE_ALL);
}

bool project_bounds(const camera_view &view, bounds box, projected_bounds &result)
{
	result.minimum_x = 1.0f;
	result.maximum_x = -1.0f;
	result.minimum_y = 1.0f;
	result.maximum_y = -1.0f;
	result.minimum_depth = std::numeric_limits<float>::infinity();
	for (uint32_t corner = 0; corner < 8; ++corner)
	{
		const vec3 point {
			(corner & 1u) != 0u ? box.max.x : box.min.x,
			(corner & 2u) != 0u ? box.max.y : box.min.y,
			(corner & 4u) != 0u ? box.max.z : box.min.z
		};
		const camera_vertex camera = to_camera(view, point);
		if (camera.depth <= k_near_depth)
		{
			return false;
		}
		const float x = camera.x / (camera.depth * view.horizontal);
		const float y = camera.y / (camera.depth * view.vertical);
		result.minimum_x = std::min(result.minimum_x, x);
		result.maximum_x = std::max(result.maximum_x, x);
		result.minimum_y = std::min(result.minimum_y, y);
		result.maximum_y = std::max(result.maximum_y, y);
		result.minimum_depth = std::min(result.minimum_depth, camera.depth);
	}
	const float grid = static_cast<float>(k_visibility_pixel_grid_size);
	result.first_x = std::clamp(static_cast<int>(std::floor((result.minimum_x + 1.0f) * 0.5f * grid)), 0,
		static_cast<int>(k_visibility_pixel_grid_size) - 1);
	result.last_x = std::clamp(static_cast<int>(std::ceil((result.maximum_x + 1.0f) * 0.5f * grid)) - 1, 0,
		static_cast<int>(k_visibility_pixel_grid_size) - 1);
	result.first_y = std::clamp(static_cast<int>(std::floor((1.0f - result.maximum_y) * 0.5f * grid)), 0,
		static_cast<int>(k_visibility_pixel_grid_size) - 1);
	result.last_y = std::clamp(static_cast<int>(std::ceil((1.0f - result.minimum_y) * 0.5f * grid)) - 1, 0,
		static_cast<int>(k_visibility_pixel_grid_size) - 1);
	return result.first_x <= result.last_x && result.first_y <= result.last_y;
}

} // namespace

capsule_query_result capsule_visible_from_origin(const bvh8_data &geometry, vec3 origin,
	std::span<const visibility_capsule> capsules, const smoke_snapshot *smokes, float smoke_age_advance,
	std::chrono::steady_clock::time_point deadline, const std::atomic_bool *stopping, capsule_query_stats *stats,
	capsule_occluder_cache *occluder_cache)
{
	if (capsules.size() != k_visibility_capsule_count)
	{
		return capsule_query_result::indeterminate;
	}
	bounds body {{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
		std::numeric_limits<float>::infinity()}, {-std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}};
	for (const visibility_capsule &capsule : capsules)
	{
		if (!valid_visibility_capsule(capsule))
		{
			return capsule_query_result::indeterminate;
		}
		if (point_segment_distance_sq(origin, capsule.start, capsule.end) <= capsule.radius * capsule.radius)
		{
			return capsule_query_result::visible;
		}
		for (vec3 point : {capsule.start, capsule.end})
		{
			body.min.x = std::min(body.min.x, point.x - capsule.radius);
			body.min.y = std::min(body.min.y, point.y - capsule.radius);
			body.min.z = std::min(body.min.z, point.z - capsule.radius);
			body.max.x = std::max(body.max.x, point.x + capsule.radius);
			body.max.y = std::max(body.max.y, point.y + capsule.radius);
			body.max.z = std::max(body.max.z, point.z + capsule.radius);
		}
	}

	camera_view view;
	if (!build_view(origin, body, view))
	{
		return capsule_query_result::indeterminate;
	}
	std::array<projected_bounds, k_visibility_capsule_count> projected {};
	for (size_t index = 0; index < capsules.size(); ++index)
	{
		const visibility_capsule &capsule = capsules[index];
		const bounds capsule_box {
			{std::min(capsule.start.x, capsule.end.x) - capsule.radius,
				std::min(capsule.start.y, capsule.end.y) - capsule.radius,
				std::min(capsule.start.z, capsule.end.z) - capsule.radius},
			{std::max(capsule.start.x, capsule.end.x) + capsule.radius,
				std::max(capsule.start.y, capsule.end.y) + capsule.radius,
				std::max(capsule.start.z, capsule.end.z) + capsule.radius}
		};
		if (!project_bounds(view, capsule_box, projected[index]))
		{
			return capsule_query_result::indeterminate;
		}
	}
	thread_local occlusion_scratch scratch;
	const map_render_result map_result = render_map_moc(geometry, view, projected, scratch,
		deadline, stopping, stats, occluder_cache);
	if (map_result == map_render_result::target_occluded)
	{
		return capsule_query_result::blocked;
	}
	if (map_result != map_render_result::complete)
	{
		return capsule_query_result::indeterminate;
	}
	if (smokes == nullptr)
	{
		for (size_t index = 0; index < capsules.size(); ++index)
		{
			const projected_bounds &capsule_projection = projected[index];
			constexpr float conservative_margin = 1.0e-4f;
			if (scratch.moc->TestRect(capsule_projection.minimum_x - conservative_margin,
				capsule_projection.minimum_y - conservative_margin,
				capsule_projection.maximum_x + conservative_margin,
				capsule_projection.maximum_y + conservative_margin,
				capsule_projection.minimum_depth * (1.0f - conservative_margin))
				== MaskedOcclusionCulling::OCCLUDED)
			{
				continue;
			}
			const auto capsule_result = test_capsule_moc(scratch, view, capsules[index]);
			if (capsule_result == MaskedOcclusionCulling::VISIBLE) return capsule_query_result::visible;
			if (capsule_result != MaskedOcclusionCulling::OCCLUDED) return capsule_query_result::indeterminate;
		}
		return capsule_query_result::blocked;
	}
	scratch.moc->ComputePixelDepthBuffer(scratch.pixel_depth.data(), false);

	const float grid = static_cast<float>(k_visibility_pixel_grid_size);
	bool geometry_visible_sample = false;
	for (size_t capsule_index = 0; capsule_index < capsules.size(); ++capsule_index)
	{
		const visibility_capsule &capsule = capsules[capsule_index];
		const projected_bounds &capsule_projection = projected[capsule_index];
		// The projected AABB contains the capsule. If even this larger, slightly
		// nearer rectangle is hidden, the capsule is proven hidden without sampling.
		constexpr float conservative_margin = 1.0e-4f;
		const auto coarse = scratch.moc->TestRect(capsule_projection.minimum_x - conservative_margin,
			capsule_projection.minimum_y - conservative_margin, capsule_projection.maximum_x + conservative_margin,
			capsule_projection.maximum_y + conservative_margin,
			capsule_projection.minimum_depth * (1.0f - conservative_margin));
		if (coarse == MaskedOcclusionCulling::OCCLUDED) continue;
		for (uint32_t ordered_y = 0; ordered_y < k_visibility_pixel_grid_size; ++ordered_y)
		{
			if ((stopping != nullptr && stopping->load()) || std::chrono::steady_clock::now() >= deadline)
			{
				return capsule_query_result::indeterminate;
			}
			const uint32_t y = center_out(ordered_y);
			if (static_cast<int>(y) < capsule_projection.first_y
				|| static_cast<int>(y) > capsule_projection.last_y) continue;
			const float screen_y = 1.0f - (2.0f * static_cast<float>(y) + 1.0f) / grid;
			for (uint32_t ordered_x = 0; ordered_x < k_visibility_pixel_grid_size; ++ordered_x)
			{
				const uint32_t x = center_out(ordered_x);
				if (static_cast<int>(x) < capsule_projection.first_x
					|| static_cast<int>(x) > capsule_projection.last_x) continue;
				const float screen_x = (2.0f * static_cast<float>(x) + 1.0f) / grid - 1.0f;
				const vec3 direction = add(view.forward, add(scale(view.right, screen_x * view.horizontal),
					scale(view.up, screen_y * view.vertical)));
				float distance = 0.0f;
				if (stats != nullptr) ++stats->sampled_pixels;
				if (!ray_capsule(origin, direction, capsule, distance)) continue;
				const size_t pixel = static_cast<size_t>(y) * k_visibility_pixel_grid_size + x;
				const float target_depth = 1.0f / distance;
				if (scratch.pixel_depth[pixel] > target_depth * (1.0f + k_depth_epsilon)) continue;
				const vec3 target = add(origin, scale(direction, distance));
				geometry_visible_sample = true;
				if (stats != nullptr) ++stats->traced_rays;
				if (smokes == nullptr || !smoke_line_blocked(*smokes, origin, target, smoke_age_advance, &geometry))
				{
					return capsule_query_result::visible;
				}
			}
		}
	}
	if (geometry_visible_sample) return capsule_query_result::blocked;

	// Pixel centers can miss a sub-pixel opening. The conservative outer capsule
	// mesh is the final proof: uncertainty reveals rather than hiding a visible player.
	for (size_t index = 0; index < capsules.size(); ++index)
	{
		if ((stopping != nullptr && stopping->load()) || std::chrono::steady_clock::now() >= deadline)
		{
			return capsule_query_result::indeterminate;
		}
		const projected_bounds &capsule_projection = projected[index];
		constexpr float conservative_margin = 1.0e-4f;
		if (scratch.moc->TestRect(capsule_projection.minimum_x - conservative_margin,
			capsule_projection.minimum_y - conservative_margin, capsule_projection.maximum_x + conservative_margin,
			capsule_projection.maximum_y + conservative_margin,
			capsule_projection.minimum_depth * (1.0f - conservative_margin))
			== MaskedOcclusionCulling::OCCLUDED)
		{
			continue;
		}
		if (test_capsule_moc(scratch, view, capsules[index]) != MaskedOcclusionCulling::OCCLUDED)
		{
			return capsule_query_result::indeterminate;
		}
	}
	return capsule_query_result::blocked;
}

} // namespace cs2fow
