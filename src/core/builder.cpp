#include "builder.h"

// Packs accepted static triangles and builds the offline eight-child tree.
// The baker owns all input/output data; invalid geometry or unsafe limits stop
// the build instead of producing a bake the runtime might trust.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace cs2fow
{
namespace
{

constexpr uint32_t k_bin_count = 32;
constexpr uint32_t k_leaf_size = 8;

vec3 minimum(vec3 a, vec3 b)
{
	return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

vec3 maximum(vec3 a, vec3 b)
{
	return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

bounds empty_bounds()
{
	const float infinity = std::numeric_limits<float>::infinity();
	return {{infinity, infinity, infinity}, {-infinity, -infinity, -infinity}};
}

void expand(bounds &box, vec3 point)
{
	box.min = minimum(box.min, point);
	box.max = maximum(box.max, point);
}

void expand(bounds &box, const bounds &other)
{
	box.min = minimum(box.min, other.min);
	box.max = maximum(box.max, other.max);
}

float area(const bounds &box)
{
	const vec3 size {std::max(0.0f, box.max.x - box.min.x), std::max(0.0f, box.max.y - box.min.y), std::max(0.0f, box.max.z - box.min.z)};
	return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
}

float component(vec3 value, uint32_t axis)
{
	return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
}

struct build_triangle
{
	triangle geometry;
	bounds box;
	vec3 centroid;
};

struct range
{
	uint32_t begin {};
	uint32_t end {};
};

struct split
{
	bool valid {};
	uint32_t axis {};
	uint32_t bin {};
	float position {};
	float gain {};
};

class builder
{
public:
	builder(std::span<const triangle> input, bvh8_data &output, std::vector<uint32_t> *packet_triangle_sources)
		: output_(output), packet_triangle_sources_(packet_triangle_sources)
	{
		triangles_.reserve(input.size());
		for (const triangle &item : input)
		{
			bounds box = empty_bounds();
			expand(box, item.v0);
			expand(box, item.v1);
			expand(box, item.v2);
			triangles_.push_back({item, box, {(item.v0.x + item.v1.x + item.v2.x) / 3.0f, (item.v0.y + item.v1.y + item.v2.y) / 3.0f, (item.v0.z + item.v1.z + item.v2.z) / 3.0f}});
		}
		indices_.resize(input.size());
		std::iota(indices_.begin(), indices_.end(), 0u);
	}

	bool run(std::string &error)
	{
		if (packet_triangle_sources_ != nullptr)
		{
			packet_triangle_sources_->clear();
			packet_triangle_sources_->reserve(((triangles_.size() + 7u) / 8u) * 8u);
		}
		if (triangles_.empty() || triangles_.size() > k_leaf_index_mask)
		{
			error = "triangle count is empty or exceeds format limit";
			return false;
		}
		output_.nodes.reserve(triangles_.size() / 24u);
		output_.packets.reserve((triangles_.size() + 7u) / 8u);
		build_node({0, static_cast<uint32_t>(indices_.size())}, 1);
		output_.header.triangle_count = static_cast<uint32_t>(triangles_.size());
		output_.header.max_depth = max_depth_;
		const bounds world = range_bounds({0, static_cast<uint32_t>(indices_.size())});
		output_.header.world_min[0] = world.min.x;
		output_.header.world_min[1] = world.min.y;
		output_.header.world_min[2] = world.min.z;
		output_.header.world_max[0] = world.max.x;
		output_.header.world_max[1] = world.max.y;
		output_.header.world_max[2] = world.max.z;
		return true;
	}

private:
	bounds range_bounds(range item) const
	{
		bounds result = empty_bounds();
		for (uint32_t i = item.begin; i < item.end; ++i)
		{
			expand(result, triangles_[indices_[i]].box);
		}
		return result;
	}

	split best_split(range item) const
	{
		const uint32_t count = item.end - item.begin;
		const bounds parent = range_bounds(item);
		const float parent_cost = area(parent) * static_cast<float>(count);
		split best {};
		for (uint32_t axis = 0; axis < 3; ++axis)
		{
			float centroid_min = std::numeric_limits<float>::infinity();
			float centroid_max = -std::numeric_limits<float>::infinity();
			for (uint32_t i = item.begin; i < item.end; ++i)
			{
				const float value = component(triangles_[indices_[i]].centroid, axis);
				centroid_min = std::min(centroid_min, value);
				centroid_max = std::max(centroid_max, value);
			}
			if (!(centroid_max > centroid_min))
			{
				continue;
			}
			std::array<uint32_t, k_bin_count> counts {};
			std::array<bounds, k_bin_count> boxes;
			for (bounds &box : boxes)
			{
				box = empty_bounds();
			}
			const float scale = static_cast<float>(k_bin_count) / (centroid_max - centroid_min);
			for (uint32_t i = item.begin; i < item.end; ++i)
			{
				const build_triangle &current = triangles_[indices_[i]];
				const uint32_t bin = std::min(k_bin_count - 1u, static_cast<uint32_t>((component(current.centroid, axis) - centroid_min) * scale));
				++counts[bin];
				expand(boxes[bin], current.box);
			}
			std::array<uint32_t, k_bin_count> left_count {};
			std::array<uint32_t, k_bin_count> right_count {};
			std::array<bounds, k_bin_count> left_box;
			std::array<bounds, k_bin_count> right_box;
			bounds left = empty_bounds();
			bounds right = empty_bounds();
			uint32_t left_total = 0;
			uint32_t right_total = 0;
			for (uint32_t i = 0; i < k_bin_count; ++i)
			{
				left_total += counts[i];
				if (counts[i] != 0)
				{
					expand(left, boxes[i]);
				}
				left_count[i] = left_total;
				left_box[i] = left;
				const uint32_t reverse = k_bin_count - 1u - i;
				right_total += counts[reverse];
				if (counts[reverse] != 0)
				{
					expand(right, boxes[reverse]);
				}
				right_count[reverse] = right_total;
				right_box[reverse] = right;
			}
			for (uint32_t i = 0; i + 1u < k_bin_count; ++i)
			{
				if (left_count[i] == 0 || right_count[i + 1u] == 0)
				{
					continue;
				}
				const float cost = area(left_box[i]) * static_cast<float>(left_count[i]) + area(right_box[i + 1u]) * static_cast<float>(right_count[i + 1u]);
				const float gain = parent_cost - cost;
				if (!best.valid || gain > best.gain)
				{
					best = {true, axis, i, centroid_min + (centroid_max - centroid_min) * static_cast<float>(i + 1u) / static_cast<float>(k_bin_count), gain};
				}
			}
		}
		return best;
	}

	std::pair<range, range> apply_split(range item, split choice)
	{
		auto first = indices_.begin() + item.begin;
		auto last = indices_.begin() + item.end;
		auto middle = std::partition(first, last, [&](uint32_t index)
		{
			return component(triangles_[index].centroid, choice.axis) < choice.position;
		});
		uint32_t split_index = static_cast<uint32_t>(middle - indices_.begin());
		if (split_index == item.begin || split_index == item.end)
		{
			const bounds box = range_bounds(item);
			const vec3 extent {box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z};
			const uint32_t axis = extent.y > extent.x ? (extent.z > extent.y ? 2u : 1u) : (extent.z > extent.x ? 2u : 0u);
			split_index = item.begin + (item.end - item.begin) / 2u;
			std::nth_element(first, indices_.begin() + split_index, last, [&](uint32_t a, uint32_t b)
			{
				return component(triangles_[a].centroid, axis) < component(triangles_[b].centroid, axis);
			});
		}
		return {{item.begin, split_index}, {split_index, item.end}};
	}

	uint32_t make_packet(range item)
	{
		triangle_packet8 packet {};
		for (uint32_t lane = 0; lane < item.end - item.begin; ++lane)
		{
			const uint32_t source_index = indices_[item.begin + lane];
			const triangle &value = triangles_[source_index].geometry;
			packet.v0_x[lane] = value.v0.x;
			packet.v0_y[lane] = value.v0.y;
			packet.v0_z[lane] = value.v0.z;
			packet.edge1_x[lane] = value.v1.x - value.v0.x;
			packet.edge1_y[lane] = value.v1.y - value.v0.y;
			packet.edge1_z[lane] = value.v1.z - value.v0.z;
			packet.edge2_x[lane] = value.v2.x - value.v0.x;
			packet.edge2_y[lane] = value.v2.y - value.v0.y;
			packet.edge2_z[lane] = value.v2.z - value.v0.z;
			if (packet_triangle_sources_ != nullptr)
			{
				packet_triangle_sources_->push_back(source_index);
			}
		}
		if (packet_triangle_sources_ != nullptr)
		{
			for (uint32_t lane = item.end - item.begin; lane < 8u; ++lane)
			{
				packet_triangle_sources_->push_back(k_invalid_ref);
			}
		}
		const uint32_t index = static_cast<uint32_t>(output_.packets.size());
		output_.packets.push_back(packet);
		return make_leaf_ref(index, item.end - item.begin);
	}

	uint32_t build_node(range item, uint32_t depth)
	{
		max_depth_ = std::max(max_depth_, depth);
		const uint32_t node_index = static_cast<uint32_t>(output_.nodes.size());
		output_.nodes.emplace_back();
		std::vector<range> children {item};
		while (children.size() < 8)
		{
			size_t selected = children.size();
			split selected_split {};
			for (size_t i = 0; i < children.size(); ++i)
			{
				if (children[i].end - children[i].begin <= k_leaf_size)
				{
					continue;
				}
				const split candidate = best_split(children[i]);
				if (candidate.valid && (selected == children.size() || candidate.gain > selected_split.gain))
				{
					selected = i;
					selected_split = candidate;
				}
			}
			if (selected == children.size())
			{
				for (size_t i = 0; i < children.size(); ++i)
				{
					if (children[i].end - children[i].begin > k_leaf_size)
					{
						selected = i;
						selected_split.valid = true;
						selected_split.axis = 0;
						selected_split.position = std::numeric_limits<float>::quiet_NaN();
						break;
					}
				}
			}
			if (selected == children.size())
			{
				break;
			}
			const auto parts = apply_split(children[selected], selected_split);
			children[selected] = parts.first;
			children.insert(children.begin() + static_cast<std::ptrdiff_t>(selected + 1u), parts.second);
		}

		bvh8_node node {};
		for (uint32_t lane = 0; lane < 8; ++lane)
		{
			node.child[lane] = k_invalid_ref;
			node.min_x[lane] = node.min_y[lane] = node.min_z[lane] = std::numeric_limits<float>::infinity();
			node.max_x[lane] = node.max_y[lane] = node.max_z[lane] = -std::numeric_limits<float>::infinity();
		}
		for (uint32_t lane = 0; lane < children.size(); ++lane)
		{
			const range child = children[lane];
			const bounds box = range_bounds(child);
			node.min_x[lane] = box.min.x;
			node.min_y[lane] = box.min.y;
			node.min_z[lane] = box.min.z;
			node.max_x[lane] = box.max.x;
			node.max_y[lane] = box.max.y;
			node.max_z[lane] = box.max.z;
			node.child[lane] = child.end - child.begin <= k_leaf_size ? make_packet(child) : build_node(child, depth + 1u);
		}
		output_.nodes[node_index] = node;
		return node_index;
	}

	std::vector<build_triangle> triangles_;
	std::vector<uint32_t> indices_;
	bvh8_data &output_;
	std::vector<uint32_t> *packet_triangle_sources_ {};
	uint32_t max_depth_ {};
};

} // namespace

bool build_bvh8(std::span<const triangle> triangles, bvh8_data &result, std::string &error,
	std::vector<uint32_t> *packet_triangle_sources)
{
	result = {};
	builder value(triangles, result, packet_triangle_sources);
	return value.run(error);
}

} // namespace cs2fow
