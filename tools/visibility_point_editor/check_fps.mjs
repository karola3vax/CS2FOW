import assert from "node:assert/strict";
import {
	FPS_CONSTANTS, FPS_DT, FpsSimulation, make_test_smoke, move_actor,
	resolve_capsule, route_between, smoke_line_blocked, validate_nav
} from "./fps_runtime.js";

const floor = {
	v0: {x: -1000, y: -1000, z: 0},
	v1: {x: 1000, y: -1000, z: 0},
	v2: {x: 0, y: 1000, z: 0}
};
const wall = {
	v0: {x: 0, y: -1000, z: -100},
	v1: {x: 0, y: 1000, z: -100},
	v2: {x: 0, y: 0, z: 200}
};

function test_map(triangles = [])
{
	return {
		metadata: {mapName: "test", worldMin: [-2000, -2000, -2000], worldMax: [2000, 2000, 2000]},
		for_each_triangle_in_bounds(_minimum, _maximum, callback)
		{
			triangles.forEach((triangle, index) => callback(index, 0, triangle));
		},
		segment_hit(start, end)
		{
			if (start.z >= 0 && end.z <= 0)
			{
				const amount = start.z / (start.z - end.z);
				return {packet: 0, lane: 0, fraction: amount, point: {
					x: start.x + (end.x - start.x) * amount,
					y: start.y + (end.y - start.y) * amount,
					z: 0
				}, normal: {x: 0, y: 0, z: 1}};
			}
			return null;
		},
		segment_blocked(_start, _end, _cache, traversal)
		{
			if (traversal) ++traversal.triangleTests;
			return {blocked: false, packet: 0xffffffff};
		},
		create_traversal() { return {triangleTests: 0}; },
		finish_traversal(value) { return {triangles: new Uint32Array(), testedTriangles: value.triangleTests}; },
		surfaceMap: {name: () => "metal_auto"}
	};
}

function actor(origin = {x: 64, y: 0, z: 0.02})
{
	return {
		origin: {...origin}, velocity: {x: 0, y: 0, z: 0}, yaw: 180, pitch: 0,
		grounded: true, crouched: false, lastSafe: {...origin}, surface: "default", stance: "standing", speed: 0
	};
}

assert.equal(FPS_DT, 1 / 64);
assert.deepEqual(
	["accelerate", "airAccelerate", "airWishSpeed", "friction", "gravity", "jumpImpulse", "stopSpeed", "globalMaxSpeed", "radius", "standingHeight", "crouchedHeight", "stepHeight", "maxSlopeDegrees"]
		.map((key) => FPS_CONSTANTS[key]),
	[5.5, 12, 30, 5.2, 800, 301.993, 80, 320, 16, 71, 35.5, 16, 50]
);

const collision = resolve_capsule(test_map([wall]), {x: 8, y: 0, z: 0}, FPS_CONSTANTS.standingHeight);
assert.ok(collision.position.x >= 15.99, "capsule should be pushed out of a wall");

const runner = actor();
for (let tick = 0; tick < 64; ++tick) move_actor(test_map([floor]), runner, {w: true}, 225);
assert.ok(runner.speed > 0 && runner.speed <= 225.01, "ground acceleration should move without exceeding weapon speed");
assert.equal(runner.surface, "metal_auto");
move_actor(test_map([floor]), runner, {jump: true}, 225);
assert.ok(runner.velocity.z > 280 && !runner.grounded, "jump impulse should leave the ground");

const bunny = actor();
bunny.velocity.x = 200;
move_actor(test_map([floor]), bunny, {jump: true}, 225);
assert.ok(bunny.speed >= 199.9, "a queued landing jump should preserve horizontal speed");

const strafer = actor({x: 64, y: 0, z: 128});
strafer.grounded = false;
strafer.velocity = {x: 225, y: 0, z: 0};
strafer.yaw = 0;
for (let tick = 0; tick < 32; ++tick) move_actor(test_map(), strafer, {d: true}, 225);
assert.ok(Math.abs(strafer.velocity.y) <= FPS_CONSTANTS.airWishSpeed + 0.01,
	"air strafe should cap added speed along the wish direction");
assert.ok(strafer.speed <= FPS_CONSTANTS.globalMaxSpeed + 0.01,
	"air strafe should never exceed the global speed cap");

const climber = actor({x: 64, y: 0, z: 64});
climber.grounded = false;
move_actor(test_map(), climber, {w: true, ladder: true, ladderDirection: 1}, 225);
assert.equal(climber.stance, "ladder", "ladder movement should enter a gravity-free ladder state");
assert.ok(climber.velocity.z > 140, "ladder movement should climb instead of sliding down");

const stepTriangles = [
	{v0: {x: 0, y: -100, z: 0}, v1: {x: 0, y: 100, z: 0}, v2: {x: 0, y: 100, z: 12}},
	{v0: {x: 0, y: -100, z: 0}, v1: {x: 0, y: 100, z: 12}, v2: {x: 0, y: -100, z: 12}},
	{v0: {x: 0, y: -100, z: 12}, v1: {x: 200, y: -100, z: 12}, v2: {x: 200, y: 100, z: 12}},
	{v0: {x: 0, y: -100, z: 12}, v1: {x: 200, y: 100, z: 12}, v2: {x: 0, y: 100, z: 12}}
];
const stepMap = test_map(stepTriangles);
stepMap.segment_hit = (start, end) =>
{
	const height = start.x >= 0 ? 12 : 0;
	if (start.z < height || end.z > height) return null;
	const amount = (start.z - height) / (start.z - end.z);
	return {packet: 0, lane: 0, fraction: amount, point: {x: start.x, y: start.y, z: height}, normal: {x: 0, y: 0, z: 1}};
};
for (const [yaw, buttons] of [[0, {w: true}], [180, {s: true}], [90, {d: true}], [270, {a: true}]])
{
	const stepper = actor({x: -20, y: 0, z: 0.02});
	stepper.yaw = yaw;
	for (let tick = 0; tick < 96; ++tick) move_actor(stepMap, stepper, buttons, 225);
	assert.ok(stepper.origin.x > 0 && stepper.origin.z >= 12, "stair lips should step up in every movement direction");
}

const smoke = make_test_smoke({x: 0, y: 0, z: 64}, 0);
const rayStart = {x: -240, y: 0, z: 64};
const rayEnd = {x: 240, y: 0, z: 64};
const openMap = test_map();
assert.equal(smoke_line_blocked(openMap, [smoke], [], rayStart, rayEnd, 0.5), false, "visual margin delays smoke blocking");
assert.equal(smoke_line_blocked(openMap, [smoke], [], rayStart, rayEnd, 3), true, "dense mature smoke blocks");
assert.equal(smoke_line_blocked(openMap, [smoke], [], rayStart, rayEnd, 3,
	[{start: rayStart, end: rayEnd, time: 2.9}]), false, "a recent bullet carves a temporary channel");
assert.equal(smoke_line_blocked(openMap, [smoke], [], rayStart, rayEnd, 3.7,
	[{start: rayStart, end: rayEnd, time: 2.9}]), true, "a bullet channel recovers after 0.8 seconds");
assert.equal(smoke_line_blocked(openMap, [smoke], [{center: {x: 0, y: 0, z: 64}, time: 1}], rayStart, rayEnd, 2), false,
	"HE after smoke opens the affected ray");
assert.equal(smoke_line_blocked(openMap, [smoke], [{center: {x: 0, y: 0, z: 64}, time: -1}], rayStart, rayEnd, 1.1), true,
	"HE before smoke cannot clear it");
assert.equal(smoke_line_blocked(openMap, [smoke], [{center: {x: 0, y: 0, z: 64}, time: 1}], rayStart, rayEnd, 3.5), true,
	"HE clearance expires at 2.5 seconds");
assert.equal(smoke_line_blocked(openMap, [smoke], [], rayStart, rayEnd, 22.5), false, "smoke fade completes safely");

const splitMap = test_map();
splitMap.segment_blocked = (start, end) => ({blocked: (start.x < 0) !== (end.x < 0), packet: 0xffffffff});
const clippedSmoke = make_test_smoke({x: -30, y: 0, z: 64}, 0, splitMap);
assert.ok(Math.max(...clippedSmoke.visibleCells.filter((_, index) => index % 4 === 0)) < 0,
	"smoke growth must not cross baked walls");
const tightMap = test_map();
tightMap.segment_blocked = () => ({blocked: true, packet: 0});
assert.ok(make_test_smoke({x: 0, y: 0, z: 0.5}, 0, tightMap).visibleCells.length > 0,
	"a smoke resting against tight geometry must still receive a render seed");

const nav = validate_nav({
	version: 1,
	map: "test",
	hulls: [{radius: 16, height: 71}],
	areas: [
		{id: 1, corners: [[0, 0, 0], [128, 0, 0], [128, 128, 0], [0, 128, 0]], connections: [{area: 2}]},
		{id: 2, corners: [[128, 0, 0], [256, 0, 0], [256, 128, 0], [128, 128, 0]], connections: [{area: 1}]}
	],
	ladders: [],
	objectives: {a: [0, 64, 0], b: [256, 64, 0]}
});
assert.equal(nav.areas.size, 2);
assert.deepEqual(nav.objectives.b, {x: 256, y: 64, z: 0});
assert.equal(route_between(nav, {x: 0, y: 0, z: 0}, {x: 256, y: 0, z: 0}).length, 2);
assert.throws(() => validate_nav({version: 1, areas: [{id: 1, corners: [], connections: []}]}), /invalid/i);

const simulation = new FpsSimulation(openMap, {
	viewer: {x: -300, y: 0, z: 0.02, yaw: 0},
	target: {x: 300, y: 0, z: 0.02, yaw: 180},
	playerSpeed: 225,
	botSpeed: 225,
	pingMs: 200
});
simulation.set_input({w: true, a: true});
const state = simulation.step();
assert.equal(state.bots.length, 3, "Play should simulate three terrorist bots");
assert.equal(state.visibilities.length, 3, "each terrorist bot needs its own visibility check");
assert.deepEqual(simulation.botBrains.map((brain) => brain.siteKey), ["b", "a", ""],
	"bots should start with B, A, and free-roaming map goals");
for (let left = 0; left < state.bots.length; ++left)
	for (let right = left + 1; right < state.bots.length; ++right)
		assert.ok(Math.hypot(state.bots[left].origin.x - state.bots[right].origin.x,
			state.bots[left].origin.y - state.bots[right].origin.y) >= 1000,
		"extra bot spawns should be separated by at least 1000 units");
assert.equal(state.visibility.origins.length / 3, 6);
assert.equal(state.visibility.targets.length / 3, 23);
assert.equal(state.visibility.blocked.length, 138);
simulation.request_traversal();
const traversalState = simulation.step();
assert.equal(traversalState.visibilities.filter((visibility) => visibility.traversal).length, 3,
	"BVH debug should capture traversal evidence for every bot");
simulation.fire_visual({x: 1, y: 0, z: 0});
const shotState = simulation.step();
assert.equal(shotState.events.some((event) => event.type === "shot" && event.hit === false), true,
	"visual gunfire should publish a tracer even when it misses geometry");

const campingSimulation = new FpsSimulation(openMap, {
	viewer: {x: 0, y: 0, z: 0.02, yaw: 0},
	target: {x: 128, y: 0, z: 0.02, yaw: 180},
	nav: {version: 1, map: "test", areas: [
		{id: 1, corners: [[0, 0, 0], [64, 0, 0], [64, 64, 0]], connections: [{area: 2}]},
		{id: 2, corners: [[64, 0, 0], [128, 0, 0], [128, 64, 0]], connections: [{area: 1}]}
	], ladders: [], objectives: {b: [128, 32, 0]}}
});
campingSimulation.bots[1].origin = {x: 4000, y: 0, z: 0.02};
campingSimulation.bots[2].origin = {x: -4000, y: 0, z: 0.02};
const campingBrain = campingSimulation.botBrains[0];
campingBrain.mode = "camp";
campingBrain.campUntil = 10;
const campState = campingSimulation.step();
assert.equal(campState.botMode, "camp");
assert.equal(Math.round(campState.bot.speed), 0, "a defending bot should wait instead of constantly holding W");
campingBrain.mode = "travel";
campingBrain.route = [{x: 512, y: 32, z: 0}];
campingBrain.routeIndex = 0;
campingBrain.nextGoal = 10;
campingSimulation.bot.grounded = true;
campingBrain.blockedTicks = 22;
campingSimulation.move_bot();
assert.ok(campingSimulation.bot.velocity.z > 0, "a bot stuck on a stair lip should hop and repath");
campingBrain.failedJumps = 0;
campingBrain.jumpOrigin = {...campingSimulation.bot.origin};
for (let jump = 0; jump < 3; ++jump)
{
	campingSimulation.bot.grounded = true;
	campingSimulation.bot.velocity = {x: 0, y: 0, z: 0};
	campingBrain.blockedTicks = 22;
	campingSimulation.move_bot();
}
assert.equal(campingBrain.mode, "reposition", "repeated jumps without meaningful progress should choose another route");
assert.equal(campingBrain.route.length, 0, "the failed jumping route should be discarded");

const grenadeSimulation = new FpsSimulation(openMap, {
	viewer: {x: 0, y: 0, z: 0.02, yaw: 0},
	target: {x: 300, y: 0, z: 0.02, yaw: 180}
});
grenadeSimulation.throw_grenade("he", {x: 0, y: 0, z: 20}, {x: 0, y: 0, z: -1});
grenadeSimulation.throw_grenade("smoke", {x: 10, y: 0, z: 20}, {x: 0, y: 0, z: -1});
assert.ok(Math.abs(grenadeSimulation.grenades[0].velocity.z) <= 750.01, "normal grenade throw should use full speed");
let grenadeState = grenadeSimulation.step();
assert.deepEqual(grenadeState.grenades.map((grenade) => grenade.id), [1, 2], "grenade visuals need stable unique IDs");
let bounced = grenadeState.events.some((event) => event.type === "grenade-bounce");
for (let tick = 0; tick < 10 && !bounced; ++tick)
{
	grenadeState = grenadeSimulation.step();
	bounced ||= grenadeState.events.some((event) => event.type === "grenade-bounce");
}
assert.equal(bounced, true, "a grenade collision should produce a bounce sound event");
for (let tick = 0; tick < 100 && grenadeState.heCount === 0; ++tick) grenadeState = grenadeSimulation.step();
assert.equal(grenadeState.heCount, 1, "HE detonation should publish one active smoke clearance");
assert.equal(grenadeState.clearances.length, 1, "the renderer should receive the active HE clearance position");
for (let tick = 0; tick < 160; ++tick) grenadeState = grenadeSimulation.step();
assert.equal(grenadeState.heCount, 0, "HE smoke clearance should expire after 2.5 seconds");

console.log("FPS movement, collision, smoke, HE, nav, and runtime-layout checks passed");
