import assert from "node:assert/strict";
import {Bvh8Map, Bvh8SurfaceMap, crc32, runtime_origins, shoulder_offset, trace_runtime_rays} from "./bvh8.js";

const headerSize = 256;
const nodeSize = 224;
const packetSize = 288;
const nodesOffset = 256;
const packetsOffset = nodesOffset + nodeSize;
const fileSize = packetsOffset + packetSize;

function fixture()
{
	const buffer = new ArrayBuffer(fileSize);
	const bytes = new Uint8Array(buffer);
	const view = new DataView(buffer);
	bytes.set(new TextEncoder().encode("CS2FOW8\0"), 0);
	view.setUint32(8, 3, true);
	view.setUint32(12, headerSize, true);
	view.setUint32(20, 0x12345678, true);
	view.setBigUint64(24, 42n, true);
	bytes.set(new TextEncoder().encode("test_map\0"), 32);
	[-1, -1, -1].forEach((value, axis) => view.setFloat32(96 + axis * 4, value, true));
	[1, 1, 1].forEach((value, axis) => view.setFloat32(108 + axis * 4, value, true));
	view.setUint32(120, 1, true);
	view.setUint32(124, 1, true);
	view.setUint32(128, 1, true);
	view.setUint32(132, 1, true);
	view.setBigUint64(136, BigInt(nodesOffset), true);
	view.setBigUint64(144, BigInt(packetsOffset), true);
	view.setBigUint64(152, BigInt(fileSize), true);
	view.setUint32(164, 1, true);

	const nodeFloats = new Float32Array(buffer, nodesOffset, nodeSize / 4);
	const nodeRefs = new Uint32Array(buffer, nodesOffset, nodeSize / 4);
	nodeFloats[0] = 0;
	nodeFloats[8] = -1;
	nodeFloats[16] = -1;
	nodeFloats[24] = 0;
	nodeFloats[32] = 1;
	nodeFloats[40] = 1;
	for (let lane = 0; lane < 8; ++lane)
	{
		nodeRefs[48 + lane] = 0xffffffff;
	}
	nodeRefs[48] = 0x80000000;

	const packet = new Float32Array(buffer, packetsOffset, packetSize / 4);
	packet[0] = 0;
	packet[8] = -1;
	packet[16] = -1;
	packet[24] = 0;
	packet[32] = 2;
	packet[40] = 0;
	packet[48] = 0;
	packet[56] = 1;
	packet[64] = 2;
	refresh_crc(buffer);
	return buffer;
}

function refresh_crc(buffer)
{
	new DataView(buffer).setUint32(160, crc32(new Uint8Array(buffer, nodesOffset)), true);
}

function rejected(change, pattern)
{
	const buffer = fixture();
	change(buffer, new DataView(buffer));
	assert.throws(() => new Bvh8Map(buffer), pattern);
}

function surface_fixture(metadata)
{
	const name = new TextEncoder().encode("concrete");
	const lanesOffset = 128 + 2 + name.length;
	const buffer = new ArrayBuffer(lanesOffset + 16);
	const bytes = new Uint8Array(buffer);
	const view = new DataView(buffer);
	bytes.set(new TextEncoder().encode("CS2SURF\0"), 0);
	view.setUint32(8, 1, true);
	view.setUint32(12, 128, true);
	view.setUint32(16, metadata.payloadCrc32, true);
	view.setUint32(20, metadata.packetCount, true);
	view.setUint32(24, metadata.triangleCount, true);
	view.setUint32(28, 1, true);
	bytes.set(new TextEncoder().encode(`${metadata.mapName}\0`), 36);
	view.setBigUint64(100, 128n, true);
	view.setBigUint64(108, BigInt(lanesOffset), true);
	view.setBigUint64(116, BigInt(buffer.byteLength), true);
	view.setUint16(128, name.length, true);
	bytes.set(name, 130);
	for (let lane = 0; lane < 8; ++lane) view.setUint16(lanesOffset + lane * 2, lane ? 0xffff : 0, true);
	view.setUint32(32, crc32(bytes.subarray(128)), true);
	return buffer;
}

const map = new Bvh8Map(fixture());
assert.equal(map.metadata.mapName, "test_map");
assert.equal(map.metadata.triangleCount, 1);
assert.equal(map.triangle_positions(1).length, 9);
assert.equal(map.segment_blocked({x: -1, y: 0, z: 0}, {x: 1, y: 0, z: 0}).blocked, true);
assert.equal(map.segment_blocked({x: -1, y: 2, z: 0}, {x: 1, y: 2, z: 0}).blocked, false);
assert.equal(shoulder_offset(0), 48);
assert.equal(shoulder_offset(200), 128);
assert.equal(runtime_origins(map, {origin: {x: -10, y: 0, z: 0}, yaw: 0, pingMs: 0, buttons: {}}).length, 5);
assert.equal(runtime_origins(map, {origin: {x: -10, y: 0, z: 0}, yaw: 0, pingMs: 0, buttons: {w: true, a: true}}).length, 6);
const traced = trace_runtime_rays(map,
	{origin: {x: -10, y: 0, z: -64}, yaw: 0, pingMs: 0, buttons: {}},
	new Float32Array([10, 0, 0]));
assert.equal(traced.blocked.length, 5);
assert.equal(traced.blocked[0], 1);
const traversal = trace_runtime_rays(map,
	{origin: {x: -10, y: 0, z: -64}, yaw: 0, pingMs: 0, buttons: {}},
	new Float32Array([10, 0, 0]), undefined, true).traversal;
assert.deepEqual([...traversal.triangles], [0], "the exact tested triangle should be exposed for visualization");
assert.equal(traversal.visitedNodes, 1);
assert.equal(traversal.testedPackets, 1);
assert.equal(traversal.testedTriangles, 1);
assert.ok(traversal.boundsTests > 0 && traversal.triangleTests > 0, "traversal work counters should be populated");
const surfaces = new Bvh8SurfaceMap(surface_fixture(map.metadata), map.metadata);
assert.equal(surfaces.name(0, 0), "concrete");
assert.equal(surfaces.name(0, 1), "default");
assert.throws(() => {
	const value = surface_fixture(map.metadata);
	new Uint8Array(value)[value.byteLength - 1] ^= 1;
	return new Bvh8SurfaceMap(value, map.metadata);
}, /CRC32/);
assert.throws(() => new Bvh8SurfaceMap(surface_fixture({...map.metadata, payloadCrc32: 1}), map.metadata), /different/);

rejected((buffer) => { new Uint8Array(buffer)[0] = 0; }, /magic/);
rejected((buffer, view) => { view.setUint32(8, 2, true); }, /version/);
rejected((buffer, view) => { view.setBigUint64(136, 288n, true); }, /offsets/);
rejected((buffer) => { new Uint8Array(buffer)[buffer.byteLength - 1] = 1; }, /CRC32/);
rejected((buffer) => {
	const refs = new Uint32Array(buffer, nodesOffset, nodeSize / 4);
	refs[48] = 0x80000001;
	refresh_crc(buffer);
}, /out of range/);
rejected((buffer) => {
	const values = new Float32Array(buffer, nodesOffset, nodeSize / 4);
	values[0] = Number.NaN;
	refresh_crc(buffer);
}, /bounds/);
rejected((buffer, view) => { view.setUint32(128, 2, true); }, /triangle count/);
assert.throws(() => new Bvh8Map(fixture().slice(0, -1)), /offsets|truncated/);

console.log("BVH8 parser, traversal, surfaces, origins, and rejection checks passed");
