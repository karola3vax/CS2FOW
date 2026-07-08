import * as THREE from "https://esm.sh/three@0.160.0";
import {OrbitControls} from "https://esm.sh/three@0.160.0/examples/jsm/controls/OrbitControls.js";
import {GLTFLoader} from "https://esm.sh/three@0.160.0/examples/jsm/loaders/GLTFLoader.js";
import {OBJLoader} from "https://esm.sh/three@0.160.0/examples/jsm/loaders/OBJLoader.js";

const k_source_units_per_meter = 39.3700787;
const k_max_prediction_speed = 500.0;
const k_min_prediction_speed = 1.0;
const k_bounds_inflate = 12.0;
const k_shoulder_origin_offset = 24.0;
const k_vertical_origin_offset = 24.0;
const k_same_point_epsilon_sq = 1.0e-4;
const k_rtt_lookahead_scale = 2.0;
const k_degrees_to_radians = 0.017453292519943295769;
const k_min_lookahead_ms = 200.0;
const k_max_lookahead_ms = 500.0;
const k_peek_margin_units = 160.0;
const k_visibility_origin_count = 10;
const k_visibility_target_count = 16;
const k_visibility_ray_count = k_visibility_origin_count * k_visibility_target_count;
const k_origin_labels = [
	"eye", "predicted eye", "left shoulder", "right shoulder", "predicted left shoulder",
	"predicted right shoulder", "eye +24z", "eye -24z", "predicted eye +24z", "predicted eye -24z"
];
const k_target_labels = [
	"min/min/min", "max/min/min", "min/max/min", "max/max/min",
	"min/min/max", "max/min/max", "min/max/max", "max/max/max",
	"x-min face", "x-max face", "y-min face", "y-max face",
	"bottom face", "top face", "center", "upper/chest"
];

const k_bvh8_version = 1;
const k_bvh8_known_flags = 1;
const k_bvh8_header_size = 256;
const k_bvh8_node_size = 224;
const k_triangle_packet8_size = 288;
const k_invalid_ref = 0xffffffff;
const k_leaf_ref = 0x80000000;
const k_leaf_index_mask = 0x0fffffff;
const k_ray_epsilon = 1.0e-5;

const $ = (id) => document.getElementById(id);

let renderer;
let camera;
let controls;
let scene;
let clock;
let loader;
let obj_loader;
let model_group;
let map_group;
let overlay_group;
let hit_group;
let active_model_key = "ct_sas";
let active_model = null;
let active_mixer = null;
let active_bvh = null;
let status_extra = "No BVH8 loaded.";
let map_status = "Map OBJ: none.";
const loaded_models = new Map();
const model_paths = new Map();
const loading_models = new Set();

function vec3(x = 0, y = 0, z = 0)
{
	return {x, y, z};
}

function add(a, b)
{
	return vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

function sub(a, b)
{
	return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

function mul(a, scale)
{
	return vec3(a.x * scale, a.y * scale, a.z * scale);
}

function dot(a, b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

function cross(a, b)
{
	return vec3(
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	);
}

function distance_sq(a, b)
{
	const x = a.x - b.x;
	const y = a.y - b.y;
	const z = a.z - b.z;
	return x * x + y * y + z * z;
}

function source_to_three(value)
{
	return new THREE.Vector3(
		value.y / k_source_units_per_meter,
		value.z / k_source_units_per_meter,
		value.x / k_source_units_per_meter
	);
}

function clamp(value, minimum, maximum)
{
	return Math.min(Math.max(value, minimum), maximum);
}

function read_number(id)
{
	const value = Number($(id).value);
	return Number.isFinite(value) ? value : 0;
}

function read_observer()
{
	return {
		eye: vec3(read_number("eye-x"), read_number("eye-y"), read_number("eye-z")),
		velocity: vec3(read_number("obs-vx"), read_number("obs-vy"), read_number("obs-vz")),
		eye_yaw_degrees: read_number("eye-yaw")
	};
}

function read_target()
{
	return {
		origin: vec3(read_number("target-x"), read_number("target-y"), read_number("target-z")),
		velocity: vec3(read_number("target-vx"), read_number("target-vy"), read_number("target-vz")),
		mins: vec3(read_number("min-x"), read_number("min-y"), read_number("min-z")),
		maxs: vec3(read_number("max-x"), read_number("max-y"), read_number("max-z"))
	};
}

function effective_lookahead_seconds(ping_ms)
{
	return clamp(k_min_lookahead_ms + Math.max(0, ping_ms) * k_rtt_lookahead_scale, 0, k_max_lookahead_ms) / 1000.0;
}

function visibility_prediction_offset(velocity, seconds)
{
	if (seconds <= 0)
	{
		return vec3();
	}

	const speed = Math.hypot(velocity.x, velocity.y);
	if (speed <= k_min_prediction_speed)
	{
		return vec3();
	}

	const capped_speed = Math.min(speed, k_max_prediction_speed);
	const distance = Math.max(capped_speed * seconds, stepped_peek_margin(speed, k_peek_margin_units));
	const scale = distance / speed;
	return vec3(velocity.x * scale, velocity.y * scale, 0);
}

function stepped_peek_margin(speed, max_margin)
{
	max_margin = Math.max(0, max_margin);
	if (speed < 25)
	{
		return max_margin * 0.10;
	}
	if (speed < 75)
	{
		return max_margin * 0.20;
	}
	if (speed < 150)
	{
		return max_margin * 0.40;
	}
	if (speed < 250)
	{
		return max_margin * 0.60;
	}
	if (speed < 300)
	{
		return max_margin * 0.80;
	}
	return max_margin;
}

function player_bounds(player, origin, inflate)
{
	return {
		min: vec3(
			origin.x + player.mins.x - inflate,
			origin.y + player.mins.y - inflate,
			origin.z + player.mins.z - inflate
		),
		max: vec3(
			origin.x + player.maxs.x + inflate,
			origin.y + player.maxs.y + inflate,
			origin.z + player.maxs.z + inflate
		)
	};
}

function merge_bounds(a, b)
{
	return {
		min: vec3(Math.min(a.min.x, b.min.x), Math.min(a.min.y, b.min.y), Math.min(a.min.z, b.min.z)),
		max: vec3(Math.max(a.max.x, b.max.x), Math.max(a.max.y, b.max.y), Math.max(a.max.z, b.max.z))
	};
}

function center_bounds(value)
{
	return vec3(
		(value.min.x + value.max.x) * 0.5,
		(value.min.y + value.max.y) * 0.5,
		(value.min.z + value.max.z) * 0.5
	);
}

function safe_origin(bvh, eye, candidate)
{
	if (distance_sq(eye, candidate) <= k_same_point_epsilon_sq)
	{
		return eye;
	}
	if (bvh && bvh.segment_blocked(eye, candidate).blocked)
	{
		return eye;
	}
	return candidate;
}

function eye_right(yaw_degrees)
{
	const yaw = yaw_degrees * k_degrees_to_radians;
	return vec3(Math.sin(yaw), -Math.cos(yaw), 0);
}

function visibility_origins(bvh, player, lookahead_seconds)
{
	const predicted = add(player.eye, visibility_prediction_offset(player.velocity, lookahead_seconds));
	const shoulder = mul(eye_right(player.eye_yaw_degrees || 0), k_shoulder_origin_offset);
	const left = sub(player.eye, shoulder);
	const right = add(player.eye, shoulder);
	const predicted_left = sub(predicted, shoulder);
	const predicted_right = add(predicted, shoulder);
	const vertical = vec3(0, 0, k_vertical_origin_offset);
	const up = add(player.eye, vertical);
	const down = sub(player.eye, vertical);
	const predicted_up = add(predicted, vertical);
	const predicted_down = sub(predicted, vertical);

	return [
		player.eye,
		safe_origin(bvh, player.eye, predicted),
		safe_origin(bvh, player.eye, left),
		safe_origin(bvh, player.eye, right),
		safe_origin(bvh, player.eye, predicted_left),
		safe_origin(bvh, player.eye, predicted_right),
		safe_origin(bvh, player.eye, up),
		safe_origin(bvh, player.eye, down),
		safe_origin(bvh, player.eye, predicted_up),
		safe_origin(bvh, player.eye, predicted_down)
	];
}

function visibility_targets(bvh, player, lookahead_seconds)
{
	let box = player_bounds(player, player.origin, k_bounds_inflate);
	const current_box = box;
	let future_box = null;
	const offset = visibility_prediction_offset(player.velocity, lookahead_seconds);
	if (distance_sq(vec3(), offset) > k_same_point_epsilon_sq)
	{
		const future = player_bounds(player, add(player.origin, offset), k_bounds_inflate);
		if (!bvh || !bvh.segment_blocked(center_bounds(box), center_bounds(future)).blocked)
		{
			box = merge_bounds(box, future);
			future_box = future;
		}
	}

	const middle = center_bounds(box);
	const upper = vec3(middle.x, middle.y, box.min.z + (box.max.z - box.min.z) * 0.75);
	return {
		box,
		current_box,
		future_box,
		points: [
			vec3(box.min.x, box.min.y, box.min.z), vec3(box.max.x, box.min.y, box.min.z),
			vec3(box.min.x, box.max.y, box.min.z), vec3(box.max.x, box.max.y, box.min.z),
			vec3(box.min.x, box.min.y, box.max.z), vec3(box.max.x, box.min.y, box.max.z),
			vec3(box.min.x, box.max.y, box.max.z), vec3(box.max.x, box.max.y, box.max.z),
			vec3(box.min.x, middle.y, middle.z), vec3(box.max.x, middle.y, middle.z),
			vec3(middle.x, box.min.y, middle.z), vec3(middle.x, box.max.y, middle.z),
			vec3(middle.x, middle.y, box.min.z), vec3(middle.x, middle.y, box.max.z),
			middle,
			upper
		]
	};
}

function hit_triangle(origin, direction, v0, edge1, edge2)
{
	const p = cross(direction, edge2);
	const det = dot(edge1, p);
	if (Math.abs(det) <= k_ray_epsilon)
	{
		return null;
	}

	const inv_det = 1.0 / det;
	const t = sub(origin, v0);
	const u = dot(t, p) * inv_det;
	if (u < 0 || u > 1)
	{
		return null;
	}

	const q = cross(t, edge1);
	const v = dot(direction, q) * inv_det;
	if (v < 0 || u + v > 1)
	{
		return null;
	}

	const distance = dot(edge2, q) * inv_det;
	if (distance <= k_ray_epsilon || distance >= 1.0 - k_ray_epsilon)
	{
		return null;
	}

	return {distance, u, v};
}

class Bvh8
{
	constructor(buffer)
	{
		this.buffer = buffer;
		this.view = new DataView(buffer);
		this.validate();
	}

	u32(offset)
	{
		return this.view.getUint32(offset, true);
	}

	u64(offset)
	{
		return Number(this.view.getBigUint64(offset, true));
	}

	f32(offset)
	{
		return this.view.getFloat32(offset, true);
	}

	validate()
	{
		if (this.buffer.byteLength < k_bvh8_header_size)
		{
			throw new Error("file is smaller than BVH8 header");
		}

		const magic = new Uint8Array(this.buffer, 0, 8);
		const expected = [67, 83, 50, 70, 79, 87, 56, 0];
		if (!expected.every((value, index) => magic[index] === value))
		{
			throw new Error("invalid BVH8 magic");
		}

		this.version = this.u32(8);
		this.header_size = this.u32(12);
		this.flags = this.u32(16);
		this.source_crc32 = this.u32(20);
		this.source_size = this.u64(24);
		this.map_name = this.read_map_name();
		this.world_min = vec3(this.f32(96), this.f32(100), this.f32(104));
		this.world_max = vec3(this.f32(108), this.f32(112), this.f32(116));
		this.node_count = this.u32(120);
		this.packet_count = this.u32(124);
		this.triangle_count = this.u32(128);
		this.max_depth = this.u32(132);
		this.nodes_offset = this.u64(136);
		this.packets_offset = this.u64(144);
		this.file_size = this.u64(152);
		this.payload_crc32 = this.u32(160);

		if (this.version !== k_bvh8_version)
		{
			throw new Error(`unsupported BVH8 version ${this.version}`);
		}
		if (this.header_size !== k_bvh8_header_size)
		{
			throw new Error(`unexpected header size ${this.header_size}`);
		}
		if ((this.flags & ~k_bvh8_known_flags) !== 0)
		{
			throw new Error(`unknown BVH8 flags 0x${this.flags.toString(16)}`);
		}
		if (this.node_count === 0 || this.packet_count === 0 || this.triangle_count === 0)
		{
			throw new Error("BVH8 has no runtime payload");
		}
		if ((this.nodes_offset & 31) !== 0 || (this.packets_offset & 31) !== 0)
		{
			throw new Error("BVH8 arrays are not 32-byte aligned");
		}
		const expected_packets = this.align_up(this.nodes_offset + this.node_count * k_bvh8_node_size, 32);
		const expected_size = this.packets_offset + this.packet_count * k_triangle_packet8_size;
		if (this.packets_offset !== expected_packets || this.file_size !== expected_size || this.file_size !== this.buffer.byteLength)
		{
			throw new Error("BVH8 offsets/counts do not match file size");
		}
	}

	read_map_name()
	{
		const bytes = new Uint8Array(this.buffer, 32, 64);
		let end = bytes.indexOf(0);
		if (end < 0)
		{
			end = bytes.length;
		}
		return new TextDecoder().decode(bytes.slice(0, end));
	}

	align_up(value, alignment)
	{
		return Math.ceil(value / alignment) * alignment;
	}

	node_base(index)
	{
		return this.nodes_offset + index * k_bvh8_node_size;
	}

	packet_base(index)
	{
		return this.packets_offset + index * k_triangle_packet8_size;
	}

	node_float(node_index, field_offset, lane)
	{
		return this.f32(this.node_base(node_index) + field_offset + lane * 4);
	}

	child_ref(node_index, lane)
	{
		return this.u32(this.node_base(node_index) + 192 + lane * 4);
	}

	packet_float(packet_index, field_offset, lane)
	{
		return this.f32(this.packet_base(packet_index) + field_offset + lane * 4);
	}

	is_leaf_ref(ref)
	{
		return ref !== k_invalid_ref && (ref & k_leaf_ref) !== 0;
	}

	leaf_count(ref)
	{
		return ((ref >>> 28) & 7) + 1;
	}

	leaf_index(ref)
	{
		return ref & k_leaf_index_mask;
	}

	hit_children(node_index, origin, direction)
	{
		let mask = 0;
		for (let lane = 0; lane < 8; ++lane)
		{
			const ref = this.child_ref(node_index, lane);
			if (ref === k_invalid_ref)
			{
				continue;
			}

			let near_t = -Infinity;
			let far_t = Infinity;
			const origins = [origin.x, origin.y, origin.z];
			const directions = [direction.x, direction.y, direction.z];
			const mins = [0, 32, 64];
			const maxs = [96, 128, 160];

			let valid = true;
			for (let axis = 0; axis < 3; ++axis)
			{
				const minimum = this.node_float(node_index, mins[axis], lane);
				const maximum = this.node_float(node_index, maxs[axis], lane);
				if (Math.abs(directions[axis]) < 1.0e-12)
				{
					if (origins[axis] < minimum || origins[axis] > maximum)
					{
						valid = false;
						break;
					}
					continue;
				}

				const inverse = 1.0 / directions[axis];
				const a = (minimum - origins[axis]) * inverse;
				const b = (maximum - origins[axis]) * inverse;
				near_t = Math.max(near_t, Math.min(a, b));
				far_t = Math.min(far_t, Math.max(a, b));
			}

			if (valid && far_t >= Math.max(near_t, 0.0) && near_t < 1.0 - k_ray_epsilon)
			{
				mask |= 1 << lane;
			}
		}
		return mask;
	}

	packet_triangle(packet_index, lane)
	{
		const v0 = vec3(
			this.packet_float(packet_index, 0, lane),
			this.packet_float(packet_index, 32, lane),
			this.packet_float(packet_index, 64, lane)
		);
		const edge1 = vec3(
			this.packet_float(packet_index, 96, lane),
			this.packet_float(packet_index, 128, lane),
			this.packet_float(packet_index, 160, lane)
		);
		const edge2 = vec3(
			this.packet_float(packet_index, 192, lane),
			this.packet_float(packet_index, 224, lane),
			this.packet_float(packet_index, 256, lane)
		);
		return {v0, edge1, edge2, v1: add(v0, edge1), v2: add(v0, edge2)};
	}

	hit_packet(packet_index, count, origin, direction)
	{
		for (let lane = 0; lane < count; ++lane)
		{
			const triangle = this.packet_triangle(packet_index, lane);
			const hit = hit_triangle(origin, direction, triangle.v0, triangle.edge1, triangle.edge2);
			if (hit)
			{
				return {lane, triangle, distance: hit.distance};
			}
		}
		return null;
	}

	segment_blocked(origin, target)
	{
		const direction = sub(target, origin);
		const stack = [0];
		while (stack.length > 0)
		{
			const node_index = stack.pop();
			const child_mask = this.hit_children(node_index, origin, direction);
			for (let lane = 0; lane < 8; ++lane)
			{
				if ((child_mask & (1 << lane)) === 0)
				{
					continue;
				}

				const ref = this.child_ref(node_index, lane);
				if (this.is_leaf_ref(ref))
				{
					const packet_index = this.leaf_index(ref);
					if (packet_index >= this.packet_count)
					{
						continue;
					}
					const packet_hit = this.hit_packet(packet_index, this.leaf_count(ref), origin, direction);
					if (packet_hit)
					{
						return {blocked: true, packet_index, lane: packet_hit.lane, triangle: packet_hit.triangle};
					}
				}
				else if (ref < this.node_count)
				{
					stack.push(ref);
				}
			}
		}
		return {blocked: false};
	}

	info_text()
	{
		return [
			`BVH8 ${this.map_name}`,
			`crc=0x${this.source_crc32.toString(16).padStart(8, "0")} flags=0x${this.flags.toString(16)}`,
			`triangles=${this.triangle_count} nodes=${this.node_count} packets=${this.packet_count} depth=${this.max_depth}`,
			`world min=(${format_vec(this.world_min)})`,
			`world max=(${format_vec(this.world_max)})`
		].join("\n");
	}
}

function clear_group(group)
{
	while (group.children.length > 0)
	{
		const child = group.children[0];
		group.remove(child);
		child.traverse((node) =>
		{
			if (node.geometry)
			{
				node.geometry.dispose();
			}
			if (node.material && !Array.isArray(node.material))
			{
				node.material.dispose();
			}
		});
	}
}

function convert_source_geometry(geometry)
{
	const positions = geometry.getAttribute("position");
	if (!positions)
	{
		return;
	}
	for (let i = 0; i < positions.count; ++i)
	{
		const x = positions.getX(i);
		const y = positions.getY(i);
		const z = positions.getZ(i);
		positions.setXYZ(i, y / k_source_units_per_meter, z / k_source_units_per_meter, x / k_source_units_per_meter);
	}
	positions.needsUpdate = true;
	geometry.computeBoundingBox();
	geometry.computeBoundingSphere();
	geometry.computeVertexNormals();
}

function load_map_obj_from_url(url)
{
	obj_loader.load(
		url,
		(object) =>
		{
			clear_group(map_group);
			const material = new THREE.MeshBasicMaterial({
				color: 0xaeb8c6,
				wireframe: true,
				transparent: true,
				opacity: 0.18,
				depthWrite: false
			});
			object.traverse((node) =>
			{
				if (node.isMesh)
				{
					convert_source_geometry(node.geometry);
					node.material = material;
				}
			});
			map_group.add(object);
			map_status = "Map OBJ loaded. Visual only; BVH8 decides blocking.";
			update_scene();
		},
		undefined,
		(error) =>
		{
			clear_group(map_group);
			map_status = `Map OBJ failed: ${error.message || error}`;
			update_scene();
		}
	);
}

function make_line(points, color, opacity = 1.0)
{
	const geometry = new THREE.BufferGeometry().setFromPoints(points.map(source_to_three));
	const material = new THREE.LineBasicMaterial({color, transparent: opacity < 1.0, opacity});
	return new THREE.Line(geometry, material);
}

function make_sphere(point, color, radius = 0.055)
{
	const geometry = new THREE.SphereGeometry(radius, 12, 8);
	const material = new THREE.MeshBasicMaterial({color});
	const mesh = new THREE.Mesh(geometry, material);
	mesh.position.copy(source_to_three(point));
	return mesh;
}

function make_label(text, point, color = "#e8eef7")
{
	const canvas = document.createElement("canvas");
	canvas.width = 160;
	canvas.height = 40;
	const context = canvas.getContext("2d");
	context.font = "24px Segoe UI";
	context.fillStyle = "rgba(0, 0, 0, 0.72)";
	context.fillRect(0, 0, canvas.width, canvas.height);
	context.fillStyle = color;
	context.fillText(text, 8, 27);

	const texture = new THREE.CanvasTexture(canvas);
	const material = new THREE.SpriteMaterial({map: texture, transparent: true});
	const sprite = new THREE.Sprite(material);
	sprite.scale.set(0.9, 0.225, 1);
	sprite.position.copy(source_to_three(add(point, vec3(0, 0, 10))));
	return sprite;
}

function add_box(group, box, color, opacity = 1.0)
{
	const min = box.min;
	const max = box.max;
	const c = [
		vec3(min.x, min.y, min.z), vec3(max.x, min.y, min.z), vec3(max.x, max.y, min.z), vec3(min.x, max.y, min.z),
		vec3(min.x, min.y, max.z), vec3(max.x, min.y, max.z), vec3(max.x, max.y, max.z), vec3(min.x, max.y, max.z)
	];
	const edges = [
		[0, 1], [1, 2], [2, 3], [3, 0],
		[4, 5], [5, 6], [6, 7], [7, 4],
		[0, 4], [1, 5], [2, 6], [3, 7]
	];
	for (const [a, b] of edges)
	{
		group.add(make_line([c[a], c[b]], color, opacity));
	}
}

function add_triangle(group, triangle)
{
	const geometry = new THREE.BufferGeometry().setFromPoints([
		source_to_three(triangle.v0),
		source_to_three(triangle.v1),
		source_to_three(triangle.v2)
	]);
	geometry.setIndex([0, 1, 2]);
	geometry.computeVertexNormals();
	const material = new THREE.MeshBasicMaterial({color: 0xff3fb4, transparent: true, opacity: 0.62, side: THREE.DoubleSide});
	group.add(new THREE.Mesh(geometry, material));
}

function format_vec(value)
{
	return `${value.x.toFixed(1)}, ${value.y.toFixed(1)}, ${value.z.toFixed(1)}`;
}

function update_model_transform()
{
	if (!active_model)
	{
		return;
	}
	const target = read_target();
	active_model.scene.position.copy(source_to_three(target.origin));
}

function set_active_model(key)
{
	active_model_key = key;
	model_group.clear();
	active_model = loaded_models.get(key) || null;
	active_mixer = null;
	const animation_select = $("animation-select");
	animation_select.innerHTML = '<option value="">none</option>';

	if (!active_model)
	{
		const path = model_paths.get(key);
		if (path && !loading_models.has(key))
		{
			status_extra = `Loading ${key} from local_assets...`;
			load_model_from_url(key, path);
		}
		update_scene();
		return;
	}

	model_group.add(active_model.scene);
	update_model_transform();
	for (let i = 0; i < active_model.animations.length; ++i)
	{
		const option = document.createElement("option");
		option.value = String(i);
		option.textContent = active_model.animations[i].name || `clip ${i}`;
		animation_select.appendChild(option);
	}
	update_scene();
}

function set_animation(index_text)
{
	if (!active_model)
	{
		return;
	}
	active_mixer = null;
	if (index_text === "")
	{
		return;
	}
	const index = Number(index_text);
	const clip = active_model.animations[index];
	if (!clip)
	{
		return;
	}
	active_mixer = new THREE.AnimationMixer(active_model.scene);
	active_mixer.clipAction(clip).play();
}

function load_model_from_url(key, url)
{
	if (loading_models.has(key))
	{
		return;
	}
	loading_models.add(key);
	loader.load(
		url,
		(gltf) =>
		{
			loading_models.delete(key);
			gltf.scene.traverse((node) =>
			{
				if (node.isMesh)
				{
					node.castShadow = false;
					node.receiveShadow = false;
				}
			});
			loaded_models.set(key, gltf);
			if (active_model_key === key)
			{
				set_active_model(key);
			}
		},
		undefined,
		(error) =>
		{
			loading_models.delete(key);
			status_extra = `Failed to load ${key}: ${error.message || error}`;
			update_scene();
		}
	);
}

async function load_manifest()
{
	try
	{
		const response = await fetch("local_assets/manifest.json", {cache: "no-store"});
		if (!response.ok)
		{
			throw new Error("manifest not found");
		}
		const manifest = await response.json();
		for (const [key, path] of Object.entries(manifest.models || {}))
		{
			model_paths.set(key, path);
		}
		status_extra = "Manifest loaded. Load a .bvh8 file for real blocking.";
		set_active_model(active_model_key);
	}
	catch (error)
	{
		status_extra = "No local_assets/manifest.json. Use the GLB file pickers or run export_assets.py.";
	}
	update_scene();
}

async function load_bvh_file(file)
{
	const buffer = await file.arrayBuffer();
	active_bvh = new Bvh8(buffer);
	status_extra = "BVH8 loaded.";
	update_scene();
}

function update_scene()
{
	clear_group(overlay_group);
	clear_group(hit_group);
	update_model_transform();

	const observer = read_observer();
	const target = read_target();
	const ping_ms = read_number("ping");
	const lookahead = effective_lookahead_seconds(ping_ms);
	const origins = visibility_origins(active_bvh, observer, lookahead);
	const targets = visibility_targets(active_bvh, target, lookahead);
	const uninflated_box = player_bounds(target, target.origin, 0);

	add_box(overlay_group, uninflated_box, 0x82aaff, 0.9);
	add_box(overlay_group, targets.current_box, 0xffd166, 0.75);
	if (targets.future_box)
	{
		add_box(overlay_group, targets.box, 0xff8f3d, 0.85);
	}

	for (let i = 0; i < origins.length; ++i)
	{
		overlay_group.add(make_sphere(origins[i], 0x66e3ff, 0.07));
		overlay_group.add(make_label(`O${i}`, origins[i], "#66e3ff"));
	}

	for (let i = 0; i < targets.points.length; ++i)
	{
		overlay_group.add(make_sphere(targets.points[i], 0xffd166, 0.052));
		overlay_group.add(make_label(`T${i}`, targets.points[i], "#ffd166"));
	}

	let blocked_count = 0;
	let first_hit = null;
	for (const origin of origins)
	{
		for (const point of targets.points)
		{
			let color = 0x8a929e;
			let opacity = 0.35;
			if (active_bvh)
			{
				const result = active_bvh.segment_blocked(origin, point);
				if (result.blocked)
				{
					++blocked_count;
					color = 0xff5c5c;
					opacity = 0.78;
					if (!first_hit && result.triangle)
					{
						first_hit = result.triangle;
					}
				}
				else
				{
					color = 0x50fa7b;
					opacity = 0.72;
				}
			}
			overlay_group.add(make_line([origin, point], color, opacity));
		}
	}

	if (first_hit)
	{
		add_triangle(hit_group, first_hit);
	}

	const hidden = active_bvh && blocked_count === k_visibility_ray_count;
	const bvh_text = active_bvh ? active_bvh.info_text() : "BVH8: none";
	$("status").textContent = [
		status_extra,
		map_status,
		`lookahead=${(lookahead * 1000).toFixed(0)}ms ping=${ping_ms.toFixed(0)}ms yaw=${observer.eye_yaw_degrees.toFixed(0)}deg`,
		`observer origins=${origins.length} target points=${targets.points.length} segments=${origins.length * targets.points.length}`,
		`blocked=${blocked_count}/${k_visibility_ray_count} decision=${hidden ? "HIDE" : "SHOW"}`,
		`target current inflated min=(${format_vec(targets.current_box.min)})`,
		`target current inflated max=(${format_vec(targets.current_box.max)})`,
		`target sampled box swept=${targets.future_box ? "yes" : "no"} min=(${format_vec(targets.box.min)})`,
		`target sampled box swept=${targets.future_box ? "yes" : "no"} max=(${format_vec(targets.box.max)})`,
		`origins: ${k_origin_labels.map((label, index) => `O${index}=${label}`).join(", ")}`,
		`targets: ${k_target_labels.map((label, index) => `T${index}=${label}`).join(", ")}`,
		bvh_text
	].join("\n");
}

function run_self_checks()
{
	const failures = [];
	const expect = (condition, message) =>
	{
		if (!condition)
		{
			failures.push(message);
		}
	};

	expect(Math.abs(effective_lookahead_seconds(0) - 0.2) < 1.0e-6, "0 ping lookahead");
	expect(Math.abs(effective_lookahead_seconds(50) - 0.3) < 1.0e-6, "50 ping lookahead");
	expect(Math.abs(effective_lookahead_seconds(150) - 0.5) < 1.0e-6, "150 ping lookahead clamp");
	expect(visibility_prediction_offset(vec3(0, 0, 0), 0.2).x === 0, "0 speed margin");
	expect(visibility_prediction_offset(vec3(1, 0, 0), 0.2).x === 0, "1 speed margin");
	expect(Math.abs(visibility_prediction_offset(vec3(10, 0, 0), 0.2).x - 16) < 1.0e-6, "10 speed margin");
	expect(Math.abs(visibility_prediction_offset(vec3(50, 0, 0), 0.2).x - 32) < 1.0e-6, "50 speed margin");
	expect(Math.abs(visibility_prediction_offset(vec3(100, 0, 0), 0.2).x - 64) < 1.0e-6, "100 speed margin");
	expect(Math.abs(visibility_prediction_offset(vec3(200, 0, 0), 0.2).x - 96) < 1.0e-6, "200 speed margin");
	expect(Math.abs(visibility_prediction_offset(vec3(275, 0, 0), 0.2).x - 128) < 1.0e-6, "275 speed margin");
	expect(Math.abs(visibility_prediction_offset(vec3(300, 0, 0), 0.2).x - 160) < 1.0e-6, "300 speed margin");
	const origins_yaw0 = visibility_origins(null, {eye: vec3(), velocity: vec3(250, 0, 0), eye_yaw_degrees: 0}, 0.2);
	const origins_yaw90 = visibility_origins(null, {eye: vec3(), velocity: vec3(), eye_yaw_degrees: 90}, 0.2);
	expect(origins_yaw0.length === 10, "origin count");
	expect(Math.abs(origins_yaw0[2].y - 24) < 1.0e-6 && Math.abs(origins_yaw0[3].y + 24) < 1.0e-6, "yaw 0 shoulders");
	expect(Math.abs(origins_yaw90[2].x + 24) < 1.0e-6 && Math.abs(origins_yaw90[3].x - 24) < 1.0e-6, "yaw 90 shoulders");
	expect(Math.abs(origins_yaw0[6].z - 24) < 1.0e-6 && Math.abs(origins_yaw0[7].z + 24) < 1.0e-6, "vertical origins");
	expect(visibility_targets(null, {origin: vec3(), velocity: vec3(0, 0, 0), mins: vec3(-16, -16, 0), maxs: vec3(16, 16, 72)}, 0.2).points.length === 16, "target count");
	expect(k_visibility_ray_count === 160, "ray count");
	expect(hit_triangle(vec3(0.25, 0.25, 1), vec3(0, 0, -2), vec3(0, 0, 0), vec3(1, 0, 0), vec3(0, 1, 0)) !== null, "single triangle hit");

	const element = $("self-check");
	if (failures.length === 0)
	{
		element.textContent = "self-check: passed";
		element.className = "status ok";
	}
	else
	{
		element.textContent = `self-check: failed\n${failures.join("\n")}`;
		element.className = "status bad";
	}
}

function init_scene()
{
	renderer = new THREE.WebGLRenderer({canvas: $("view"), antialias: true});
	renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
	renderer.setSize(window.innerWidth, window.innerHeight);

	scene = new THREE.Scene();
	scene.background = new THREE.Color(0x0b0e12);
	camera = new THREE.PerspectiveCamera(58, window.innerWidth / window.innerHeight, 0.02, 3000);
	camera.position.set(5, 4, 8);

	controls = new OrbitControls(camera, renderer.domElement);
	controls.target.set(0, 1.0, 0);
	controls.update();

	clock = new THREE.Clock();
	loader = new GLTFLoader();
	obj_loader = new OBJLoader();
	map_group = new THREE.Group();
	model_group = new THREE.Group();
	overlay_group = new THREE.Group();
	hit_group = new THREE.Group();
	scene.add(map_group, model_group, overlay_group, hit_group);

	const light = new THREE.HemisphereLight(0xffffff, 0x252a33, 2.2);
	scene.add(light);
	scene.add(new THREE.GridHelper(16, 16, 0x364150, 0x1d252f));
	scene.add(new THREE.AxesHelper(1.5));

	window.addEventListener("resize", () =>
	{
		camera.aspect = window.innerWidth / window.innerHeight;
		camera.updateProjectionMatrix();
		renderer.setSize(window.innerWidth, window.innerHeight);
	});
}

function install_ui()
{
	$("model-select").addEventListener("change", (event) => set_active_model(event.target.value));
	$("animation-select").addEventListener("change", (event) => set_animation(event.target.value));
	$("ct-file").addEventListener("change", (event) =>
	{
		const file = event.target.files[0];
		if (file)
		{
			load_model_from_url("ct_sas", URL.createObjectURL(file));
		}
	});
	$("t-file").addEventListener("change", (event) =>
	{
		const file = event.target.files[0];
		if (file)
		{
			load_model_from_url("t_phoenix", URL.createObjectURL(file));
		}
	});
	$("bvh-file").addEventListener("change", async (event) =>
	{
		const file = event.target.files[0];
		if (!file)
		{
			return;
		}
		try
		{
			await load_bvh_file(file);
		}
		catch (error)
		{
			active_bvh = null;
			status_extra = `Failed to load BVH8: ${error.message || error}`;
			update_scene();
		}
	});
	$("map-obj-file").addEventListener("change", (event) =>
	{
		const file = event.target.files[0];
		if (file)
		{
			map_status = "Loading map OBJ...";
			update_scene();
			load_map_obj_from_url(URL.createObjectURL(file));
		}
	});

	for (const input of document.querySelectorAll("input[type='number']"))
	{
		input.addEventListener("input", update_scene);
	}
}

function animate()
{
	requestAnimationFrame(animate);
	const delta = clock.getDelta();
	if (active_mixer)
	{
		active_mixer.update(delta);
	}
	controls.update();
	renderer.render(scene, camera);
}

init_scene();
install_ui();
run_self_checks();
set_active_model(active_model_key);
load_manifest();
update_scene();
animate();
