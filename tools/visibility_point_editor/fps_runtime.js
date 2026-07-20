// Worker-side FPS test range. It owns deterministic movement, a small nav bot,
// synthetic smoke inputs, and the same wall/smoke visibility decision as CS2FOW.

import {runtime_origins, BVH8_INVALID_REF} from "./bvh8.js";

export const FPS_TICK_RATE = 64;
export const FPS_DT = 1 / FPS_TICK_RATE;
export const FPS_CONSTANTS = Object.freeze({
	accelerate: 5.5,
	airAccelerate: 12,
	airWishSpeed: 30,
	friction: 5.2,
	gravity: 800,
	jumpImpulse: 301.993,
	stopSpeed: 80,
	globalMaxSpeed: 320,
	radius: 16,
	standingHeight: 71,
	crouchedHeight: 35.5,
	stepHeight: 16,
	maxSlopeDegrees: 50
});

const SMOKE_AXIS = 32;
const SMOKE_CELLS = SMOKE_AXIS ** 3;
const SMOKE_HALF = 320;
const SMOKE_CELL = 20;
const SMOKE_IGNORE = 0.1;
const SMOKE_OPAQUE = 0.8;
const SMOKE_BLOCK = 0.2;
const SMOKE_MAX_STEPS = 128;
const SMOKE_LIMIT = 32;
const HE_LIMIT = 64;
const HE_RADIUS = 100;
const HE_SECONDS = 2.5;
const BULLET_SMOKE_RADIUS = 28;
const BULLET_SMOKE_SECONDS = 0.8;
const GROUND_NORMAL_Z = Math.cos(FPS_CONSTANTS.maxSlopeDegrees * Math.PI / 180);

const add = (a, b) => ({x: a.x + b.x, y: a.y + b.y, z: a.z + b.z});
const sub = (a, b) => ({x: a.x - b.x, y: a.y - b.y, z: a.z - b.z});
const mul = (a, amount) => ({x: a.x * amount, y: a.y * amount, z: a.z * amount});
const dot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
const length_sq = (a) => dot(a, a);
const length = (a) => Math.sqrt(length_sq(a));
const finite_vec = (a) => Number.isFinite(a.x) && Number.isFinite(a.y) && Number.isFinite(a.z);
const clamp = (value, minimum, maximum) => Math.min(maximum, Math.max(minimum, value));
const normalize = (a) =>
{
	const size = length(a);
	return size > 1.0e-8 ? mul(a, 1 / size) : {x: 0, y: 0, z: 0};
};
const cross = (a, b) => ({x: a.y * b.z - a.z * b.y, y: a.z * b.x - a.x * b.z, z: a.x * b.y - a.y * b.x});

function closest_point_segment(point, start, end)
{
	const direction = sub(end, start);
	const amount = length_sq(direction) <= 1.0e-10 ? 0 : clamp(dot(sub(point, start), direction) / length_sq(direction), 0, 1);
	return add(start, mul(direction, amount));
}

function closest_point_triangle(point, a, b, c)
{
	const ab = sub(b, a);
	const ac = sub(c, a);
	const ap = sub(point, a);
	const d1 = dot(ab, ap);
	const d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	const bp = sub(point, b);
	const d3 = dot(ab, bp);
	const d4 = dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;
	const vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return add(a, mul(ab, d1 / (d1 - d3)));
	const cp = sub(point, c);
	const d5 = dot(ab, cp);
	const d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;
	const vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return add(a, mul(ac, d2 / (d2 - d6)));
	const va = d3 * d6 - d5 * d4;
	if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0)
	{
		return add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
	}
	const inverse = 1 / (va + vb + vc);
	return add(a, add(mul(ab, vb * inverse), mul(ac, vc * inverse)));
}

function closest_segments(p1, q1, p2, q2)
{
	const d1 = sub(q1, p1);
	const d2 = sub(q2, p2);
	const r = sub(p1, p2);
	const a = dot(d1, d1);
	const e = dot(d2, d2);
	const f = dot(d2, r);
	let s = 0;
	let t = 0;
	if (a <= 1.0e-10 && e <= 1.0e-10) return {a: p1, b: p2};
	if (a <= 1.0e-10)
	{
		t = clamp(f / e, 0, 1);
	}
	else
	{
		const c = dot(d1, r);
		if (e <= 1.0e-10)
		{
			s = clamp(-c / a, 0, 1);
		}
		else
		{
			const b = dot(d1, d2);
			const denominator = a * e - b * b;
			s = denominator === 0 ? 0 : clamp((b * f - c * e) / denominator, 0, 1);
			t = (b * s + f) / e;
			if (t < 0) { t = 0; s = clamp(-c / a, 0, 1); }
			else if (t > 1) { t = 1; s = clamp((b - c) / a, 0, 1); }
		}
	}
	return {a: add(p1, mul(d1, s)), b: add(p2, mul(d2, t))};
}

function capsule_triangle_contact(origin, height, triangle)
{
	const radius = FPS_CONSTANTS.radius;
	const segmentStart = add(origin, {x: 0, y: 0, z: radius});
	const segmentEnd = add(origin, {x: 0, y: 0, z: Math.max(radius, height - radius)});
	let closestA = segmentStart;
	let closestB = closest_point_triangle(segmentStart, triangle.v0, triangle.v1, triangle.v2);
	let best = length_sq(sub(closestA, closestB));
	const consider = (a, b) =>
	{
		const distance = length_sq(sub(a, b));
		if (distance < best) { best = distance; closestA = a; closestB = b; }
	};
	consider(segmentEnd, closest_point_triangle(segmentEnd, triangle.v0, triangle.v1, triangle.v2));
	for (const vertex of [triangle.v0, triangle.v1, triangle.v2]) consider(closest_point_segment(vertex, segmentStart, segmentEnd), vertex);
	for (const [a, b] of [[triangle.v0, triangle.v1], [triangle.v1, triangle.v2], [triangle.v2, triangle.v0]])
	{
		const pair = closest_segments(segmentStart, segmentEnd, a, b);
		consider(pair.a, pair.b);
	}
	const distance = Math.sqrt(best);
	if (distance >= radius - 0.001) return null;
	let normal = distance > 1.0e-6 ? mul(sub(closestA, closestB), 1 / distance)
		: normalize(cross(sub(triangle.v1, triangle.v0), sub(triangle.v2, triangle.v0)));
	if (dot(normal, sub(add(origin, {x: 0, y: 0, z: height * 0.5}), triangle.v0)) < 0) normal = mul(normal, -1);
	return {normal, penetration: radius - distance};
}

export function resolve_capsule(map, origin, height)
{
	let position = {...origin};
	const normals = [];
	for (let pass = 0; pass < 6; ++pass)
	{
		let deepest = null;
		const minimum = {x: position.x - 16.1, y: position.y - 16.1, z: position.z - 0.1};
		const maximum = {x: position.x + 16.1, y: position.y + 16.1, z: position.z + height + 0.1};
		map.for_each_triangle_in_bounds(minimum, maximum, (_packet, _lane, triangle) =>
		{
			const contact = capsule_triangle_contact(position, height, triangle);
			if (contact && (!deepest || contact.penetration > deepest.penetration)) deepest = contact;
		});
		if (!deepest) break;
		position = add(position, mul(deepest.normal, deepest.penetration + 0.01));
		normals.push(deepest.normal);
	}
	return {position, normals};
}

function ground_hit(map, origin, distance = 5)
{
	const start = add(origin, {x: 0, y: 0, z: 2});
	const hit = map.segment_hit(start, add(origin, {x: 0, y: 0, z: -distance}));
	return hit && hit.normal.z >= GROUND_NORMAL_Z ? hit : null;
}

function accelerate(velocity, wishDirection, wishSpeed, amount, dt)
{
	const current = dot(velocity, wishDirection);
	const addSpeed = wishSpeed - current;
	if (addSpeed <= 0) return velocity;
	return add(velocity, mul(wishDirection, Math.min(amount * dt * wishSpeed, addSpeed)));
}

function air_accelerate(velocity, wishDirection, wishSpeed, dt)
{
	// Source keeps the full input speed in the acceleration amount, but limits
	// how much speed may be gained along the requested air-strafe direction.
	const cappedWishSpeed = Math.min(wishSpeed, FPS_CONSTANTS.airWishSpeed);
	const current = dot(velocity, wishDirection);
	const addSpeed = cappedWishSpeed - current;
	if (addSpeed <= 0) return velocity;
	const acceleration = FPS_CONSTANTS.airAccelerate * wishSpeed * dt;
	return add(velocity, mul(wishDirection, Math.min(acceleration, addSpeed)));
}

function cap_horizontal_speed(velocity, maximum)
{
	const speed = Math.hypot(velocity.x, velocity.y);
	if (speed <= maximum || speed <= 0) return velocity;
	const scale = maximum / speed;
	return {...velocity, x: velocity.x * scale, y: velocity.y * scale};
}

function make_actor(pose)
{
	const origin = {x: Number(pose.x), y: Number(pose.y), z: Number(pose.z)};
	return {
		origin,
		velocity: {x: 0, y: 0, z: 0},
		yaw: Number(pose.yaw) || 0,
		pitch: 0,
		grounded: false,
		crouched: false,
		lastSafe: {...origin},
		surface: "default",
		stance: "standing",
		speed: 0
	};
}

export function move_actor(map, actor, buttons, maximumSpeed, dt = FPS_DT)
{
	const wasGrounded = actor.grounded;
	const onLadder = Boolean(buttons.ladder);
	const height = buttons.crouch ? FPS_CONSTANTS.crouchedHeight : FPS_CONSTANTS.standingHeight;
	actor.crouched = !onLadder && (buttons.crouch || (actor.crouched && resolve_capsule(map, actor.origin, FPS_CONSTANTS.standingHeight).normals.length > 0));
	const actualHeight = actor.crouched ? FPS_CONSTANTS.crouchedHeight : height;
	const ground = ground_hit(map, actor.origin, wasGrounded ? FPS_CONSTANTS.stepHeight + 2 : 5);
	actor.grounded = !onLadder && Boolean(ground) && actor.velocity.z <= 20;
	if (actor.grounded)
	{
		actor.origin.z = ground.point.z + 0.02;
		actor.surface = map.surfaceMap?.name(ground.packet, ground.lane) || "concrete";
		actor.velocity.z = Math.max(0, actor.velocity.z);
		const horizontalSpeed = Math.hypot(actor.velocity.x, actor.velocity.y);
		if (horizontalSpeed > 0 && !buttons.jump)
		{
			const drop = Math.max(horizontalSpeed, FPS_CONSTANTS.stopSpeed) * FPS_CONSTANTS.friction * dt;
			const next = Math.max(0, horizontalSpeed - drop) / horizontalSpeed;
			actor.velocity.x *= next;
			actor.velocity.y *= next;
		}
	}
	const yaw = actor.yaw * Math.PI / 180;
	const forward = {x: Math.cos(yaw), y: Math.sin(yaw), z: 0};
	const right = {x: Math.sin(yaw), y: -Math.cos(yaw), z: 0};
	let wish = add(mul(forward, Number(Boolean(buttons.w)) - Number(Boolean(buttons.s))),
		mul(right, Number(Boolean(buttons.d)) - Number(Boolean(buttons.a))));
	const wishLength = Math.hypot(wish.x, wish.y);
	if (wishLength > 0) wish = mul(wish, 1 / wishLength);
	let wishSpeed = Math.min(FPS_CONSTANTS.globalMaxSpeed, Math.max(1, maximumSpeed));
	if (buttons.walk) wishSpeed *= 0.52;
	if (actor.crouched) wishSpeed *= 0.34;
	if (wishLength)
	{
		actor.velocity = actor.grounded
			? accelerate(actor.velocity, wish, wishSpeed, FPS_CONSTANTS.accelerate, dt)
			: air_accelerate(actor.velocity, wish, wishSpeed, dt);
	}
	actor.velocity = cap_horizontal_speed(actor.velocity,
		actor.grounded ? wishSpeed : FPS_CONSTANTS.globalMaxSpeed);
	if (onLadder)
	{
		actor.velocity.x *= 0.35;
		actor.velocity.y *= 0.35;
		actor.velocity.z = clamp(Number(buttons.ladderDirection) || 0, -1, 1) * 150;
		actor.surface = "ladder_wood";
	}
	else if (buttons.jump && actor.grounded && !actor.crouched)
	{
		actor.velocity.z = FPS_CONSTANTS.jumpImpulse;
		actor.grounded = false;
	}
	if (!actor.grounded && !onLadder) actor.velocity.z -= FPS_CONSTANTS.gravity * dt;

	const original = {...actor.origin};
	const wanted = add(actor.origin, mul(actor.velocity, dt));
	let resolved = resolve_capsule(map, wanted, actualHeight);
	let chosen = resolved.position;
	let collisionNormals = resolved.normals;
	const horizontalBlocked = resolved.normals.some((normal) => Math.abs(normal.z) < GROUND_NORMAL_Z);
	const wantedDirection = normalize({x: wanted.x - original.x, y: wanted.y - original.y, z: 0});
	const wantedProgress = Math.hypot(wanted.x - original.x, wanted.y - original.y);
	const normalProgress = dot(sub(chosen, original), wantedDirection);
	if (actor.grounded && wantedProgress > 0.001 && (horizontalBlocked || normalProgress + 0.001 < wantedProgress))
	{
		const stepProbe = FPS_CONSTANTS.stepHeight + 2;
		const raised = resolve_capsule(map, add(original, {x: 0, y: 0, z: stepProbe}), actualHeight);
		const stepped = resolve_capsule(map, add(raised.position, {x: actor.velocity.x * dt, y: actor.velocity.y * dt, z: 0}), actualHeight);
		const down = ground_hit(map, stepped.position, stepProbe + 4);
		const stepProgress = dot(sub(stepped.position, original), wantedDirection);
		if (down && down.normal.z >= GROUND_NORMAL_Z && stepProgress > normalProgress + 0.001)
		{
			chosen = {...stepped.position, z: down.point.z + 0.02};
			collisionNormals = stepped.normals;
		}
	}
	actor.origin = chosen;
	for (const normal of collisionNormals)
	{
		const into = dot(actor.velocity, normal);
		if (into < 0) actor.velocity = sub(actor.velocity, mul(normal, into));
	}
	const afterGround = ground_hit(map, actor.origin, actor.grounded && actor.velocity.z <= 20 ? FPS_CONSTANTS.stepHeight + 2 : 5);
	actor.grounded = !onLadder && Boolean(afterGround) && actor.velocity.z <= 20;
	if (actor.grounded)
	{
		actor.origin.z = afterGround.point.z + 0.02;
		actor.velocity.z = Math.max(0, actor.velocity.z);
		actor.lastSafe = {...actor.origin};
	}
	if (!finite_vec(actor.origin) || actor.origin.z < map.metadata.worldMin[2] - 1024)
	{
		actor.origin = {...actor.lastSafe};
		actor.velocity = {x: 0, y: 0, z: 0};
	}
	actor.speed = Math.hypot(actor.velocity.x, actor.velocity.y);
	actor.stance = onLadder ? "ladder" : actor.crouched ? "crouched" : actor.grounded ? (actor.speed > 1 ? (buttons.walk ? "walking" : "running") : "standing") : "airborne";
	return actor;
}

function nearby_ladder(nav, actor, buttons)
{
	if (!nav || (!buttons.w && !buttons.s)) return null;
	for (const ladder of nav.ladders || [])
	{
		if (!Array.isArray(ladder.top) || !Array.isArray(ladder.bottom)) continue;
		const top = {x: Number(ladder.top[0]), y: Number(ladder.top[1]), z: Number(ladder.top[2])};
		const bottom = {x: Number(ladder.bottom[0]), y: Number(ladder.bottom[1]), z: Number(ladder.bottom[2])};
		if (!finite_vec(top) || !finite_vec(bottom)) continue;
		const minimumZ = Math.min(top.z, bottom.z) - 20;
		const maximumZ = Math.max(top.z, bottom.z) + 20;
		const radius = Math.max(28, Number(ladder.Width || ladder.width || 0) * 0.5 + FPS_CONSTANTS.radius);
		if (actor.origin.z >= minimumZ && actor.origin.z <= maximumZ
			&& Math.hypot(actor.origin.x - bottom.x, actor.origin.y - bottom.y) <= radius)
			return {ladder: true, ladderDirection: buttons.w ? 1 : -1};
	}
	return null;
}

function smoothstep(value)
{
	value = clamp(value, 0, 1);
	return value * value * (3 - 2 * value);
}

function smoke_age_scale(age)
{
	return smoothstep(((age - 0.5) - 0.1) / 1.4) * smoothstep((22 - (age + 0.5)) / 5);
}

function morton_index(x, y, z)
{
	let result = 0;
	for (let bit = 0; bit < 5; ++bit)
	{
		result |= ((x >> bit) & 1) << (bit * 3);
		result |= ((y >> bit) & 1) << (bit * 3 + 1);
		result |= ((z >> bit) & 1) << (bit * 3 + 2);
	}
	return result;
}

export function make_test_smoke(center, startTime, map = null)
{
	const density = new Float32Array(SMOKE_CELLS);
	const opaque = new Uint8Array(SMOKE_CELLS);
	const visibleCells = [];
	const reachable = new Uint8Array(SMOKE_CELLS);
	const coordinate = (axis) => (axis + 0.5) * SMOKE_CELL - SMOKE_HALF;
	if (map)
	{
		const queue = [];
		const seedOrigins = [add(center, {x: 0, y: 0, z: 4}), add(center, {x: 0, y: 0, z: 16}), center];
		for (const z of [15, 16]) for (const y of [15, 16]) for (const x of [15, 16])
		{
			const point = {x: center.x + coordinate(x), y: center.y + coordinate(y), z: center.z + coordinate(z)};
			if (seedOrigins.some((origin) => !map.segment_blocked(origin, point).blocked)) queue.push([x, y, z]);
		}
		if (queue.length === 0) queue.push([16, 16, 16]);
		for (let next = 0; next < queue.length; ++next)
		{
			const [x, y, z] = queue[next];
			const index = morton_index(x, y, z);
			if (reachable[index]) continue;
			reachable[index] = 1;
			const point = {x: center.x + coordinate(x), y: center.y + coordinate(y), z: center.z + coordinate(z)};
			for (const [dx, dy, dz] of [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]])
			{
				const nx = x + dx, ny = y + dy, nz = z + dz;
				if (nx < 0 || nx >= SMOKE_AXIS || ny < 0 || ny >= SMOKE_AXIS || nz < 0 || nz >= SMOKE_AXIS) continue;
				const local = {x: coordinate(nx), y: coordinate(ny), z: coordinate(nz)};
				if (Math.hypot(local.x, local.y, local.z) >= 160 || reachable[morton_index(nx, ny, nz)]) continue;
				const neighbor = add(center, local);
				if (!map.segment_blocked(point, neighbor).blocked) queue.push([nx, ny, nz]);
			}
		}
	}
	for (let z = 0; z < SMOKE_AXIS; ++z)
	{
		for (let y = 0; y < SMOKE_AXIS; ++y)
		{
			for (let x = 0; x < SMOKE_AXIS; ++x)
			{
				const local = {x: (x + 0.5) * 20 - 320, y: (y + 0.5) * 20 - 320, z: (z + 0.5) * 20 - 320};
				const radius = Math.hypot(local.x, local.y, local.z);
				const value = radius <= 120 ? 1 : radius < 160 ? smoothstep((160 - radius) / 40) : 0;
				if (value <= 0 || map && !reachable[morton_index(x, y, z)]) continue;
				const index = morton_index(x, y, z);
				density[index] = value;
				opaque[index] = Number(value >= SMOKE_OPAQUE);
				visibleCells.push(center.x + local.x, center.y + local.y, center.z + local.z, value);
			}
		}
	}
	return {center: {...center}, startTime, density, opaque, visibleCells: new Float32Array(visibleCells)};
}

function point_segment_distance_sq(point, start, end)
{
	const closest = closest_point_segment(point, start, end);
	return length_sq(sub(point, closest));
}

function bullet_clears_cell(cuts, smoke, cell, now)
{
	return cuts.some((cut) => cut.time >= smoke.startTime && now - cut.time >= 0 && now - cut.time < BULLET_SMOKE_SECONDS
		&& point_segment_distance_sq(cell, cut.start, cut.end) <= BULLET_SMOKE_RADIUS ** 2);
}

function clip_axis(origin, direction, minimum, maximum, range)
{
	if (Math.abs(direction) < 1.0e-7) return origin >= minimum && origin <= maximum;
	let left = (minimum - origin) / direction;
	let right = (maximum - origin) / direction;
	if (left > right) [left, right] = [right, left];
	range.first = Math.max(range.first, left);
	range.last = Math.min(range.last, right);
	return range.first <= range.last;
}

function smoke_volume_density(volume, origin, target, cuts, now)
{
	const direction = sub(target, origin);
	const minimum = sub(volume.center, {x: SMOKE_HALF, y: SMOKE_HALF, z: SMOKE_HALF});
	const maximum = add(volume.center, {x: SMOKE_HALF, y: SMOKE_HALF, z: SMOKE_HALF});
	const range = {first: 0, last: 1};
	if (!clip_axis(origin.x, direction.x, minimum.x, maximum.x, range)
		|| !clip_axis(origin.y, direction.y, minimum.y, maximum.y, range)
		|| !clip_axis(origin.z, direction.z, minimum.z, maximum.z, range)) return 0;
	const entry = clamp(range.first, 0, 1);
	const exit = clamp(range.last, 0, 1);
	if (entry > exit) return 0;
	let startParameter = entry;
	let start = add(origin, mul(direction, entry));
	if (entry > 0)
	{
		const rayLength = length(direction);
		const inward = rayLength > 0 ? Math.min(1 / rayLength, Math.max(exit - entry, 0)) : 0;
		startParameter += inward;
		start = add(start, mul(direction, inward));
	}
	const finish = add(origin, mul(direction, exit));
	const coordinate = (value, low) => clamp(Math.floor((value - low) / SMOKE_CELL), 0, SMOKE_AXIS - 1);
	const cell = [coordinate(start.x, minimum.x), coordinate(start.y, minimum.y), coordinate(start.z, minimum.z)];
	const end = [coordinate(finish.x, minimum.x), coordinate(finish.y, minimum.y), coordinate(finish.z, minimum.z)];
	const starts = [start.x, start.y, start.z];
	const directions = [direction.x, direction.y, direction.z];
	const lows = [minimum.x, minimum.y, minimum.z];
	const step = [0, 0, 0];
	const next = [Infinity, Infinity, Infinity];
	const delta = [Infinity, Infinity, Infinity];
	for (let axis = 0; axis < 3; ++axis)
	{
		if (directions[axis] > 0)
		{
			step[axis] = 1;
			next[axis] = startParameter + (lows[axis] + (cell[axis] + 1) * SMOKE_CELL - starts[axis]) / directions[axis];
			delta[axis] = SMOKE_CELL / directions[axis];
		}
		else if (directions[axis] < 0)
		{
			step[axis] = -1;
			next[axis] = startParameter + (lows[axis] + cell[axis] * SMOKE_CELL - starts[axis]) / directions[axis];
			delta[axis] = -SMOKE_CELL / directions[axis];
		}
	}
	let accumulated = 0;
	for (let visited = 0; visited < SMOKE_MAX_STEPS; ++visited)
	{
		if (cell.some((value) => value < 0 || value >= SMOKE_AXIS)) break;
		const index = morton_index(cell[0], cell[1], cell[2]);
		const center = {x: minimum.x + (cell[0] + 0.5) * SMOKE_CELL, y: minimum.y + (cell[1] + 0.5) * SMOKE_CELL, z: minimum.z + (cell[2] + 0.5) * SMOKE_CELL};
		if (bullet_clears_cell(cuts, volume, center, now))
		{
			if (cell[0] === end[0] && cell[1] === end[1] && cell[2] === end[2]) break;
			const axis = next[1] <= next[0] ? (next[1] <= next[2] ? 1 : 2) : (next[0] <= next[2] ? 0 : 2);
			if (next[axis] > exit) break;
			cell[axis] += step[axis];
			next[axis] += delta[axis];
			continue;
		}
		if (volume.opaque[index] || volume.density[index] >= SMOKE_OPAQUE) return 1;
		if (volume.density[index] > SMOKE_IGNORE)
		{
			accumulated += volume.density[index];
			if (accumulated >= SMOKE_BLOCK) return 1;
		}
		if (cell[0] === end[0] && cell[1] === end[1] && cell[2] === end[2]) break;
		const axis = next[1] <= next[0] ? (next[1] <= next[2] ? 1 : 2) : (next[0] <= next[2] ? 0 : 2);
		if (next[axis] > exit) break;
		cell[axis] += step[axis];
		next[axis] += delta[axis];
	}
	return accumulated;
}

function he_opens_smoke(map, clearance, smoke, origin, target, now)
{
	const age = now - clearance.time;
	if (age < 0 || age >= HE_SECONDS || clearance.time < smoke.startTime) return false;
	const boxDistance = {
		x: Math.max(Math.abs(clearance.center.x - smoke.center.x) - SMOKE_HALF, 0),
		y: Math.max(Math.abs(clearance.center.y - smoke.center.y) - SMOKE_HALF, 0),
		z: Math.max(Math.abs(clearance.center.z - smoke.center.z) - SMOKE_HALF, 0)
	};
	if (length_sq(boxDistance) > HE_RADIUS ** 2) return false;
	const direction = sub(target, origin);
	const parameter = length_sq(direction) <= 0 ? 0 : clamp(dot(sub(clearance.center, origin), direction) / length_sq(direction), 0, 1);
	const closest = add(origin, mul(direction, parameter));
	return length_sq(sub(clearance.center, closest)) <= HE_RADIUS ** 2 && !map.segment_blocked(clearance.center, closest).blocked;
}

export function smoke_line_blocked(map, smokes, clearances, origin, target, now, cuts = [])
{
	if (!finite_vec(origin) || !finite_vec(target)) return false;
	let total = 0;
	for (const smoke of smokes)
	{
		if (clearances.some((clearance) => he_opens_smoke(map, clearance, smoke, origin, target, now))) continue;
		total += smoke_volume_density(smoke, origin, target, cuts, now) * smoke_age_scale(now - smoke.startTime);
		if (total >= SMOKE_BLOCK) return true;
	}
	return false;
}

function area_center(area)
{
	if (Array.isArray(area.center) && area.center.length >= 3) return {x: area.center[0], y: area.center[1], z: area.center[2]};
	const corners = area.corners || [];
	const divisor = Math.max(1, corners.length);
	return corners.reduce((sum, point) => add(sum, {x: point[0], y: point[1], z: point[2]}), {x: 0, y: 0, z: 0});
}

export function validate_nav(value)
{
	if (!value || value.version !== 1 || !Array.isArray(value.areas)) throw new Error("invalid Studio nav graph");
	const areas = new Map();
	for (const raw of value.areas)
	{
		if (!Number.isInteger(raw.id) || areas.has(raw.id) || !Array.isArray(raw.corners) || raw.corners.length < 3) throw new Error("invalid nav area");
		const center = raw.center ? {x: raw.center[0], y: raw.center[1], z: raw.center[2]} : mul(area_center(raw), 1 / raw.corners.length);
		if (!finite_vec(center)) throw new Error("invalid nav area center");
		areas.set(raw.id, {...raw, center, connections: Array.isArray(raw.connections) ? raw.connections : []});
	}
	for (const area of areas.values())
	{
		area.connections = area.connections.filter((connection) => areas.has(connection_id(connection)));
	}
	const ladders = Array.isArray(value.ladders) ? value.ladders : [];
	for (const ladder of ladders)
	{
		const top = [...new Set(Object.entries(ladder.areas || {}).filter(([key, id]) => key.startsWith("top") && areas.has(Number(id))).map(([, id]) => Number(id)))];
		const bottom = [...new Set(Object.entries(ladder.areas || {}).filter(([key, id]) => key.startsWith("bottom") && areas.has(Number(id))).map(([, id]) => Number(id)))];
		for (const from of bottom) for (const to of top) areas.get(from).connections.push({area: to, kind: "ladder", ladder: ladder.id});
		for (const from of top) for (const to of bottom) areas.get(from).connections.push({area: to, kind: "ladder", ladder: ladder.id});
	}
	const objectives = {};
	for (const key of ["a", "b"])
	{
		const raw = value.objectives?.[key];
		const point = Array.isArray(raw) && raw.length >= 3 ? {x: Number(raw[0]), y: Number(raw[1]), z: Number(raw[2])} : null;
		if (point && finite_vec(point)) objectives[key] = point;
	}
	return {map: String(value.map || ""), areas, ladders, objectives};
}

function connection_id(connection)
{
	return Number(typeof connection === "object" && connection !== null ? connection.area ?? connection.target : connection);
}

function nearest_area(nav, point)
{
	let found = null;
	let best = Infinity;
	for (const area of nav.areas.values())
	{
		const distance = length_sq(sub(area.center, point));
		if (distance < best) { best = distance; found = area; }
	}
	return found;
}

export function route_between(nav, startPoint, targetPoint)
{
	const start = nearest_area(nav, startPoint);
	const goal = nearest_area(nav, targetPoint);
	if (!start || !goal) return [];
	const open = [start.id];
	const came = new Map();
	const score = new Map([[start.id, 0]]);
	while (open.length)
	{
		open.sort((a, b) => (score.get(a) + length(sub(nav.areas.get(a).center, goal.center)))
			- (score.get(b) + length(sub(nav.areas.get(b).center, goal.center))));
		const current = open.shift();
		if (current === goal.id)
		{
			const result = [];
			for (let id = current; id !== undefined;)
			{
				const link = came.get(id);
				result.push({...nav.areas.get(id).center, traversal: link?.kind || "walk", ladder: link?.ladder, flags: nav.areas.get(id).flags || "0x0"});
				id = link?.previous;
			}
			return result.reverse();
		}
		for (const connection of nav.areas.get(current).connections)
		{
			const next = connection_id(connection);
			const candidate = score.get(current) + length(sub(nav.areas.get(current).center, nav.areas.get(next).center));
			if (candidate < (score.get(next) ?? Infinity))
			{
				came.set(next, {previous: current, kind: connection.kind || "walk", ladder: connection.ladder});
				score.set(next, candidate);
				if (!open.includes(next)) open.push(next);
			}
		}
	}
	return [];
}

export function default_targets(bot)
{
	const local = [
		[-24, -24, 0], [24, -24, 0], [-24, 24, 0], [24, 24, 0],
		[-24, -24, 80], [24, -24, 80], [-24, 24, 80], [24, 24, 80],
		[0, 0, 64], [0, 0, 59], [0, 0, 54], [0, 0, 38], [-8, 8, 61], [8, -8, 61],
		[-8, 4, 42], [8, -4, 42], [-7, 3, 22], [7, -3, 22], [-7, 3, 7], [7, -3, 7],
		[-6, 8, 48], [6, -8, 48], [0, 0, 47]
	];
	const values = new Float32Array(local.length * 3);
	local.forEach((point, index) => values.set([bot.origin.x + point[0], bot.origin.y + point[1], bot.origin.z + point[2]], index * 3));
	return values;
}

function make_bot_brain(bot, index)
{
	return {
		route: [], routeIndex: 0, wanderYaw: bot.yaw, nextGoal: 0, mode: "travel", site: null,
		campUntil: 0, blockedTicks: 0, safeOrigin: {...bot.origin}, jumpOrigin: {...bot.origin}, failedJumps: 0,
		destination: null, siteKey: index === 0 ? "b" : index === 1 ? "a" : ""
	};
}

export class FpsSimulation
{
	constructor(map, settings)
	{
		this.map = map;
		this.time = 0;
		this.player = make_actor(settings.viewer);
		this.bot = make_actor(settings.target);
		this.playerButtons = {};
		this.playerSpeed = Number(settings.playerSpeed) || 225;
		this.botSpeed = Number(settings.botSpeed) || 225;
		this.pingMs = Number(settings.pingMs) || 0;
		this.debug = false;
		this.targets = null;
		this.smokes = [];
		this.clearances = [];
		this.smokeCuts = [];
		this.grenades = [];
		this.nextGrenadeId = 1;
		this.nav = settings.nav && (!settings.nav.map || settings.nav.map === map.metadata.mapName) ? validate_nav(settings.nav) : null;
		this.bots = [this.bot, ...(settings.extraTargets || []).slice(0, 2).map(make_actor)];
		while (this.bots.length < 3) this.bots.push(this.spawn_bot());
		this.botBrains = this.bots.map(make_bot_brain);
		this.caches = this.bots.map(() => null);
		this.events = [];
		this.captureTraversal = false;
	}

	spawn_bot()
	{
		let origin = {...this.bot.origin};
		if (this.nav?.areas.size)
		{
			const areas = [...this.nav.areas.values()];
			const distance = (area) => Math.min(...this.bots.map((bot) => length_sq(sub(area.center, bot.origin))));
			const separated = areas.filter((area) => distance(area) >= 1000 ** 2);
			const chosen = (separated.length ? separated : areas).reduce((best, area) => !best || distance(area) > distance(best) ? area : best, null);
			if (chosen) origin = {...chosen.center};
		}
		else
		{
			const angle = this.bots.length * Math.PI * 2 / 3;
			origin = add(origin, {x: Math.cos(angle) * 1200, y: Math.sin(angle) * 1200, z: 0});
		}
		return make_actor({...origin, yaw: Math.random() * 360});
	}

	set_input(value) { this.playerButtons = {...value}; }
	set_look(yaw, pitch) { if (Number.isFinite(yaw)) this.player.yaw = yaw; if (Number.isFinite(pitch)) this.player.pitch = clamp(pitch, -89, 89); }
	set_targets(values) { if (values instanceof Float32Array && values.length >= 3) this.targets = values; }
	set_debug(value) { this.debug = Boolean(value); }
	request_traversal() { this.captureTraversal = true; }
	set_player_speed(value) { if (Number.isFinite(value) && value > 0) this.playerSpeed = Math.min(value, FPS_CONSTANTS.globalMaxSpeed); }

	throw_grenade(kind, origin, direction, speed = 750)
	{
		if (kind !== "smoke" && kind !== "he" || !finite_vec(origin) || !finite_vec(direction)) return;
		const throwSpeed = clamp(Number(speed) || 750, 250, 750);
		this.grenades.push({id: this.nextGrenadeId++, kind, origin: {...origin}, velocity: add(mul(normalize(direction), throwSpeed), this.player.velocity), fuse: 1.5, lastBounce: -Infinity});
	}

	fire_visual(direction)
	{
		if (!finite_vec(direction)) return;
		const start = add(this.player.origin, {x: 0, y: 0, z: this.player.crouched ? 28.5 : 64});
		const requestedEnd = add(start, mul(normalize(direction), 8192));
		const hit = this.map.segment_hit(start, requestedEnd);
		this.events.push({
			type: "shot",
			start,
			end: hit?.point || requestedEnd,
			hit: Boolean(hit),
			normal: hit?.normal || {x: 0, y: 0, z: 1}
		});
		if (this.smokeCuts.length >= 64) this.smokeCuts.shift();
		this.smokeCuts.push({start, end: hit?.point || requestedEnd, time: this.time});
	}

	choose_bot_route(bot, brain)
	{
		if (!this.nav || this.nav.areas.size < 2) return;
		if (!brain.site && brain.siteKey)
		{
			brain.site = this.nav.objectives[brain.siteKey] || null;
		}
		let candidates;
		if (brain.site && brain.mode === "travel")
		{
			candidates = [nearest_area(this.nav, brain.site)].filter(Boolean);
		}
		else if (brain.site)
		{
			candidates = [...this.nav.areas.values()].filter((area) =>
				length_sq(sub(area.center, brain.site)) < 420 ** 2 && length_sq(sub(area.center, bot.origin)) > 96 ** 2);
		}
		else
		{
			candidates = [...this.nav.areas.values()].filter((area) => length_sq(sub(area.center, bot.origin)) > 1024 ** 2);
		}
		const otherBots = this.bots.filter((other) => other !== bot);
		const otherGoals = this.botBrains.filter((other) => other !== brain && other.destination).map((other) => other.destination);
		const separated = candidates.filter((area) => otherBots.every((other) => length_sq(sub(area.center, other.origin)) >= 1000 ** 2)
			&& otherGoals.every((goal) => length_sq(sub(area.center, goal)) >= 1000 ** 2));
		if (separated.length) candidates = separated;
		const chosen = candidates[Math.floor(Math.random() * candidates.length)] || [...this.nav.areas.values()][Math.floor(Math.random() * this.nav.areas.size)];
		if (!chosen)
		{
			brain.route = [];
			brain.routeIndex = 0;
			return;
		}
		brain.destination = {...chosen.center};
		brain.route = route_between(this.nav, bot.origin, chosen.center);
		brain.routeIndex = brain.route.length > 1 ? 1 : brain.route.length;
	}

	move_bot(bot = this.bot, brain = this.botBrains[0])
	{
		const buttons = {};
		let target = null;
		let desiredYaw = bot.yaw;
		const nearby = this.bots.filter((other) => other !== bot && length_sq(sub(other.origin, bot.origin)) < 1000 ** 2);
		if (nearby.length)
		{
			brain.mode = "reposition";
			brain.site = null;
			brain.siteKey = "";
			brain.route = [];
			brain.routeIndex = 0;
			brain.nextGoal = 0;
			brain.destination = null;
			if (!this.nav)
			{
				const away = nearby.reduce((sum, other) => add(sum, sub(bot.origin, other.origin)), {x: 0, y: 0, z: 0});
				brain.wanderYaw = Math.atan2(away.y, away.x) * 180 / Math.PI;
			}
		}
		if (this.nav)
		{
			if (brain.mode === "camp" && this.time < brain.campUntil)
			{
				desiredYaw = Math.atan2(this.player.origin.y - bot.origin.y, this.player.origin.x - bot.origin.x) * 180 / Math.PI;
			}
			else if (brain.mode === "camp")
			{
				brain.mode = "reposition";
				brain.route = [];
			}
			if (brain.mode !== "camp" && (brain.route.length === 0 || brain.routeIndex >= brain.route.length || this.time >= brain.nextGoal))
			{
				this.choose_bot_route(bot, brain);
				brain.nextGoal = this.time + 30;
			}
			target = brain.mode === "camp" ? null : brain.route[brain.routeIndex];
			while (target && Math.hypot(target.x - bot.origin.x, target.y - bot.origin.y) < 32)
				target = brain.route[++brain.routeIndex];
			if (!target && brain.mode !== "camp" && brain.routeIndex >= brain.route.length)
			{
				if (brain.mode === "travel")
				{
					brain.mode = "reposition";
					brain.route = [];
					this.choose_bot_route(bot, brain);
					target = brain.route[brain.routeIndex];
				}
				else
				{
					brain.mode = "camp";
					brain.campUntil = this.time + 8 + Math.random() * 10;
				}
			}
		}
		if (!target && !this.nav)
		{
			buttons.w = true;
			const yaw = brain.wanderYaw * Math.PI / 180;
			const probe = add(bot.origin, {x: Math.cos(yaw) * 96, y: Math.sin(yaw) * 96, z: 36});
			if (this.map.segment_blocked(add(bot.origin, {x: 0, y: 0, z: 36}), probe).blocked) brain.wanderYaw += 90 + Math.random() * 90;
			desiredYaw = brain.wanderYaw;
		}
		else
		{
			if (!target)
			{
				const turn = ((desiredYaw - bot.yaw + 540) % 360) - 180;
				bot.yaw += clamp(turn, -120 * FPS_DT, 120 * FPS_DT);
				move_actor(this.map, bot, buttons, this.botSpeed);
				return;
			}
			buttons.w = true;
			buttons.walk = brain.mode === "reposition" || brain.site && length_sq(sub(bot.origin, brain.site)) < 480 ** 2;
			desiredYaw = Math.atan2(target.y - bot.origin.y, target.x - bot.origin.x) * 180 / Math.PI;
			const dz = target.z - bot.origin.z;
			buttons.jump = dz > FPS_CONSTANTS.stepHeight && dz <= 68
				|| brain.blockedTicks > FPS_TICK_RATE / 3 && bot.grounded;
			if (target.traversal === "ladder" && Math.abs(dz) > 8)
			{
				buttons.ladder = true;
				buttons.ladderDirection = Math.sign(dz);
			}
		}
		const turn = ((desiredYaw - bot.yaw + 540) % 360) - 180;
		bot.yaw += clamp(turn, -240 * FPS_DT, 240 * FPS_DT);
		const wasGrounded = bot.grounded;
		move_actor(this.map, bot, buttons, this.botSpeed);
		const jumpProgress = Math.hypot(bot.origin.x - brain.jumpOrigin.x, bot.origin.y - brain.jumpOrigin.y);
		if (jumpProgress >= 64)
		{
			brain.jumpOrigin = {...bot.origin};
			brain.failedJumps = 0;
		}
		if (buttons.jump && wasGrounded && !bot.grounded && ++brain.failedJumps >= 3)
		{
			brain.mode = "reposition";
			brain.site = null;
			brain.route = [];
			brain.routeIndex = 0;
			brain.nextGoal = 0;
			brain.wanderYaw += 90 + Math.random() * 90;
			brain.jumpOrigin = {...bot.origin};
			brain.failedJumps = 0;
		}
		brain.blockedTicks = bot.speed < 5 ? brain.blockedTicks + 1 : 0;
		if (bot.grounded && bot.speed > 40) brain.safeOrigin = {...bot.origin};
		if (brain.blockedTicks > FPS_TICK_RATE)
		{
			bot.origin = {...brain.safeOrigin};
			bot.velocity = {x: 0, y: 0, z: 0};
			brain.route = [];
			brain.nextGoal = 0;
			brain.wanderYaw += 90;
			brain.blockedTicks = 0;
		}
	}

	move_grenades()
	{
		for (const grenade of this.grenades)
		{
			grenade.fuse -= FPS_DT;
			grenade.velocity.z -= 320 * FPS_DT;
			const wanted = add(grenade.origin, mul(grenade.velocity, FPS_DT));
			const hit = this.map.segment_hit(grenade.origin, wanted);
			if (hit)
			{
				grenade.origin = add(hit.point, mul(hit.normal, 0.5));
				const into = dot(grenade.velocity, hit.normal);
				if (length(grenade.velocity) > 80 && this.time - grenade.lastBounce >= 0.08)
				{
					grenade.lastBounce = this.time;
					this.events.push({type: "grenade-bounce", kind: grenade.kind, center: {...grenade.origin}});
				}
				grenade.velocity = mul(sub(grenade.velocity, mul(hit.normal, 2 * into)), 0.45);
			}
			else grenade.origin = wanted;
		}
		const detonated = this.grenades.filter((grenade) => grenade.fuse <= 0);
		this.grenades = this.grenades.filter((grenade) => grenade.fuse > 0);
		for (const grenade of detonated)
		{
			if (grenade.kind === "smoke" && this.smokes.length < SMOKE_LIMIT)
			{
				const smoke = make_test_smoke(grenade.origin, this.time, this.map);
				this.smokes.push(smoke);
				this.events.push({type: "smoke-created", center: smoke.center, startTime: smoke.startTime, cells: smoke.visibleCells});
			}
			else if (grenade.kind === "he")
			{
				if (this.clearances.length >= HE_LIMIT) this.clearances.shift();
				this.clearances.push({center: {...grenade.origin}, time: this.time});
				this.events.push({type: "he-detonated", center: {...grenade.origin}, time: this.time});
			}
		}
		this.smokes = this.smokes.filter((smoke) => this.time - smoke.startTime < 22.5);
		this.clearances = this.clearances.filter((clearance) => this.time - clearance.time < HE_SECONDS);
		this.smokeCuts = this.smokeCuts.filter((cut) => this.time - cut.time < BULLET_SMOKE_SECONDS);
	}

	visibility(bot, botIndex, captureTraversal = false)
	{
		const targetValues = botIndex === 0 && this.targets || default_targets(bot);
		const targets = [];
		for (let index = 0; index < targetValues.length; index += 3) targets.push({x: targetValues[index], y: targetValues[index + 1], z: targetValues[index + 2]});
		const traversal = captureTraversal ? this.map.create_traversal() : null;
		const origins = runtime_origins(this.map, {
			origin: this.player.origin,
			yaw: this.player.yaw,
			pingMs: this.pingMs,
			buttons: this.playerButtons
		}, traversal);
		const rayCount = origins.length * targets.length;
		const previousCache = this.caches[botIndex];
		const cache = previousCache?.length === rayCount ? previousCache : new Uint32Array(rayCount).fill(BVH8_INVALID_REF);
		const blocked = new Uint8Array(rayCount);
		let clearCount = 0;
		let ray = 0;
		for (const origin of origins)
		{
			for (const target of targets)
			{
				const wall = this.map.segment_blocked(origin, target, cache[ray], traversal);
				cache[ray] = wall.packet;
				blocked[ray] = wall.blocked ? 1 : smoke_line_blocked(this.map, this.smokes, this.clearances, origin, target, this.time, this.smokeCuts) ? 2 : 0;
				clearCount += Number(blocked[ray] === 0);
				++ray;
			}
		}
		this.caches[botIndex] = cache;
		const originValues = new Float32Array(origins.length * 3);
		origins.forEach((origin, index) => originValues.set([origin.x, origin.y, origin.z], index * 3));
		return {origins: originValues, targets: new Float32Array(targetValues), blocked, clearCount, visible: clearCount > 0,
			traversal: traversal ? this.map.finish_traversal(traversal) : null};
	}

	step()
	{
		this.time += FPS_DT;
		const playerButtons = {...this.playerButtons, ...nearby_ladder(this.nav, this.player, this.playerButtons)};
		move_actor(this.map, this.player, playerButtons, this.playerSpeed);
		this.bots.forEach((bot, index) => this.move_bot(bot, this.botBrains[index]));
		this.move_grenades();
		const captureTraversal = this.captureTraversal;
		this.captureTraversal = false;
		const visibilities = this.bots.map((bot, index) => this.visibility(bot, index, captureTraversal));
		const visibility = visibilities[0];
		this.bot = this.bots[0];
		return {
			time: this.time,
			player: this.player,
			bot: this.bot,
			bots: this.bots,
			grenades: this.grenades.map((grenade) => ({id: grenade.id, kind: grenade.kind, origin: grenade.origin})),
			clearances: this.clearances.map((clearance) => ({center: {...clearance.center}, time: clearance.time})),
			smokeCuts: this.smokeCuts.map((cut) => ({start: {...cut.start}, end: {...cut.end}, time: cut.time})),
			smokeCount: this.smokes.length,
			heCount: this.clearances.length,
			route: this.botBrains[0].route.slice(this.botBrains[0].routeIndex),
			routes: this.botBrains.map((brain) => brain.route.slice(brain.routeIndex)),
			botMode: this.botBrains[0].mode,
			visibility,
			visibilities,
			events: this.events.splice(0)
		};
	}
}
