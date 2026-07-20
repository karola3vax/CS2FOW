#include "bvh8.h"

// Answers whether a line segment crosses baked triangles. The background
// visibility worker supplies immutable, already-validated BVH8 data and copied
// points; traversal uses fixed stack storage and returns the blocking packet.

#include <bit>
#include <cmath>
#include <immintrin.h>
#include <limits>

#if defined(_WIN32)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace cs2fow
{
namespace
{

constexpr float k_ray_epsilon = 1.0e-5f;

#if defined(__GNUC__) && !defined(_MSC_VER)
#define CS2FOW_AVX __attribute__((target("avx")))
#else
#define CS2FOW_AVX
#endif

uint32_t packet_mask(uint32_t count)
{
	return count == 8 ? 0xffu : ((1u << count) - 1u);
}

CS2FOW_AVX uint32_t hit_packet(const triangle_packet8 &packet, uint32_t count, vec3 origin, vec3 direction)
{
	const __m256 ox = _mm256_set1_ps(origin.x);
	const __m256 oy = _mm256_set1_ps(origin.y);
	const __m256 oz = _mm256_set1_ps(origin.z);
	const __m256 dx = _mm256_set1_ps(direction.x);
	const __m256 dy = _mm256_set1_ps(direction.y);
	const __m256 dz = _mm256_set1_ps(direction.z);
	const __m256 e1x = _mm256_load_ps(packet.edge1_x);
	const __m256 e1y = _mm256_load_ps(packet.edge1_y);
	const __m256 e1z = _mm256_load_ps(packet.edge1_z);
	const __m256 e2x = _mm256_load_ps(packet.edge2_x);
	const __m256 e2y = _mm256_load_ps(packet.edge2_y);
	const __m256 e2z = _mm256_load_ps(packet.edge2_z);

	const __m256 px = _mm256_sub_ps(_mm256_mul_ps(dy, e2z), _mm256_mul_ps(dz, e2y));
	const __m256 py = _mm256_sub_ps(_mm256_mul_ps(dz, e2x), _mm256_mul_ps(dx, e2z));
	const __m256 pz = _mm256_sub_ps(_mm256_mul_ps(dx, e2y), _mm256_mul_ps(dy, e2x));
	const __m256 det = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(e1x, px), _mm256_mul_ps(e1y, py)), _mm256_mul_ps(e1z, pz));
	const __m256 abs_det = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), det);
	__m256 valid = _mm256_cmp_ps(abs_det, _mm256_set1_ps(k_ray_epsilon), _CMP_GT_OQ);
	const __m256 inv_det = _mm256_div_ps(_mm256_set1_ps(1.0f), det);

	const __m256 tx = _mm256_sub_ps(ox, _mm256_load_ps(packet.v0_x));
	const __m256 ty = _mm256_sub_ps(oy, _mm256_load_ps(packet.v0_y));
	const __m256 tz = _mm256_sub_ps(oz, _mm256_load_ps(packet.v0_z));
	const __m256 u = _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(tx, px), _mm256_mul_ps(ty, py)), _mm256_mul_ps(tz, pz)), inv_det);
	valid = _mm256_and_ps(valid, _mm256_cmp_ps(u, _mm256_setzero_ps(), _CMP_GE_OQ));
	valid = _mm256_and_ps(valid, _mm256_cmp_ps(u, _mm256_set1_ps(1.0f), _CMP_LE_OQ));

	const __m256 qx = _mm256_sub_ps(_mm256_mul_ps(ty, e1z), _mm256_mul_ps(tz, e1y));
	const __m256 qy = _mm256_sub_ps(_mm256_mul_ps(tz, e1x), _mm256_mul_ps(tx, e1z));
	const __m256 qz = _mm256_sub_ps(_mm256_mul_ps(tx, e1y), _mm256_mul_ps(ty, e1x));
	const __m256 v = _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(dx, qx), _mm256_mul_ps(dy, qy)), _mm256_mul_ps(dz, qz)), inv_det);
	valid = _mm256_and_ps(valid, _mm256_cmp_ps(v, _mm256_setzero_ps(), _CMP_GE_OQ));
	valid = _mm256_and_ps(valid, _mm256_cmp_ps(_mm256_add_ps(u, v), _mm256_set1_ps(1.0f), _CMP_LE_OQ));

	const __m256 distance = _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(e2x, qx), _mm256_mul_ps(e2y, qy)), _mm256_mul_ps(e2z, qz)), inv_det);
	valid = _mm256_and_ps(valid, _mm256_cmp_ps(distance, _mm256_set1_ps(k_ray_epsilon), _CMP_GT_OQ));
	valid = _mm256_and_ps(valid, _mm256_cmp_ps(distance, _mm256_set1_ps(1.0f - k_ray_epsilon), _CMP_LT_OQ));
	return static_cast<uint32_t>(_mm256_movemask_ps(valid)) & packet_mask(count);
}

CS2FOW_AVX uint32_t hit_children(const bvh8_node &node, vec3 origin, vec3 direction)
{
	__m256 near_t = _mm256_set1_ps(-std::numeric_limits<float>::infinity());
	__m256 far_t = _mm256_set1_ps(std::numeric_limits<float>::infinity());
	const float origins[3] = {origin.x, origin.y, origin.z};
	const float directions[3] = {direction.x, direction.y, direction.z};
	const float *mins[3] = {node.min_x, node.min_y, node.min_z};
	const float *maxs[3] = {node.max_x, node.max_y, node.max_z};

	for (int axis = 0; axis < 3; ++axis)
	{
		const __m256 minimum = _mm256_load_ps(mins[axis]);
		const __m256 maximum = _mm256_load_ps(maxs[axis]);
		const __m256 position = _mm256_set1_ps(origins[axis]);
		if (std::fabs(directions[axis]) < 1.0e-12f)
		{
			const __m256 inside = _mm256_and_ps(_mm256_cmp_ps(position, minimum, _CMP_GE_OQ), _mm256_cmp_ps(position, maximum, _CMP_LE_OQ));
			far_t = _mm256_blendv_ps(_mm256_set1_ps(-std::numeric_limits<float>::infinity()), far_t, inside);
			continue;
		}

		const __m256 inverse = _mm256_set1_ps(1.0f / directions[axis]);
		const __m256 a = _mm256_mul_ps(_mm256_sub_ps(minimum, position), inverse);
		const __m256 b = _mm256_mul_ps(_mm256_sub_ps(maximum, position), inverse);
		near_t = _mm256_max_ps(near_t, _mm256_min_ps(a, b));
		far_t = _mm256_min_ps(far_t, _mm256_max_ps(a, b));
	}

	__m256 hit = _mm256_cmp_ps(far_t, _mm256_max_ps(near_t, _mm256_setzero_ps()), _CMP_GE_OQ);
	hit = _mm256_and_ps(hit, _mm256_cmp_ps(near_t, _mm256_set1_ps(1.0f - k_ray_epsilon), _CMP_LT_OQ));
	uint32_t mask = static_cast<uint32_t>(_mm256_movemask_ps(hit));
	for (uint32_t i = 0; i < 8; ++i)
	{
		if (node.child[i] == k_invalid_ref)
		{
			mask &= ~(1u << i);
		}
	}
	return mask;
}

#undef CS2FOW_AVX

} // namespace

bool cpu_supports_avx()
{
#if defined(_WIN32)
	int registers[4] {};
	__cpuid(registers, 1);
	return (registers[2] & (1 << 27)) != 0 && (registers[2] & (1 << 28)) != 0 && (_xgetbv(0) & 6) == 6;
#else
	unsigned int eax = 0;
	unsigned int ebx = 0;
	unsigned int ecx = 0;
	unsigned int edx = 0;
	if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || (ecx & bit_OSXSAVE) == 0 || (ecx & bit_AVX) == 0)
	{
		return false;
	}
	uint32_t xcr0_low = 0;
	uint32_t xcr0_high = 0;
	__asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
	return (xcr0_low & 6u) == 6u;
#endif
}

bool packet_blocks_segment(const triangle_packet8 &packet, uint32_t count, vec3 origin, vec3 target)
{
	return hit_packet(packet, count, origin, {target.x - origin.x, target.y - origin.y, target.z - origin.z}) != 0;
}

ray_hit segment_blocked(const bvh8_data &data, vec3 origin, vec3 target, uint32_t cached_packet)
{
	const vec3 direction {target.x - origin.x, target.y - origin.y, target.z - origin.z};
	if (cached_packet < data.packets.size() && hit_packet(data.packets[cached_packet], 8, origin, direction) != 0)
	{
		return {true, cached_packet};
	}

	uint32_t stack[512] {};
	uint32_t stack_size = 1;
	stack[0] = 0;
	while (stack_size != 0)
	{
		const uint32_t node_index = stack[--stack_size];
		const bvh8_node &node = data.nodes[node_index];
		uint32_t mask = hit_children(node, origin, direction);
		while (mask != 0)
		{
			const uint32_t lane = std::countr_zero(mask);
			mask &= mask - 1u;
			const uint32_t ref = node.child[lane];
			if (is_leaf_ref(ref))
			{
				const uint32_t packet_index = leaf_index(ref);
				if (packet_index != cached_packet && hit_packet(data.packets[packet_index], leaf_count(ref), origin, direction) != 0)
				{
					return {true, packet_index};
				}
			}
			else if (stack_size < std::size(stack))
			{
				stack[stack_size++] = ref;
			}
			else
			{
				return {};
			}
		}
	}
	return {};
}

} // namespace cs2fow
