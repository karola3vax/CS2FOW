#pragma once

// Declares the offline triangle-to-BVH8 builder. It receives accepted map
// triangles and either returns one complete tree or a plain error.

#include "bvh8.h"

#include <span>
#include <vector>

namespace cs2fow
{

bool build_bvh8(std::span<const triangle> triangles, bvh8_data &result, std::string &error,
	std::vector<uint32_t> *packet_triangle_sources = nullptr);

} // namespace cs2fow
