// Browser-neutral reader and scalar tracer for CS2FOW BVH8 version 3 files.
// The production plugin uses AVX for the same bounds and triangle tests.

export const BVH8_INVALID_REF = 0xffffffff;

const k_magic = [0x43, 0x53, 0x32, 0x46, 0x4f, 0x57, 0x38, 0x00];
const k_version = 3;
const k_recipe_version = 1;
const k_header_size = 256;
const k_node_size = 224;
const k_packet_size = 288;
const k_node_float_count = k_node_size / 4;
const k_packet_float_count = k_packet_size / 4;
const k_leaf_ref = 0x80000000;
const k_leaf_index_mask = 0x0fffffff;
const k_ray_epsilon = 1.0e-5;
const k_max_tree_depth = 64;
const k_crc32_table = new Uint32Array(256);
for (let index = 0; index < k_crc32_table.length; ++index)
{
	let value = index;
	for (let bit = 0; bit < 8; ++bit) value = (value >>> 1) ^ (0xedb88320 & (-(value & 1)));
	k_crc32_table[index] = value >>> 0;
}

function require_value(condition, message)
{
	if (!condition)
	{
		throw new Error(message);
	}
}

function finite_bounds(minimum, maximum)
{
	return minimum.every((value, axis) => Number.isFinite(value)
		&& Number.isFinite(maximum[axis]) && value <= maximum[axis]);
}

function safe_uint64(view, offset, label)
{
	const value = view.getBigUint64(offset, true);
	require_value(value <= BigInt(Number.MAX_SAFE_INTEGER), `${label} is too large`);
	return Number(value);
}

function align_up(value, alignment)
{
	return Math.ceil(value / alignment) * alignment;
}

function leaf_count(reference)
{
	return ((reference >>> 28) & 7) + 1;
}

function leaf_index(reference)
{
	return reference & k_leaf_index_mask;
}

function is_leaf(reference)
{
	return reference !== BVH8_INVALID_REF && (reference & k_leaf_ref) !== 0;
}

export function crc32(bytes, previous = 0)
{
	let value = (~previous) >>> 0;
	for (const byte of bytes)
	{
		value = (value >>> 8) ^ k_crc32_table[(value ^ byte) & 0xff];
	}
	return (~value) >>> 0;
}

function read_map_name(bytes)
{
	const end = bytes.indexOf(0);
	require_value(end > 0, "invalid BVH8 map name");
	return new TextDecoder("utf-8", {fatal: true}).decode(bytes.subarray(0, end));
}

export class Bvh8Map
{
	constructor(buffer)
	{
		require_value(buffer instanceof ArrayBuffer && buffer.byteLength >= k_header_size, "BVH8 file is truncated");
		this.buffer = buffer;
		this.view = new DataView(buffer);
		const bytes = new Uint8Array(buffer);
		require_value(k_magic.every((value, index) => bytes[index] === value), "invalid BVH8 magic");
		const version = this.view.getUint32(8, true);
		const headerSize = this.view.getUint32(12, true);
		const flags = this.view.getUint32(16, true);
		const sourceCrc32 = this.view.getUint32(20, true);
		const sourceSize = safe_uint64(this.view, 24, "source size");
		const mapName = read_map_name(bytes.subarray(32, 96));
		const worldMin = [this.view.getFloat32(96, true), this.view.getFloat32(100, true), this.view.getFloat32(104, true)];
		const worldMax = [this.view.getFloat32(108, true), this.view.getFloat32(112, true), this.view.getFloat32(116, true)];
		this.nodeCount = this.view.getUint32(120, true);
		this.packetCount = this.view.getUint32(124, true);
		this.triangleCount = this.view.getUint32(128, true);
		const maxDepth = this.view.getUint32(132, true);
		this.nodesOffset = safe_uint64(this.view, 136, "node offset");
		this.packetsOffset = safe_uint64(this.view, 144, "packet offset");
		const fileSize = safe_uint64(this.view, 152, "file size");
		const payloadCrc32 = this.view.getUint32(160, true);
		const recipeVersion = this.view.getUint32(164, true);

		require_value(version === k_version && headerSize === k_header_size && recipeVersion === k_recipe_version,
			"invalid BVH8 version, header, or bake recipe");
		require_value((flags & ~1) === 0, "BVH8 contains unsupported flags");
		require_value(bytes.subarray(168, 256).every((value) => value === 0), "BVH8 reserved header bytes are not zero");
		require_value(this.nodeCount > 0 && this.packetCount > 0 && this.triangleCount > 0
			&& maxDepth > 0 && maxDepth <= k_max_tree_depth && this.triangleCount <= this.packetCount * 8,
			"invalid BVH8 counts or depth");
		require_value(finite_bounds(worldMin, worldMax), "invalid BVH8 world bounds");
		const expectedNodes = align_up(k_header_size, 32);
		const expectedPackets = align_up(expectedNodes + this.nodeCount * k_node_size, 32);
		const expectedSize = expectedPackets + this.packetCount * k_packet_size;
		require_value(Number.isSafeInteger(expectedSize) && this.nodesOffset === expectedNodes
			&& this.packetsOffset === expectedPackets && fileSize === expectedSize && buffer.byteLength === expectedSize,
			"BVH8 offsets or file size are invalid");
		require_value(crc32(bytes.subarray(this.nodesOffset, fileSize)) === payloadCrc32, "BVH8 payload CRC32 does not match");

		this.nodeFloats = new Float32Array(buffer, this.nodesOffset, this.nodeCount * k_node_float_count);
		this.nodeRefs = new Uint32Array(buffer, this.nodesOffset, this.nodeCount * k_node_float_count);
		this.packetFloats = new Float32Array(buffer, this.packetsOffset, this.packetCount * k_packet_float_count);
		this.packetTriangleCounts = new Uint8Array(this.packetCount);
		this.validate_tree(maxDepth);
		this.packetTriangleOffsets = new Uint32Array(this.packetCount);
		for (let packet = 1; packet < this.packetCount; ++packet)
		{
			this.packetTriangleOffsets[packet] = this.packetTriangleOffsets[packet - 1] + this.packetTriangleCounts[packet - 1];
		}
		this.metadata = {
			mapName,
			flags,
			sourceCrc32,
			sourceSize,
			worldMin,
			worldMax,
			nodeCount: this.nodeCount,
			packetCount: this.packetCount,
			triangleCount: this.triangleCount,
			maxDepth,
			payloadCrc32,
			fileSize
		};
		this.stack = new Uint32Array(512);
	}

	validate_tree(expectedDepth)
	{
		const visitedNodes = new Uint8Array(this.nodeCount);
		const visitedPackets = new Uint8Array(this.packetCount);
		const pendingNodes = [0];
		const pendingDepths = [1];
		visitedNodes[0] = 1;
		let nodes = 0;
		let packets = 0;
		let triangles = 0;
		let maxDepth = 0;
		while (pendingNodes.length)
		{
			const node = pendingNodes.pop();
			const depth = pendingDepths.pop();
			++nodes;
			maxDepth = Math.max(maxDepth, depth);
			require_value(depth <= k_max_tree_depth, "BVH8 tree is too deep");
			const base = node * k_node_float_count;
			for (let lane = 0; lane < 8; ++lane)
			{
				const reference = this.nodeRefs[base + 48 + lane];
				if (reference === BVH8_INVALID_REF)
				{
					continue;
				}
				const minimum = [this.nodeFloats[base + lane], this.nodeFloats[base + 8 + lane], this.nodeFloats[base + 16 + lane]];
				const maximum = [this.nodeFloats[base + 24 + lane], this.nodeFloats[base + 32 + lane], this.nodeFloats[base + 40 + lane]];
				require_value(finite_bounds(minimum, maximum), "BVH8 contains invalid child bounds");
				if (is_leaf(reference))
				{
					const packet = leaf_index(reference);
					require_value(packet < this.packetCount, "BVH8 leaf reference is out of range");
					require_value(visitedPackets[packet] === 0, "BVH8 packet has more than one parent");
					visitedPackets[packet] = 1;
					this.packetTriangleCounts[packet] = leaf_count(reference);
					++packets;
					triangles += this.packetTriangleCounts[packet];
				}
				else
				{
					require_value(reference < this.nodeCount && reference > node, "BVH8 node reference is invalid");
					require_value(visitedNodes[reference] === 0, "BVH8 node has more than one parent");
					visitedNodes[reference] = 1;
					pendingNodes.push(reference);
					pendingDepths.push(depth + 1);
				}
			}
		}
		require_value(nodes === this.nodeCount && packets === this.packetCount, "BVH8 contains unreachable nodes or packets");
		require_value(triangles === this.triangleCount, "BVH8 triangle count is inconsistent");
		require_value(maxDepth === expectedDepth, "BVH8 depth is inconsistent");
		for (let packet = 0; packet < this.packetCount; ++packet)
		{
			const base = packet * k_packet_float_count;
			for (let component = 0; component < 9; ++component)
			{
				for (let lane = 0; lane < this.packetTriangleCounts[packet]; ++lane)
				{
					require_value(Number.isFinite(this.packetFloats[base + component * 8 + lane]), "BVH8 contains invalid triangle data");
				}
			}
		}
	}

	triangle_positions_for(indices, unitsPerMeter)
	{
		const positions = new Float32Array(indices.length * 9);
		let output = 0;
		const put = (x, y, z) =>
		{
			positions[output++] = y / unitsPerMeter;
			positions[output++] = z / unitsPerMeter;
			positions[output++] = x / unitsPerMeter;
		};
		let packet = 0;
		for (const triangleIndex of indices)
		{
			while (packet + 1 < this.packetCount && this.packetTriangleOffsets[packet + 1] <= triangleIndex) ++packet;
			const lane = triangleIndex - this.packetTriangleOffsets[packet];
			require_value(triangleIndex < this.triangleCount && lane < this.packetTriangleCounts[packet], "triangle index is out of range");
			const base = packet * k_packet_float_count;
			const x = this.packetFloats[base + lane];
			const y = this.packetFloats[base + 8 + lane];
			const z = this.packetFloats[base + 16 + lane];
			const e1x = this.packetFloats[base + 24 + lane];
			const e1y = this.packetFloats[base + 32 + lane];
			const e1z = this.packetFloats[base + 40 + lane];
			const e2x = this.packetFloats[base + 48 + lane];
			const e2y = this.packetFloats[base + 56 + lane];
			const e2z = this.packetFloats[base + 64 + lane];
			put(x, y, z);
			put(x + e1x, y + e1y, z + e1z);
			put(x + e2x, y + e2y, z + e2z);
		}
		return positions;
	}

	triangle_positions(unitsPerMeter, maximumTriangles = this.triangleCount)
	{
		const count = Math.min(this.triangleCount, Math.max(1, Math.floor(maximumTriangles)));
		const indices = new Uint32Array(count);
		for (let index = 0; index < count; ++index) indices[index] = Math.floor(index * this.triangleCount / count);
		return this.triangle_positions_for(indices, unitsPerMeter);
	}

	packet_hit(packet, count, origin, direction, traversal = null)
	{
		if (traversal)
		{
			++traversal.packetTests;
			traversal.packets.add(packet);
		}
		const base = packet * k_packet_float_count;
		for (let lane = 0; lane < count; ++lane)
		{
			if (traversal)
			{
				++traversal.triangleTests;
				if (lane < this.packetTriangleCounts[packet]) traversal.triangles.add(this.packetTriangleOffsets[packet] + lane);
			}
			const e1x = this.packetFloats[base + 24 + lane];
			const e1y = this.packetFloats[base + 32 + lane];
			const e1z = this.packetFloats[base + 40 + lane];
			const e2x = this.packetFloats[base + 48 + lane];
			const e2y = this.packetFloats[base + 56 + lane];
			const e2z = this.packetFloats[base + 64 + lane];
			const px = direction.y * e2z - direction.z * e2y;
			const py = direction.z * e2x - direction.x * e2z;
			const pz = direction.x * e2y - direction.y * e2x;
			const determinant = e1x * px + e1y * py + e1z * pz;
			if (Math.abs(determinant) <= k_ray_epsilon)
			{
				continue;
			}
			const inverse = 1 / determinant;
			const tx = origin.x - this.packetFloats[base + lane];
			const ty = origin.y - this.packetFloats[base + 8 + lane];
			const tz = origin.z - this.packetFloats[base + 16 + lane];
			const u = (tx * px + ty * py + tz * pz) * inverse;
			if (u < 0 || u > 1)
			{
				continue;
			}
			const qx = ty * e1z - tz * e1y;
			const qy = tz * e1x - tx * e1z;
			const qz = tx * e1y - ty * e1x;
			const v = (direction.x * qx + direction.y * qy + direction.z * qz) * inverse;
			if (v < 0 || u + v > 1)
			{
				continue;
			}
			const distance = (e2x * qx + e2y * qy + e2z * qz) * inverse;
			if (distance > k_ray_epsilon && distance < 1 - k_ray_epsilon)
			{
				return true;
			}
		}
		return false;
	}

	packet_nearest_hit(packet, count, origin, direction, maximum = 1)
	{
		const base = packet * k_packet_float_count;
		let nearest = maximum;
		let nearestLane = -1;
		for (let lane = 0; lane < count; ++lane)
		{
			const e1x = this.packetFloats[base + 24 + lane];
			const e1y = this.packetFloats[base + 32 + lane];
			const e1z = this.packetFloats[base + 40 + lane];
			const e2x = this.packetFloats[base + 48 + lane];
			const e2y = this.packetFloats[base + 56 + lane];
			const e2z = this.packetFloats[base + 64 + lane];
			const px = direction.y * e2z - direction.z * e2y;
			const py = direction.z * e2x - direction.x * e2z;
			const pz = direction.x * e2y - direction.y * e2x;
			const determinant = e1x * px + e1y * py + e1z * pz;
			if (Math.abs(determinant) <= k_ray_epsilon) continue;
			const inverse = 1 / determinant;
			const tx = origin.x - this.packetFloats[base + lane];
			const ty = origin.y - this.packetFloats[base + 8 + lane];
			const tz = origin.z - this.packetFloats[base + 16 + lane];
			const u = (tx * px + ty * py + tz * pz) * inverse;
			if (u < 0 || u > 1) continue;
			const qx = ty * e1z - tz * e1y;
			const qy = tz * e1x - tx * e1z;
			const qz = tx * e1y - ty * e1x;
			const v = (direction.x * qx + direction.y * qy + direction.z * qz) * inverse;
			if (v < 0 || u + v > 1) continue;
			const fraction = (e2x * qx + e2y * qy + e2z * qz) * inverse;
			if (fraction > k_ray_epsilon && fraction < nearest - k_ray_epsilon)
			{
				nearest = fraction;
				nearestLane = lane;
			}
		}
		return nearestLane < 0 ? null : {packet, lane: nearestLane, fraction: nearest};
	}

	triangle(packet, lane)
	{
		const base = packet * k_packet_float_count;
		const v0 = {
			x: this.packetFloats[base + lane],
			y: this.packetFloats[base + 8 + lane],
			z: this.packetFloats[base + 16 + lane]
		};
		const v1 = {
			x: v0.x + this.packetFloats[base + 24 + lane],
			y: v0.y + this.packetFloats[base + 32 + lane],
			z: v0.z + this.packetFloats[base + 40 + lane]
		};
		const v2 = {
			x: v0.x + this.packetFloats[base + 48 + lane],
			y: v0.y + this.packetFloats[base + 56 + lane],
			z: v0.z + this.packetFloats[base + 64 + lane]
		};
		return {v0, v1, v2};
	}

	child_hit(node, lane, origin, direction)
	{
		const base = node * k_node_float_count;
		let near = -Infinity;
		let far = Infinity;
		for (let axis = 0; axis < 3; ++axis)
		{
			const position = [origin.x, origin.y, origin.z][axis];
			const delta = [direction.x, direction.y, direction.z][axis];
			const minimum = this.nodeFloats[base + axis * 8 + lane];
			const maximum = this.nodeFloats[base + (axis + 3) * 8 + lane];
			if (Math.abs(delta) < 1.0e-12)
			{
				if (position < minimum || position > maximum)
				{
					return false;
				}
				continue;
			}
			const inverse = 1 / delta;
			const a = (minimum - position) * inverse;
			const b = (maximum - position) * inverse;
			near = Math.max(near, Math.min(a, b));
			far = Math.min(far, Math.max(a, b));
		}
		return far >= Math.max(near, 0) && near < 1 - k_ray_epsilon;
	}

	segment_blocked(origin, target, cachedPacket = BVH8_INVALID_REF, traversal = null)
	{
		if (![origin.x, origin.y, origin.z, target.x, target.y, target.z].every(Number.isFinite))
		{
			return {blocked: false, packet: BVH8_INVALID_REF};
		}
		const direction = {x: target.x - origin.x, y: target.y - origin.y, z: target.z - origin.z};
		if (cachedPacket < this.packetCount)
		{
			if (traversal) ++traversal.cacheTests;
			if (this.packet_hit(cachedPacket, 8, origin, direction, traversal))
			{
				if (traversal) ++traversal.cacheHits;
				return {blocked: true, packet: cachedPacket};
			}
		}
		let stackSize = 1;
		this.stack[0] = 0;
		while (stackSize)
		{
			const node = this.stack[--stackSize];
			if (traversal)
			{
				++traversal.nodeVisits;
				traversal.nodes.add(node);
			}
			const base = node * k_node_float_count;
			for (let lane = 0; lane < 8; ++lane)
			{
				const reference = this.nodeRefs[base + 48 + lane];
				if (reference === BVH8_INVALID_REF)
				{
					continue;
				}
				if (traversal) ++traversal.boundsTests;
				if (!this.child_hit(node, lane, origin, direction)) continue;
				if (traversal) ++traversal.boundsHits;
				if (is_leaf(reference))
				{
					const packet = leaf_index(reference);
					if (packet !== cachedPacket && this.packet_hit(packet, leaf_count(reference), origin, direction, traversal))
					{
						return {blocked: true, packet};
					}
				}
				else if (stackSize < this.stack.length)
				{
					this.stack[stackSize++] = reference;
				}
				else
				{
					return {blocked: false, packet: BVH8_INVALID_REF};
				}
			}
		}
		return {blocked: false, packet: BVH8_INVALID_REF};
	}

	create_traversal()
	{
		return {nodes: new Set(), packets: new Set(), triangles: new Set(), nodeVisits: 0, packetTests: 0,
			triangleTests: 0, boundsTests: 0, boundsHits: 0, cacheTests: 0, cacheHits: 0};
	}

	finish_traversal(value)
	{
		return {
			triangles: new Uint32Array([...value.triangles].sort((a, b) => a - b)),
			visitedNodes: value.nodes.size,
			testedPackets: value.packets.size,
			testedTriangles: value.triangles.size,
			nodeVisits: value.nodeVisits,
			packetTests: value.packetTests,
			triangleTests: value.triangleTests,
			boundsTests: value.boundsTests,
			boundsHits: value.boundsHits,
			cacheTests: value.cacheTests,
			cacheHits: value.cacheHits
		};
	}

	segment_hit(origin, target)
	{
		if (![origin.x, origin.y, origin.z, target.x, target.y, target.z].every(Number.isFinite)) return null;
		const direction = {x: target.x - origin.x, y: target.y - origin.y, z: target.z - origin.z};
		let nearest = null;
		let stackSize = 1;
		this.stack[0] = 0;
		while (stackSize)
		{
			const node = this.stack[--stackSize];
			const base = node * k_node_float_count;
			for (let lane = 0; lane < 8; ++lane)
			{
				const reference = this.nodeRefs[base + 48 + lane];
				if (reference === BVH8_INVALID_REF || !this.child_hit(node, lane, origin, direction)) continue;
				if (is_leaf(reference))
				{
					const hit = this.packet_nearest_hit(leaf_index(reference), leaf_count(reference), origin, direction,
						nearest?.fraction ?? 1);
					if (hit) nearest = hit;
				}
				else if (stackSize < this.stack.length)
				{
					this.stack[stackSize++] = reference;
				}
			}
		}
		if (!nearest) return null;
		const triangle = this.triangle(nearest.packet, nearest.lane);
		const edge1 = subtract(triangle.v1, triangle.v0);
		const edge2 = subtract(triangle.v2, triangle.v0);
		let normal = {
			x: edge1.y * edge2.z - edge1.z * edge2.y,
			y: edge1.z * edge2.x - edge1.x * edge2.z,
			z: edge1.x * edge2.y - edge1.y * edge2.x
		};
		const length = Math.hypot(normal.x, normal.y, normal.z) || 1;
		normal = scale(normal, 1 / length);
		if (normal.x * direction.x + normal.y * direction.y + normal.z * direction.z > 0) normal = scale(normal, -1);
		return {
			...nearest,
			point: add(origin, scale(direction, nearest.fraction)),
			normal
		};
	}

	for_each_triangle_in_bounds(minimum, maximum, callback)
	{
		const overlaps = (node, lane) =>
		{
			const base = node * k_node_float_count;
			return this.nodeFloats[base + lane] <= maximum.x && this.nodeFloats[base + 24 + lane] >= minimum.x
				&& this.nodeFloats[base + 8 + lane] <= maximum.y && this.nodeFloats[base + 32 + lane] >= minimum.y
				&& this.nodeFloats[base + 16 + lane] <= maximum.z && this.nodeFloats[base + 40 + lane] >= minimum.z;
		};
		let stackSize = 1;
		this.stack[0] = 0;
		while (stackSize)
		{
			const node = this.stack[--stackSize];
			const base = node * k_node_float_count;
			for (let lane = 0; lane < 8; ++lane)
			{
				const reference = this.nodeRefs[base + 48 + lane];
				if (reference === BVH8_INVALID_REF || !overlaps(node, lane)) continue;
				if (is_leaf(reference))
				{
					const packet = leaf_index(reference);
					for (let triangleLane = 0; triangleLane < leaf_count(reference); ++triangleLane)
					{
						if (callback(packet, triangleLane, this.triangle(packet, triangleLane)) === false) return false;
					}
				}
				else if (stackSize < this.stack.length)
				{
					this.stack[stackSize++] = reference;
				}
			}
		}
		return true;
	}

	for_each_packet_triangle(packet, callback, traversal = null)
	{
		if (!Number.isInteger(packet) || packet < 0 || packet >= this.packetCount) return false;
		if (traversal)
		{
			++traversal.packetTests;
			traversal.packets.add(packet);
		}
		for (let lane = 0; lane < this.packetTriangleCounts[packet]; ++lane)
		{
			if (traversal)
			{
				++traversal.triangleTests;
				traversal.triangles.add(this.packetTriangleOffsets[packet] + lane);
			}
			if (callback(packet, lane, this.triangle(packet, lane)) === false) return false;
		}
		return true;
	}

	for_each_triangle_in_view(view, callback, traversal = null, stats = null, afterLeaf = null)
	{
		const radius = (axis, extent) => Math.abs(axis.x) * extent.x
			+ Math.abs(axis.y) * extent.y + Math.abs(axis.z) * extent.z;
		const intersects = (node, lane) =>
		{
			const base = node * k_node_float_count;
			const minimum = {x: this.nodeFloats[base + lane], y: this.nodeFloats[base + 8 + lane],
				z: this.nodeFloats[base + 16 + lane]};
			const maximum = {x: this.nodeFloats[base + 24 + lane], y: this.nodeFloats[base + 32 + lane],
				z: this.nodeFloats[base + 40 + lane]};
			const center = scale(add(minimum, maximum), 0.5);
			const extent = scale(subtract(maximum, minimum), 0.5);
			const offset = subtract(center, view.origin);
			const depth = offset.x * view.forward.x + offset.y * view.forward.y + offset.z * view.forward.z;
			const x = offset.x * view.right.x + offset.y * view.right.y + offset.z * view.right.z;
			const y = offset.x * view.up.x + offset.y * view.up.y + offset.z * view.up.z;
			const depthRadius = radius(view.forward, extent);
			const rightRadius = radius(view.right, extent);
			const upRadius = radius(view.up, extent);
			return depth + depthRadius >= view.nearDepth && depth - depthRadius <= view.farDepth
				&& view.horizontal * depth + x + view.horizontal * depthRadius + rightRadius >= 0
				&& view.horizontal * depth - x + view.horizontal * depthRadius + rightRadius >= 0
				&& view.vertical * depth + y + view.vertical * depthRadius + upRadius >= 0
				&& view.vertical * depth - y + view.vertical * depthRadius + upRadius >= 0
				? depth - depthRadius : null;
		};
		const queue = [];
		const push = (entry) =>
		{
			let index = queue.length;
			queue.push(entry);
			while (index > 0)
			{
				const parent = (index - 1) >> 1;
				if (queue[parent].nearDepth <= entry.nearDepth) break;
				queue[index] = queue[parent];
				index = parent;
			}
			queue[index] = entry;
		};
		const pop = () =>
		{
			const result = queue[0];
			const last = queue.pop();
			if (queue.length)
			{
				let index = 0;
				while (index * 2 + 1 < queue.length)
				{
					let child = index * 2 + 1;
					if (child + 1 < queue.length && queue[child + 1].nearDepth < queue[child].nearDepth) ++child;
					if (queue[child].nearDepth >= last.nearDepth) break;
					queue[index] = queue[child];
					index = child;
				}
				queue[index] = last;
			}
			return result;
		};
		push({reference: 0, nearDepth: -Infinity});
		while (queue.length)
		{
			const {reference} = pop();
			if (is_leaf(reference))
			{
				const packet = leaf_index(reference);
				if (!this.for_each_packet_triangle(packet, callback, traversal)) return false;
				if (afterLeaf?.(packet) === false) return false;
				continue;
			}
			const node = reference;
			if (stats) ++stats.visitedNodes;
			if (traversal)
			{
				++traversal.nodeVisits;
				traversal.nodes.add(node);
			}
			const base = node * k_node_float_count;
			for (let lane = 0; lane < 8; ++lane)
			{
				const reference = this.nodeRefs[base + 48 + lane];
				if (reference === BVH8_INVALID_REF) continue;
				if (traversal) ++traversal.boundsTests;
				const nearDepth = intersects(node, lane);
				if (nearDepth === null) continue;
				if (traversal) ++traversal.boundsHits;
				push({reference, nearDepth});
			}
		}
		return true;
	}
}

export class Bvh8SurfaceMap
{
	constructor(buffer, expected)
	{
		require_value(buffer instanceof ArrayBuffer && buffer.byteLength >= 128, "surface sidecar is truncated");
		const bytes = new Uint8Array(buffer);
		const view = new DataView(buffer);
		const magic = [0x43, 0x53, 0x32, 0x53, 0x55, 0x52, 0x46, 0x00];
		require_value(magic.every((value, index) => bytes[index] === value), "invalid surface sidecar magic");
		require_value(view.getUint32(8, true) === 1 && view.getUint32(12, true) === 128,
			"invalid surface sidecar version");
		const payloadCrc32 = view.getUint32(16, true);
		const packetCount = view.getUint32(20, true);
		const triangleCount = view.getUint32(24, true);
		const surfaceCount = view.getUint32(28, true);
		const sidecarCrc32 = view.getUint32(32, true);
		const mapName = read_map_name(bytes.subarray(36, 100));
		const stringsOffset = safe_uint64(view, 100, "surface string offset");
		const lanesOffset = safe_uint64(view, 108, "surface lane offset");
		const fileSize = safe_uint64(view, 116, "surface file size");
		require_value(bytes.subarray(124, 128).every((value) => value === 0), "surface sidecar reserved bytes are not zero");
		require_value(stringsOffset === 128 && lanesOffset >= stringsOffset && fileSize === buffer.byteLength
			&& lanesOffset + packetCount * 16 === fileSize, "surface sidecar offsets are invalid");
		require_value(crc32(bytes.subarray(128)) === sidecarCrc32, "surface sidecar CRC32 does not match");
		if (expected)
		{
			require_value(mapName === expected.mapName && payloadCrc32 === expected.payloadCrc32
				&& packetCount === expected.packetCount && triangleCount === expected.triangleCount,
				"surface sidecar belongs to a different BVH8 map");
		}
		this.names = [];
		const decoder = new TextDecoder("utf-8", {fatal: true});
		let offset = stringsOffset;
		for (let index = 0; index < surfaceCount; ++index)
		{
			require_value(offset + 2 <= lanesOffset, "surface name table is truncated");
			const size = view.getUint16(offset, true);
			offset += 2;
			require_value(size > 0 && offset + size <= lanesOffset, "surface name is invalid");
			this.names.push(decoder.decode(bytes.subarray(offset, offset + size)));
			offset += size;
		}
		require_value(offset === lanesOffset, "surface name table has trailing data");
		if (lanesOffset % Uint16Array.BYTES_PER_ELEMENT === 0)
		{
			this.lanes = new Uint16Array(buffer, lanesOffset, packetCount * 8);
		}
		else
		{
			this.lanes = new Uint16Array(packetCount * 8);
			for (let index = 0; index < this.lanes.length; ++index)
				this.lanes[index] = view.getUint16(lanesOffset + index * 2, true);
		}
		for (const id of this.lanes) require_value(id === 0xffff || id < this.names.length, "surface lane ID is invalid");
		this.metadata = {mapName, payloadCrc32, packetCount, triangleCount, surfaceCount};
	}

	name(packet, lane)
	{
		if (!Number.isInteger(packet) || packet < 0 || packet * 8 + lane >= this.lanes.length) return "default";
		const id = this.lanes[packet * 8 + lane];
		return id === 0xffff ? "default" : this.names[id];
	}
}

function add(a, b)
{
	return {x: a.x + b.x, y: a.y + b.y, z: a.z + b.z};
}

function subtract(a, b)
{
	return {x: a.x - b.x, y: a.y - b.y, z: a.z - b.z};
}

function scale(value, amount)
{
	return {x: value.x * amount, y: value.y * amount, z: value.z * amount};
}

function distance_sq(a, b)
{
	const value = subtract(a, b);
	return value.x * value.x + value.y * value.y + value.z * value.z;
}

function add_unique(points, point)
{
	if (!points.some((existing) => distance_sq(existing, point) <= 1.0e-4))
	{
		points.push(point);
	}
}

function safe_origin(map, eye, candidate, traversal = null)
{
	return distance_sq(eye, candidate) <= 1.0e-4 || map.segment_blocked(eye, candidate, BVH8_INVALID_REF, traversal).blocked ? eye : candidate;
}

function clip_destination(map, origin, destination, traversal = null)
{
	if (distance_sq(origin, destination) <= 1.0e-4 || !map.segment_blocked(origin, destination, BVH8_INVALID_REF, traversal).blocked)
	{
		return destination;
	}
	let clear = origin;
	let blocked = destination;
	for (let step = 0; step < 8; ++step)
	{
		const middle = scale(add(clear, blocked), 0.5);
		if (map.segment_blocked(origin, middle, BVH8_INVALID_REF, traversal).blocked)
		{
			blocked = middle;
		}
		else
		{
			clear = middle;
		}
	}
	return clear;
}

export function shoulder_offset(pingMs, tuning = {})
{
	const requestedBase = Number(tuning.shoulderBase);
	const requestedScale = Number(tuning.shoulderRttScale);
	const requestedMaximum = Number(tuning.maxShoulder);
	const base = Math.min(256, Math.max(0, Number.isFinite(requestedBase) ? requestedBase : 48));
	const scalePerMs = Math.min(4, Math.max(0, Number.isFinite(requestedScale) ? requestedScale : 0.4));
	const maximum = Math.max(base, Math.min(256, Math.max(0, Number.isFinite(requestedMaximum) ? requestedMaximum : 128)));
	const stepped = Math.floor(Math.max(0, pingMs) / 25) * 25;
	return Math.min(maximum, base + stepped * scalePerMs);
}

export function runtime_origins(map, viewer, traversal = null)
{
	const yaw = viewer.yaw * Math.PI / 180;
	const forward = {x: Math.cos(yaw), y: Math.sin(yaw), z: 0};
	const right = {x: Math.sin(yaw), y: -Math.cos(yaw), z: 0};
	const offset = shoulder_offset(viewer.pingMs, viewer.tuning);
	const eye = viewer.eye || add(viewer.origin, {x: 0, y: 0, z: Number(viewer.eyeHeight) || 64});
	const origins = [];
	add_unique(origins, eye);
	add_unique(origins, safe_origin(map, eye, subtract(eye, scale(right, offset)), traversal));
	add_unique(origins, safe_origin(map, eye, add(eye, scale(right, offset)), traversal));
	add_unique(origins, safe_origin(map, eye, add(eye, {x: 0, y: 0, z: 16}), traversal));
	add_unique(origins, viewer.origin);
	const forwardInput = Number(Boolean(viewer.buttons?.w)) - Number(Boolean(viewer.buttons?.s));
	const sideInput = Number(Boolean(viewer.buttons?.d)) - Number(Boolean(viewer.buttons?.a));
	if (forwardInput !== 0)
	{
		let direction = add(scale(forward, forwardInput), scale(right, sideInput));
		const length = Math.hypot(direction.x, direction.y);
		direction = scale(direction, offset / length);
		add_unique(origins, clip_destination(map, eye, add(eye, direction), traversal));
	}
	return origins;
}

export function trace_runtime_rays(map, viewer, targetValues, cachedPackets, captureTraversal = false)
{
	const targets = [];
	for (let index = 0; index < targetValues.length; index += 3)
	{
		targets.push({x: targetValues[index], y: targetValues[index + 1], z: targetValues[index + 2]});
	}
	const traversal = captureTraversal ? map.create_traversal() : null;
	const origins = runtime_origins(map, viewer, traversal);
	const rayCount = origins.length * targets.length;
	const cache = cachedPackets?.length === rayCount ? cachedPackets : new Uint32Array(rayCount).fill(BVH8_INVALID_REF);
	const blocked = new Uint8Array(rayCount);
	let ray = 0;
	let clearCount = 0;
	for (const origin of origins)
	{
		for (const target of targets)
		{
			const result = map.segment_blocked(origin, target, cache[ray], traversal);
			blocked[ray] = Number(result.blocked);
			cache[ray] = result.packet;
			clearCount += Number(!result.blocked);
			++ray;
		}
	}
	const originValues = new Float32Array(origins.length * 3);
	origins.forEach((origin, index) => originValues.set([origin.x, origin.y, origin.z], index * 3));
	return {origins: originValues, blocked, cache, clearCount, visible: clearCount > 0,
		traversal: traversal ? map.finish_traversal(traversal) : null};
}
