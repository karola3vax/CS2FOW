// Browser port of the runtime's target-fitted 32x32 capsule visibility query.
// Map triangles are rasterized into an inverse-depth buffer; only geometry-clear
// capsule samples become exact smoke rays. Uncertainty fails open.

export const VISIBILITY_CAPSULE_COUNT = 19;
export const VISIBILITY_GRID_SIZE = 32;
export const VISIBILITY_CAPSULE_FLOATS = VISIBILITY_CAPSULE_COUNT * 7;

const PIXEL_COUNT = VISIBILITY_GRID_SIZE ** 2;
const EPSILON = 1.0e-5;
const NEAR_DEPTH = 0.125;
const VIEW_MARGIN = 1.02;
const DEPTH_EPSILON = 1.0e-5;
const OUTER_RADIAL_SCALE = 1.06;
const now_ms = () => globalThis.performance?.now?.() ?? Date.now();
const add = (a, b) => ({x: a.x + b.x, y: a.y + b.y, z: a.z + b.z});
const subtract = (a, b) => ({x: a.x - b.x, y: a.y - b.y, z: a.z - b.z});
const scale = (a, amount) => ({x: a.x * amount, y: a.y * amount, z: a.z * amount});
const dot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
const cross = (a, b) => ({x: a.y * b.z - a.z * b.y, y: a.z * b.x - a.x * b.z,
	z: a.x * b.y - a.y * b.x});
const length_sq = (a) => dot(a, a);
const finite_vec = (a) => Number.isFinite(a.x) && Number.isFinite(a.y) && Number.isFinite(a.z);

function normalize(value)
{
	const squared = length_sq(value);
	return Number.isFinite(squared) && squared > EPSILON ? scale(value, 1 / Math.sqrt(squared)) : null;
}

function point_segment_distance_sq(point, start, end)
{
	const segment = subtract(end, start);
	const denominator = length_sq(segment);
	const amount = denominator <= EPSILON ? 0 : Math.min(1, Math.max(0, dot(subtract(point, start), segment) / denominator));
	return length_sq(subtract(point, add(start, scale(segment, amount))));
}

function ray_sphere(origin, direction, center, radius)
{
	const offset = subtract(origin, center);
	const a = length_sq(direction);
	const b = dot(offset, direction);
	const c = length_sq(offset) - radius * radius;
	const discriminant = b * b - a * c;
	if (a <= EPSILON || discriminant < 0) return null;
	const root = Math.sqrt(Math.max(0, discriminant));
	let distance = (-b - root) / a;
	if (distance <= EPSILON) distance = (-b + root) / a;
	return Number.isFinite(distance) && distance > EPSILON ? distance : null;
}

function ray_capsule(origin, direction, capsule)
{
	const axis = subtract(capsule.end, capsule.start);
	const axisLengthSq = length_sq(axis);
	if (axisLengthSq <= EPSILON) return ray_sphere(origin, direction, capsule.start, capsule.radius);
	const offset = subtract(origin, capsule.start);
	const directionLengthSq = length_sq(direction);
	const axisDirection = dot(axis, direction);
	const axisOffset = dot(axis, offset);
	const directionOffset = dot(direction, offset);
	const a = axisLengthSq * directionLengthSq - axisDirection * axisDirection;
	const b = axisLengthSq * directionOffset - axisOffset * axisDirection;
	const c = axisLengthSq * (length_sq(offset) - capsule.radius * capsule.radius) - axisOffset * axisOffset;
	if (a > EPSILON)
	{
		const discriminant = b * b - a * c;
		if (discriminant >= 0)
		{
			const distance = (-b - Math.sqrt(Math.max(0, discriminant))) / a;
			const alongAxis = axisOffset + distance * axisDirection;
			if (distance > EPSILON && alongAxis > 0 && alongAxis < axisLengthSq) return distance;
		}
	}
	const startDistance = ray_sphere(origin, direction, capsule.start, capsule.radius);
	const endDistance = ray_sphere(origin, direction, capsule.end, capsule.radius);
	if (startDistance === null) return endDistance;
	if (endDistance === null) return startDistance;
	return Math.min(startDistance, endDistance);
}

function read_capsules(values)
{
	if (!(values instanceof Float32Array) || values.length !== VISIBILITY_CAPSULE_FLOATS) return null;
	const capsules = [];
	for (let index = 0; index < values.length; index += 7)
	{
		const capsule = {
			start: {x: values[index], y: values[index + 1], z: values[index + 2]},
			end: {x: values[index + 3], y: values[index + 4], z: values[index + 5]},
			radius: values[index + 6]
		};
		if (!finite_vec(capsule.start) || !finite_vec(capsule.end)
			|| !Number.isFinite(capsule.radius) || capsule.radius <= 0) return null;
		capsules.push(capsule);
	}
	return capsules;
}

function to_camera(view, point)
{
	const offset = subtract(point, view.origin);
	return {x: dot(offset, view.right), y: dot(offset, view.up), z: dot(offset, view.forward)};
}

function build_view(origin, box)
{
	const forward = normalize(subtract(scale(add(box.min, box.max), 0.5), origin));
	if (!forward) return null;
	const right = normalize(cross(forward, {x: 0, y: 0, z: 1}))
		|| normalize(cross(forward, {x: 0, y: 1, z: 0}));
	if (!right) return null;
	const view = {origin, forward, right, up: cross(right, forward), horizontal: EPSILON,
		vertical: EPSILON, farDepth: 0, nearDepth: NEAR_DEPTH};
	for (let corner = 0; corner < 8; ++corner)
	{
		const camera = to_camera(view, {
			x: corner & 1 ? box.max.x : box.min.x,
			y: corner & 2 ? box.max.y : box.min.y,
			z: corner & 4 ? box.max.z : box.min.z
		});
		if (!Number.isFinite(camera.z) || camera.z <= NEAR_DEPTH) return null;
		view.horizontal = Math.max(view.horizontal, Math.abs(camera.x / camera.z));
		view.vertical = Math.max(view.vertical, Math.abs(camera.y / camera.z));
		view.farDepth = Math.max(view.farDepth, camera.z);
	}
	view.horizontal *= VIEW_MARGIN;
	view.vertical *= VIEW_MARGIN;
	return Number.isFinite(view.horizontal) && Number.isFinite(view.vertical)
		&& Number.isFinite(view.farDepth) && view.farDepth > NEAR_DEPTH ? view : null;
}

function project_bounds(view, box)
{
	const result = {minimumX: 1, maximumX: -1, minimumY: 1, maximumY: -1, minimumDepth: Infinity};
	for (let corner = 0; corner < 8; ++corner)
	{
		const camera = to_camera(view, {
			x: corner & 1 ? box.max.x : box.min.x,
			y: corner & 2 ? box.max.y : box.min.y,
			z: corner & 4 ? box.max.z : box.min.z
		});
		if (camera.z <= NEAR_DEPTH) return null;
		const x = camera.x / (camera.z * view.horizontal);
		const y = camera.y / (camera.z * view.vertical);
		result.minimumX = Math.min(result.minimumX, x);
		result.maximumX = Math.max(result.maximumX, x);
		result.minimumY = Math.min(result.minimumY, y);
		result.maximumY = Math.max(result.maximumY, y);
		result.minimumDepth = Math.min(result.minimumDepth, camera.z);
	}
	const grid = VISIBILITY_GRID_SIZE;
	result.firstX = Math.min(grid - 1, Math.max(0, Math.floor((result.minimumX + 1) * 0.5 * grid)));
	result.lastX = Math.min(grid - 1, Math.max(0, Math.ceil((result.maximumX + 1) * 0.5 * grid) - 1));
	result.firstY = Math.min(grid - 1, Math.max(0, Math.floor((1 - result.maximumY) * 0.5 * grid)));
	result.lastY = Math.min(grid - 1, Math.max(0, Math.ceil((1 - result.minimumY) * 0.5 * grid) - 1));
	return result.firstX <= result.lastX && result.firstY <= result.lastY ? result : null;
}

function clip_polygon(points, distance)
{
	const result = [];
	for (let index = 0; index < points.length; ++index)
	{
		const current = points[index];
		const previous = points[(index + points.length - 1) % points.length];
		const currentDistance = distance(current);
		const previousDistance = distance(previous);
		const currentInside = currentDistance >= 0;
		const previousInside = previousDistance >= 0;
		if (currentInside !== previousInside)
		{
			const amount = previousDistance / (previousDistance - currentDistance);
			result.push({x: previous.x + (current.x - previous.x) * amount,
				y: previous.y + (current.y - previous.y) * amount,
				z: previous.z + (current.z - previous.z) * amount});
		}
		if (currentInside) result.push(current);
	}
	return result;
}

function clip_triangle(view, points)
{
	let polygon = points;
	for (const plane of [
		(point) => point.z - view.nearDepth,
		(point) => view.farDepth - point.z,
		(point) => point.x + view.horizontal * point.z,
		(point) => view.horizontal * point.z - point.x,
		(point) => point.y + view.vertical * point.z,
		(point) => view.vertical * point.z - point.y
	])
	{
		polygon = clip_polygon(polygon, plane);
		if (polygon.length < 3) return [];
	}
	return polygon;
}

const edge = (a, b, x, y) => (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);

function raster_triangle(view, first, second, third, visit)
{
	const projected = [first, second, third].map((point) => ({
		x: (point.x / (point.z * view.horizontal) + 1) * 0.5 * VISIBILITY_GRID_SIZE,
		y: (1 - point.y / (point.z * view.vertical)) * 0.5 * VISIBILITY_GRID_SIZE,
		inverseDepth: 1 / point.z
	}));
	const area = edge(projected[0], projected[1], projected[2].x, projected[2].y);
	if (!Number.isFinite(area) || Math.abs(area) <= EPSILON) return true;
	const firstX = Math.max(0, Math.floor(Math.min(...projected.map((point) => point.x)) - 0.5));
	const lastX = Math.min(VISIBILITY_GRID_SIZE - 1, Math.floor(Math.max(...projected.map((point) => point.x)) - 0.5));
	const firstY = Math.max(0, Math.floor(Math.min(...projected.map((point) => point.y)) - 0.5));
	const lastY = Math.min(VISIBILITY_GRID_SIZE - 1, Math.floor(Math.max(...projected.map((point) => point.y)) - 0.5));
	for (let y = firstY; y <= lastY; ++y)
	{
		for (let x = firstX; x <= lastX; ++x)
		{
			const px = x + 0.5;
			const py = y + 0.5;
			const w0 = edge(projected[1], projected[2], px, py) / area;
			const w1 = edge(projected[2], projected[0], px, py) / area;
			const w2 = 1 - w0 - w1;
			if (w0 < -EPSILON || w1 < -EPSILON || w2 < -EPSILON) continue;
			const inverseDepth = w0 * projected[0].inverseDepth + w1 * projected[1].inverseDepth
				+ w2 * projected[2].inverseDepth;
			if (visit(y * VISIBILITY_GRID_SIZE + x, inverseDepth) === false) return false;
		}
	}
	return true;
}

function raster_polygon(view, polygon, visit)
{
	for (let index = 1; index + 1 < polygon.length; ++index)
	{
		if (!raster_triangle(view, polygon[0], polygon[index], polygon[index + 1], visit)) return false;
	}
	return true;
}

function all_projections_occluded(projections, depth)
{
	const combined = {
		minimumDepth: Math.min(...projections.map((value) => value.minimumDepth)),
		firstX: Math.min(...projections.map((value) => value.firstX)),
		lastX: Math.max(...projections.map((value) => value.lastX)),
		firstY: Math.min(...projections.map((value) => value.firstY)),
		lastY: Math.max(...projections.map((value) => value.lastY))
	};
	return projected_bounds_occluded(combined, depth)
		|| projections.every((projection) => projected_bounds_occluded(projection, depth));
}

function raster_map(map, view, projections, deadline, depth, traversal, stats, previousCache)
{
	let checked = 0;
	const raster = (_packet, _lane, triangle) =>
	{
		if ((checked++ & 63) === 0 && now_ms() >= deadline) return false;
		++stats.rasterizedTriangles;
		const polygon = clip_triangle(view, [to_camera(view, triangle.v0), to_camera(view, triangle.v1), to_camera(view, triangle.v2)]);
		return raster_polygon(view, polygon, (pixel, inverseDepth) =>
		{
			depth[pixel] = Math.max(depth[pixel], inverseDepth);
			return true;
		});
	};
	const cached = Array.isArray(previousCache) ? previousCache.slice(0, 8) : [];
	if (cached.length && typeof map.for_each_packet_triangle === "function")
	{
		let valid = true;
		for (const packet of cached)
		{
			if (!map.for_each_packet_triangle(packet, raster, traversal))
			{
				if (now_ms() >= deadline) return {complete: false, occluded: false, cache: cached};
				valid = false;
				break;
			}
		}
		if (valid && all_projections_occluded(projections, depth))
			return {complete: true, occluded: true, cache: cached};
		depth.fill(0);
	}
	const candidate = [];
	let occluded = false;
	const complete = map.for_each_triangle_in_view(view, raster, traversal, stats, (packet) =>
	{
		if (candidate.length < 8) candidate.push(packet);
		occluded = all_projections_occluded(projections, depth);
		return !occluded;
	});
	return {complete: complete || occluded, occluded,
		cache: occluded ? candidate : complete ? [] : cached};
}

function center_out(value)
{
	const half = VISIBILITY_GRID_SIZE / 2;
	return value % 2 === 0 ? half - 1 - value / 2 : half + Math.floor(value / 2);
}

function projected_bounds_occluded(projection, depth)
{
	const conservativeTargetDepth = 1 / (projection.minimumDepth * (1 - 1.0e-4));
	for (let y = projection.firstY; y <= projection.lastY; ++y)
		for (let x = projection.firstX; x <= projection.lastX; ++x)
			if (depth[y * VISIBILITY_GRID_SIZE + x] <= conservativeTargetDepth * (1 + DEPTH_EPSILON)) return false;
	return true;
}

function capsule_outer_mesh(capsule)
{
	const sides = 12;
	const segments = 4;
	const rings = segments * 2;
	let axis = normalize(subtract(capsule.end, capsule.start)) || {x: 0, y: 0, z: 1};
	const side = normalize(cross(axis, {x: 0, y: 0, z: 1})) || normalize(cross(axis, {x: 0, y: 1, z: 0}));
	if (!side) return null;
	const up = cross(side, axis);
	const radius = capsule.radius * OUTER_RADIAL_SCALE;
	const points = [add(capsule.start, scale(axis, -radius))];
	for (let ring = 1; ring <= segments; ++ring)
	{
		const latitude = -0.5 * Math.PI + 0.5 * Math.PI * ring / segments;
		for (let lane = 0; lane < sides; ++lane)
		{
			const longitude = 2 * Math.PI * lane / sides;
			const radial = add(scale(side, Math.cos(longitude)), scale(up, Math.sin(longitude)));
			points.push(add(capsule.start, add(scale(axis, Math.sin(latitude) * radius),
				scale(radial, Math.cos(latitude) * radius))));
		}
	}
	for (let ring = 0; ring < segments; ++ring)
	{
		const latitude = 0.5 * Math.PI * ring / segments;
		for (let lane = 0; lane < sides; ++lane)
		{
			const longitude = 2 * Math.PI * lane / sides;
			const radial = add(scale(side, Math.cos(longitude)), scale(up, Math.sin(longitude)));
			points.push(add(capsule.end, add(scale(axis, Math.sin(latitude) * radius),
				scale(radial, Math.cos(latitude) * radius))));
		}
	}
	points.push(add(capsule.end, scale(axis, radius)));
	const faces = [];
	const ringStart = (ring) => 1 + ring * sides;
	const top = points.length - 1;
	for (let lane = 0; lane < sides; ++lane)
	{
		const next = (lane + 1) % sides;
		faces.push([0, ringStart(0) + next, ringStart(0) + lane]);
		for (let ring = 0; ring + 1 < rings; ++ring)
		{
			const lower = ringStart(ring);
			const upper = ringStart(ring + 1);
			faces.push([lower + lane, lower + next, upper + lane], [lower + next, upper + next, upper + lane]);
		}
		faces.push([ringStart(rings - 1) + lane, ringStart(rings - 1) + next, top]);
	}
	return {points, faces};
}

function test_outer_mesh(view, capsule, mapDepth)
{
	const mesh = capsule_outer_mesh(capsule);
	if (!mesh) return "indeterminate";
	let covered = false;
	let visible = false;
	for (const face of mesh.faces)
	{
		const polygon = clip_triangle(view, face.map((index) => to_camera(view, mesh.points[index])));
		raster_polygon(view, polygon, (pixel, inverseDepth) =>
		{
			covered = true;
			if (mapDepth[pixel] <= inverseDepth * (1 + DEPTH_EPSILON))
			{
				visible = true;
				return false;
			}
			return true;
		});
		if (visible) return "visible";
	}
	return covered ? "occluded" : "indeterminate";
}

export function capsule_visible_from_origin(map, origin, capsuleValues, options = {})
{
	const stats = {sampledPixels: 0, tracedRays: 0, visitedNodes: 0, rasterizedTriangles: 0};
	const rays = [];
	const blocked = [];
	const capsules = read_capsules(capsuleValues);
	if (!capsules || !finite_vec(origin)) return {result: "indeterminate", stats, rays, blocked};
	const body = {min: {x: Infinity, y: Infinity, z: Infinity}, max: {x: -Infinity, y: -Infinity, z: -Infinity}};
	for (const capsule of capsules)
	{
		if (point_segment_distance_sq(origin, capsule.start, capsule.end) <= capsule.radius ** 2)
			return {result: "visible", stats, rays, blocked};
		for (const point of [capsule.start, capsule.end])
		{
			body.min.x = Math.min(body.min.x, point.x - capsule.radius);
			body.min.y = Math.min(body.min.y, point.y - capsule.radius);
			body.min.z = Math.min(body.min.z, point.z - capsule.radius);
			body.max.x = Math.max(body.max.x, point.x + capsule.radius);
			body.max.y = Math.max(body.max.y, point.y + capsule.radius);
			body.max.z = Math.max(body.max.z, point.z + capsule.radius);
		}
	}
	const view = build_view(origin, body);
	if (!view) return {result: "indeterminate", stats, rays, blocked};
	const projections = capsules.map((capsule) => project_bounds(view, {
		min: {x: Math.min(capsule.start.x, capsule.end.x) - capsule.radius,
			y: Math.min(capsule.start.y, capsule.end.y) - capsule.radius,
			z: Math.min(capsule.start.z, capsule.end.z) - capsule.radius},
		max: {x: Math.max(capsule.start.x, capsule.end.x) + capsule.radius,
			y: Math.max(capsule.start.y, capsule.end.y) + capsule.radius,
			z: Math.max(capsule.start.z, capsule.end.z) + capsule.radius}
	}));
	if (projections.some((projection) => !projection)) return {result: "indeterminate", stats, rays, blocked};
	const deadline = Number.isFinite(options.deadline) ? options.deadline : now_ms() + 75;
	const depth = new Float32Array(PIXEL_COUNT);
	const traversal = options.traversal || null;
	const mapResult = raster_map(map, view, projections, deadline, depth, traversal, stats, options.occluderCache);
	if (!mapResult.complete)
		return {result: "indeterminate", stats, rays, blocked};
	if (mapResult.occluded)
		return {result: "blocked", reason: "wall", stats, rays, blocked, occluderCache: mapResult.cache};
	const smokeActive = options.smokeActive ?? typeof options.smokeBlocked === "function";
	if (!smokeActive)
	{
		for (let index = 0; index < capsules.length; ++index)
		{
			if (projected_bounds_occluded(projections[index], depth)) continue;
			const result = test_outer_mesh(view, capsules[index], depth);
			if (result === "visible")
				return {result: "visible", stats, rays, blocked, occluderCache: mapResult.cache};
			if (result !== "occluded")
				return {result: "indeterminate", stats, rays, blocked, occluderCache: mapResult.cache};
		}
		return {result: "blocked", reason: "wall", stats, rays, blocked, occluderCache: mapResult.cache};
	}
	let geometryVisibleSample = false;
	for (let capsuleIndex = 0; capsuleIndex < capsules.length; ++capsuleIndex)
	{
		const capsule = capsules[capsuleIndex];
		const projection = projections[capsuleIndex];
		if (projected_bounds_occluded(projection, depth)) continue;
		for (let orderedY = 0; orderedY < VISIBILITY_GRID_SIZE; ++orderedY)
		{
			if (now_ms() >= deadline)
				return {result: "indeterminate", stats, rays, blocked, occluderCache: mapResult.cache};
			const y = center_out(orderedY);
			if (y < projection.firstY || y > projection.lastY) continue;
			const screenY = 1 - (2 * y + 1) / VISIBILITY_GRID_SIZE;
			for (let orderedX = 0; orderedX < VISIBILITY_GRID_SIZE; ++orderedX)
			{
				const x = center_out(orderedX);
				if (x < projection.firstX || x > projection.lastX) continue;
				const screenX = (2 * x + 1) / VISIBILITY_GRID_SIZE - 1;
				const direction = add(view.forward, add(scale(view.right, screenX * view.horizontal),
					scale(view.up, screenY * view.vertical)));
				++stats.sampledPixels;
				const distance = ray_capsule(origin, direction, capsule);
				if (distance === null) continue;
				const pixel = y * VISIBILITY_GRID_SIZE + x;
				const targetDepth = 1 / distance;
				if (depth[pixel] > targetDepth * (1 + DEPTH_EPSILON)) continue;
				const target = add(origin, scale(direction, distance));
				geometryVisibleSample = true;
				++stats.tracedRays;
				const smokeBlocked = Boolean(options.smokeBlocked?.(origin, target));
				if (options.debug)
				{
					rays.push(origin.x, origin.y, origin.z, target.x, target.y, target.z);
					blocked.push(smokeBlocked ? 2 : 0);
				}
				if (!smokeBlocked)
					return {result: "visible", stats, rays, blocked, occluderCache: mapResult.cache};
			}
		}
	}
	if (geometryVisibleSample)
		return {result: "blocked", reason: "smoke", stats, rays, blocked, occluderCache: mapResult.cache};
	for (let index = 0; index < capsules.length; ++index)
	{
		if (now_ms() >= deadline)
			return {result: "indeterminate", stats, rays, blocked, occluderCache: mapResult.cache};
		if (projected_bounds_occluded(projections[index], depth)) continue;
		if (test_outer_mesh(view, capsules[index], depth) !== "occluded")
			return {result: "indeterminate", stats, rays, blocked, occluderCache: mapResult.cache};
	}
	return {result: "blocked", reason: "wall", stats, rays, blocked, occluderCache: mapResult.cache};
}
