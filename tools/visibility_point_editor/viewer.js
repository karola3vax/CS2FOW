// Local browser editor for body points, generated axis-aligned box corners, and
// a weapon-muzzle preview. It also reads baked BVH8 maps in a worker and shows
// the runtime wall decisions without changing plugin state.

import * as THREE from "three";
import {OrbitControls} from "three/addons/controls/OrbitControls.js";
import {TransformControls} from "three/addons/controls/TransformControls.js";
import {GLTFLoader} from "three/addons/loaders/GLTFLoader.js";
import {clone as clone_skeleton} from "three/addons/utils/SkeletonUtils.js";
import {shoulder_offset} from "./bvh8.js";
import {FPS_CONSTANTS, FPS_DT, target_muzzle, weapon_muzzle_length} from "./fps_runtime.js";

const k_source_units_per_meter = 39.3700787;
const k_default_preset = "default_sas_visibility_points.json";
const k_aabb_color = 0x007c91;
const k_body_color = 0xffffff;
const k_selected_color = 0xdf1f2d;
const k_muzzle_color = 0x92278f;
const k_ray_color = 0xdf1f2d;
const k_clear_ray_color = new THREE.Color(0x20a466);
const k_blocked_ray_color = new THREE.Color(0xe51d2e);
const k_origin_color = 0x1769aa;
const k_map_color = 0x555b63;
const k_viewer_distance = 256;
const k_eye_height = 64;
const k_shoulder_offset = 48;
const k_vertical_origin_offset = 16;
const k_horizontal_bounds_padding = 8;
const k_top_bounds_padding = 8;
const k_aabb_dot_radius = 0.022 / 3.0;
const k_body_dot_radius = 0.06 / 3.0;
const k_selected_dot_radius = 0.088 / 3.0;
const k_muzzle_dot_radius = 0.038 / 3.0;
const k_animation_transition_seconds = 0.22;
const k_debug_draw_interval = 1 / 16;
const k_bvh_snapshot_interval = 1 / 16;
const k_mouse_yaw = 0.022;
const k_viewmodel_fov = 44;
const k_world_fov = 58;
const k_viewmodel_offset = Object.freeze({x: 2.5, y: 2, z: -1});
// Fire rate, magazine size, and movement speed follow the current CS2 weapon
// spreadsheet linked from the Studio README. Reserve ammunition is unlimited.
const k_weapon_stats = Object.freeze({
	usp_silencer: {label: "USP-S", rpm: 352.94, clip: 12, speed: 240, automatic: false},
	m4a1_silencer: {label: "M4A1-S", rpm: 600, clip: 20, speed: 225, automatic: true},
	awp: {label: "AWP", rpm: 41.24, clip: 5, speed: 200, automatic: false},
	knife: {label: "Karambit", rpm: 120, clip: null, speed: 250, automatic: true}
});
const k_weapon_grips = {
	usp_silencer: {x: 0, y: 0, z: 0, rx: 180, ry: 180, rz: 0, scale: 1},
	m4a1_silencer: {x: 0, y: 0, z: 0, rx: 180, ry: 180, rz: 0, scale: 1},
	awp: {x: 0, y: 0, z: 0, rx: 180, ry: 180, rz: 0, scale: 1},
	knife: {x: 0, y: 0, z: 0, rx: 180, ry: 180, rz: 0, scale: 1}
};
const viewmodel_weapon_offsets = {
	usp_silencer: {x: 3, y: 5.75, z: 0},
	m4a1_silencer: {x: 4.5, y: 3.5, z: 0},
	awp: {x: 9.25, y: 5.5, z: 0},
	knife: {x: 0, y: 0, z: 0},
	smokegrenade: {x: 0, y: 0, z: 0},
	hegrenade: {x: 0, y: 0, z: 0}
};
const k_weapon_muzzle_offsets = {
	usp_silencer: {x: 18, y: 0, z: 0},
	m4a1_silencer: {x: 36, y: 0, z: 0},
	awp: {x: 52, y: 0, z: 0}
};
const k_runtime_body_bones = [
	"head_0", "neck_0", "spine_3", "pelvis", "arm_upper_L",
	"arm_upper_R", "leg_upper_L", "leg_upper_R", "leg_lower_L", "leg_lower_R",
	"ankle_L", "ankle_R", "arm_lower_R", "arm_lower_L", "spine_2"
];
const k_skeleton_edges = [
	[0, 1], [1, 2], [2, 14], [14, 3],
	[1, 4], [4, 13], [1, 5], [5, 12],
	[3, 6], [6, 8], [8, 10], [3, 7], [7, 9], [9, 11]
];
// Valve's current shared CS2 player hitbox set (Source units, bone-local).
const k_valve_hitbox_capsules = [
	["head_0", [-1, 1.8, 0], [3.5, 0.2, 0], 4.3],
	["neck_0", [0, -0.4, 0], [1.4, -0.2, 0], 3.5],
	["pelvis", [-2.7, 1.1, -3.2], [-2.7, 1.1, 3.2], 6],
	["spine_0", [1.4, 0.8, 3.1], [1.4, 0.8, -3.1], 6],
	["spine_1", [3.8, 0.8, -2.4], [3.8, 0.4, 2.4], 6.5],
	["spine_2", [4.8, 0.15, -4.1], [4.8, 0.15, 4.1], 6.2],
	["spine_3", [2.5, -0.6, -6], [2.5, -0.6, 6], 5],
	["leg_upper_l", [1.3, -0.2, 0], [16.5, -0.7, 0], 5],
	["leg_upper_r", [-1.3, 0, -0.6], [-16.5, 0, -0.7], 5],
	["leg_lower_l", [0.1, -0.4, 0.2], [17, -0.4, 0.7], 4],
	["leg_lower_r", [-0.1, 0, -0.2], [-17, 0.4, -0.7], 4],
	["ankle_l", [0, -3.43, -0.52], [8, 0.74, 0.33], 2.6],
	["ankle_r", [-7.98, -0.75, -0.27], [-0.02, 3.44, 0.58], 2.6],
	["hand_l", [0, 0.3, 0], [3.59, 1.15, 0.11], 2.3],
	["hand_r", [0, -0.3, 0.02], [-3.44, -1.17, -0.09], 2.3],
	["arm_upper_l", [0, 0, 0], [11.2, 0, 0], 3.3],
	["arm_lower_l", [0, 0, 0], [10, 0, 0], 3],
	["arm_upper_r", [0, 0, 0], [-11.2, 0, 0], 3.3],
	["arm_lower_r", [0, 0, 0], [-10, 0, -0.5], 3]
];
const k_aabb_edges = [
	[0, 1], [0, 2], [1, 3], [2, 3],
	[4, 5], [4, 6], [5, 7], [6, 7],
	[0, 4], [1, 5], [2, 6], [3, 7]
];
const k_map_spawn_pairs = {
	de_ancient: {
		viewer: {x: -416, y: 1568, z: 87.031319},
		target: {x: -552, y: -2384, z: -99.255737}
	},
	de_anubis: {
		viewer: {x: -360, y: 2120, z: 80.056595},
		target: {x: -304, y: -1608, z: 52.157570}
	},
	de_inferno: {
		viewer: {x: 2353, y: 1977, z: 199.071655},
		target: {x: -1650, y: 718, z: 0.03125}
	},
	de_mirage: {
		viewer: {x: -1902, y: -1976, z: -212.14798},
		target: {x: 1376, y: -16, z: -103.96875}
	},
	de_nuke: {
		viewer: {x: 2552, y: -424, z: -287.96875},
		target: {x: -1808, y: -1089, z: -351.96875}
	},
	de_overpass: {
		viewer: {x: -2120, y: 790, z: 540.300903},
		target: {x: -2256, y: 837, z: 540.032837}
	}
};

function locomotion_clips(base, suffix)
{
	const directions = [
		["forward", "n"], ["forward_right", "ne"], ["right", "e"], ["backward_right", "se"],
		["backward", "s"], ["backward_left", "sw"], ["left", "w"], ["forward_left", "nw"]
	];
	return [
		{id: "idle", label: "Standing idle", clip: `${base}/idle_${suffix}`},
		...directions.flatMap(([id, direction]) => [
			{id: `walk_${id}`, label: `Walk ${id.replaceAll("_", " ")}`, clip: `${base}/walk_${direction}_${suffix}`},
			{id: `run_${id}`, label: `Run ${id.replaceAll("_", " ")}`, clip: `${base}/run_${direction}_${suffix}`},
			{id: `crouch_${id}`, label: `Crouch ${id.replaceAll("_", " ")}`, clip: `${base}/crouch_${direction}_${suffix}`},
			{id: `jump_${id}`, label: `Jump ${id.replaceAll("_", " ")}`, clip: `${base}/jump_${direction}_${suffix}`}
		]),
		{id: "crouch_idle", label: "Crouch idle", clip: `${base}/idle_crouch_${suffix}`}
	];
}

const k_animation_sets = {
	default: locomotion_clips("animation/anims/world/knife/_default_knife", "knife"),
	knife: [
		...locomotion_clips("animation/anims/world/knife/_default_knife", "knife"),
		{id: "draw", label: "Draw", clip: "animation/anims/world/knife/default_ct/draw_default_ct"},
		{id: "shoot", label: "Light swing", clip: "animation/anims/world/knife/_default_knife/frontswing_knife"},
		{id: "shoot2", label: "Heavy swing", clip: "animation/anims/world/knife/_default_knife/frontstab_knife"}
	],
	usp_silencer: [
		...locomotion_clips("animation/anims/world/pistol/_default_pistol", "pistol"),
		{id: "draw", label: "Draw", clip: "animation/anims/world/pistol/pistol_usp/draw_usp", crouchClip: "animation/anims/world/pistol/pistol_usp/draw_crouch_usp"},
		{id: "shoot", label: "Fire", clip: "animation/anims/world/pistol/pistol_usp/shoot_usp"},
		{id: "reload", label: "Reload", clip: "animation/anims/world/pistol/pistol_usp/reload_usp", crouchClip: "animation/anims/world/pistol/pistol_usp/reload_crouch_usp"}
	],
	m4a1_silencer: [
		...locomotion_clips("animation/anims/world/rifle/_default_rifle", "rifle"),
		{id: "draw", label: "Draw", clip: "animation/anims/world/rifle/rifle_m4a1_silencer/draw_m4a1s", crouchClip: "animation/anims/world/rifle/rifle_m4a1_silencer/draw_crouch_m4a1s"},
		{id: "shoot", label: "Fire", clip: "animation/anims/world/rifle/rifle_m4a1_silencer/shoot_m4a1s"},
		{id: "reload", label: "Reload", clip: "animation/anims/world/rifle/rifle_m4a1_silencer/reload_m4a1s", crouchClip: "animation/anims/world/rifle/rifle_m4a1_silencer/reload_crouch_m4a1s"}
	],
	awp: [
		...locomotion_clips("animation/anims/world/rifle/_default_rifle", "rifle"),
		{id: "draw", label: "Draw", clip: "animation/anims/world/rifle/rifle_awp/draw_awp", crouchClip: "animation/anims/world/rifle/rifle_awp/draw_crouch_awp"},
		{id: "shoot", label: "Fire", clip: "animation/anims/world/rifle/rifle_awp/shoot_awp"},
		{id: "reload", label: "Reload", clip: "animation/anims/world/rifle/rifle_awp/reload_awp", crouchClip: "animation/anims/world/rifle/rifle_awp/reload_crouch_awp"}
	],
	grenade: [
		...locomotion_clips("animation/anims/world/knife/_default_knife", "knife")
			.filter((entry) => entry.id !== "idle" && entry.id !== "crouch_idle"),
		{id: "idle", label: "Standing idle", clip: "animation/anims/world/grenade/_default_grenade/idle_grenade"},
		{id: "crouch_idle", label: "Crouch idle", clip: "animation/anims/world/grenade/_default_grenade/idle_crouch_grenade"},
		{id: "draw", label: "Draw", clip: "animation/anims/world/grenade/_default_grenade/draw_grenade", crouchClip: "animation/anims/world/grenade/_default_grenade/draw_crouch_grenade"},
		{id: "pullpin", label: "Pull pin", clip: "animation/anims/world/grenade/_default_grenade/pullpin_grenade", crouchClip: "animation/anims/world/grenade/_default_grenade/pullpin_crouch_grenade"},
		{id: "throw", label: "Overhand throw", clip: "animation/anims/world/grenade/_default_grenade/throw_overhand_grenade", crouchClip: "animation/anims/world/grenade/_default_grenade/crouch_throw_far_grenade"},
		{id: "throw2", label: "Underhand throw", clip: "animation/anims/world/grenade/_default_grenade/throw_underhand_grenade", crouchClip: "animation/anims/world/grenade/_default_grenade/crouch_throw_near_grenade"}
	]
};

const k_reload_sound_sequences = Object.freeze({
	usp_silencer: [[7 / 30, "usp_reload_out"], [21 / 30, "usp_reload_in"], [27 / 30, "usp_reload_addammo"], [41 / 30, "usp_reload_slideback"], [46 / 30, "usp_reload_slide"]],
	m4a1_silencer: [[13 / 30, "m4_reload_out"], [35 / 30, "m4_reload_in"], [41 / 30, "m4_reload_addammo"], [60 / 30, "m4_reload_hit"]],
	awp: [[8 / 30, "awp_reload_out"], [46 / 30, "awp_reload_in"], [60 / 30, "awp_reload_hit"], [79 / 30, "awp_reload_bolt_back"], [89 / 30, "awp_reload_bolt_forward"]]
});

const $ = (id) => document.getElementById(id);
const source_to_three = (p) => new THREE.Vector3(p.y / k_source_units_per_meter, p.z / k_source_units_per_meter, p.x / k_source_units_per_meter);
const three_to_source = (p) => ({x: p.z * k_source_units_per_meter, y: p.x * k_source_units_per_meter, z: p.y * k_source_units_per_meter});
const read_number = (id) =>
{
	const value = Number($(id).value);
	return Number.isFinite(value) ? value : 0;
};
const runtime_tuning = () => ({
	shoulderBase: read_number("shoulder-base"),
	shoulderRttScale: read_number("shoulder-rtt-scale"),
	maxShoulder: read_number("shoulder-max")
});
const clone_point = (point) => ({name: point.name, x: Number(point.x), y: Number(point.y), z: Number(point.z)});
const can_delete_point = (count) => count > 1;

function unique_point_name(base)
{
	let name = base;
	let suffix = 2;
	while (points.some((point) => point.name === name))
	{
		name = `${base}_${suffix++}`;
	}
	return name;
}

function validated_points(value, label)
{
	if (value?.version !== 1 || value.coordinate_space !== "source_local" || value.model !== "ctm_sas")
	{
		throw new Error(`${label} has unsupported metadata`);
	}
	if (!Array.isArray(value.points) || !Number.isInteger(value.point_count)
		|| value.point_count !== value.points.length || value.points.length < 1 || value.points.length > 32)
	{
		throw new Error(`${label} has an invalid point count`);
	}
	const names = new Set();
	return value.points.map((point) =>
	{
		const copy = clone_point(point);
		if (typeof copy.name !== "string" || !copy.name.trim() || names.has(copy.name)
			|| !Number.isFinite(copy.x) || !Number.isFinite(copy.y) || !Number.isFinite(copy.z))
		{
			throw new Error(`${label} contains an invalid or duplicate point`);
		}
		names.add(copy.name);
		return copy;
	});
}

let renderer;
let camera;
let scene;
let orbit;
let transform;
let loader;
let model;
let viewer_model;
let model_load_id = 0;
let model_mixer;
let viewer_mixer;
let model_animations = [];
let viewer_animations = [];
const masked_clip_cache = new WeakMap();
let runtime_body_bindings = [];
let runtime_animation_enabled = !window.matchMedia("(prefers-reduced-motion: reduce)").matches;
let animation_clock;
let manifest_models = {};
let local_manifest = {};
let weapon_model;
let weapon_mount;
let weapon_load_id = 0;
let bot_weapon_model;
let bot_weapon_mount;
let bot_weapon_key = "";
let bot_weapon_load_id = 0;
const extra_bot_models = [];
const extra_bot_mixers = [];
const extra_bot_body_bindings = [];
const extra_bot_capsule_bindings = [];
const extra_bot_debug_groups = [];
const extra_bot_capsule_groups = [];
let player_weapon_model;
let player_weapon_mount;
let player_weapon_load_id = 0;
let viewmodel_root;
let viewmodel_scene;
let viewmodel_camera;
let viewmodel_arms;
let viewmodel_weapon;
let viewmodel_weapon_mount;
let viewmodel_mixer;
let viewmodel_animations = {};
let viewmodel_load_id = 0;
const viewmodel_asset_cache = new Map();
const grenade_templates = {};
const casing_templates = {};
const particle_textures = {};
const material_textures = {};
const grenade_visuals = new Map();
let audio_context;
const audio_buffers = new Map();
const last_sound_choices = new Map();
let last_player_step = 0;
const last_bot_steps = [0, 0, 0];
let points = [];
let default_points = [];
let selected_index = 0;
let marker_group;
let skeleton_group;
let skeleton_lines;
let hitbox_group;
let hitbox_bindings = [];
let hitbox_capsules_enabled = true;
let aabb_group;
let muzzle_group;
let ray_group;
let origin_group;
let ray_lines;
let ray_count = 0;
let status_extra = "";
let active_weapon_key = "usp_silencer";
let model_status = "Model unavailable";
let map_worker;
let map_mesh;
let map_traversal_mesh;
let map_metadata;
let map_report;
let map_wireframe = false;
let map_focus = true;
let map_load_id = 0;
let placement_pick_id = 0;
let trace_id = 0;
let trace_in_flight = false;
let trace_dirty = false;
let placement_mode = "";
let clear_ray_count = 0;
let blocked_ray_count = 0;
let wall_visible = null;
let studio_ground;
let studio_grid;
let play_active = false;
let play_paused = true;
let play_debug = false;
let play_rays_enabled = true;
let last_play_debug_draw = -Infinity;
let last_extra_debug_draw = -Infinity;
let last_play_traversal_request = -Infinity;
let debug_trace_smoothing = null;
const extra_debug_smoothing = [];
let play_third_person = false;
let play_scoped = false;
let play_state = null;
let play_pitch = 0;
let play_eye_height = 64;
let play_eye_height_target = 64;
let play_look_dirty = false;
let play_grenade = "";
let play_grenade_holding = false;
let play_grenade_throw_speed = 750;
let play_weapon_key = "m4a1_silencer";
let play_firing = false;
let play_next_fire_time = 0;
let play_busy_until = 0;
let play_action_serial = 0;
let player_world_action_until = 0;
const play_ammo = {usp_silencer: 12, m4a1_silencer: 20, awp: 5};
let play_nav = null;
let last_play_target_send = 0;
let play_session_id = 0;
let nav_group;
let smoke_group;
let grenade_group;
let effect_group;
const play_keys = {w: false, a: false, s: false, d: false, walk: false, crouch: false, jump: false};
const play_scheduled = [];
const smoke_visuals = [];
const explosion_effects = [];
const shot_effects = [];
const impact_marks = [];
const casing_effects = [];
let viewmodel_motion_time = 0;
const target_pose = {x: 0, y: 0, z: 0, yaw: 0, height: 72, placed: false};
const extra_target_poses = [
	{x: 0, y: 0, z: 0, yaw: 0, height: 72, placed: false},
	{x: 0, y: 0, z: 0, yaw: 0, height: 72, placed: false}
];
const extra_bot_render_poses = extra_target_poses.map((pose) => ({...pose}));
const viewer_pose = {x: k_viewer_distance, y: 0, z: 0, yaw: 180, placed: false};
const movement_buttons = {w: false, a: false, s: false, d: false};
let play_pose_from = null;
let play_pose_to = null;
let play_pose_elapsed = 0;

function reset_camera()
{
	if (model)
	{
		const center = source_to_three({x: target_pose.x, y: target_pose.y, z: target_pose.z + 36});
		const direction = new THREE.Vector3(1, 0.45, 1).normalize();
		camera.position.copy(center).addScaledVector(direction, 3.2);
		camera.near = 0.02;
		camera.far = 3000;
		update_camera_projection();
		orbit.target.copy(center);
		orbit.update();
		return;
	}
	camera.position.set(4.2, 2.5, 4.2);
	orbit.target.set(0, 0.95, 0);
	orbit.update();
}

function update_camera_projection()
{
	camera.aspect = window.innerWidth / window.innerHeight;
	camera.clearViewOffset();
	if (!play_active)
	{
		const inspector = $("inspector")?.getBoundingClientRect();
		const coveredWidth = inspector ? window.innerWidth - inspector.left : 0;
		camera.setViewOffset(window.innerWidth, window.innerHeight, coveredWidth / 2, 0,
			window.innerWidth, window.innerHeight);
	}
	camera.updateProjectionMatrix();
}

function degrees_to_radians(value)
{
	return value * Math.PI / 180.0;
}

function lerp_degrees(from, to, amount)
{
	const delta = ((to - from + 540) % 360) - 180;
	return from + delta * amount;
}

function actor_pose(actor)
{
	return {x: actor.origin.x, y: actor.origin.y, z: actor.origin.z, yaw: actor.yaw,
		height: actor.crouched ? FPS_CONSTANTS.crouchedHeight : 72, placed: true};
}

function interpolate_pose(destination, from, to, amount, interpolateYaw = true)
{
	destination.x = THREE.MathUtils.lerp(from.x, to.x, amount);
	destination.y = THREE.MathUtils.lerp(from.y, to.y, amount);
	destination.z = THREE.MathUtils.lerp(from.z, to.z, amount);
	if (interpolateYaw) destination.yaw = lerp_degrees(from.yaw, to.yaw, amount);
	if (Number.isFinite(from.height) && Number.isFinite(to.height))
		destination.height = THREE.MathUtils.lerp(from.height, to.height, amount);
	destination.placed = true;
}

function point_vec(point)
{
	return {x: Number(point.x), y: Number(point.y), z: Number(point.z)};
}

function rotate_source(point, yaw_degrees)
{
	const yaw = degrees_to_radians(yaw_degrees);
	const cosine = Math.cos(yaw);
	const sine = Math.sin(yaw);
	return {x: point.x * cosine - point.y * sine, y: point.x * sine + point.y * cosine, z: point.z};
}

function target_world_point(local)
{
	const rotated = rotate_source(local, target_pose.yaw);
	return {x: target_pose.x + rotated.x, y: target_pose.y + rotated.y, z: target_pose.z + rotated.z};
}

function target_local_point(world)
{
	return rotate_source({x: world.x - target_pose.x, y: world.y - target_pose.y, z: world.z - target_pose.z}, -target_pose.yaw);
}

function pose_origin(pose)
{
	return {x: pose.x, y: pose.y, z: pose.z};
}

function map_simulation_ready()
{
	return Boolean(map_metadata && target_pose.placed && viewer_pose.placed);
}

function play_ready()
{
	return map_simulation_ready() && Boolean(model);
}

function format_number(value)
{
	return Number(value).toFixed(2).replace(/\.?0+$/, "");
}

function generated_aabb_points()
{
	const min = {
		x: read_number("min-x") - k_horizontal_bounds_padding,
		y: read_number("min-y") - k_horizontal_bounds_padding,
		z: read_number("min-z")
	};
	const max = {
		x: read_number("max-x") + k_horizontal_bounds_padding,
		y: read_number("max-y") + k_horizontal_bounds_padding,
		z: read_number("max-z") + k_top_bounds_padding
	};
	return [
		{x: min.x, y: min.y, z: min.z}, {x: max.x, y: min.y, z: min.z},
		{x: min.x, y: max.y, z: min.z}, {x: max.x, y: max.y, z: min.z},
		{x: min.x, y: min.y, z: max.z}, {x: max.x, y: min.y, z: max.z},
		{x: min.x, y: max.y, z: max.z}, {x: max.x, y: max.y, z: max.z}
	];
}

function target_aabb_points()
{
	return generated_aabb_points().map((point, index) => ({
		x: point.x + target_pose.x,
		y: point.y + target_pose.y,
		z: (play_active && index >= 4 ? target_pose.height + k_top_bounds_padding : point.z) + target_pose.z
	}));
}

function apply_player_transforms()
{
	if (model)
	{
		model.position.copy(source_to_three(pose_origin(target_pose)));
		model.rotation.set(0, degrees_to_radians(target_pose.yaw), 0);
	}
	if (viewer_model)
	{
		viewer_model.position.copy(source_to_three(pose_origin(viewer_pose)));
		viewer_model.rotation.set(0, degrees_to_radians(viewer_pose.yaw), 0);
	}
	if (play_active) return;
	const targetVisible = !map_metadata || target_pose.placed;
	const viewerVisible = should_show_viewer_model(Boolean(map_metadata), viewer_pose.placed, play_active, play_third_person);
	if (model) model.visible = targetVisible;
	if (viewer_model) viewer_model.visible = viewerVisible;
	marker_group.visible = targetVisible;
	skeleton_group.visible = targetVisible;
	hitbox_group.visible = hitbox_capsules_enabled && targetVisible;
	for (let index = 0; index < extra_bot_capsule_groups.length; ++index)
		extra_bot_capsule_groups[index].visible = hitbox_capsules_enabled && Boolean(extra_target_poses[index]?.placed);
	aabb_group.visible = targetVisible;
	muzzle_group.visible = targetVisible;
}

function should_show_viewer_model(hasMap, placed, playing, thirdPerson)
{
	return (!hasMap || placed) && (!playing || thirdPerson);
}

function set_model_opacity()
{
	const opacity = read_number("model-opacity");
	for (const root of [model, viewer_model])
	{
		root?.traverse((node) =>
		{
			if (!node.isMesh)
			{
				return;
			}
			const materials = Array.isArray(node.material) ? node.material : [node.material];
			for (const material of materials)
			{
				material.transparent = opacity < 1;
				material.opacity = opacity;
				material.depthWrite = opacity >= 1;
				material.needsUpdate = true;
			}
		});
	}
}

function stationary_viewer_origins()
{
	const eye = {x: k_viewer_distance, y: 0, z: k_eye_height};
	const left = {x: eye.x, y: -k_shoulder_offset, z: eye.z};
	const right = {x: eye.x, y: k_shoulder_offset, z: eye.z};
	const up = {x: eye.x, y: eye.y, z: eye.z + k_vertical_origin_offset};
	const feet = {x: eye.x, y: eye.y, z: 0};
	return [eye, left, right, up, feet];
}

function runtime_animation_active()
{
	return runtime_animation_enabled && points.length === k_runtime_body_bones.length
		&& runtime_body_bindings.length === points.length;
}

function runtime_body_positions()
{
	if (!runtime_animation_active())
	{
		return points.map((point) => source_to_three(target_world_point(point_vec(point))));
	}
	model.updateWorldMatrix(true, true);
	return points.map((point, index) =>
	{
		const binding = runtime_body_bindings[index];
		return binding ? binding.bone.localToWorld(binding.offset.clone()) : source_to_three(target_world_point(point_vec(point)));
	});
}

function muzzle_position()
{
	const selectedModel = play_active ? bot_weapon_model : weapon_model;
	const selectedKey = play_active ? bot_weapon_key : active_weapon_key;
	const offset = k_weapon_muzzle_offsets[selectedKey];
	if (!selectedModel || !offset)
	{
		return null;
	}
	selectedModel.updateWorldMatrix(true, true);
	return selectedModel.localToWorld(source_to_three(offset).clone());
}

function runtime_muzzle_position()
{
	const key = play_active ? $("bot-weapon-select").value : active_weapon_key;
	const muzzle = target_muzzle({origin: pose_origin(target_pose), yaw: target_pose.yaw,
		height: play_active ? target_pose.height : 72,
		crouched: Boolean(play_active && play_state?.bot?.crouched)}, weapon_muzzle_length(key));
	return muzzle ? source_to_three(muzzle) : null;
}

function runtime_targets()
{
	const targets = [
		...target_aabb_points().map(source_to_three),
		...runtime_body_positions()
	];
	const muzzle = runtime_muzzle_position();
	if (muzzle)
	{
		targets.push(muzzle);
	}
	return targets;
}

function runtime_ray_vertices(targets)
{
	const vertices = [];
	for (const origin of stationary_viewer_origins())
	{
		const start = source_to_three(origin);
		for (const target of targets)
		{
			vertices.push(start, target);
		}
	}
	return vertices;
}

function draw_runtime_rays()
{
	if (map_metadata)
	{
		if (!map_simulation_ready())
		{
			clear_group(ray_group);
			clear_group(origin_group);
			ray_lines = null;
			ray_count = 0;
			clear_ray_count = 0;
			blocked_ray_count = 0;
			wall_visible = null;
			update_status();
			return;
		}
		request_map_trace();
		return;
	}
	clear_group(ray_group);
	clear_group(origin_group);
	ray_lines = null;
	ray_count = 0;
	if (!viewer_model)
	{
		return;
	}

	const targets = runtime_targets();
	const origins = stationary_viewer_origins();
	ray_count = origins.length * targets.length;
	const geometry = new THREE.BufferGeometry().setFromPoints(runtime_ray_vertices(targets));
	const material = new THREE.LineBasicMaterial({color: k_ray_color, transparent: true, opacity: 0.18, depthTest: false});
	ray_lines = new THREE.LineSegments(geometry, material);
	ray_lines.renderOrder = 4;
	ray_group.add(ray_lines);

	for (const origin of origins)
	{
		origin_group.add(make_marker(origin, k_origin_color, k_aabb_dot_radius));
	}
}

function capsule_values(root, bindings)
{
	if (!root || bindings?.length !== k_valve_hitbox_capsules.length) return new Float32Array();
	root.updateWorldMatrix(true, true);
	const values = new Float32Array(bindings.length * 7);
	for (let index = 0; index < bindings.length; ++index)
	{
		const binding = bindings[index];
		const start = three_to_source(binding.bone.localToWorld(binding.worldStart.copy(binding.localStart)));
		const end = three_to_source(binding.bone.localToWorld(binding.worldEnd.copy(binding.localEnd)));
		values.set([start.x, start.y, start.z, end.x, end.y, end.z, binding.radius], index * 7);
	}
	return values;
}

function muzzle_values(pose, actor)
{
	const muzzle = target_muzzle({origin: pose_origin(pose), yaw: pose.yaw, height: pose.height,
		crouched: Boolean(actor?.crouched)}, weapon_muzzle_length($("bot-weapon-select").value));
	return muzzle ? new Float32Array([muzzle.x, muzzle.y, muzzle.z]) : null;
}

function runtime_target_snapshot()
{
	return {capsules: capsule_values(model, hitbox_bindings), muzzle: muzzle_values(target_pose, play_state?.bots?.[0])};
}

function extra_runtime_target_snapshot(index)
{
	const pose = play_active ? extra_bot_render_poses[index] : extra_target_poses[index];
	return {capsules: capsule_values(extra_bot_models[index], extra_bot_capsule_bindings[index]),
		muzzle: muzzle_values(pose, play_state?.bots?.[index + 1])};
}

function target_transfer_list(targetSets)
{
	return targetSets.flatMap((target) => [target.capsules.buffer, ...(target.muzzle ? [target.muzzle.buffer] : [])]);
}

function request_map_trace()
{
	if (!map_simulation_ready())
	{
		return;
	}
	trace_dirty = true;
	if (trace_in_flight)
	{
		return;
	}
	trace_dirty = false;
	trace_in_flight = true;
	const targetSets = [runtime_target_snapshot(), ...extra_target_poses
		.map((pose, index) => pose.placed ? extra_runtime_target_snapshot(index) : null).filter(Boolean)];
	const tuning = runtime_tuning();
	map_worker.postMessage({
		type: "trace",
		id: ++trace_id,
		viewer: {
			origin: pose_origin(viewer_pose),
			yaw: viewer_pose.yaw,
			pingMs: read_number("viewer-ping"),
			tuning,
			buttons: {...movement_buttons}
		},
		targetSets
	}, target_transfer_list(targetSets));
}

function draw_map_trace(origins, rays, blocked, visible, clearCount)
{
	const originCount = origins.length / 3;
	ray_count = rays.length / 6;
	clear_ray_count = clearCount;
	blocked_ray_count = ray_count - clearCount;
	wall_visible = visible;
	const positions = new Float32Array(ray_count * 6);
	const colors = new Float32Array(ray_count * 6);
	const originPositions = [];
	for (let origin = 0; origin < originCount; ++origin)
	{
		const sourceOrigin = {x: origins[origin * 3], y: origins[origin * 3 + 1], z: origins[origin * 3 + 2]};
		originPositions.push(source_to_three(sourceOrigin));
	}
	for (let ray = 0; ray < ray_count; ++ray)
	{
		const start = source_to_three({x: rays[ray * 6], y: rays[ray * 6 + 1], z: rays[ray * 6 + 2]});
		const end = source_to_three({x: rays[ray * 6 + 3], y: rays[ray * 6 + 4], z: rays[ray * 6 + 5]});
		positions.set([start.x, start.y, start.z, end.x, end.y, end.z], ray * 6);
		const color = blocked[ray] ? k_blocked_ray_color : k_clear_ray_color;
		colors.set([color.r, color.g, color.b, color.r, color.g, color.b], ray * 6);
	}
	const positionAttribute = ray_lines?.geometry.getAttribute("position");
	const canReuse = positionAttribute?.array.length === positions.length && origin_group.children.length === originCount;
	if (!canReuse)
	{
		clear_group(ray_group);
		clear_group(origin_group);
		const geometry = new THREE.BufferGeometry();
		geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
		geometry.setAttribute("color", new THREE.BufferAttribute(colors, 3));
		const material = new THREE.LineBasicMaterial({vertexColors: true, transparent: true, opacity: 0.52, depthTest: false});
		ray_lines = new THREE.LineSegments(geometry, material);
		ray_lines.renderOrder = 4;
		ray_group.add(ray_lines);
		for (const position of originPositions)
		{
			origin_group.add(make_position_marker(position, k_origin_color, k_aabb_dot_radius));
		}
		debug_trace_smoothing = null;
	}
	else
	{
		const colorAttribute = ray_lines.geometry.getAttribute("color");
		colorAttribute.array.set(colors);
		colorAttribute.needsUpdate = true;
		debug_trace_smoothing = {
			elapsed: 0,
			fromRays: positionAttribute.array.slice(),
			toRays: positions,
			fromOrigins: origin_group.children.map((marker) => marker.position.clone()),
			toOrigins: originPositions
		};
	}
	update_status();
}

function draw_extra_bot_debug(results)
{
	const previousPositions = extra_bot_debug_groups.map((group) => Object.fromEntries(group.children
		.filter((child) => child.name && child.geometry?.getAttribute("position"))
		.map((child) => [child.name, child.geometry.getAttribute("position").array.slice()])));
	extra_debug_smoothing.length = 0;
	for (let index = 0; index < extra_bot_debug_groups.length; ++index)
	{
		const group = extra_bot_debug_groups[index];
		clear_group(group);
		const result = results[index];
		if (!result) continue;
		const rayPositions = new Float32Array(result.rays.length);
		const rayColors = new Float32Array(rayPositions.length);
		for (let ray = 0; ray < result.rays.length / 6; ++ray)
		{
			const origin = source_to_three({x: result.rays[ray * 6], y: result.rays[ray * 6 + 1], z: result.rays[ray * 6 + 2]});
			const target = source_to_three({x: result.rays[ray * 6 + 3], y: result.rays[ray * 6 + 4], z: result.rays[ray * 6 + 5]});
			rayPositions.set([origin.x, origin.y, origin.z, target.x, target.y, target.z], ray * 6);
			const color = result.blocked[ray] ? k_blocked_ray_color : k_clear_ray_color;
			rayColors.set([color.r, color.g, color.b, color.r, color.g, color.b], ray * 6);
		}
		const rayGeometry = new THREE.BufferGeometry();
		rayGeometry.setAttribute("position", new THREE.BufferAttribute(rayPositions, 3));
		rayGeometry.setAttribute("color", new THREE.BufferAttribute(rayColors, 3));
		const rays = new THREE.LineSegments(rayGeometry, new THREE.LineBasicMaterial({vertexColors: true, transparent: true, opacity: 0.46, depthTest: false}));
		rays.name = "debug-rays";
		rays.visible = !play_active || play_rays_enabled;
		rays.renderOrder = 4;
		queue_extra_debug_interpolation(rays, rayPositions, previousPositions[index][rays.name]);
		group.add(rays);
		group.visible = !play_active || play_debug;
	}
}

function three_position_array(points)
{
	const positions = new Float32Array(points.length * 3);
	for (let index = 0; index < points.length; ++index) positions.set(points[index].toArray(), index * 3);
	return positions;
}

function queue_extra_debug_interpolation(object, target, previous)
{
	if (!previous || previous.length !== target.length) return;
	const destination = target.slice();
	object.geometry.getAttribute("position").array.set(previous);
	extra_debug_smoothing.push({object, from: previous, to: destination, elapsed: 0});
}

function update_debug_trace_smoothing(delta)
{
	if (!debug_trace_smoothing || !ray_lines) return;
	debug_trace_smoothing.elapsed += delta;
	const amount = Math.min(1, debug_trace_smoothing.elapsed / k_debug_draw_interval);
	const positions = ray_lines.geometry.getAttribute("position");
	for (let index = 0; index < positions.array.length; ++index)
	{
		positions.array[index] = THREE.MathUtils.lerp(
			debug_trace_smoothing.fromRays[index], debug_trace_smoothing.toRays[index], amount);
	}
	positions.needsUpdate = true;
	for (let index = 0; index < debug_trace_smoothing.toOrigins.length; ++index)
	{
		origin_group.children[index]?.position.lerpVectors(
			debug_trace_smoothing.fromOrigins[index], debug_trace_smoothing.toOrigins[index], amount);
	}
	if (amount >= 1) debug_trace_smoothing = null;
}

function update_extra_debug_smoothing(delta)
{
	for (let item = extra_debug_smoothing.length - 1; item >= 0; --item)
	{
		const smoothing = extra_debug_smoothing[item];
		if (!smoothing.object.parent)
		{
			extra_debug_smoothing.splice(item, 1);
			continue;
		}
		smoothing.elapsed += delta;
		const amount = Math.min(1, smoothing.elapsed / k_debug_draw_interval);
		const positions = smoothing.object.geometry.getAttribute("position");
		for (let index = 0; index < positions.array.length; ++index)
			positions.array[index] = THREE.MathUtils.lerp(smoothing.from[index], smoothing.to[index], amount);
		positions.needsUpdate = true;
		if (amount >= 1) extra_debug_smoothing.splice(item, 1);
	}
}

function remember_trace_result(rays, visible, clearCount)
{
	ray_count = rays.length / 6;
	clear_ray_count = clearCount;
	blocked_ray_count = ray_count - clearCount;
	wall_visible = visible;
}

function make_marker(point, color, radius)
{
	return make_position_marker(source_to_three(point), color, radius);
}

function make_position_marker(position, color, radius)
{
	const material = new THREE.MeshBasicMaterial({
		color,
		transparent: true,
		opacity: read_number("point-opacity"),
		depthTest: false
	});
	const mesh = new THREE.Mesh(new THREE.SphereGeometry(radius, 16, 10), material);
	mesh.position.copy(position);
	mesh.renderOrder = 10;
	return mesh;
}

function aabb_vertices(points)
{
	const vertices = [];
	for (const [a, b] of k_aabb_edges)
	{
		vertices.push(source_to_three(points[a]), source_to_three(points[b]));
	}
	return vertices;
}

function make_aabb_wire(points)
{
	const geometry = new THREE.BufferGeometry().setFromPoints(aabb_vertices(points));
	const material = new THREE.LineBasicMaterial({color: k_aabb_color, transparent: true, opacity: 0.75});
	const lines = new THREE.LineSegments(geometry, material);
	lines.renderOrder = 8;
	return lines;
}

function skeleton_vertices(positions)
{
	const vertices = [];
	for (const [start, end] of k_skeleton_edges)
	{
		if (positions[start] && positions[end])
		{
			vertices.push(positions[start], positions[end]);
		}
	}
	return vertices;
}

function draw_skeleton(positions)
{
	clear_group(skeleton_group);
	const geometry = new THREE.BufferGeometry().setFromPoints(skeleton_vertices(positions));
	const material = new THREE.LineBasicMaterial({
		color: 0xffffff,
		transparent: true,
		opacity: read_number("point-opacity") * 0.9,
		depthTest: false
	});
	skeleton_lines = new THREE.LineSegments(geometry, material);
	skeleton_lines.renderOrder = 9;
	skeleton_group.add(skeleton_lines);
}

function clear_group(group)
{
	while (group.children.length)
	{
		const child = group.children[0];
		group.remove(child);
		child.traverse?.((node) =>
		{
			node.geometry?.dispose?.();
			if (Array.isArray(node.material))
			{
				node.material.forEach((m) => m.dispose?.());
			}
			else
			{
				node.material?.dispose?.();
			}
		});
	}
}

function capsule_bindings_for(root)
{
	if (!root) return [];
	const bones = new Map();
	root.traverse((node) => bones.set(node.name.toLowerCase(), node));
	const bindings = k_valve_hitbox_capsules.map(([boneName, start, end, radius]) =>
	{
		const bone = bones.get(boneName);
		return bone ? {bone, radius,
			localStart: new THREE.Vector3(...start).divideScalar(k_source_units_per_meter),
			localEnd: new THREE.Vector3(...end).divideScalar(k_source_units_per_meter),
			worldStart: new THREE.Vector3(), worldEnd: new THREE.Vector3()} : null;
	});
	return bindings.every(Boolean) ? bindings : [];
}

function make_capsule_visual(binding)
{
	const geometry = new THREE.CapsuleGeometry(binding.radius / k_source_units_per_meter,
		binding.localStart.distanceTo(binding.localEnd), 6, 12);
	const capsule = new THREE.Group();
	const fill = new THREE.Mesh(geometry, new THREE.MeshBasicMaterial({
		color: 0xff8a00, transparent: true, opacity: 0.45, depthTest: false, depthWrite: false,
		side: THREE.DoubleSide
	}));
	const outline = new THREE.LineSegments(new THREE.EdgesGeometry(geometry, 15),
		new THREE.LineBasicMaterial({color: 0x111111, transparent: true, opacity: 0.75, depthTest: false}));
	fill.renderOrder = 12;
	outline.renderOrder = 13;
	capsule.add(fill, outline);
	binding.capsule = capsule;
	return capsule;
}

function rebuild_hitbox_capsules()
{
	clear_group(hitbox_group);
	hitbox_bindings = capsule_bindings_for(model);
	for (const binding of hitbox_bindings)
	{
		hitbox_group.add(make_capsule_visual(binding));
	}
	update_hitbox_capsules();
}

function update_hitbox_capsules()
{
	update_capsule_bindings(model, hitbox_bindings);
	for (let index = 0; index < extra_bot_models.length; ++index)
		update_capsule_bindings(extra_bot_models[index], extra_bot_capsule_bindings[index]);
}

function update_capsule_bindings(root, bindings)
{
	if (!root || !bindings?.length) return;
	root.updateWorldMatrix(true, true);
	const axis = new THREE.Vector3(0, 1, 0);
	for (const {bone, capsule, localStart, localEnd} of bindings)
	{
		if (!capsule) continue;
		const start = bone.localToWorld(localStart.clone());
		const end = bone.localToWorld(localEnd.clone());
		const direction = end.clone().sub(start);
		capsule.position.copy(start).add(end).multiplyScalar(0.5);
		capsule.quaternion.setFromUnitVectors(axis, direction.normalize());
	}
}

function dispose_root(root, disposeGeometry = false, disposeMaterials = false, disposeTextures = false)
{
	root?.traverse((node) =>
	{
		if (disposeGeometry) node.geometry?.dispose?.();
		if (!disposeMaterials || !node.material) return;
		const materials = Array.isArray(node.material) ? node.material : [node.material];
		for (const material of materials)
		{
			if (disposeTextures)
				for (const value of Object.values(material))
					if (value?.isTexture) value.dispose();
			material.dispose?.();
		}
	});
}

function draw_muzzle_point()
{
	clear_group(muzzle_group);
	const position = muzzle_position();
	if (!position)
	{
		return;
	}
	const material = new THREE.MeshBasicMaterial({
		color: k_muzzle_color,
		transparent: true,
		opacity: read_number("point-opacity"),
		depthTest: false
	});
	const mesh = new THREE.Mesh(new THREE.SphereGeometry(k_muzzle_dot_radius, 16, 10), material);
	mesh.position.copy(position);
	mesh.renderOrder = 11;
	muzzle_group.add(mesh);
}

function draw_points()
{
	apply_player_transforms();
	update_hitbox_capsules();
	transform.detach();
	clear_group(marker_group);
	clear_group(aabb_group);

	const aabb_points = target_aabb_points();
	aabb_group.add(make_aabb_wire(aabb_points));
	for (const point of aabb_points)
	{
		aabb_group.add(make_marker(point, k_aabb_color, k_aabb_dot_radius));
	}

	const body_positions = runtime_body_positions();
	draw_skeleton(body_positions);
	for (let index = 0; index < body_positions.length; ++index)
	{
		const marker = make_position_marker(body_positions[index], index === selected_index ? k_selected_color : k_body_color, index === selected_index ? k_selected_dot_radius : k_body_dot_radius);
		marker.userData.pointIndex = index;
		marker_group.add(marker);
		if (index === selected_index && !runtime_animation_active())
		{
			transform.attach(marker);
		}
	}

	if (points.length === 0)
	{
		transform.detach();
	}
	draw_muzzle_point();
	draw_runtime_rays();
}

function update_animated_preview()
{
	if (!runtime_animation_active() && !play_active)
	{
		return;
	}
	const body_positions = runtime_body_positions();
	update_hitbox_capsules();
	for (let index = 0; index < Math.min(body_positions.length, marker_group.children.length); ++index)
	{
		marker_group.children[index].position.copy(body_positions[index]);
	}
	skeleton_lines?.geometry.setFromPoints(skeleton_vertices(body_positions));
	const aabbPoints = target_aabb_points();
	aabb_group.children[0]?.geometry.setFromPoints(aabb_vertices(aabbPoints));
	for (let index = 0; index < Math.min(aabbPoints.length, aabb_group.children.length - 1); ++index)
	{
		aabb_group.children[index + 1].position.copy(source_to_three(aabbPoints[index]));
	}
	const muzzle = muzzle_position();
	if (muzzle_group.children[0] && muzzle)
	{
		muzzle_group.children[0].position.copy(muzzle);
	}
	if (play_active)
	{
		const now = performance.now();
		if (now - last_play_target_send >= FPS_DT * 1000)
		{
			last_play_target_send = now;
			const targetSets = [runtime_target_snapshot(), ...extra_bot_models.map((_, index) => extra_runtime_target_snapshot(index))];
			map_worker.postMessage({type: "play-targets", targetSets}, target_transfer_list(targetSets));
		}
	}
	else if (map_metadata)
	{
		request_map_trace();
	}
	else if (ray_lines)
	{
		ray_lines.geometry.setFromPoints(runtime_ray_vertices(runtime_targets()));
	}
}

function render_point_list()
{
	const list = $("points-list");
	list.innerHTML = "";
	for (let index = 0; index < points.length; ++index)
	{
		const point = points[index];
		const row = document.createElement("button");
		row.type = "button";
		row.className = "point-row";
		row.setAttribute("role", "option");
		row.setAttribute("aria-selected", String(index === selected_index));
		row.addEventListener("click", () => select_point(index));

		const name = document.createElement("span");
		name.className = "point-name";
		name.textContent = point.name;
		const coordinates = document.createElement("span");
		coordinates.className = "point-coords";
		coordinates.textContent = `${format_number(point.x)}, ${format_number(point.y)}, ${format_number(point.z)}`;
		row.append(name, coordinates);
		list.appendChild(row);
	}
}

function render_selected_point()
{
	const point = points[selected_index];
	const locked = runtime_animation_active();
	for (const id of ["point-name", "point-x", "point-y", "point-z"])
	{
		$(id).disabled = !point || locked;
	}
	$("add-point").disabled = locked || points.length >= 32;
	$("duplicate-point").disabled = locked || !point || points.length >= 32;
	$("delete-point").disabled = locked || !can_delete_point(points.length);
	$("reset-points").disabled = locked;
	$("point-count").textContent = `${points.length} point${points.length === 1 ? "" : "s"}`;
	$("point-name").value = point?.name ?? "";
	$("point-x").value = point ? format_number(point.x) : "";
	$("point-y").value = point ? format_number(point.y) : "";
	$("point-z").value = point ? format_number(point.z) : "";
}

function render_point_editor()
{
	render_point_list();
	render_selected_point();
}

function update_status()
{
	const runtimeWeapon = play_active ? $("bot-weapon-select").value : active_weapon_key;
	const hasRuntimeMuzzle = weapon_muzzle_length(runtimeWeapon) > 0;
	const bindingsReady = points.length === k_runtime_body_bones.length && runtime_body_bindings.length === points.length;
	$("status-body-count").textContent = hitbox_bindings.length;
	$("status-aabb-count").textContent = points.length;
	$("status-target-count").textContent = hitbox_bindings.length + Number(hasRuntimeMuzzle);
	$("status-ray-count").textContent = ray_count;
	$("status-origin-count").textContent = origin_group?.children.length || (map_metadata ? 0 : stationary_viewer_origins().length);
	$("status-ray-result").textContent = map_metadata ? `${clear_ray_count}/${blocked_ray_count}` : "--";
	$("status-wall-result").textContent = wall_visible === null ? (map_metadata ? "Place players" : "No map") : (wall_visible ? "Visible" : "Hidden");
	$("status-wall-result").style.color = wall_visible === null ? "" : (wall_visible ? "#13784a" : "#b90f20");
	$("status-muzzle").textContent = hasRuntimeMuzzle ? runtimeWeapon : "None";
	$("status-selected").textContent = points[selected_index]?.name ?? "None";
	$("status").textContent = status_extra || (model ? "Studio ready." : "Load a local SAS model to begin.");
	$("model-status").textContent = model_status;
	$("model-status").classList.toggle("ready", Boolean(model));
	$("animation-clip").disabled = !bindingsReady;
	$("runtime-animation").disabled = !bindingsReady;
	for (const id of ["viewer-ping", "shoulder-base", "shoulder-rtt-scale", "shoulder-max",
		"he-clear-radius", "he-clear-seconds", "simulation-seed", "bot-weapon-select"])
	{
		$(id).disabled = play_active;
	}
	$("play-mode").disabled = !play_ready();
	$("runtime-animation").setAttribute("aria-pressed", String(runtime_animation_enabled && !play_active));
	$("edit-mode").setAttribute("aria-pressed", String(!runtime_animation_enabled && !play_active));
	$("play-mode").setAttribute("aria-pressed", String(play_active));
}

function update_scene()
{
	selected_index = points.length ? Math.min(Math.max(selected_index, 0), points.length - 1) : -1;
	set_model_opacity();
	draw_points();
	render_point_editor();
	update_status();
}

function set_map_summary(title, detail, bad = false)
{
	const summary = $("map-summary");
	summary.replaceChildren();
	const heading = document.createElement("strong");
	heading.textContent = title;
	summary.append(heading, document.createTextNode(detail));
	summary.classList.toggle("bad", bad);
}

function update_placement_status()
{
	let text = "Load a map, then place the target and viewer on its collision mesh.";
	if (map_metadata)
	{
		if (placement_mode)
		{
			text = `Click the map to place the ${placement_mode}.`;
		}
		else if (map_simulation_ready())
		{
			text = "Both players are placed. Rays now use baked wall checks.";
		}
		else
		{
			text = "Place both players before wall-result rays can run.";
		}
	}
	$("placement-status").textContent = text;
	for (const role of ["target", "viewer"])
	{
		const button = $(`place-${role}`);
		button.disabled = !map_metadata;
		button.setAttribute("aria-pressed", String(placement_mode === role));
		button.classList.toggle("primary", placement_mode === role);
	}
	$("unload-map").disabled = !map_metadata;
	$("frame-map").disabled = !map_metadata;
	$("frame-players").disabled = !map_simulation_ready();
}

function write_pose_fields(role, pose)
{
	for (const key of ["x", "y", "z", "yaw"])
	{
		$(`${role}-${key}`).value = format_number(pose[key]);
	}
}

function update_player_poses()
{
	apply_player_transforms();
	draw_points();
	update_placement_status();
	update_status();
}

function face_players()
{
	const viewerToTarget = Math.atan2(target_pose.y - viewer_pose.y, target_pose.x - viewer_pose.x) * 180 / Math.PI;
	viewer_pose.yaw = viewerToTarget;
	target_pose.yaw = viewerToTarget + 180;
	write_pose_fields("viewer", viewer_pose);
	write_pose_fields("target", target_pose);
}

function apply_default_spawn_pair(map_name)
{
	const pair = k_map_spawn_pairs[map_name];
	if (!pair)
	{
		return false;
	}
	Object.assign(target_pose, pair.target, {placed: true});
	Object.assign(viewer_pose, pair.viewer, {placed: true});
	face_players();
	return true;
}

function frame_box(box)
{
	if (box.isEmpty())
	{
		return;
	}
	const center = box.getCenter(new THREE.Vector3());
	const size = box.getSize(new THREE.Vector3());
	const maximum = Math.max(size.x, size.y, size.z, 1);
	const distance = maximum / (2 * Math.tan(degrees_to_radians(camera.fov) / 2)) * 1.25;
	const direction = new THREE.Vector3(1, 0.72, 1).normalize();
	camera.position.copy(center).addScaledVector(direction, distance);
	camera.near = Math.max(0.02, distance / 10000);
	camera.far = Math.max(3000, distance * 4);
	update_camera_projection();
	orbit.target.copy(center);
	orbit.update();
}

function frame_map()
{
	if (map_mesh)
	{
		frame_box(new THREE.Box3().setFromObject(map_mesh));
	}
}

function frame_players()
{
	if (!map_simulation_ready())
	{
		return;
	}
	const box = new THREE.Box3();
	box.expandByPoint(source_to_three(pose_origin(target_pose)));
	for (const pose of extra_target_poses.filter((pose) => pose.placed)) box.expandByPoint(source_to_three(pose_origin(pose)));
	box.expandByPoint(source_to_three(pose_origin(viewer_pose)));
	box.expandByScalar(2.5);
	frame_box(box);
}

function clear_map_mesh()
{
	clear_map_traversal();
	clear_extra_bots();
	for (const pose of extra_target_poses) pose.placed = false;
	for (const group of extra_bot_debug_groups) clear_group(group);
	if (!map_mesh)
	{
		return;
	}
	scene.remove(map_mesh);
	map_mesh.geometry.dispose();
	map_mesh.material.dispose();
	map_mesh = null;
}

function clear_map_traversal()
{
	if (!map_traversal_mesh) return;
	scene.remove(map_traversal_mesh);
	for (const child of map_traversal_mesh.children) child.material?.dispose?.();
	map_traversal_mesh.geometry.dispose();
	map_traversal_mesh.material.dispose();
	map_traversal_mesh = null;
}

function merge_bot_traversals(visibilities)
{
	const values = visibilities.map((visibility) => visibility.traversal).filter(Boolean);
	if (!values.length) return null;
	const triangles = new Set();
	for (const value of values) for (const triangle of value.triangles) triangles.add(triangle);
	const positions = new Float32Array(values.reduce((count, value) => count + (value.positions?.length || 0), 0));
	let positionOffset = 0;
	for (const value of values)
	{
		if (!value.positions) continue;
		positions.set(value.positions, positionOffset);
		positionOffset += value.positions.length;
	}
	const total = (name) => values.reduce((sum, value) => sum + value[name], 0);
	return {triangles: new Uint32Array([...triangles]), positions, botCount: values.length,
		visitedNodes: total("visitedNodes"), testedPackets: total("testedPackets"),
		testedTriangles: total("testedTriangles"), boundsHits: total("boundsHits"),
		boundsTests: total("boundsTests"), cacheHits: total("cacheHits"), cacheTests: total("cacheTests")};
}

function draw_map_traversal(value)
{
	if (!map_mesh || !map_metadata || !value?.positions) return;
	clear_map_traversal();
	const geometry = new THREE.BufferGeometry();
	geometry.setAttribute("position", new THREE.BufferAttribute(value.positions, 3));
	const material = new THREE.MeshBasicMaterial({
		color: 0xf0ad31, transparent: true, opacity: 0.33, depthWrite: false,
		side: THREE.DoubleSide, polygonOffset: true, polygonOffsetFactor: -2
	});
	map_traversal_mesh = new THREE.Mesh(geometry, material);
	map_traversal_mesh.renderOrder = 2;
	const outline = new THREE.Mesh(geometry, new THREE.MeshBasicMaterial({
		color: 0x000000, wireframe: true, transparent: true, opacity: 0.16, depthWrite: false,
		side: THREE.DoubleSide, polygonOffset: true, polygonOffsetFactor: -3
	}));
	outline.renderOrder = 3;
	map_traversal_mesh.add(outline);
	scene.add(map_traversal_mesh);
	update_map_material();
	const skipped = map_metadata.triangleCount ? 100 * (1 - value.testedTriangles / map_metadata.triangleCount) : 0;
	$("play-bvh").textContent = `BVH ${value.visitedNodes.toLocaleString()}/${map_metadata.nodeCount.toLocaleString()} nodes · ${value.testedPackets.toLocaleString()}/${map_metadata.packetCount.toLocaleString()} packets · ${value.testedTriangles.toLocaleString()}/${map_metadata.triangleCount.toLocaleString()} triangles · ${skipped.toFixed(2)}% skipped · boxes ${value.boundsHits}/${value.boundsTests} · cache ${value.cacheHits}/${value.cacheTests}`;
}

function reset_wall_result()
{
	++trace_id;
	clear_group(ray_group);
	clear_group(origin_group);
	ray_lines = null;
	ray_count = 0;
	clear_ray_count = 0;
	blocked_ray_count = 0;
	wall_visible = null;
	trace_in_flight = false;
	trace_dirty = false;
}

function unload_map(message = "Map unloaded. Static Studio rays restored.")
{
	leave_play_mode();
	++map_load_id;
	map_worker.postMessage({type: "clear"});
	clear_map_mesh();
	map_metadata = null;
	map_report = null;
	placement_mode = "";
	Object.assign(target_pose, {x: 0, y: 0, z: 0, yaw: 0, placed: false});
	Object.assign(viewer_pose, {x: k_viewer_distance, y: 0, z: 0, yaw: 180, placed: false});
	write_pose_fields("target", target_pose);
	write_pose_fields("viewer", viewer_pose);
	studio_ground.visible = true;
	studio_grid.visible = true;
	reset_wall_result();
	set_map_summary("No map loaded", "Mirage will load automatically when its local BVH8 is available.");
	status_extra = message;
	update_scene();
	update_placement_status();
}

function report_matches_map(report, metadata)
{
	return report?.map === metadata.mapName
		&& Number.parseInt(report.source_crc32, 16) === metadata.sourceCrc32
		&& Number(report.source_size) === metadata.sourceSize
		&& Number(report.nodes) === metadata.nodeCount
		&& Number(report.packets) === metadata.packetCount
		&& Number(report.baked_triangles) === metadata.triangleCount
		&& Number(report.max_depth) === metadata.maxDepth;
}

function install_loaded_map(metadata, positions)
{
	if (map_report && !report_matches_map(map_report, metadata))
	{
		map_worker.postMessage({type: "clear"});
		clear_map_mesh();
		map_metadata = null;
		placement_mode = "";
		studio_ground.visible = true;
		studio_grid.visible = true;
		reset_wall_result();
		set_map_summary("Mirage report mismatch", "The local BVH8 and JSON report do not describe the same bake.", true);
		status_extra = "Map rejected: Mirage report mismatch.";
		update_scene();
		update_placement_status();
		update_status();
		return;
	}
	clear_map_mesh();
	const geometry = new THREE.BufferGeometry();
	geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
	geometry.boundingBox = new THREE.Box3(
		source_to_three({x: metadata.worldMin[0], y: metadata.worldMin[1], z: metadata.worldMin[2]}),
		source_to_three({x: metadata.worldMax[0], y: metadata.worldMax[1], z: metadata.worldMax[2]}));
	geometry.boundingSphere = geometry.boundingBox.getBoundingSphere(new THREE.Sphere());
	const material = new THREE.MeshBasicMaterial({
		color: k_map_color,
		wireframe: map_wireframe,
		transparent: true,
		opacity: read_number("map-opacity"),
		depthWrite: false,
		side: THREE.DoubleSide
	});
	install_map_focus(material);
	map_mesh = new THREE.Mesh(geometry, material);
	map_mesh.renderOrder = 1;
	scene.add(map_mesh);
	update_map_material();
	map_metadata = metadata;
	load_surface_sidecar(metadata.mapName);
	const has_default_spawns = apply_default_spawn_pair(metadata.mapName);
	if (!has_default_spawns)
	{
		target_pose.placed = false;
		viewer_pose.placed = false;
	}
	placement_mode = has_default_spawns ? "" : "target";
	write_pose_fields("target", target_pose);
	write_pose_fields("viewer", viewer_pose);
	prepare_preview_bots();
	studio_ground.visible = false;
	studio_grid.visible = false;
	reset_wall_result();
	set_map_summary(metadata.mapName,
		`${metadata.triangleCount.toLocaleString()} collision triangles, ${metadata.renderedTriangleCount.toLocaleString()} rendered, ${metadata.nodeCount.toLocaleString()} nodes, ${(metadata.fileSize / 1048576).toFixed(1)} MB`);
	status_extra = has_default_spawns
		? `${metadata.mapName} loaded with a default spawn pair.`
		: `${metadata.mapName} loaded. Click the map to place the target.`;
	update_scene();
	update_placement_status();
	if (has_default_spawns)
	{
		reset_camera();
	}
	else
	{
		frame_map();
	}
}

async function load_surface_sidecar(mapName)
{
	try
	{
		let response = await fetch(`local_assets/maps/${mapName}.surfaces`, {cache: "no-store"});
		if (!response.ok) response = await fetch(`../../data/maps/${mapName}.surfaces`, {cache: "no-store"});
		if (!response.ok) return;
		const buffer = await response.arrayBuffer();
		if (map_metadata?.mapName === mapName) map_worker.postMessage({type: "load-surfaces", buffer}, [buffer]);
	}
	catch
	{
		// Surface data is Studio-only. Collision and concrete footsteps remain available.
	}
}

function init_map_worker()
{
	map_worker = new Worker(new URL("./bvh8_worker.js?studio=37", import.meta.url), {type: "module"});
	map_worker.addEventListener("message", (event) =>
	{
		const message = event.data;
		if (message.type === "loaded" && message.id === map_load_id)
		{
			install_loaded_map(message.metadata, message.positions);
		}
		else if (message.type === "traced" && message.id === trace_id)
		{
			trace_in_flight = false;
			if (map_simulation_ready() && message.results?.length)
			{
				const primary = message.results[0];
				draw_map_trace(primary.origins, primary.rays, primary.blocked, primary.visible, primary.clearCount);
				const now = performance.now() / 1000;
				if (now - last_extra_debug_draw >= k_debug_draw_interval)
				{
					last_extra_debug_draw = now;
					draw_extra_bot_debug(message.results.slice(1));
				}
			}
			if (trace_dirty)
			{
				request_map_trace();
			}
		}
		else if (message.type === "picked" && message.id === placement_pick_id
			&& message.mapId === map_load_id && message.mode === placement_mode && message.point)
		{
			place_map_actor(message.mode, message.point);
		}
		else if (message.type === "play-state")
		{
			handle_play_state(message.state);
		}
		else if (message.type === "surfaces-loaded")
		{
			status_extra = `${message.metadata.surfaceCount} map surfaces loaded for footsteps.`;
			update_status();
		}
		else if (message.type === "play-started")
		{
			status_extra = play_nav ? "FPS worker active with nav routing." : "FPS worker active with BVH fallback routing.";
			update_status();
		}
		else if (message.type === "error" && message.operation === "load" && message.id === map_load_id)
		{
			set_map_summary("Map could not load", message.message, true);
			status_extra = `Map rejected: ${message.message}`;
			update_status();
		}
		else if (message.type === "error" && message.operation === "trace")
		{
			trace_in_flight = false;
			wall_visible = null;
			status_extra = `Map trace failed open: ${message.message}`;
			update_status();
		}
		else if (message.type === "error" && message.operation === "load-surfaces")
		{
			status_extra = `Surface sidecar rejected; concrete footsteps remain available: ${message.message}`;
			update_status();
		}
		else if (message.type === "error" && message.operation === "play")
		{
			status_extra = `Play stopped safely: ${message.message}`;
			leave_play_mode();
			update_status();
		}
	});
	map_worker.addEventListener("error", (event) =>
	{
		trace_in_flight = false;
		set_map_summary("Map worker failed", event.message || "Unknown worker error", true);
		status_extra = "Map worker failed; Studio wall results are unavailable.";
		update_status();
	});
}

function send_map_buffer(buffer, report = null)
{
	leave_play_mode();
	map_report = report;
	set_map_summary("Validating map", "Reading the BVH8 tree and rebuilding its collision wireframe...");
	status_extra = "Validating BVH8 in the background...";
	update_status();
	map_worker.postMessage({type: "load", id: map_load_id, buffer, unitsPerMeter: k_source_units_per_meter}, [buffer]);
}

async function load_mirage()
{
	leave_play_mode();
	const id = ++map_load_id;
	set_map_summary("Loading de_mirage", "Fetching the local 7.7 MB BVH8 bake...");
	status_extra = "Loading local de_mirage BVH8...";
	update_status();
	try
	{
		const [mapResponse, reportResponse] = await Promise.all([
			fetch("../../data/maps/de_mirage.bvh8", {cache: "no-store"}),
			fetch("../../data/maps/de_mirage.json", {cache: "no-store"})
		]);
		if (!mapResponse.ok)
		{
			throw new Error(`BVH8 returned HTTP ${mapResponse.status}`);
		}
		if (!reportResponse.ok)
		{
			throw new Error(`JSON report returned HTTP ${reportResponse.status}`);
		}
		const [buffer, report] = await Promise.all([mapResponse.arrayBuffer(), reportResponse.json()]);
		if (id === map_load_id)
		{
			send_map_buffer(buffer, report);
		}
	}
	catch (error)
	{
		if (id !== map_load_id)
		{
			return;
		}
		set_map_summary("Mirage unavailable", "Use Load BVH8 to choose a baked map from this machine.");
		status_extra = `Local Mirage did not load: ${error.message || error}`;
		update_status();
	}
}

async function load_map_file(file)
{
	leave_play_mode();
	const id = ++map_load_id;
	try
	{
		const buffer = await file.arrayBuffer();
		if (id === map_load_id)
		{
			send_map_buffer(buffer);
		}
	}
	catch (error)
	{
		set_map_summary("Map could not load", error.message || String(error), true);
		status_extra = `Could not read ${file.name}.`;
		update_status();
	}
}

function select_point(index)
{
	selected_index = points.length ? Math.min(Math.max(index, 0), points.length - 1) : -1;
	update_scene();
}

function export_json()
{
	const export_points = validated_points({
		version: 1,
		coordinate_space: "source_local",
		model: "ctm_sas",
		point_count: points.length,
		points
	}, "current preset");
	return JSON.stringify({
		version: 1,
		coordinate_space: "source_local",
		model: "ctm_sas",
		point_count: export_points.length,
		points: export_points
	}, null, "\t") + "\n";
}

function set_points(next_points)
{
	points = next_points.map(clone_point);
	selected_index = 0;
	if (!capture_runtime_body_bindings() && model)
	{
		runtime_animation_enabled = false;
	}
	update_scene();
}

function make_mixer(gltf, root)
{
	const mixer = new THREE.AnimationMixer(root);
	const pose = THREE.AnimationClip.findByName(gltf.animations || [], "tools_preview");
	if (pose)
	{
		mixer.cs2fow_action = mixer.clipAction(pose).play();
		mixer.setTime(0);
	}
	return mixer;
}

function capture_runtime_body_bindings()
{
	runtime_body_bindings = [];
	if (!model || points.length !== k_runtime_body_bones.length)
	{
		return false;
	}
	model.position.copy(source_to_three(pose_origin(target_pose)));
	model.rotation.set(0, degrees_to_radians(target_pose.yaw), 0);
	model.updateWorldMatrix(true, true);
	for (let index = 0; index < k_runtime_body_bones.length; ++index)
	{
		const bone = model.getObjectByName(k_runtime_body_bones[index]);
		if (!bone)
		{
			runtime_body_bindings = [];
			return false;
		}
		runtime_body_bindings.push({
			bone,
			offset: bone.worldToLocal(source_to_three(target_world_point(point_vec(points[index]))).clone())
		});
	}
	return true;
}

function animation_choices()
{
	return k_animation_sets[active_weapon_key] || k_animation_sets.default;
}

function refresh_animation_choices()
{
	const select = $("animation-clip");
	const previous = select.value || "idle";
	const available = new Set(model_animations.map((clip) => clip.name));
	let choices = animation_choices().filter((choice) => available.size === 0 || available.has(choice.clip));
	if (choices.length === 0 && available.has("tools_preview"))
	{
		choices = [{id: "idle", label: "Standing pose", clip: "tools_preview"}];
	}
	const preferred = choices.map((choice) =>
	{
		const option = document.createElement("option");
		option.value = choice.id;
		option.textContent = choice.label;
		return option;
	});
	const used = new Set(choices.map((choice) => choice.clip));
	const library = model_animations.filter((clip) => !used.has(clip.name)).map((clip) =>
	{
		const option = document.createElement("option");
		option.value = `clip:${clip.name}`;
		option.textContent = clip.name;
		return option;
	});
	select.replaceChildren(...preferred, ...library);
	select.value = [...preferred, ...library].some((option) => option.value === previous) ? previous : (choices[0]?.id || library[0]?.value || "");
}

function selected_animation_name()
{
	const selected = $("animation-clip").value;
	if (selected.startsWith("clip:")) return selected.slice(5);
	return animation_choices().find((choice) => choice.id === selected)?.clip || "tools_preview";
}

function set_mixer_animation(mixer, animations, name, transition, restart = false)
{
	if (!mixer)
	{
		return 0;
	}
	const clip = THREE.AnimationClip.findByName(animations, name);
	if (!clip)
	{
		return 0;
	}
	const previous = mixer.cs2fow_action;
	const next = mixer.clipAction(clip);
	if (previous === next)
	{
		if (restart) next.reset().play();
		return clip.duration;
	}
	next.reset().setEffectiveTimeScale(1).setEffectiveWeight(1).play();
	if (transition && previous)
	{
		previous.crossFadeTo(next, k_animation_transition_seconds, false);
	}
	else
	{
		previous?.stop();
		mixer.setTime(0);
	}
	mixer.cs2fow_action = next;
	return clip.duration;
}

function play_selected_animation(transition = true)
{
	const name = selected_animation_name();
	set_mixer_animation(model_mixer, model_animations, name, transition);
	set_mixer_animation(viewer_mixer, viewer_animations, name, transition);
}

function show_standing_pose()
{
	for (const [mixer, animations] of [[model_mixer, model_animations], [viewer_mixer, viewer_animations]])
	{
		set_mixer_animation(mixer, animations, "tools_preview", false);
	}
}

function play_pointer_locked()
{
	return document.pointerLockElement === renderer?.domElement;
}

function update_play_input()
{
	if (!play_active) return;
	map_worker.postMessage({type: "play-input", buttons: {...play_keys}});
}

function clear_play_input()
{
	for (const key of Object.keys(play_keys)) play_keys[key] = false;
	update_play_input();
}

function play_now()
{
	return play_state?.time || 0;
}

function schedule_play(delay, callback, serial = play_action_serial)
{
	play_scheduled.push({at: play_now() + delay, callback, serial});
}

function run_play_schedule(now)
{
	for (let index = play_scheduled.length - 1; index >= 0; --index)
	{
		const item = play_scheduled[index];
		if (item.at > now) continue;
		play_scheduled.splice(index, 1);
		if (play_active && item.serial === play_action_serial) item.callback();
	}
}

function set_root_opacity(root, opacity)
{
	root?.traverse((node) =>
	{
		if (!node.isMesh) return;
		const materials = Array.isArray(node.material) ? node.material : [node.material];
		for (const material of materials)
		{
			const transparent = opacity < 1;
			if (material.opacity === opacity && material.transparent === transparent && material.depthWrite === !transparent) continue;
			const modeChanged = material.transparent !== transparent;
			material.transparent = transparent;
			material.opacity = opacity;
			material.depthWrite = !transparent;
			if (modeChanged) material.needsUpdate = true;
		}
	});
}

function update_play_visibility()
{
	if (!play_active) return;
	const botVisible = play_debug || wall_visible !== false;
	if (model) model.visible = botVisible;
	set_root_opacity(model, play_debug && wall_visible === false ? 0.28 : 1);
	for (let index = 0; index < extra_bot_models.length; ++index)
	{
		const visible = play_state?.visibilities?.[index + 1]?.visible !== false;
		extra_bot_models[index].visible = play_debug || visible;
		set_root_opacity(extra_bot_models[index], play_debug && !visible ? 0.28 : 1);
	}
	if (viewer_model) viewer_model.visible = play_third_person;
	if (player_weapon_mount) player_weapon_mount.visible = play_third_person;
	if (viewmodel_root) viewmodel_root.visible = !play_third_person && !play_scoped;
	for (const group of [marker_group, skeleton_group, aabb_group, muzzle_group, origin_group, nav_group])
	{
		if (group) group.visible = play_debug;
	}
	if (hitbox_group) hitbox_group.visible = hitbox_capsules_enabled && play_debug;
	for (const group of extra_bot_capsule_groups) group.visible = hitbox_capsules_enabled && play_debug;
	for (const group of extra_bot_debug_groups)
	{
		group.visible = play_debug;
		const rays = group.getObjectByName("debug-rays");
		if (rays) rays.visible = play_rays_enabled;
	}
	if (ray_group) ray_group.visible = play_debug && play_rays_enabled;
	const visibilities = play_state?.visibilities || (play_state?.visibility ? [play_state.visibility] : []);
	const visibleBots = visibilities.filter((visibility) => visibility.visible).length;
	const totalBots = visibilities.length || 3;
	$("play-result").classList.toggle("partial", visibleBots > 0 && visibleBots < totalBots);
	$("play-result").classList.toggle("visible", visibleBots === totalBots);
	$("play-result").textContent = `${visibleBots}/${totalBots} visible`;
	$("play-debug-state").textContent = play_debug ? `Debug · Rays ${play_rays_enabled ? "on" : "off"}` : "Real";
	$("play-view").textContent = play_third_person ? "Third person" : "First person";
}

function update_map_material()
{
	if (!map_mesh) return;
	const {opacity, transparent} = map_material_state(read_number("map-opacity"));
	if (map_mesh.material.transparent !== transparent || map_mesh.material.depthWrite === transparent)
	{
		map_mesh.material.transparent = transparent;
		map_mesh.material.depthWrite = !transparent;
		map_mesh.material.needsUpdate = true;
	}
	map_mesh.material.opacity = opacity;
	if (map_mesh.material.userData.focusUniforms)
		map_mesh.material.userData.focusUniforms.enabled.value = Number(map_focus);
}

function install_map_focus(material)
{
	material.onBeforeCompile = (shader) =>
	{
		shader.uniforms.focusCenter = {value: new THREE.Vector3()};
		shader.uniforms.focusEnabled = {value: Number(map_focus)};
		shader.vertexShader = shader.vertexShader
			.replace("#include <common>", "#include <common>\nvarying vec3 focusWorldPosition;")
			.replace("#include <worldpos_vertex>", "#include <worldpos_vertex>\nfocusWorldPosition = (modelMatrix * vec4(transformed, 1.0)).xyz;");
		shader.fragmentShader = shader.fragmentShader
			.replace("#include <common>", "#include <common>\nvarying vec3 focusWorldPosition;\nuniform vec3 focusCenter;\nuniform float focusEnabled;")
			.replace("#include <opaque_fragment>", "diffuseColor.a *= mix(1.0, mix(0.08, 1.0, 1.0 - smoothstep(15.24, 25.4, distance(focusWorldPosition, focusCenter))), focusEnabled);\n#include <opaque_fragment>");
		material.userData.focusUniforms = {center: shader.uniforms.focusCenter, enabled: shader.uniforms.focusEnabled};
	};
	material.customProgramCacheKey = () => "cs2fow-map-focus-v1";
}

function update_map_focus()
{
	const uniforms = map_mesh?.material.userData.focusUniforms;
	if (uniforms) uniforms.center.value.copy(source_to_three(pose_origin(viewer_pose)));
}

function map_material_state(opacity)
{
	return {opacity, transparent: opacity < 1 || map_focus};
}

function set_play_scope(enabled)
{
	play_scoped = Boolean(enabled && play_weapon_key === "awp" && !play_grenade);
	camera.fov = play_scoped ? 15 : k_world_fov;
	update_camera_projection();
	if (play_active) map_worker?.postMessage({
		type: "play-speed",
		value: play_scoped ? 100 : k_weapon_stats[play_weapon_key].speed
	});
	update_play_visibility();
}

function update_play_camera()
{
	if (!play_active || !play_state) return;
	const eye = {x: viewer_pose.x, y: viewer_pose.y, z: viewer_pose.z + play_eye_height};
	const yaw = viewer_pose.yaw * Math.PI / 180;
	const pitch = play_pitch * Math.PI / 180;
	const direction = {
		x: Math.cos(yaw) * Math.cos(pitch),
		y: Math.sin(yaw) * Math.cos(pitch),
		z: Math.sin(pitch)
	};
	let cameraPosition = eye;
	let lookPosition = add_source(eye, direction);
	if (play_third_person)
	{
		cameraPosition = {
			x: eye.x - Math.cos(yaw) * 112,
			y: eye.y - Math.sin(yaw) * 112,
			z: eye.z + 24
		};
		lookPosition = add_source(eye, {x: direction.x * 96, y: direction.y * 96, z: direction.z * 96 - 8});
	}
	camera.position.copy(source_to_three(cameraPosition));
	const look = source_to_three(lookPosition);
	camera.up.set(0, 1, 0);
	camera.lookAt(look);
}

function weapon_draw_sound(key)
{
	return key === "m4a1_silencer" ? "m4_draw" : key === "awp" ? "awp_draw" : key === "knife" ? "knife_draw" : "usp_draw";
}

function weapon_label(key)
{
	return k_weapon_stats[key]?.label || "Unarmed";
}

function set_hud_icon(id, key)
{
	const image = $(id);
	const url = local_manifest.icons?.[key] || "";
	image.hidden = !url;
	if (url) image.src = url;
}

function update_play_item()
{
	const primary = $("player-primary-select").value;
	const grenadeKey = play_grenade === "he" ? "hegrenade" : "smokegrenade";
	const activeSlot = play_grenade ? "grenade" : play_weapon_key === "usp_silencer" ? "secondary"
		: play_weapon_key === "knife" ? "knife" : "primary";
	for (const slot of ["primary", "secondary", "knife", "grenade"])
		$(`play-slot-${slot}`).classList.toggle("active", slot === activeSlot);
	$("play-label-primary").textContent = weapon_label(primary);
	$("play-label-grenade").textContent = grenadeKey === "hegrenade" ? "HE" : "Smoke";
	set_hud_icon("play-icon-primary", primary);
	set_hud_icon("play-icon-secondary", "usp_silencer");
	set_hud_icon("play-icon-knife", "knife");
	set_hud_icon("play-icon-grenade", grenadeKey);
}

function add_source(a, b)
{
	return {x: a.x + b.x, y: a.y + b.y, z: a.z + b.z};
}

function draw_play_route(route)
{
	clear_group(nav_group);
	if (!route?.length || !play_state) return;
	const points = [source_to_three(play_state.bot.origin), ...route.map(source_to_three)];
	const geometry = new THREE.BufferGeometry().setFromPoints(points);
	const material = new THREE.LineBasicMaterial({color: 0xf3b51b, transparent: true, opacity: 0.9, depthTest: false});
	const line = new THREE.Line(geometry, material);
	line.renderOrder = 13;
	nav_group.add(line);
}

function add_smoke_visual(event)
{
	if (!event.cells?.length) return;
	const buckets = new Map();
	for (let cell = 0; cell < event.cells.length / 4; ++cell)
	{
		const point = {x: event.cells[cell * 4], y: event.cells[cell * 4 + 1], z: event.cells[cell * 4 + 2]};
		const key = `${Math.floor((point.x - event.center.x + 320) / 60)}:${Math.floor((point.y - event.center.y + 320) / 60)}:${Math.floor((point.z - event.center.z + 320) / 60)}`;
		const weight = Math.max(0.1, event.cells[cell * 4 + 3]);
		const bucket = buckets.get(key) || {x: 0, y: 0, z: 0, weight: 0};
		bucket.x += point.x * weight;
		bucket.y += point.y * weight;
		bucket.z += point.z * weight;
		bucket.weight += weight;
		buckets.set(key, bucket);
	}
	const clusters = [...buckets.values()].map((bucket) => ({
		x: bucket.x / bucket.weight,
		y: bucket.y / bucket.weight,
		z: bucket.z / bucket.weight
	}));
	const root = new THREE.Group();
	const frames = particle_textures.smoke_voxel || [];
	const layers = [];
	for (let layer = 0; layer < 3; ++layer)
	{
		const cells = clusters.filter((_, index) => index % 3 === layer);
		const positions = new Float32Array(cells.length * 3);
		const jitter = new Float32Array(cells.length * 3);
		for (let cell = 0; cell < cells.length; ++cell)
		{
			for (let axis = 0; axis < 3; ++axis)
			{
				const noise = Math.sin((cell + 1) * 12.9898 + (layer + 1) * 78.233 + (axis + 1) * 37.719) * 43758.5453;
				jitter[cell * 3 + axis] = (noise - Math.floor(noise) - 0.5) * (14 + layer * 3);
			}
			const point = {
				x: cells[cell].x + jitter[cell * 3],
				y: cells[cell].y + jitter[cell * 3 + 1],
				z: cells[cell].z + jitter[cell * 3 + 2]
			};
			positions.set(source_to_three(point).toArray(), cell * 3);
		}
		const geometry = new THREE.BufferGeometry();
		geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
		const material = new THREE.PointsMaterial({
			map: frames[(layer * 5) % Math.max(1, frames.length)] || null,
			color: [0xd8d2c9, 0xcac4bc, 0xe0d9ce][layer],
			size: (290 + layer * 50) / k_source_units_per_meter,
			transparent: true,
			opacity: 0,
			alphaTest: 0.012,
			depthWrite: false,
			sizeAttenuation: true
		});
		const points = new THREE.Points(geometry, material);
		points.frustumCulled = false;
		points.renderOrder = 5 + layer;
		root.add(points);
		layers.push({points, cells, jitter, frame: -1});
	}
	smoke_group.add(root);
	smoke_visuals.push({mesh: root, layers, center: event.center, startTime: event.startTime, stateKey: ""});
}

function source_point_segment_distance(point, start, end)
{
	const dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
	const lengthSquared = dx * dx + dy * dy + dz * dz;
	const amount = lengthSquared > 0 ? Math.max(0, Math.min(1,
		((point.x - start.x) * dx + (point.y - start.y) * dy + (point.z - start.z) * dz) / lengthSquared)) : 0;
	return Math.hypot(point.x - start.x - dx * amount, point.y - start.y - dy * amount, point.z - start.z - dz * amount);
}

function update_smoke_visuals()
{
	if (!play_state) return;
	const heRadius = Math.min(320, Math.max(0, read_number("he-clear-radius")));
	const heSeconds = Math.min(10, Math.max(0, read_number("he-clear-seconds")));
	for (let index = smoke_visuals.length - 1; index >= 0; --index)
	{
		const visual = smoke_visuals[index];
		const age = play_state.time - visual.startTime;
		if (age >= 22.5)
		{
			smoke_group.remove(visual.mesh);
			clear_group(visual.mesh);
			smoke_visuals.splice(index, 1);
			continue;
		}
		const grow = Math.min(1, Math.max(0, age / 0.7));
		const fade = Math.min(1, Math.max(0, (21.5 - age) / 5));
		const frames = particle_textures.smoke_voxel || [];
		for (let layer = 0; layer < visual.layers.length; ++layer)
		{
			const current = visual.layers[layer];
			current.points.material.opacity = grow * fade;
			const frame = frames.length ? (Math.floor(Math.max(0, age) * 30) + layer * Math.floor(frames.length / 3)) % frames.length : -1;
			if (frame !== current.frame)
			{
				current.points.material.map = frames[frame] || null;
				current.frame = frame;
			}
		}
		const clearances = (play_state.clearances || []).filter((clearance) =>
			clearance.time >= visual.startTime && play_state.time - clearance.time < heSeconds);
		const cuts = (play_state.smokeCuts || []).filter((cut) =>
			cut.time >= visual.startTime && play_state.time - cut.time < 0.8);
		const stateKey = `${Math.floor(grow * 32)}|${heRadius}|${heSeconds}|${clearances.map((clearance) => `${clearance.time}:${clearance.center.x}:${clearance.center.y}:${clearance.center.z}`).join("|")}|${cuts.map((cut) => cut.time).join("|")}`;
		if (stateKey !== visual.stateKey)
		{
			for (let layer = 0; layer < visual.layers.length; ++layer)
			{
				const current = visual.layers[layer];
				const positions = current.points.geometry.getAttribute("position");
				for (let cell = 0; cell < current.cells.length; ++cell)
				{
					const point = current.cells[cell];
					const grown = Math.hypot(point.x - visual.center.x, point.y - visual.center.y, point.z - visual.center.z) <= 24 + grow * 145;
					const cleared = heRadius > 0 && clearances.some((clearance) =>
						Math.hypot(point.x - clearance.center.x, point.y - clearance.center.y, point.z - clearance.center.z) <= heRadius)
						|| cuts.some((cut) => source_point_segment_distance(point, cut.start, cut.end) <= 28);
					const offset = cell * 3;
					const shown = {
						x: point.x + current.jitter[offset],
						y: point.y + current.jitter[offset + 1],
						z: point.z + current.jitter[offset + 2]
					};
					positions.array.set(grown && !cleared ? source_to_three(shown).toArray() : [1e6, 1e6, 1e6], offset);
				}
				positions.needsUpdate = true;
			}
			visual.stateKey = stateKey;
		}
	}
}

function make_explosion_points(count, color, size, speedMinimum, speedMaximum, texture = null)
{
	const positions = new Float32Array(count * 3);
	const velocities = [];
	for (let index = 0; index < count; ++index)
	{
		const yaw = Math.random() * Math.PI * 2;
		const z = Math.random() * 1.4 - 0.2;
		const horizontal = Math.sqrt(Math.max(0, 1 - Math.min(1, z * z)));
		const speed = speedMinimum + Math.random() * (speedMaximum - speedMinimum);
		velocities.push(new THREE.Vector3(Math.cos(yaw) * horizontal * speed, z * speed, Math.sin(yaw) * horizontal * speed));
	}
	const geometry = new THREE.BufferGeometry();
	geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
	const material = new THREE.PointsMaterial({
		color, size, map: texture, alphaTest: texture ? 0.01 : 0, transparent: true, opacity: 1, depthWrite: false,
		blending: THREE.AdditiveBlending, sizeAttenuation: true
	});
	return {points: new THREE.Points(geometry, material), velocities};
}

function spawn_he_explosion(center)
{
	const root = new THREE.Group();
	root.position.copy(source_to_three(center));
	const sparks = make_explosion_points(72, 0xffa45c, 0.11, 2.5, 8.5, particle_textures.he_sparks?.[0] || null);
	const smoke = make_explosion_points(42, 0x8a817a, 0.48, 0.5, 2.1, particle_textures.he_smoke?.[0] || null);
	const flash = new THREE.Sprite(new THREE.SpriteMaterial({
		map: particle_textures.he_flare?.[0] || null,
		color: 0xffc36a, transparent: true, opacity: 0.95,
		blending: THREE.AdditiveBlending, depthWrite: false
	}));
	flash.scale.set(2.2, 2.2, 1);
	const flame = new THREE.Sprite(new THREE.SpriteMaterial({
		map: particle_textures.he_flame?.[0] || null,
		color: 0xffffff, transparent: true, opacity: 1,
		blending: THREE.AdditiveBlending, depthWrite: false
	}));
	flame.scale.set(4.5, 4.5, 1);
	const light = new THREE.PointLight(0xff6a18, 18, 8, 2);
	root.add(sparks.points, smoke.points, flame, flash, light);
	effect_group.add(root);
	explosion_effects.push({root, sparks, smoke, flame, flash, light, age: 0});
}

function update_explosion_effects(delta)
{
	for (let index = explosion_effects.length - 1; index >= 0; --index)
	{
		const effect = explosion_effects[index];
		effect.age += delta;
		for (const set of [effect.sparks, effect.smoke])
		{
			const positions = set.points.geometry.getAttribute("position");
			for (let particle = 0; particle < set.velocities.length; ++particle)
			{
				const velocity = set.velocities[particle];
				positions.array[particle * 3] += velocity.x * delta;
				positions.array[particle * 3 + 1] += velocity.y * delta;
				positions.array[particle * 3 + 2] += velocity.z * delta;
				velocity.y -= (set === effect.sparks ? 8 : 0.7) * delta;
			}
			positions.needsUpdate = true;
		}
		effect.sparks.points.material.opacity = Math.max(0, 1 - effect.age / 0.7);
		effect.smoke.points.material.opacity = Math.max(0, 0.62 * (1 - effect.age / 1.35));
		const flameFrames = particle_textures.he_flame || [];
		if (flameFrames.length)
		{
			effect.flame.material.map = flameFrames[Math.min(flameFrames.length - 1, Math.floor(effect.age / 0.7 * flameFrames.length))];
			effect.flame.material.needsUpdate = true;
		}
		const smokeFrames = particle_textures.he_smoke || [];
		if (smokeFrames.length)
		{
			effect.smoke.points.material.map = smokeFrames[Math.min(smokeFrames.length - 1, Math.floor(effect.age / 1.35 * smokeFrames.length))];
			effect.smoke.points.material.needsUpdate = true;
		}
		effect.flame.material.opacity = Math.max(0, 1 - effect.age / 0.7);
		effect.flame.scale.setScalar(4.5 + effect.age * 4);
		effect.flash.scale.setScalar(1 + effect.age * 8);
		effect.flash.material.opacity = Math.max(0, 0.95 * (1 - effect.age / 0.22));
		effect.light.intensity = Math.max(0, 18 * (1 - effect.age / 0.18));
		if (effect.age < 1.4) continue;
		effect_group.remove(effect.root);
		effect.root.traverse((node) => { node.geometry?.dispose?.(); node.material?.dispose?.(); });
		explosion_effects.splice(index, 1);
	}
}

function spawn_viewmodel_muzzle_flash()
{
	if (!viewmodel_root?.visible) return;
	const flash = new THREE.Sprite(new THREE.SpriteMaterial({
		map: particle_textures.he_flare?.[0] || null,
		color: 0xffcf70, transparent: true, opacity: 0.95,
		blending: THREE.AdditiveBlending, depthWrite: false
	}));
	flash.scale.set(0.12, 0.22, 1);
	flash.position.set(-0.08, -0.015, -0.72);
	viewmodel_camera.add(flash);
	shot_effects.push({age: 0, flash});
}

function spawn_shot_effect(event)
{
	const start = source_to_three(event.start);
	const end = source_to_three(event.end);
	const line = new THREE.Sprite(new THREE.SpriteMaterial({
		map: particle_textures.tracer?.[0] || null,
		color: 0xffd36a, transparent: true, opacity: 0.9,
		blending: THREE.AdditiveBlending, depthWrite: false
	}));
	line.position.copy(start).add(end).multiplyScalar(0.5);
	line.scale.set(0.055, Math.max(0.08, start.distanceTo(end)), 1);
	const projectedStart = start.clone().project(camera);
	const projectedEnd = end.clone().project(camera);
	line.material.rotation = Math.atan2(projectedEnd.y - projectedStart.y, projectedEnd.x - projectedStart.x) - Math.PI / 2;
	line.renderOrder = 20;
	effect_group.add(line);
	shot_effects.push({age: 0, line});
	if (!event.hit) return;
	const normal = new THREE.Vector3(event.normal.y, event.normal.z, event.normal.x).normalize();
	const mark = new THREE.Mesh(
		new THREE.CircleGeometry(2.4 / k_source_units_per_meter, 10),
		new THREE.MeshBasicMaterial({map: particle_textures.bullet_hole?.[0] || null, color: 0xffffff, transparent: true, opacity: 0.9, depthWrite: false, polygonOffset: true, polygonOffsetFactor: -4})
	);
	mark.position.copy(source_to_three(event.end)).addScaledVector(normal, 0.005);
	mark.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), normal);
	mark.renderOrder = 4;
	effect_group.add(mark);
	impact_marks.push({mesh: mark, age: 0});
	while (impact_marks.length > 128)
	{
		const old = impact_marks.shift();
		effect_group.remove(old.mesh);
		old.mesh.geometry.dispose();
		old.mesh.material.dispose();
	}
	play_sound("bullet_impact", event.end, 0.34);
}

function update_shot_effects(delta)
{
	for (let index = casing_effects.length - 1; index >= 0; --index)
	{
		const casing = casing_effects[index];
		casing.age += delta;
		casing.model.position.addScaledVector(casing.velocity, delta);
		casing.velocity.y -= 2.8 * delta;
		casing.model.rotation.x += 12 * delta;
		casing.model.rotation.z += 18 * delta;
		if (casing.age < 1.2) continue;
		viewmodel_camera.remove(casing.model);
		casing_effects.splice(index, 1);
	}
	for (let index = impact_marks.length - 1; index >= 0; --index)
	{
		const impact = impact_marks[index];
		impact.age += delta;
		const fade = impact.age <= 1.5 ? 1 : Math.max(0, (2 - impact.age) / 0.5);
		impact.mesh.material.opacity = 0.9 * fade;
		if (impact.age < 2) continue;
		effect_group.remove(impact.mesh);
		impact.mesh.geometry.dispose();
		impact.mesh.material.dispose();
		impact_marks.splice(index, 1);
	}
	for (let index = shot_effects.length - 1; index >= 0; --index)
	{
		const effect = shot_effects[index];
		effect.age += delta;
		if (effect.line) effect.line.material.opacity = Math.max(0, 0.9 * (1 - effect.age / 0.1));
		if (effect.flash)
		{
			effect.flash.material.opacity = Math.max(0, 0.95 * (1 - effect.age / 0.055));
			effect.flash.scale.setScalar(1 + effect.age * 18);
		}
		if (effect.age < 0.12) continue;
		if (effect.line)
		{
			effect_group.remove(effect.line);
			effect.line.material.dispose();
		}
		if (effect.flash)
		{
			viewmodel_camera.remove(effect.flash);
			effect.flash.geometry.dispose();
			effect.flash.material.dispose();
		}
		shot_effects.splice(index, 1);
	}
}

function update_viewmodel_motion(delta)
{
	if (!viewmodel_root || !play_active || play_third_person || play_scoped) return;
	const speed = play_state?.player?.speed || 0;
	const moving = Math.min(1, speed / Math.max(1, k_weapon_stats[play_weapon_key]?.speed || 250));
	viewmodel_motion_time += delta * (6 + moving * 7);
	const grounded = play_state?.player?.grounded !== false;
	const bob = grounded ? moving : 0.35;
	const baseX = k_viewmodel_offset.x / k_source_units_per_meter;
	const baseY = k_viewmodel_offset.z / k_source_units_per_meter;
	const baseZ = -k_viewmodel_offset.y / k_source_units_per_meter;
	viewmodel_root.position.set(
		baseX + Math.sin(viewmodel_motion_time) * 0.008 * bob,
		baseY + Math.abs(Math.cos(viewmodel_motion_time)) * 0.006 * bob,
		baseZ + Math.sin(viewmodel_motion_time * 0.5) * 0.004 * bob
	);
	viewmodel_root.rotation.set(Math.sin(viewmodel_motion_time) * 0.01 * bob, Math.PI, Math.cos(viewmodel_motion_time) * 0.014 * bob);
}

function draw_grenades(grenades)
{
	const active = new Set();
	for (const grenade of grenades || [])
	{
		active.add(grenade.id);
		let mesh = grenade_visuals.get(grenade.id);
		let created = false;
		if (!mesh)
		{
			const template = grenade_templates[grenade.kind];
			if (template)
			{
				mesh = clone_skeleton(template);
				mesh.traverse((node) =>
				{
					if (node.geometry) node.geometry = node.geometry.clone();
					if (node.material) node.material = Array.isArray(node.material) ? node.material.map((value) => value.clone()) : node.material.clone();
				});
			}
			else
			{
				const geometry = new THREE.SphereGeometry(2.5 / k_source_units_per_meter, 8, 6);
				const material = new THREE.MeshBasicMaterial({color: grenade.kind === "smoke" ? 0x75a56c : 0xe5962d});
				mesh = new THREE.Mesh(geometry, material);
			}
			grenade_group.add(mesh);
			grenade_visuals.set(grenade.id, mesh);
			created = true;
		}
		const destination = source_to_three(grenade.origin);
		if (created) mesh.position.copy(destination);
		else mesh.userData.positionInterpolation = {from: mesh.position.clone(), to: destination, elapsed: 0};
	}
	for (const [id, mesh] of grenade_visuals)
	{
		if (active.has(id)) continue;
		grenade_group.remove(mesh);
		mesh.traverse((node) =>
		{
			node.geometry?.dispose?.();
			if (Array.isArray(node.material)) node.material.forEach((material) => material.dispose?.());
			else node.material?.dispose?.();
		});
		grenade_visuals.delete(id);
	}
}

function update_grenade_interpolation(delta)
{
	for (const mesh of grenade_visuals.values())
	{
		const interpolation = mesh.userData.positionInterpolation;
		if (interpolation)
		{
			interpolation.elapsed += delta;
			const amount = Math.min(1, interpolation.elapsed / FPS_DT);
			mesh.position.lerpVectors(interpolation.from, interpolation.to, amount);
			if (amount >= 1) delete mesh.userData.positionInterpolation;
		}
		mesh.rotation.x += delta * 9;
	}
}

function clear_grenade_visuals()
{
	clear_group(grenade_group);
	grenade_visuals.clear();
}

function locomotion_animation_id(actor)
{
	if (actor.speed <= 8) return actor.crouched ? "crouch_idle" : "idle";
	const yaw = actor.yaw * Math.PI / 180;
	const forward = actor.velocity.x * Math.cos(yaw) + actor.velocity.y * Math.sin(yaw);
	const side = actor.velocity.x * Math.sin(yaw) - actor.velocity.y * Math.cos(yaw);
	const directions = ["forward", "forward_right", "right", "backward_right", "backward", "backward_left", "left", "forward_left"];
	const angle = (Math.atan2(side, forward) + Math.PI * 2) % (Math.PI * 2);
	const direction = directions[Math.round(angle / (Math.PI / 4)) % 8];
	if (actor.crouched) return actor.speed > 8 ? `crouch_${direction}` : "crouch_idle";
	if (!actor.grounded) return `jump_${direction}`;
	return `${actor.speed > 130 ? "run" : "walk"}_${direction}`;
}

function choose_bot_animation(actor, mixer = model_mixer)
{
	if (!mixer || !actor) return;
	const id = locomotion_animation_id(actor);
	const weapon = $("bot-weapon-select").value;
	const choice = (k_animation_sets[weapon] || k_animation_sets.default).find((entry) => entry.id === id);
	if (choice && mixer.cs2fow_play_name !== choice.clip)
	{
		set_mixer_animation(mixer, model_animations, choice.clip, true);
		mixer.cs2fow_play_name = choice.clip;
	}
}

function masked_world_clip(clip, upper)
{
	let cached = masked_clip_cache.get(clip);
	if (!cached)
	{
		const isLowerBody = (track) =>
		{
			const target = track.name.slice(0, track.name.lastIndexOf("."));
			return target === "root_motion" || target === "pelvis" || /^(leg_|ankle_|ball_)/.test(target) || target.includes(".vmdl_c");
		};
		cached = {
			lower: new THREE.AnimationClip(`${clip.name}:lower`, clip.duration, clip.tracks.filter(isLowerBody)),
			upper: new THREE.AnimationClip(`${clip.name}:upper`, clip.duration, clip.tracks.filter((track) => !isLowerBody(track)))
		};
		masked_clip_cache.set(clip, cached);
	}
	return upper ? cached.upper : cached.lower;
}

function set_player_base_clip(clip, name)
{
	const previous = viewer_mixer.cs2fow_action;
	const next = viewer_mixer.clipAction(clip);
	if (previous !== next)
	{
		next.reset().play();
		if (previous) previous.crossFadeTo(next, k_animation_transition_seconds, false);
		viewer_mixer.cs2fow_action = next;
	}
	viewer_mixer.cs2fow_play_name = name;
}

function choose_player_animation(actor)
{
	if (!viewer_mixer || !actor) return;
	const id = locomotion_animation_id(actor);
	const choice = (k_animation_sets[play_grenade ? "grenade" : play_weapon_key] || k_animation_sets.default).find((entry) => entry.id === id);
	if (!choice) return;
	const clip = THREE.AnimationClip.findByName(viewer_animations, choice.clip);
	if (!clip) return;
	if (play_now() < player_world_action_until)
	{
		const name = `${choice.clip}:lower`;
		if (viewer_mixer.cs2fow_play_name !== name) set_player_base_clip(masked_world_clip(clip, false), name);
		return;
	}
	if (viewer_mixer.cs2fow_overlay)
	{
		viewer_mixer.cs2fow_overlay.fadeOut(k_animation_transition_seconds);
		viewer_mixer.cs2fow_overlay = null;
	}
	if (viewer_mixer.cs2fow_play_name !== choice.clip)
	{
		set_mixer_animation(viewer_mixer, viewer_animations, choice.clip, true);
		viewer_mixer.cs2fow_play_name = choice.clip;
	}
}

function queue_play_pose_interpolation(state)
{
	const bots = state.bots?.length ? state.bots : [state.bot];
	play_pose_from = [{...viewer_pose}, {...target_pose}, ...extra_bot_render_poses.map((pose) => ({...pose}))];
	play_pose_to = [actor_pose(state.player), actor_pose(state.bot), ...bots.slice(1).map(actor_pose)];
	play_pose_elapsed = 0;
}

function update_play_pose_interpolation(delta)
{
	if (!play_pose_from || !play_pose_to) return;
	play_pose_elapsed += delta;
	const amount = Math.min(1, play_pose_elapsed / FPS_DT);
	// Mouse look stays immediate; only the local player's networked position is delayed by one tick.
	interpolate_pose(viewer_pose, play_pose_from[0], play_pose_to[0], amount, false);
	interpolate_pose(target_pose, play_pose_from[1], play_pose_to[1], amount);
	for (let index = 0; index < extra_bot_render_poses.length; ++index)
	{
		if (!play_pose_from[index + 2] || !play_pose_to[index + 2]) continue;
		const pose = extra_bot_render_poses[index];
		interpolate_pose(pose, play_pose_from[index + 2], play_pose_to[index + 2], amount);
		const bot = extra_bot_models[index];
		if (!bot) continue;
		bot.position.copy(source_to_three(pose));
		bot.rotation.set(0, degrees_to_radians(pose.yaw), 0);
	}
	apply_player_transforms();
	const routePositions = nav_group?.children[0]?.geometry?.getAttribute("position");
	if (routePositions?.count)
	{
		routePositions.array.set(source_to_three(target_pose).toArray(), 0);
		routePositions.needsUpdate = true;
	}
}

function handle_play_state(state)
{
	if (!play_active) return;
	play_state = state;
	queue_play_pose_interpolation(state);
	run_play_schedule(state.time);
	const traversal = play_debug ? merge_bot_traversals(state.visibilities || [state.visibility]) : null;
	if (traversal) draw_map_traversal(traversal);
	const now = performance.now() / 1000;
	if (play_debug && now - last_play_traversal_request >= k_bvh_snapshot_interval)
	{
		last_play_traversal_request = now;
		map_worker.postMessage({type: "play-traversal"});
	}
	for (let index = 0; index < extra_bot_models.length; ++index)
	{
		const actor = state.bots?.[index + 1];
		if (actor) choose_bot_animation(actor, extra_bot_mixers[index]);
	}
	play_eye_height_target = state.player.crouched ? 28.5 : 64;
	choose_bot_animation(state.bot);
	choose_player_animation(state.player);
	const drawDebug = play_debug && state.time - last_play_debug_draw >= k_debug_draw_interval;
	if (drawDebug)
	{
		last_play_debug_draw = state.time;
		draw_map_trace(state.visibility.origins, state.visibility.rays, state.visibility.blocked,
			state.visibility.visible, state.visibility.clearCount);
		draw_extra_bot_debug((state.visibilities || []).slice(1));
	}
	else
	{
		remember_trace_result(state.visibility.rays, state.visibility.visible, state.visibility.clearCount);
	}
	for (const event of state.events || [])
	{
		if (event.type === "smoke-created")
		{
			add_smoke_visual(event);
			play_sound("smoke_emit", event.center, 0.52);
		}
		else if (event.type === "he-detonated")
		{
			spawn_he_explosion(event.center);
			play_sound("he_detonate", event.center, 0.8);
		}
		else if (event.type === "grenade-bounce") play_sound(event.kind === "smoke" ? "smoke_bounce" : "he_bounce", event.center, 0.42);
		else if (event.type === "shot") spawn_shot_effect(event);
	}
	const playerStepSpeed = state.player.stance === "ladder" ? Math.abs(state.player.velocity.z) : state.player.speed;
	const playerStepGap = Math.max(0.24, 72 / Math.max(1, playerStepSpeed));
	if (state.player.stance !== "walking" && (state.player.grounded || state.player.stance === "ladder") && playerStepSpeed > 55 && state.time - last_player_step >= playerStepGap)
	{
		last_player_step = state.time;
		play_sound(surface_sound(state.player.surface, "ct"), null, 0.38);
	}
	for (let index = 0; index < (state.bots || [state.bot]).length; ++index)
	{
		const bot = (state.bots || [state.bot])[index];
		const speed = bot.stance === "ladder" ? Math.abs(bot.velocity.z) : bot.speed;
		const gap = Math.max(0.24, 72 / Math.max(1, speed));
		if (bot.stance !== "walking" && (bot.grounded || bot.stance === "ladder") && speed > 55 && state.time - last_bot_steps[index] >= gap)
		{
			last_bot_steps[index] = state.time;
			play_sound(surface_sound(bot.surface, "t"), bot.origin, 0.48);
		}
	}
	draw_grenades(state.grenades);
	if (drawDebug)
	{
		draw_play_route(state.route);
	}
	update_smoke_visuals();
	const visibilities = state.visibilities || [state.visibility];
	const sampledPixels = visibilities.reduce((total, visibility) => total + visibility.sampledPixels, 0);
	const tracedRays = visibilities.reduce((total, visibility) => total + visibility.tracedRays, 0);
	$("play-rays").textContent = `${sampledPixels}/${tracedRays}`;
	const hidden = visibilities.filter((visibility) => !visibility.visible);
	$("play-blocker").textContent = visibilities.every((visibility) => visibility.visible) ? "clear"
		: hidden.some((visibility) => visibility.wallBlocked) && hidden.some((visibility) => visibility.smokeBlocked)
			? "wall + smoke" : hidden.some((visibility) => visibility.smokeBlocked) ? "smoke" : "wall";
	$("play-smokes").textContent = state.smokeCount;
	$("play-hes").textContent = state.heCount;
	const visibleBots = visibilities.filter((visibility) => visibility.visible).length;
	$("play-bot-count").textContent = `${visibleBots}/${state.bots?.length || 1} bots visible`;
	update_play_visibility();
}

async function load_play_nav()
{
	if (!map_metadata) return null;
	try
	{
		const response = await fetch(`local_assets/maps/${map_metadata.mapName}.nav.json`, {cache: "no-store"});
		if (!response.ok) throw new Error(String(response.status));
		return await response.json();
	}
	catch
	{
		return null;
	}
}

async function prepare_preview_bots()
{
	if (!map_metadata || !target_pose.placed) return;
	const nav = await load_play_nav();
	const areas = nav?.areas || [];
	const used = [target_pose];
	for (let index = 0; index < extra_target_poses.length; ++index)
	{
		const preferred = nav?.objectives?.[index === 0 ? "a" : "b"];
		const candidates = areas.map((area) => area.center).filter(Boolean);
		let point = preferred;
		if (!point || used.some((other) => Math.hypot(point[0] - other.x, point[1] - other.y) < 1000))
		{
			point = candidates.reduce((best, candidate) =>
			{
				const distance = Math.min(...used.map((other) => Math.hypot(candidate[0] - other.x, candidate[1] - other.y)));
				return !best || distance > best.distance ? {point: candidate, distance} : best;
			}, null)?.point;
		}
		if (!point) point = [target_pose.x + (index ? -1200 : 1200), target_pose.y, target_pose.z];
		const pose = extra_target_poses[index];
		Object.assign(pose, {x: point[0], y: point[1], z: point[2], placed: true});
		pose.yaw = Math.atan2(viewer_pose.y - pose.y, viewer_pose.x - pose.x) * 180 / Math.PI;
		used.push(pose);
	}
	create_extra_bots();
	request_map_trace();
}

async function enter_play_mode()
{
	if (!play_ready() || play_active) return;
	const session = ++play_session_id;
	const primaryKey = $("player-primary-select").value;
	const botKey = $("bot-weapon-select").value;
	try
	{
		const navPromise = load_play_nav();
		const loads = await Promise.allSettled([
			bot_weapon_key === botKey ? Promise.resolve() : load_bot_weapon(botKey),
			load_viewmodel_weapon(primaryKey)
		]);
		const nav = await navPromise;
		if (session !== play_session_id || !play_ready()) return;
		const loadError = loads.find((result) => result.status === "rejected");
		if (loadError) status_extra = `Play visual asset unavailable: ${loadError.reason?.message || loadError.reason}`;

		play_active = true;
		play_paused = true;
		play_debug = false;
		last_play_debug_draw = -Infinity;
		last_play_traversal_request = -Infinity;
		debug_trace_smoothing = null;
		extra_debug_smoothing.length = 0;
		play_pose_from = null;
		play_pose_to = null;
		play_pose_elapsed = 0;
		for (let index = 0; index < extra_bot_render_poses.length; ++index)
			Object.assign(extra_bot_render_poses[index], extra_target_poses[index]);
		clear_map_traversal();
		$("play-bvh").textContent = "BVH traversal appears in Debug mode.";
		update_map_material();
		play_third_person = false;
		play_scoped = false;
		play_eye_height = 64;
		play_eye_height_target = 64;
		camera.fov = k_world_fov;
		update_camera_projection();
		play_weapon_key = primaryKey;
		play_firing = false;
		play_grenade_holding = false;
		play_next_fire_time = 0;
		play_busy_until = 0;
		player_world_action_until = 0;
		++play_action_serial;
		play_scheduled.length = 0;
		last_player_step = 0;
		last_bot_steps.fill(0);
		clear_play_input();
		for (const [key, stats] of Object.entries(k_weapon_stats))
		{
			if (stats.clip !== null) play_ammo[key] = stats.clip;
		}
		runtime_animation_enabled = true;
		play_nav = nav;
		orbit.enabled = false;
		transform.detach();
		if (viewer_model) viewer_model.visible = false;
		if (weapon_mount) weapon_mount.visible = false;
		if (bot_weapon_mount) bot_weapon_mount.visible = true;
		create_extra_bots();
		if (viewmodel_root) viewmodel_root.visible = true;
		play_viewmodel_action("draw");
		play_world_action("draw");
		play_sound(weapon_draw_sound(play_weapon_key), null, 0.42);
		$("play-hud").hidden = false;
		update_play_item();
		$("play-paused").hidden = false;
		const tuning = runtime_tuning();
		map_worker.postMessage({type: "play-start", settings: {
			viewer: {...viewer_pose},
			target: {...target_pose},
			extraTargets: extra_target_poses.map((pose) => ({...pose})),
			pingMs: read_number("viewer-ping"),
			tuning,
			heRadius: read_number("he-clear-radius"),
			heSeconds: read_number("he-clear-seconds"),
			seed: read_number("simulation-seed"),
			botMuzzleLength: weapon_muzzle_length(botKey),
			playerSpeed: k_weapon_stats[play_weapon_key].speed,
			botSpeed: k_weapon_stats[botKey]?.speed || 225,
			nav: play_nav
		}, paused: true});
		status_extra = play_nav ? "Play mode started with CS2 navigation." : "Play mode started with BVH fallback roaming.";
		update_status();
		await request_play_pointer_lock();
	}
	catch (error)
	{
		if (session !== play_session_id) return;
		leave_play_mode();
		status_extra = `Play could not start: ${error.message || error}`;
		update_status();
	}
}

function leave_play_mode()
{
	++play_session_id;
	if (!play_active) return;
	if (play_state?.bots)
		for (let index = 0; index < extra_target_poses.length; ++index)
			Object.assign(extra_target_poses[index], play_state.bots[index + 1]?.origin || extra_target_poses[index],
				{yaw: play_state.bots[index + 1]?.yaw ?? extra_target_poses[index].yaw, placed: true});
	map_worker.postMessage({type: "play-stop"});
	play_active = false;
	debug_trace_smoothing = null;
	extra_debug_smoothing.length = 0;
	play_pose_from = null;
	play_pose_to = null;
	play_pose_elapsed = 0;
	for (let index = 0; index < extra_bot_render_poses.length; ++index)
		Object.assign(extra_bot_render_poses[index], extra_target_poses[index]);
	clear_map_traversal();
	update_map_material();
	play_paused = true;
	play_firing = false;
	play_look_dirty = false;
	clear_play_input();
	play_grenade_holding = false;
	play_third_person = false;
	play_scoped = false;
	player_world_action_until = 0;
	camera.fov = k_world_fov;
	update_camera_projection();
	++play_action_serial;
	play_scheduled.length = 0;
	play_state = null;
	if (play_pointer_locked()) document.exitPointerLock();
	document.body.classList.remove("play-locked");
	$("play-hud").hidden = true;
	if (weapon_mount) weapon_mount.visible = true;
	if (bot_weapon_mount) bot_weapon_mount.visible = false;
	create_extra_bots();
	if (viewmodel_root) viewmodel_root.visible = false;
	clear_player_weapon();
	orbit.enabled = true;
	clear_group(nav_group);
	clear_grenade_visuals();
	clear_group(smoke_group);
	smoke_visuals.splice(0);
	clear_group(effect_group);
	explosion_effects.splice(0);
	impact_marks.splice(0);
	for (const effect of shot_effects)
	{
		if (!effect.flash) continue;
		viewmodel_camera.remove(effect.flash);
		effect.flash.geometry.dispose();
		effect.flash.material.dispose();
	}
	shot_effects.splice(0);
	for (const casing of casing_effects) viewmodel_camera.remove(casing.model);
	casing_effects.splice(0);
	write_pose_fields("viewer", viewer_pose);
	write_pose_fields("target", target_pose);
	set_model_opacity();
	update_scene();
}

function set_runtime_mode(preview)
{
	leave_play_mode();
	if (preview && !capture_runtime_body_bindings())
	{
		status_extra = points.length === k_runtime_body_bones.length
			? "Runtime preview unavailable: the loaded model is missing a required bone."
			: "Runtime preview requires exactly 15 body points.";
		update_status();
		return;
	}
	runtime_animation_enabled = preview;
	if (preview)
	{
		play_selected_animation();
	}
	else
	{
		show_standing_pose();
	}
	status_extra = preview ? "Runtime bone preview is active." : "Static point editing is active.";
	update_scene();
}

function apply_readable_materials(root, releaseSource = false)
{
	root.traverse((node) =>
	{
		if (!node.isMesh)
		{
			return;
		}
		if (releaseSource)
		{
			const materials = Array.isArray(node.material) ? node.material : [node.material];
			for (const material of materials)
			{
				for (const value of Object.values(material || {})) if (value?.isTexture) value.dispose();
				material?.dispose?.();
			}
		}
		node.material = new THREE.MeshStandardMaterial({
			color: 0x747a81,
			roughness: 0.72,
			metalness: 0.0
		});
		node.castShadow = true;
		node.receiveShadow = true;
	});
}

function apply_viewmodel_materials(root)
{
	root.traverse((node) =>
	{
		if (!node.isMesh) return;
		const make_material = (source) =>
		{
			const material = source?.clone?.() || new THREE.MeshBasicMaterial({color: 0xe9edf2});
			material.side = THREE.FrontSide;
			material.transparent = false;
			material.opacity = 1;
			material.alphaTest = 0;
			material.depthTest = true;
			material.depthWrite = true;
			material.toneMapped = false;
			material.needsUpdate = true;
			return material;
		};
		node.material = Array.isArray(node.material) ? node.material.map(make_material) : make_material(node.material);
		node.frustumCulled = false;
		node.renderOrder = 100;
	});
}

async function load_particle_art(resources)
{
	const textureLoader = new THREE.TextureLoader();
	await Promise.all(Object.entries(resources || {}).map(async ([key, paths]) =>
	{
		const textures = await Promise.all((paths || []).map((path) => textureLoader.loadAsync(path)));
		for (const texture of textures)
		{
			texture.colorSpace = THREE.SRGBColorSpace;
			texture.needsUpdate = true;
		}
		particle_textures[key] = textures;
	}));
}

async function load_material_art(resources)
{
	const textureLoader = new THREE.TextureLoader();
	await Promise.all(Object.entries(resources || {}).map(async ([key, path]) =>
	{
		const texture = await textureLoader.loadAsync(path);
		texture.colorSpace = THREE.SRGBColorSpace;
		texture.flipY = false;
		texture.needsUpdate = true;
		material_textures[key] = texture;
	}));
}

function apply_ct_sas_arms(root)
{
	const sleeve = material_textures.ct_sas_sleeve;
	if (!sleeve) return;
	root.traverse((node) =>
	{
		if (!node.isMesh) return;
		const materials = Array.isArray(node.material) ? node.material : [node.material];
		for (const material of materials)
		{
			if (!material.name?.toLowerCase().includes("bare_arm")) continue;
			material.map = sleeve;
			material.needsUpdate = true;
		}
	});
}

function hand_parent()
{
	return model?.getObjectByName("hand_R") || model;
}

function clear_weapon()
{
	++weapon_load_id;
	weapon_mount?.parent?.remove(weapon_mount);
	dispose_root(weapon_model, true, true, true);
	weapon_mount = null;
	weapon_model = null;
}

function update_weapon_transform()
{
	if (!weapon_mount)
	{
		return;
	}
	weapon_mount.position.copy(source_to_three({
		x: read_number("weapon-x"),
		y: read_number("weapon-y"),
		z: read_number("weapon-z")
	}));
	weapon_mount.rotation.set(
		degrees_to_radians(read_number("weapon-rx")),
		degrees_to_radians(read_number("weapon-ry")),
		degrees_to_radians(read_number("weapon-rz"))
	);
	const scale = Math.max(0.01, read_number("weapon-scale"));
	weapon_mount.scale.setScalar(scale);
	draw_muzzle_point();
	draw_runtime_rays();
}

function update_viewmodel_weapon_transform(key)
{
	const offset = viewmodel_weapon_offsets[key];
	if (!offset) return;
	for (const axis of ["x", "y", "z"])
	{
		const value = read_number(`viewmodel-weapon-${axis}`);
		offset[axis] = value;
	}
	viewmodel_weapon_mount?.position.copy(source_to_three(offset));
}

function show_viewmodel_weapon_transform(key)
{
	const offset = viewmodel_weapon_offsets[key] || {x: 0, y: 0, z: 0};
	for (const axis of ["x", "y", "z"]) $( `viewmodel-weapon-${axis}`).value = offset[axis];
	viewmodel_weapon_mount?.position.copy(source_to_three(offset));
}

function apply_weapon_grip(key)
{
	const grip = k_weapon_grips[key];
	if (!grip)
	{
		return;
	}
	$("weapon-x").value = grip.x;
	$("weapon-y").value = grip.y;
	$("weapon-z").value = grip.z;
	$("weapon-rx").value = grip.rx;
	$("weapon-ry").value = grip.ry;
	$("weapon-rz").value = grip.rz;
	$("weapon-scale").value = grip.scale;
	update_weapon_transform();
	update_range_labels();
}

async function load_weapon(key)
{
	clear_weapon();
	const loadId = weapon_load_id;
	active_weapon_key = key;
	if (!play_active)
		load_viewmodel_weapon(key).catch((error) => { status_extra = `Viewmodel unavailable: ${error.message}`; update_status(); });
	refresh_animation_choices();
	if (runtime_animation_enabled)
	{
		play_selected_animation();
	}
	if (!key)
	{
		status_extra = model ? "Weapon preview cleared." : "No local SAS model loaded.";
		draw_muzzle_point();
		draw_runtime_rays();
		update_status();
		return;
	}
	const url = manifest_models[key];
	if (!url || !model)
	{
		status_extra = `Weapon not available: ${key}`;
		draw_muzzle_point();
		update_status();
		return;
	}
	try
	{
		await new Promise((resolve, reject) =>
		{
			loader.load(url, (gltf) =>
			{
				if (loadId !== weapon_load_id || active_weapon_key !== key)
				{
					dispose_root(gltf.scene, true, true, true);
					resolve();
					return;
				}
				weapon_model = gltf.scene;
				weapon_model.updateWorldMatrix(true, true);
				const grip = weapon_model.getObjectByName("ag1_hand_r");
				if (!grip)
				{
					reject(new Error(`${key} has no right-hand grip`));
					return;
				}
				grip.updateWorldMatrix(true, false);
				weapon_model.applyMatrix4(grip.matrixWorld.clone().invert());
				weapon_mount = new THREE.Group();
				hand_parent().add(weapon_mount);
				weapon_mount.add(weapon_model);
				apply_weapon_grip(key);
				update_weapon_transform();
				status_extra = `Weapon loaded: ${key}`;
				update_status();
				resolve();
			}, undefined, reject);
		});
	}
	catch (error)
	{
		clear_weapon();
		status_extra = `Weapon could not load: ${error.message}`;
		draw_muzzle_point();
		draw_runtime_rays();
		update_status();
	}
}

function clear_bot_weapon()
{
	++bot_weapon_load_id;
	bot_weapon_mount?.parent?.remove(bot_weapon_mount);
	dispose_root(bot_weapon_model, true, true, true);
	bot_weapon_mount = null;
	bot_weapon_model = null;
	bot_weapon_key = "";
}

function clear_extra_bots()
{
	for (const mixer of extra_bot_mixers) mixer.stopAllAction();
	for (const bot of extra_bot_models)
	{
		scene.remove(bot);
		dispose_root(bot, false, true);
	}
	extra_bot_models.length = 0;
	extra_bot_mixers.length = 0;
	extra_bot_body_bindings.length = 0;
	extra_bot_capsule_bindings.length = 0;
	for (const group of extra_bot_capsule_groups) clear_group(group);
}

function create_extra_bots()
{
	clear_extra_bots();
	if (!model || !map_metadata || extra_target_poses.some((pose) => !pose.placed)) return;
	const weaponWasVisible = bot_weapon_mount?.visible;
	if (bot_weapon_mount) bot_weapon_mount.visible = true;
	for (let index = 0; index < 2; ++index)
	{
		const bot = clone_skeleton(model);
		bot.traverse((node) =>
		{
			if (!node.material) return;
			node.material = Array.isArray(node.material) ? node.material.map((material) => material.clone()) : node.material.clone();
		});
		bot.position.copy(source_to_three(extra_target_poses[index]));
		bot.rotation.set(0, degrees_to_radians(extra_target_poses[index].yaw), 0);
		bot.visible = true;
		scene.add(bot);
		extra_bot_models.push(bot);
		const bindings = runtime_body_bindings.map((binding) =>
		{
			const bone = bot.getObjectByName(binding.bone.name);
			return bone ? {bone, offset: binding.offset.clone(), position: new THREE.Vector3()} : null;
		});
		extra_bot_body_bindings.push(bindings.length === k_runtime_body_bones.length && bindings.every(Boolean) ? bindings : []);
		const capsuleBindings = capsule_bindings_for(bot);
		extra_bot_capsule_bindings.push(capsuleBindings);
		for (const binding of capsuleBindings) extra_bot_capsule_groups[index].add(make_capsule_visual(binding));
		const mixer = new THREE.AnimationMixer(bot);
		extra_bot_mixers.push(mixer);
		choose_bot_animation({velocity: {x: 0, y: 0, z: 0}, speed: 0, grounded: true, crouched: false, yaw: extra_target_poses[index].yaw}, mixer);
	}
	if (bot_weapon_mount) bot_weapon_mount.visible = weaponWasVisible;
	update_hitbox_capsules();
}

async function load_bot_weapon(key)
{
	clear_extra_bots();
	clear_bot_weapon();
	const loadId = bot_weapon_load_id;
	const url = manifest_models[key];
	if (!url || !model || !key)
	{
		create_extra_bots();
		return;
	}
	let gltf;
	try
	{
		gltf = await loader.loadAsync(url);
	}
	catch (error)
	{
		if (loadId === bot_weapon_load_id) create_extra_bots();
		throw error;
	}
	if (loadId !== bot_weapon_load_id || $("bot-weapon-select").value !== key || !model)
	{
		dispose_root(gltf.scene, true, true, true);
		return;
	}
	const grip = gltf.scene.getObjectByName("ag1_hand_r");
	if (!grip)
	{
		dispose_root(gltf.scene, true, true, true);
		create_extra_bots();
		throw new Error(`${key} has no right-hand grip`);
	}
	bot_weapon_key = key;
	bot_weapon_model = gltf.scene;
	bot_weapon_model.updateWorldMatrix(true, true);
	grip.updateWorldMatrix(true, false);
	bot_weapon_model.applyMatrix4(grip.matrixWorld.clone().invert());
	bot_weapon_mount = new THREE.Group();
	(model.getObjectByName("hand_R") || model).add(bot_weapon_mount);
	bot_weapon_mount.add(bot_weapon_model);
	bot_weapon_mount.rotation.set(Math.PI, Math.PI, 0);
	create_extra_bots();
	draw_muzzle_point();
}

function clear_player_weapon()
{
	++player_weapon_load_id;
	player_weapon_mount?.parent?.remove(player_weapon_mount);
	player_weapon_mount = null;
	player_weapon_model = null;
}

async function load_player_weapon(key)
{
	clear_player_weapon();
	const loadId = player_weapon_load_id;
	if (!play_third_person) return;
	if (!viewer_model || !key || !manifest_models[key]) return;
	const asset = await load_viewmodel_asset(key);
	const heldKey = play_grenade === "smoke" ? "smokegrenade" : play_grenade === "he" ? "hegrenade" : play_weapon_key;
	if (loadId !== player_weapon_load_id || !play_active || heldKey !== key) return;
	player_weapon_model = clone_skeleton(asset.scene);
	player_weapon_model.updateWorldMatrix(true, true);
	const grip = player_weapon_model.getObjectByName("ag1_hand_r");
	if (grip)
	{
		grip.updateWorldMatrix(true, false);
		player_weapon_model.applyMatrix4(grip.matrixWorld.clone().invert());
	}
	player_weapon_mount = new THREE.Group();
	(viewer_model.getObjectByName("hand_R") || viewer_model).add(player_weapon_mount);
	player_weapon_mount.add(player_weapon_model);
	player_weapon_mount.rotation.set(Math.PI, Math.PI, 0);
	player_weapon_mount.visible = play_third_person;
}

function load_viewmodel_asset(key)
{
	if (!viewmodel_asset_cache.has(key))
	{
		const promise = (async () =>
		{
			const animationEntries = Object.entries(local_manifest.animations?.[key] || {});
			const gltf = await loader.loadAsync(manifest_models[key]);
			const clipAssets = await Promise.allSettled(animationEntries.map(([, url]) => loader.loadAsync(url)));
			const animations = {};
			for (let index = 0; index < animationEntries.length; ++index)
			{
				const clip = clipAssets[index].status === "fulfilled" ? clipAssets[index].value.animations?.[0] : null;
				if (clip) animations[animationEntries[index][0]] = clip;
			}
			return {scene: gltf.scene, animations};
		})();
		viewmodel_asset_cache.set(key, promise);
		promise.catch(() => { if (viewmodel_asset_cache.get(key) === promise) viewmodel_asset_cache.delete(key); });
	}
	return viewmodel_asset_cache.get(key);
}

async function load_viewmodel_weapon(key)
{
	if (!viewmodel_arms) return;
	const loadId = ++viewmodel_load_id;
	viewmodel_mixer?.stopAllAction();
	viewmodel_weapon_mount?.parent?.remove(viewmodel_weapon_mount);
	dispose_root(viewmodel_weapon, false, true);
	viewmodel_weapon_mount = null;
	viewmodel_weapon = null;
	viewmodel_animations = {};
	if (!key || !manifest_models[key]) return;
	const asset = await load_viewmodel_asset(key);
	if (loadId !== viewmodel_load_id) return;
	viewmodel_weapon = clone_skeleton(asset.scene);
	remove_duplicate_viewmodel_body(viewmodel_weapon);
	for (const [name, source] of Object.entries(asset.animations))
	{
		const clip = source.clone();
		if (clip)
		{
			clip.name = name;
			viewmodel_animations[clip.name] = clip;
		}
	}
	viewmodel_weapon.updateWorldMatrix(true, true);
	const weaponBone = viewmodel_weapon.getObjectByName("weapon");
	if (weaponBone)
	{
		weaponBone.updateWorldMatrix(true, false);
		viewmodel_weapon.applyMatrix4(weaponBone.matrixWorld.clone().invert());
	}
	const mount = viewmodel_arms.getObjectByName("wpn") || viewmodel_arms.getObjectByName("hand_R") || viewmodel_root;
	viewmodel_weapon_mount = new THREE.Group();
	mount.add(viewmodel_weapon_mount);
	viewmodel_weapon_mount.add(viewmodel_weapon);
	show_viewmodel_weapon_transform(key);
	apply_viewmodel_materials(viewmodel_weapon);
	const targets = new Set();
	viewmodel_root.traverse((node) => { if (node.name) targets.add(node.name); });
	for (const clip of Object.values(viewmodel_animations))
	{
		clip.tracks = clip.tracks.filter((track) => targets.has(track.name.slice(0, track.name.lastIndexOf("."))));
	}
	viewmodel_mixer = new THREE.AnimationMixer(viewmodel_root);
	viewmodel_mixer.addEventListener("finished", () => play_viewmodel_action("idle"));
	play_viewmodel_action(play_active ? "draw" : "idle");
}

function is_hd_viewmodel_body(name)
{
	return typeof name === "string" && name.toLowerCase().endsWith(".body_hd");
}

function remove_duplicate_viewmodel_body(root)
{
	let hasLegacyBody = false;
	const hdBodies = [];
	root.traverse((node) =>
	{
		const name = node.name?.toLowerCase() || "";
		if (name.endsWith(".body_legacy")) hasLegacyBody = true;
		else if (is_hd_viewmodel_body(name)) hdBodies.push(node);
	});
	if (hasLegacyBody)
		for (const body of hdBodies) body.parent?.remove(body);
}

async function sound_buffer(key)
{
	const available = local_manifest.sounds?.[key];
	const urls = Array.isArray(available) ? available : available ? [available] : [];
	if (!urls.length) return null;
	let index = 0;
	if (urls.length > 1)
	{
		const previous = last_sound_choices.get(key) ?? -1;
		index = (previous + 1 + Math.floor(Math.random() * (urls.length - 1))) % urls.length;
		last_sound_choices.set(key, index);
	}
	const url = urls[index];
	if (!audio_context) audio_context = new AudioContext();
	if (audio_context.state === "suspended") await audio_context.resume();
	if (!audio_buffers.has(url))
	{
		audio_buffers.set(url, fetch(url).then((response) =>
		{
			if (!response.ok) throw new Error(`${key}: ${response.status}`);
			return response.arrayBuffer();
		}).then((bytes) => audio_context.decodeAudioData(bytes)));
	}
	try { return await audio_buffers.get(url); }
	catch { audio_buffers.delete(url); return null; }
}

function play_sound_sequence(entries, serial = play_action_serial)
{
	for (const [delay, key, volume = 0.45] of entries)
	{
		schedule_play(delay, () => play_sound(key, null, volume), serial);
	}
}

function play_reload_sounds(key, serial)
{
	play_sound_sequence(k_reload_sound_sequences[key] || [], serial);
}

async function play_sound(key, sourcePosition = null, volume = 0.6)
{
	let spatial = null;
	if (sourcePosition && play_state)
	{
		const offset = {x: sourcePosition.x - play_state.player.origin.x, y: sourcePosition.y - play_state.player.origin.y};
		const distance = Math.hypot(offset.x, offset.y);
		if (distance >= 1250) return;
		spatial = {offset, distance};
	}
	const buffer = await sound_buffer(key);
	if (!buffer || !audio_context) return;
	const source = audio_context.createBufferSource();
	const gain = audio_context.createGain();
	const pan = audio_context.createStereoPanner();
	let level = volume;
	if (spatial && play_state)
	{
		const yaw = play_state.player.yaw * Math.PI / 180;
		const right = {x: Math.sin(yaw), y: -Math.cos(yaw)};
		pan.pan.value = Math.max(-1, Math.min(1, (spatial.offset.x * right.x + spatial.offset.y * right.y) / Math.max(100, spatial.distance)));
		const edgeFade = Math.min(1, Math.max(0, (1250 - spatial.distance) / 450));
		level *= edgeFade / (1 + spatial.distance / 700);
	}
	gain.gain.value = level;
	source.buffer = buffer;
	source.connect(gain).connect(pan).connect(audio_context.destination);
	source.start();
}

function surface_sound(surface, team)
{
	const value = String(surface || "").toLowerCase();
	if (value.includes("ladder")) return "ladder";
	if (value.includes("metal")) return "metal";
	if (value.includes("wood")) return "wood";
	if (value.includes("carpet")) return "carpet";
	if (value.includes("dirt") || value.includes("sand") || value.includes("gravel") || value.includes("grass")) return "dirt";
	return team === "ct" ? "ct_concrete" : "t_concrete";
}

function play_viewmodel_action(id)
{
	const clip = viewmodel_animations[id] || viewmodel_animations.idle;
	if (!clip || !viewmodel_mixer) return;
	viewmodel_mixer.stopAllAction();
	const action = viewmodel_mixer.clipAction(clip).reset();
	if (id === "idle") action.setLoop(THREE.LoopRepeat, Infinity);
	else action.setLoop(THREE.LoopOnce, 1).setEffectiveTimeScale(1);
	action.clampWhenFinished = id !== "idle";
	action.play();
	return clip.duration;
}

async function load_viewmodel_arms(url)
{
	if (!url || !viewmodel_root) return;
	const gltf = await loader.loadAsync(url);
	++viewmodel_load_id;
	viewmodel_mixer?.stopAllAction();
	viewmodel_weapon_mount?.parent?.remove(viewmodel_weapon_mount);
	dispose_root(viewmodel_weapon, false, true);
	dispose_root(viewmodel_arms, true, true, true);
	viewmodel_weapon_mount = null;
	viewmodel_weapon = null;
	viewmodel_animations = {};
	viewmodel_root.clear();
	viewmodel_arms = gltf.scene;
	apply_viewmodel_materials(viewmodel_arms);
	apply_ct_sas_arms(viewmodel_arms);
	viewmodel_root.add(viewmodel_arms);
	viewmodel_root.visible = play_active;
	await load_viewmodel_weapon(play_active ? play_weapon_key : active_weapon_key);
}

async function load_grenade_templates()
{
	for (const [kind, key] of [["smoke", "smokegrenade"], ["he", "hegrenade"]])
	{
		if (!manifest_models[key]) continue;
		const gltf = await loader.loadAsync(manifest_models[key]);
		apply_viewmodel_materials(gltf.scene);
		gltf.scene.updateWorldMatrix(true, true);
		const bounds = new THREE.Box3().setFromObject(gltf.scene);
		const center = bounds.getCenter(new THREE.Vector3());
		gltf.scene.position.sub(center);
		const holder = new THREE.Group();
		holder.add(gltf.scene);
		grenade_templates[kind] = holder;
	}
	for (const [kind, key] of [["pistol", "casing_pistol"], ["rifle", "casing_rifle"], ["awp", "casing_awp"]])
	{
		if (!manifest_models[key]) continue;
		const gltf = await loader.loadAsync(manifest_models[key]);
		apply_viewmodel_materials(gltf.scene);
		casing_templates[kind] = gltf.scene;
	}
}

function spawn_shell_casing()
{
	if (!viewmodel_root?.visible) return;
	const kind = play_weapon_key === "awp" ? "awp" : play_weapon_key === "m4a1_silencer" ? "rifle" : "pistol";
	const template = casing_templates[kind];
	if (!template) return;
	const model = template.clone(true);
	model.position.set(0.13, -0.04, -0.48);
	model.rotation.set(Math.random() * Math.PI, Math.random() * Math.PI, Math.random() * Math.PI);
	viewmodel_camera.add(model);
	casing_effects.push({model, velocity: new THREE.Vector3(0.65, 0.38, 0.22), age: 0});
}

async function load_bot_model_from_url(url)
{
	leave_play_mode();
	const loadId = ++model_load_id;
	const gltf = await loader.loadAsync(url);
	if (loadId !== model_load_id)
	{
		dispose_root(gltf.scene, true, true, true);
		return false;
	}
	clear_extra_bots();
	clear_bot_weapon();
	clear_weapon();
	if (model)
	{
		scene.remove(model);
		// viewer_model is a skeleton clone and still shares this geometry.
		dispose_root(model, false, true);
	}
	model = gltf.scene;
	model_animations = viewer_animations.length ? viewer_animations : (gltf.animations || []);
	model.rotation.set(0, 0, 0);
	apply_readable_materials(model, true);
	scene.add(model);
	model_mixer = make_mixer({animations: model_animations}, model);
	capture_runtime_body_bindings();
	rebuild_hitbox_capsules();
	refresh_animation_choices();
	apply_player_transforms();
	return true;
}

async function load_preset(url)
{
	const response = await fetch(url, {cache: "no-store"});
	if (!response.ok)
	{
		throw new Error(`${url}: ${response.status}`);
	}
	const value = await response.json();
	default_points = validated_points(value, "default preset");
	set_points(default_points);
}

async function load_model_from_url(url)
{
	leave_play_mode();
	const loadId = ++model_load_id;
	return new Promise((resolve, reject) =>
	{
		loader.load(url, (gltf) =>
		{
			if (loadId !== model_load_id)
			{
				dispose_root(gltf.scene, true, true, true);
				resolve(false);
				return;
			}
			if (model)
			{
				clear_extra_bots();
				clear_bot_weapon();
				clear_weapon();
				scene.remove(model);
				dispose_root(model, true, true);
			}
			if (viewer_model)
			{
				scene.remove(viewer_model);
				dispose_root(viewer_model, true, true);
				viewer_model = null;
			}
			model = gltf.scene;
			model_animations = gltf.animations || [];
			refresh_animation_choices();
			model.rotation.set(0, 0, 0);
			apply_readable_materials(model, true);
			scene.add(model);
			model_mixer = make_mixer(gltf, model);
			const animated = capture_runtime_body_bindings();
			rebuild_hitbox_capsules();
			viewer_model = clone_skeleton(model);
			viewer_animations = model_animations;
			apply_readable_materials(viewer_model);
			viewer_mixer = make_mixer(gltf, viewer_model);
			viewer_model.position.copy(source_to_three({x: k_viewer_distance, y: 0, z: 0}));
			viewer_model.rotation.set(0, Math.PI, 0);
			scene.add(viewer_model);
			if (runtime_animation_enabled)
			{
				play_selected_animation();
			}
			else
			{
				show_standing_pose();
			}
			set_model_opacity();
			model_status = "Model loaded";
			status_extra = animated
				? (runtime_animation_active() ? "SAS loaded. Runtime bone animation is active." : "SAS loaded. Static point editing is active.")
				: "SAS loaded without the 15 runtime bones; using static points.";
			update_scene();
			resolve(true);
		}, undefined, reject);
	});
}

async function load_manifest()
{
	try
	{
		const response = await fetch("local_assets/manifest.json", {cache: "no-store"});
		if (!response.ok)
		{
			throw new Error(`${response.status}`);
		}
		const manifest = await response.json();
		local_manifest = manifest;
		manifest_models = manifest.models || {};
		update_play_item();
		if (!manifest.models?.ct_sas) throw new Error("manifest has no ct_sas model");
		model_status = "Model loading";
		status_extra = "Loading local CT, T, and viewmodel assets...";
		update_status();
		if (!await load_model_from_url(manifest.models.ct_sas)) return;

		const warnings = [];
		const optional = async (label, operation) =>
		{
			try { return await operation() !== false; }
			catch (error) { warnings.push(`${label}: ${error.message || error}`); return false; }
		};
		const phoenixLoaded = Boolean(manifest.models.t_phoenix)
			&& await optional("Phoenix", () => load_bot_model_from_url(manifest.models.t_phoenix));
		await optional("materials", () => load_material_art(manifest.materials || {}));
		if (manifest.models.viewmodel_arms) await optional("viewmodel arms", () => load_viewmodel_arms(manifest.models.viewmodel_arms));
		await optional("particles", () => load_particle_art(manifest.particles || {}));
		await optional("grenades", load_grenade_templates);
		await optional("animations", () => Promise.all(Object.keys(manifest.animations || {}).map((key) => load_viewmodel_asset(key))));
		await optional("bot weapon", () => load_bot_weapon($("bot-weapon-select").value));
		if (bot_weapon_mount) bot_weapon_mount.visible = false;
		if (active_weapon_key) await optional("weapon preview", () => load_weapon(active_weapon_key));
		model_status = phoenixLoaded ? "CT + Phoenix loaded" : "SAS loaded; Phoenix missing";
		status_extra = warnings.length
			? `Core models loaded; optional assets skipped (${warnings.join("; ")}).`
			: phoenixLoaded ? "Local CT, Phoenix, and FPS assets loaded."
				: "Phoenix is unavailable; Play uses the loaded target model.";
		update_scene();
		reset_camera();
	}
	catch (error)
	{
		model_status = "Model unavailable";
		status_extra = `No local SAS model loaded yet. ${error.message || error}`;
		update_status();
	}
}

function set_export_menu(open)
{
	$("export-menu").hidden = !open;
	$("export-toggle").setAttribute("aria-expanded", String(open));
	if (open)
	{
		$("copy-json").focus();
	}
}

function update_range_labels()
{
	$("model-opacity-value").textContent = `${Math.round(read_number("model-opacity") * 100)}%`;
	$("point-opacity-value").textContent = `${Math.round(read_number("point-opacity") * 100)}%`;
	$("weapon-scale-value").textContent = `${read_number("weapon-scale").toFixed(2)}x`;
	$("map-opacity-value").textContent = `${Math.round(read_number("map-opacity") * 100)}%`;
	$("viewer-ping-value").textContent = `${Math.round(read_number("viewer-ping"))} ms / ${format_number(shoulder_offset(read_number("viewer-ping"), runtime_tuning()))} units`;
}

function install_ui()
{
	for (const button of document.querySelectorAll("#load-sas, [data-load-model]"))
	{
		button.addEventListener("click", () => $("sas-file").click());
	}
	$("import-los").addEventListener("click", () => $("import-json").click());
	$("reset-camera").addEventListener("click", reset_camera);
	$("reload-mirage").addEventListener("click", load_mirage);
	$("load-map").addEventListener("click", () => $("map-file").click());
	$("unload-map").addEventListener("click", () => unload_map());
	$("frame-map").addEventListener("click", frame_map);
	$("frame-players").addEventListener("click", frame_players);
	$("hitbox-capsules").addEventListener("click", () =>
	{
		hitbox_capsules_enabled = !hitbox_capsules_enabled;
		$("hitbox-capsules").setAttribute("aria-pressed", String(hitbox_capsules_enabled));
		$("hitbox-capsules").classList.toggle("primary", hitbox_capsules_enabled);
		if (play_active) update_play_visibility();
		else apply_player_transforms();
	});
	$("map-wireframe").addEventListener("click", () =>
	{
		map_wireframe = !map_wireframe;
		$("map-wireframe").setAttribute("aria-pressed", String(map_wireframe));
		$("map-wireframe").classList.toggle("primary", map_wireframe);
		if (map_mesh)
		{
			map_mesh.material.wireframe = map_wireframe;
			map_mesh.material.needsUpdate = true;
		}
	});
	$("map-focus").addEventListener("click", () =>
	{
		map_focus = !map_focus;
		$("map-focus").setAttribute("aria-pressed", String(map_focus));
		$("map-focus").classList.toggle("primary", map_focus);
		$("map-focus").textContent = map_focus ? "Nearby walls" : "Full map";
		update_map_material();
	});
	for (const role of ["target", "viewer"])
	{
		$(`place-${role}`).addEventListener("click", () =>
		{
			placement_mode = placement_mode === role ? "" : role;
			update_placement_status();
		});
		const pose = role === "target" ? target_pose : viewer_pose;
		for (const key of ["x", "y", "z", "yaw"])
		{
			$(`${role}-${key}`).addEventListener("input", (event) =>
			{
				const value = Number(event.target.value);
				if (!Number.isFinite(value))
				{
					return;
				}
				pose[key] = value;
				if (map_metadata)
				{
					pose.placed = true;
					placement_mode = "";
				}
				update_player_poses();
			});
		}
	}
	for (const id of ["viewer-ping", "shoulder-base", "shoulder-rtt-scale", "shoulder-max", "he-clear-radius", "he-clear-seconds", "simulation-seed"])
	{
		$(id).addEventListener("input", () =>
		{
			update_range_labels();
			if (id.startsWith("shoulder") || id === "viewer-ping") request_map_trace();
		});
	}
	for (const button of document.querySelectorAll("[data-movement-button]"))
	{
		button.addEventListener("click", () =>
		{
			const key = button.dataset.movementButton;
			movement_buttons[key] = !movement_buttons[key];
			button.setAttribute("aria-pressed", String(movement_buttons[key]));
			request_map_trace();
		});
	}
	$("map-opacity").addEventListener("input", () =>
	{
		update_map_material();
		update_range_labels();
	});
	$("map-file").addEventListener("change", (event) =>
	{
		const file = event.target.files[0];
		if (file)
		{
			load_map_file(file);
		}
		event.target.value = "";
	});
	$("animation-clip").addEventListener("change", () =>
	{
		if (runtime_animation_enabled)
		{
			play_selected_animation();
		}
		draw_points();
		update_status();
	});
	$("runtime-animation").addEventListener("click", () => set_runtime_mode(true));
	$("edit-mode").addEventListener("click", () => set_runtime_mode(false));
	$("play-mode").addEventListener("click", enter_play_mode);
	$("mouse-sensitivity").addEventListener("input", () =>
	{
		$("mouse-sensitivity-value").textContent = read_number("mouse-sensitivity").toFixed(2);
		try { localStorage.setItem("cs2fow-studio-sensitivity-v2", $("mouse-sensitivity").value); } catch {}
	});
	for (const tab of document.querySelectorAll("[data-scene-tab]"))
	{
		tab.addEventListener("click", () =>
		{
			for (const button of document.querySelectorAll("[data-scene-tab]"))
			{
				button.setAttribute("aria-pressed", String(button === tab));
			}
			for (const panel of document.querySelectorAll("[data-scene-panel]"))
			{
				panel.hidden = panel.dataset.scenePanel !== tab.dataset.sceneTab;
			}
		});
	}
	$("reset-weapon").addEventListener("click", () => apply_weapon_grip($("weapon-select").value));
	$("weapon-select").addEventListener("change", (event) => load_weapon(event.target.value));
	$("player-primary-select").addEventListener("change", (event) =>
	{
		if (play_active && (play_weapon_key === "m4a1_silencer" || play_weapon_key === "awp"))
			equip_play_weapon(event.target.value);
	});
	$("bot-weapon-select").addEventListener("change", (event) =>
	{
		load_bot_weapon(event.target.value).catch((error) =>
		{
			status_extra = `Bot weapon unavailable: ${error.message || error}`;
			update_status();
		});
	});
	for (const id of ["weapon-x", "weapon-y", "weapon-z", "weapon-rx", "weapon-ry", "weapon-rz", "weapon-scale"])
	{
		$(id).addEventListener("input", () =>
		{
			update_weapon_transform();
			update_range_labels();
		});
	}
	for (const axis of ["x", "y", "z"])
	{
		$(`viewmodel-weapon-${axis}`).addEventListener("input", () =>
		{
			const key = play_grenade === "smoke" ? "smokegrenade" : play_grenade === "he" ? "hegrenade"
				: play_active ? play_weapon_key : active_weapon_key;
			update_viewmodel_weapon_transform(key);
		});
	}
	$("sas-file").addEventListener("change", async (event) =>
	{
		const file = event.target.files[0];
		event.target.value = "";
		if (!file) return;
		const url = URL.createObjectURL(file);
		try
		{
			await load_model_from_url(url);
		}
		catch (error)
		{
			status_extra = `Model could not load: ${error.message || error}`;
			update_status();
		}
		finally { URL.revokeObjectURL(url); }
	});
	for (const id of ["model-opacity", "point-opacity", "min-x", "min-y", "min-z", "max-x", "max-y", "max-z"])
	{
		$(id).addEventListener("input", () =>
		{
			update_range_labels();
			update_scene();
		});
	}
	$("point-name").addEventListener("input", (event) =>
	{
		if (points[selected_index])
		{
			points[selected_index].name = event.target.value;
			render_point_list();
			update_status();
		}
	});
	for (const key of ["x", "y", "z"])
	{
		$(`point-${key}`).addEventListener("input", (event) =>
		{
			const value = Number(event.target.value);
			if (points[selected_index] && Number.isFinite(value))
			{
				points[selected_index][key] = value;
				draw_points();
				render_point_list();
				update_status();
			}
		});
	}
	$("add-point").addEventListener("click", () =>
	{
		if (points.length >= 32)
		{
			return;
		}
		let number = points.length + 1;
		while (points.some((point) => point.name === `custom_${number}`))
		{
			++number;
		}
		points.push({name: `custom_${number}`, x: 0, y: 0, z: 36});
		select_point(points.length - 1);
	});
	$("duplicate-point").addEventListener("click", () =>
	{
		if (!points.length || points.length >= 32)
		{
			return;
		}
		const copy = clone_point(points[selected_index]);
		copy.name = unique_point_name(`${copy.name}_copy`);
		points.splice(selected_index + 1, 0, copy);
		select_point(selected_index + 1);
	});
	$("delete-point").addEventListener("click", () =>
	{
		if (!can_delete_point(points.length))
		{
			status_extra = "At least one LOS point is required.";
			update_status();
			return;
		}
		points.splice(selected_index, 1);
		select_point(Math.min(selected_index, points.length - 1));
	});
	$("reset-points").addEventListener("click", () => set_points(default_points));
	$("export-toggle").addEventListener("click", () => set_export_menu($("export-menu").hidden));
	$("export-menu").addEventListener("keydown", (event) =>
	{
		const items = [$("copy-json"), $("download-json")];
		const index = items.indexOf(document.activeElement);
		if (event.key === "Escape")
		{
			event.preventDefault();
			set_export_menu(false);
			$("export-toggle").focus();
		}
		else if (event.key === "ArrowDown" || event.key === "ArrowUp")
		{
			event.preventDefault();
			const direction = event.key === "ArrowDown" ? 1 : -1;
			items[(index + direction + items.length) % items.length].focus();
		}
	});
	document.addEventListener("pointerdown", (event) =>
	{
		if (!$("export-wrap").contains(event.target))
		{
			set_export_menu(false);
		}
	});
	$("copy-json").addEventListener("click", async () =>
	{
		try
		{
			await navigator.clipboard.writeText(export_json());
			status_extra = "Copied JSON to clipboard.";
		}
		catch (error)
		{
			status_extra = `Copy failed: ${error.message || error}`;
		}
		set_export_menu(false);
		update_status();
	});
	$("download-json").addEventListener("click", () =>
	{
		try
		{
			const blob = new Blob([export_json()], {type: "application/json"});
			const url = URL.createObjectURL(blob);
			const anchor = document.createElement("a");
			anchor.href = url;
			anchor.download = "los_points_sas.json";
			anchor.click();
			URL.revokeObjectURL(url);
			status_extra = "Downloaded LOS JSON.";
		}
		catch (error)
		{
			status_extra = `Download failed: ${error.message || error}`;
		}
		set_export_menu(false);
		update_status();
	});
	$("import-json").addEventListener("change", async (event) =>
	{
		const file = event.target.files[0];
		if (!file)
		{
			return;
		}
		try
		{
			const value = JSON.parse(await file.text());
			set_points(validated_points(value, file.name));
			status_extra = `Imported ${file.name}.`;
		}
		catch (error)
		{
			status_extra = `Import failed: ${error.message || error}`;
		}
		update_status();
	});
	update_range_labels();
}

function play_view_direction()
{
	const yaw = viewer_pose.yaw * Math.PI / 180;
	const pitch = play_pitch * Math.PI / 180;
	return {x: Math.cos(yaw) * Math.cos(pitch), y: Math.sin(yaw) * Math.cos(pitch), z: Math.sin(pitch)};
}

function set_play_key(event, down)
{
	const bindings = {
		KeyW: "w", KeyA: "a", KeyS: "s", KeyD: "d",
		ShiftLeft: "walk", ShiftRight: "walk", ControlLeft: "crouch", ControlRight: "crouch", Space: "jump"
	};
	const key = bindings[event.code];
	if (!key) return false;
	play_keys[key] = down;
	update_play_input();
	return true;
}

function play_world_action(id)
{
	const choices = k_animation_sets[play_grenade ? "grenade" : play_weapon_key] || k_animation_sets.default;
	const choice = choices.find((entry) => entry.id === id);
	if (!choice) return 0;
	const name = play_state?.player.crouched && choice.crouchClip ? choice.crouchClip : choice.clip;
	const clip = THREE.AnimationClip.findByName(viewer_animations, name);
	if (!clip || !viewer_mixer) return 0;
	player_world_action_until = play_now() + clip.duration;
	choose_player_animation(play_state?.player);
	viewer_mixer.cs2fow_overlay?.fadeOut(0.08);
	const overlay = viewer_mixer.clipAction(masked_world_clip(clip, true)).reset();
	overlay.setLoop(THREE.LoopOnce, 1);
	overlay.clampWhenFinished = true;
	overlay.fadeIn(0.08).play();
	viewer_mixer.cs2fow_overlay = overlay;
	return clip.duration;
}

function play_action(id)
{
	const worldDuration = play_world_action(id);
	const viewmodelAction = id === "shoot" ? "fire" : id === "shoot2" ? "fire2" : id;
	const duration = play_viewmodel_action(viewmodelAction) || 0;
	if (id === "shoot" || id === "shoot2")
	{
		const sound = play_weapon_key === "m4a1_silencer" ? "m4_fire"
			: play_weapon_key === "awp" ? "awp_fire"
			: play_weapon_key === "knife" ? "knife_slash" : "usp_fire";
		play_sound(sound, null, 0.55);
	}
	status_extra = `${id[0].toUpperCase()}${id.slice(1)} preview triggered.`;
	update_status();
	return Math.max(duration, worldDuration);
}

async function equip_play_weapon(key)
{
	if (!k_weapon_stats[key]) return;
	set_play_scope(false);
	play_grenade = "";
	play_grenade_holding = false;
	play_grenade_throw_speed = 750;
	play_weapon_key = key;
	play_firing = false;
	play_busy_until = 0;
	++play_action_serial;
	update_play_item();
	map_worker?.postMessage({type: "play-speed", value: k_weapon_stats[key].speed});
	try
	{
		await Promise.all([load_viewmodel_weapon(key), load_player_weapon(key)]);
		if (!play_active || play_weapon_key !== key || play_grenade) return;
		play_viewmodel_action("draw");
		play_world_action("draw");
		play_sound(weapon_draw_sound(key), null, 0.42);
	}
	catch (error)
	{
		status_extra = `${weapon_label(key)} viewmodel unavailable: ${error.message || error}`;
		update_status();
	}
}

async function equip_play_grenade(kind)
{
	set_play_scope(false);
	play_grenade = kind;
	play_grenade_holding = false;
	play_grenade_throw_speed = 750;
	play_firing = false;
	play_busy_until = 0;
	++play_action_serial;
	update_play_item();
	const key = kind === "smoke" ? "smokegrenade" : "hegrenade";
	try
	{
		await Promise.all([load_viewmodel_weapon(key), load_player_weapon(key)]);
		if (!play_active || play_grenade !== kind) return;
		play_viewmodel_action("draw");
		play_world_action("draw");
		play_sound(kind === "smoke" ? "smoke_draw" : "he_draw", null, 0.42);
	}
	catch (error)
	{
		status_extra = `${kind === "smoke" ? "Smoke" : "HE"} viewmodel unavailable: ${error.message || error}`;
		update_status();
	}
}

function reload_play_weapon()
{
	if (play_grenade) return;
	const stats = k_weapon_stats[play_weapon_key];
	if (!stats || stats.clip === null || play_ammo[play_weapon_key] >= stats.clip) return;
	set_play_scope(false);
	const now = play_now();
	if (now < play_busy_until) return;
	const key = play_weapon_key;
	const serial = ++play_action_serial;
	const duration = Math.max(0.2, play_action("reload") || 1.5);
	play_busy_until = now + duration;
	play_reload_sounds(key, serial);
	schedule_play(duration, () =>
	{
		if (!play_active || serial !== play_action_serial || play_weapon_key !== key || play_grenade) return;
		play_ammo[key] = stats.clip;
		play_busy_until = 0;
		update_play_item();
	}, serial);
}

function fire_play_weapon()
{
	if (play_grenade) return;
	const stats = k_weapon_stats[play_weapon_key];
	if (!stats) return;
	const now = play_now();
	if (now < play_next_fire_time || now < play_busy_until) return;
	if (stats.clip !== null)
	{
		if (play_ammo[play_weapon_key] <= 0) return;
		--play_ammo[play_weapon_key];
		update_play_item();
	}
	play_next_fire_time = now + 60 / stats.rpm;
	play_action("shoot");
	if (stats.clip !== null)
	{
		spawn_viewmodel_muzzle_flash();
		spawn_shell_casing();
		map_worker.postMessage({type: "play-shot", direction: play_view_direction()});
	}
	if (play_weapon_key === "awp" && play_scoped) set_play_scope(false);
}

function use_play_secondary()
{
	if (play_grenade)
	{
		play_grenade_throw_speed = 375;
		use_play_item();
		return;
	}
	if (play_weapon_key === "awp")
	{
		set_play_scope(!play_scoped);
		return;
	}
	if (play_weapon_key !== "knife") return;
	const now = play_now();
	if (now < play_next_fire_time || now < play_busy_until) return;
	play_next_fire_time = now + 1;
	play_action("shoot2");
}

function release_play_grenade()
{
	if (!play_grenade || !play_grenade_holding) return;
	const grenade = play_grenade;
	const origin = add_source(pose_origin(viewer_pose), {x: 0, y: 0, z: play_state?.player?.crouched ? 28.5 : 64});
	const duration = Math.max(play_viewmodel_action("throw") || 0.45,
		play_world_action(play_grenade_throw_speed < 750 ? "throw2" : "throw"));
	map_worker.postMessage({type: "play-throw", kind: grenade, origin, direction: play_view_direction(), speed: play_grenade_throw_speed});
	play_sound("grenade_throw", null, 0.5);
	play_grenade = "";
	play_grenade_holding = false;
	play_grenade_throw_speed = 750;
	++play_action_serial;
	update_play_item();
	schedule_play(Math.max(0.18, duration), () =>
	{
		if (play_active && !play_grenade) equip_play_weapon(play_weapon_key);
	});
}

function use_play_item()
{
	if (play_grenade)
	{
		if (!play_grenade_holding)
		{
			play_grenade_holding = true;
			if (play_grenade_throw_speed !== 375) play_grenade_throw_speed = 750;
			play_world_action("pullpin");
			play_sound(play_grenade === "smoke" ? "smoke_pin" : "he_pin", null, 0.36);
			update_play_item();
		}
		return;
	}
	fire_play_weapon();
}

function install_play_controls()
{
	try
	{
		const saved = Number(localStorage.getItem("cs2fow-studio-sensitivity-v2"));
		if (Number.isFinite(saved) && saved >= 0.1 && saved <= 3) $("mouse-sensitivity").value = saved;
	}
	catch {}
	$("mouse-sensitivity-value").textContent = read_number("mouse-sensitivity").toFixed(2);
	document.addEventListener("pointerlockchange", () =>
	{
		if (!play_active) return;
		play_paused = !play_pointer_locked();
		if (play_paused)
		{
			play_look_dirty = false;
			play_firing = false;
			clear_play_input();
		}
		document.body.classList.toggle("play-locked", !play_paused);
		$("play-paused").hidden = !play_paused;
		map_worker.postMessage({type: "play-pause", paused: play_paused});
	});
	document.addEventListener("mousemove", (event) =>
	{
		if (!play_active || !play_pointer_locked()) return;
		const sensitivity = read_number("mouse-sensitivity") * k_mouse_yaw;
		viewer_pose.yaw = (viewer_pose.yaw - event.movementX * sensitivity) % 360;
		play_pitch = Math.max(-89, Math.min(89, play_pitch - event.movementY * sensitivity));
		play_look_dirty = true;
		update_play_camera();
	});
	document.addEventListener("keydown", (event) =>
	{
		if (!play_active || play_paused) return;
		if (set_play_key(event, true)) { event.preventDefault(); return; }
		if (event.repeat) return;
		if (event.code === "Digit1") equip_play_weapon($("player-primary-select").value);
		else if (event.code === "Digit2") equip_play_weapon("usp_silencer");
		else if (event.code === "Digit3") equip_play_weapon("knife");
		else if (event.code === "Digit4")
		{
			equip_play_grenade(play_grenade === "smoke" ? "he" : "smoke");
		}
		else if (event.code === "KeyV")
		{
			play_debug = !play_debug;
			last_play_debug_draw = -Infinity;
			last_play_traversal_request = -Infinity;
			debug_trace_smoothing = null;
			if (!play_debug)
			{
				clear_map_traversal();
				$("play-bvh").textContent = "BVH traversal appears in Debug mode.";
			}
			else $("play-bvh").textContent = "Sampling the current runtime BVH8 traversal…";
			update_map_material();
			map_worker.postMessage({type: "play-debug", enabled: play_debug});
			update_play_visibility();
		}
		else if (event.code === "KeyC")
		{
			play_rays_enabled = !play_rays_enabled;
			last_play_debug_draw = -Infinity;
			debug_trace_smoothing = null;
			update_play_visibility();
		}
		else if (event.code === "KeyJ")
		{
			set_play_scope(false);
			play_third_person = !play_third_person;
			if (play_third_person)
			{
				const key = play_grenade ? `${play_grenade}grenade` : play_weapon_key;
				load_player_weapon(key).catch((error) => { status_extra = `World weapon unavailable: ${error.message}`; update_status(); });
			}
			else clear_player_weapon();
			update_play_visibility();
			update_play_camera();
		}
		else if (event.code === "KeyR") reload_play_weapon();
		else if (event.code === "KeyF") play_action("inspect");
	});
	document.addEventListener("keyup", (event) =>
	{
		if (play_active && set_play_key(event, false)) event.preventDefault();
	});
	renderer.domElement.addEventListener("contextmenu", (event) => { if (play_active) event.preventDefault(); });
	renderer.domElement.addEventListener("mousedown", (event) =>
	{
		if (!play_active || (event.button !== 0 && event.button !== 2)) return;
		if (!play_pointer_locked()) void request_play_pointer_lock();
		else
		{
			const throwingGrenade = Boolean(play_grenade);
			if (event.button === 2) use_play_secondary();
			else
			{
				play_firing = true;
				use_play_item();
				if (throwingGrenade || !k_weapon_stats[play_weapon_key]?.automatic) play_firing = false;
			}
		}
	});
	document.addEventListener("mouseup", (event) =>
	{
		if (event.button !== 0 && event.button !== 2) return;
		if (event.button === 0) play_firing = false;
		if (play_active && play_pointer_locked()) release_play_grenade();
	});
}

async function request_play_pointer_lock()
{
	try
	{
		await renderer.domElement.requestPointerLock({unadjustedMovement: true});
	}
	catch
	{
		try { await renderer.domElement.requestPointerLock(); } catch {}
	}
}

function place_map_actor(mode, point)
{
	const pose = mode === "target" ? target_pose : viewer_pose;
	Object.assign(pose, point, {placed: true});
	write_pose_fields(mode, pose);
	if (mode === "target" && !viewer_pose.placed)
	{
		placement_mode = "viewer";
		status_extra = "Target placed. Click the map to place the viewer.";
	}
	else
	{
		placement_mode = "";
		status_extra = "Players placed. Live BVH8 wall checks are active.";
	}
	if (map_simulation_ready()) face_players();
	update_player_poses();
	if (map_simulation_ready()) frame_players();
}

function install_picking()
{
	const raycaster = new THREE.Raycaster();
	const pointer = new THREE.Vector2();
	renderer.domElement.addEventListener("pointerdown", (event) =>
	{
		if (play_active) return;
		const rect = renderer.domElement.getBoundingClientRect();
		pointer.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
		pointer.y = -(((event.clientY - rect.top) / rect.height) * 2 - 1);
		raycaster.setFromCamera(pointer, camera);
		if (placement_mode && map_mesh)
		{
			const origin = three_to_source(raycaster.ray.origin);
			const target = three_to_source(raycaster.ray.origin.clone().addScaledVector(raycaster.ray.direction, 100000));
			map_worker.postMessage({type: "pick", id: ++placement_pick_id, mapId: map_load_id,
				mode: placement_mode, origin, target});
			return;
		}
		const hit = raycaster.intersectObjects(marker_group.children, false)[0];
		if (hit?.object?.userData?.pointIndex !== undefined)
		{
			select_point(hit.object.userData.pointIndex);
		}
	});
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
	expect(default_points.length === 15, "default body point count");
	expect(generated_aabb_points().length === 8, "AABB fallback count");
	expect(generated_aabb_points()[0].x === -24 && generated_aabb_points()[7].z === 80, "runtime AABB padding");
	const viewer_origins = stationary_viewer_origins();
	expect(viewer_origins.length === 5 && viewer_origins[0].x === 256, "fixed viewer origins");
	expect(new Set(viewer_origins.map((point) => `${point.x},${point.y},${point.z}`)).size === 5, "stationary origins are unique");
	expect(viewer_origins.length * (default_points.length + generated_aabb_points().length) === 115, "stationary ray count");
	const roundtrip = JSON.parse(export_json());
	const imported = validated_points(roundtrip, "self-check round trip");
	expect(roundtrip.points.length === points.length, "JSON round trip count");
	expect(roundtrip.point_count === points.length, "JSON point count metadata");
	expect(roundtrip.version === 1 && roundtrip.model === "ctm_sas", "JSON metadata");
	expect(roundtrip.coordinate_space === "source_local", "JSON coordinate space");
	expect(roundtrip.points.every((point, index) => point.name === points[index].name), "JSON point order");
	expect(imported.every((point, index) => point.name === points[index].name), "validated import round trip");
	expect(!can_delete_point(1) && can_delete_point(2), "final point protection");
	for (const invalid of [
		{...roundtrip, point_count: 0, points: []},
		{...roundtrip, points: roundtrip.points.map((point, index) => ({...point, name: index === 0 ? " " : point.name}))},
		{...roundtrip, points: roundtrip.points.map((point, index) => ({...point, name: index === 1 ? roundtrip.points[0].name : point.name}))},
		{...roundtrip, points: roundtrip.points.map((point, index) => ({...point, x: index === 0 ? Infinity : point.x}))}
	])
	{
		let rejected = false;
		try { validated_points(invalid, "self-check"); } catch { rejected = true; }
		expect(rejected, "invalid export rejection");
	}
	const previous_status = status_extra;
	status_extra = "Export failed: self-check";
	update_status();
	expect($("status").textContent === status_extra, "visible export validation feedback");
	status_extra = previous_status;
	update_status();
	expect($("points-list").querySelectorAll('[role="option"]').length === 15, "point list count");
	expect($("point-name").value === points[selected_index]?.name, "selected point synchronization");
	for (const id of ["load-sas", "import-los", "export-toggle", "animation-clip", "runtime-animation", "edit-mode", "play-mode", "points-list", "point-name", "inspector", "points-disclosure", "metrics-hud", "state-hud", "play-hud", "play-result", "play-rays", "play-smokes", "play-hes", "play-bot-count", "play-debug-state", "play-view", "play-blocker", "play-bvh", "player-primary-select", "load-map", "reload-mirage", "unload-map", "place-target", "place-viewer", "viewer-ping", "shoulder-base", "shoulder-rtt-scale", "shoulder-max", "he-clear-radius", "he-clear-seconds", "simulation-seed", "mouse-sensitivity", "map-opacity", "map-focus", "map-wireframe", "frame-map", "frame-players", "hitbox-capsules"])
	{
		expect(Boolean($(id)), `redesigned control: ${id}`);
	}
	expect(document.querySelectorAll("[data-scene-panel]").length === 5, "five inspector panels");
	expect(document.querySelectorAll("[data-movement-button]").length === 4, "WASD controls");
	expect(Boolean(map_worker), "BVH8 worker");
	expect(k_debug_draw_interval === 1 / 16 && k_bvh_snapshot_interval === 1 / 16,
		"16 Hz LOS and BVH debug geometry");
	expect(shoulder_offset(0) === 48 && shoulder_offset(200) === 128, "runtime ping shoulder range");
	expect(Object.keys(k_map_spawn_pairs).length === 6, "default map spawn pairs");
	expect($("points-disclosure").open === false, "collapsed point list");
	expect(k_runtime_body_bones.length === 15 && new Set(k_runtime_body_bones).size === 15, "runtime bone bindings");
	expect(k_skeleton_edges.length === 14, "body skeleton edges");
	expect(skeleton_group.children.length === 1
		&& skeleton_lines?.geometry.getAttribute("position")?.count === k_skeleton_edges.length * 2,
		"body skeleton geometry");
	expect(k_animation_sets.default.length === 34, "base animation choices");
	for (const [key, expected] of Object.entries({knife: 37, usp_silencer: 37, m4a1_silencer: 37, awp: 37, grenade: 38}))
	{
		const choices = k_animation_sets[key];
		expect(choices.length === expected && new Set(choices.map((choice) => choice.id)).size === choices.length,
			`${key} animation choices`);
	}
	const moving = {crouched: false, grounded: true, speed: 200, yaw: 0};
	expect(locomotion_animation_id({...moving, velocity: {x: 200, y: 0}}) === "run_forward"
		&& locomotion_animation_id({...moving, velocity: {x: -200, y: 0}}) === "run_backward"
		&& locomotion_animation_id({...moving, velocity: {x: 0, y: 200}}) === "run_left"
		&& locomotion_animation_id({...moving, velocity: {x: 0, y: -200}}) === "run_right"
		&& locomotion_animation_id({...moving, velocity: {x: 200, y: -200}}) === "run_forward_right",
		"directional third-person locomotion");
	const masked = new THREE.AnimationClip("mask-check", 1, [
		new THREE.VectorKeyframeTrack("pelvis.position", [0, 1], [0, 0, 0, 1, 1, 1]),
		new THREE.QuaternionKeyframeTrack("spine_0.quaternion", [0, 1], [0, 0, 0, 1, 0, 0, 0, 1])
	]);
	expect(masked_world_clip(masked, false).tracks.length === 1 && masked_world_clip(masked, true).tracks.length === 1,
		"layered upper/lower body actions");
	const crouchedEye = THREE.MathUtils.damp(64, 28.5, 14, 1 / 60);
	expect(crouchedEye < 64 && crouchedEye > 28.5, "smooth crouch camera");
	const halfwayPose = {};
	interpolate_pose(halfwayPose, {x: 0, y: 10, z: 20, yaw: 350, height: 72},
		{x: 10, y: 20, z: 30, yaw: 10, height: FPS_CONSTANTS.crouchedHeight}, 0.5);
	expect(halfwayPose.x === 5 && halfwayPose.y === 15 && halfwayPose.z === 25
		&& halfwayPose.yaw === 360 && halfwayPose.height < 72 && halfwayPose.height > FPS_CONSTANTS.crouchedHeight,
		"one-tick actor interpolation and shortest-path yaw");
	const debugGeometry = new THREE.BufferGeometry();
	const debugTarget = new Float32Array([2, 0, 0]);
	debugGeometry.setAttribute("position", new THREE.BufferAttribute(debugTarget, 3));
	const debugPoint = new THREE.Points(debugGeometry, new THREE.PointsMaterial());
	const debugParent = new THREE.Group();
	debugParent.add(debugPoint);
	queue_extra_debug_interpolation(debugPoint, debugTarget, new Float32Array([0, 0, 0]));
	update_extra_debug_smoothing(k_debug_draw_interval / 2);
	expect(debugGeometry.getAttribute("position").array[0] === 1, "extra debug geometry interpolation");
	extra_debug_smoothing.length = 0;
	clear_group(debugParent);
	expect(Boolean(document.querySelector(".play-gate-line")) && !document.querySelector(".play-top-status, .play-help, .play-movement"),
		"focused Play HUD");
	expect(k_reload_sound_sequences.usp_silencer.length === 5
		&& k_reload_sound_sequences.m4a1_silencer.length === 4
		&& k_reload_sound_sequences.awp[0][0] === 8 / 30
		&& k_reload_sound_sequences.awp.at(-1)[0] === 89 / 30,
		"reload animation event timing");
	expect(k_weapon_stats.usp_silencer.clip === 12 && k_weapon_stats.usp_silencer.rpm === 352.94
		&& k_weapon_stats.usp_silencer.speed === 240, "USP-S gameplay values");
	expect(k_weapon_stats.m4a1_silencer.clip === 20 && k_weapon_stats.m4a1_silencer.rpm === 600
		&& k_weapon_stats.m4a1_silencer.speed === 225, "M4A1-S gameplay values");
	expect(k_weapon_stats.awp.clip === 5 && k_weapon_stats.awp.rpm === 41.24
		&& k_weapon_stats.awp.speed === 200, "AWP gameplay values");
	expect(k_mouse_yaw === 0.022 && Number($("mouse-sensitivity").defaultValue) === 0.94, "CS2 mouse defaults");
	expect(viewmodel_camera?.fov === 44, "44-degree viewmodel FOV");
	expect(is_hd_viewmodel_body("weapon.body_hd") && !is_hd_viewmodel_body("weapon.body_legacy"),
		"viewmodel body-group selection");
	expect(!should_show_viewer_model(true, true, true, false)
		&& should_show_viewer_model(true, true, true, true), "first-person world-model isolation");
	expect(map_material_state(0.2).opacity === 0.2 && map_material_state(0.2).transparent,
		"map opacity control");
	expect(map_material_state(0.8).opacity === 0.8, "BVH traversal preserves map opacity");
	expect(k_viewmodel_offset.x === 2.5 && k_viewmodel_offset.y === 2 && k_viewmodel_offset.z === -1,
		"CS2 viewmodel offsets");
	expect(["x", "y", "z"].every((axis) => $(`viewmodel-weapon-${axis}`))
		&& viewmodel_weapon_offsets.awp.x === 9.25 && viewmodel_weapon_offsets.usp_silencer.y === 5.75
		&& viewmodel_weapon_offsets.m4a1_silencer.x === 4.5 && viewmodel_weapon_offsets.m4a1_silencer.y === 3.5,
		"editable first-person weapon position");
	expect(["primary", "secondary", "knife", "grenade"].every((slot) => $(`play-slot-${slot}`))
		&& ["primary", "secondary", "knife", "grenade"].every((slot) => $(`play-icon-${slot}`)),
		"CS2-style Play HUD inventory");

	const element = $("self-check");
	if (failures.length === 0)
	{
		element.textContent = "Self-check passed";
		element.className = "pill ok";
	}
	else
	{
		element.textContent = `Self-check failed: ${failures.join(", ")}`;
		element.className = "pill bad";
	}
}

function init_scene()
{
	renderer = new THREE.WebGLRenderer({canvas: $("view"), antialias: true, alpha: true});
	renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
	renderer.setSize(window.innerWidth, window.innerHeight);
	renderer.setClearColor(0x000000, 0);
	renderer.outputColorSpace = THREE.SRGBColorSpace;
	renderer.toneMapping = THREE.ACESFilmicToneMapping;
	renderer.toneMappingExposure = 1.15;
	renderer.shadowMap.enabled = true;
	renderer.shadowMap.type = THREE.PCFSoftShadowMap;
	renderer.autoClear = false;

	scene = new THREE.Scene();
	camera = new THREE.PerspectiveCamera(k_world_fov, window.innerWidth / window.innerHeight, 0.02, 3000);
	camera.position.set(8.5, 4.5, 10.5);
	scene.add(camera);
	viewmodel_scene = new THREE.Scene();
	viewmodel_camera = new THREE.PerspectiveCamera(k_viewmodel_fov, window.innerWidth / window.innerHeight, 0.01, 20);
	viewmodel_scene.add(viewmodel_camera);
	viewmodel_root = new THREE.Group();
	viewmodel_root.position.set(
		k_viewmodel_offset.x / k_source_units_per_meter,
		k_viewmodel_offset.z / k_source_units_per_meter,
		-k_viewmodel_offset.y / k_source_units_per_meter
	);
	viewmodel_root.rotation.y = Math.PI;
	viewmodel_root.visible = false;
	viewmodel_camera.add(viewmodel_root);
	viewmodel_scene.add(new THREE.HemisphereLight(0xffffff, 0x6d737a, 3.0));
	const viewmodelLight = new THREE.DirectionalLight(0xffffff, 2.2);
	viewmodelLight.position.set(-2, 4, 3);
	viewmodel_scene.add(viewmodelLight);

	orbit = new OrbitControls(camera, renderer.domElement);
	orbit.target.set(0, 0.95, 0);
	orbit.enableDamping = true;
	orbit.dampingFactor = 0.08;
	orbit.rotateSpeed = 0.18;
	orbit.zoomSpeed = 0.12;
	orbit.panSpeed = 0.22;
	orbit.minDistance = 0.65;
	orbit.maxDistance = 80;
	orbit.update();

	transform = new TransformControls(camera, renderer.domElement);
	transform.setMode("translate");
	transform.setSize(0.58);
	transform.addEventListener("dragging-changed", (event) => { orbit.enabled = !event.value; });
	transform.addEventListener("objectChange", () =>
	{
		if (!points[selected_index])
		{
			return;
		}
		const value = target_local_point(three_to_source(transform.object.position));
		points[selected_index].x = value.x;
		points[selected_index].y = value.y;
		points[selected_index].z = value.z;
		draw_runtime_rays();
		render_point_editor();
		update_status();
	});
	scene.add(transform);

	loader = new GLTFLoader();
	animation_clock = new THREE.Clock();
	marker_group = new THREE.Group();
	skeleton_group = new THREE.Group();
	hitbox_group = new THREE.Group();
	aabb_group = new THREE.Group();
	muzzle_group = new THREE.Group();
	ray_group = new THREE.Group();
	origin_group = new THREE.Group();
	nav_group = new THREE.Group();
	smoke_group = new THREE.Group();
	grenade_group = new THREE.Group();
	effect_group = new THREE.Group();
	for (let index = 0; index < 2; ++index)
	{
		extra_bot_debug_groups.push(new THREE.Group());
		extra_bot_capsule_groups.push(new THREE.Group());
	}
	scene.add(ray_group, origin_group, aabb_group, skeleton_group, marker_group, muzzle_group, hitbox_group,
		nav_group, smoke_group, grenade_group, effect_group, ...extra_bot_debug_groups, ...extra_bot_capsule_groups);
	scene.add(new THREE.HemisphereLight(0xffffff, 0xc9cdd2, 2.4));
	const key_light = new THREE.DirectionalLight(0xffffff, 3.2);
	key_light.position.set(4, 9, 6);
	key_light.castShadow = true;
	key_light.shadow.mapSize.set(1024, 1024);
	key_light.shadow.camera.left = -7;
	key_light.shadow.camera.right = 7;
	key_light.shadow.camera.top = 7;
	key_light.shadow.camera.bottom = -7;
	scene.add(key_light);
	const rim_light = new THREE.PointLight(0xffe8eb, 2.2, 18);
	rim_light.position.set(-4, 3, -2);
	scene.add(rim_light);
	studio_ground = new THREE.Mesh(
		new THREE.PlaneGeometry(13, 13),
		new THREE.ShadowMaterial({color: 0x5c6269, opacity: 0.12})
	);
	studio_ground.rotation.x = -Math.PI / 2;
	studio_ground.position.y = -0.01;
	studio_ground.receiveShadow = true;
	scene.add(studio_ground);
	studio_grid = new THREE.GridHelper(10, 20, 0x9da3aa, 0xc8cdd2);
	studio_grid.material.transparent = true;
	studio_grid.material.opacity = 0.32;
	scene.add(studio_grid);

	window.addEventListener("resize", () =>
	{
		update_camera_projection();
		viewmodel_camera.aspect = camera.aspect;
		viewmodel_camera.updateProjectionMatrix();
		renderer.setSize(window.innerWidth, window.innerHeight);
	});
}

function animate()
{
	requestAnimationFrame(animate);
	const delta = animation_clock.getDelta();
	const activeDelta = play_active && play_paused ? 0 : delta;
	if (play_look_dirty && play_active && !play_paused)
	{
		play_look_dirty = false;
		map_worker.postMessage({type: "play-look", yaw: viewer_pose.yaw, pitch: play_pitch});
	}
	if (play_active && !play_paused && play_firing && !play_grenade)
		fire_play_weapon();
	if (play_active)
	{
		update_play_pose_interpolation(activeDelta);
		play_eye_height = THREE.MathUtils.damp(play_eye_height, play_eye_height_target, 14, activeDelta);
		update_play_camera();
	}
	update_map_focus();
	const animationsActive = runtime_animation_active() || play_active;
	if (animationsActive)
	{
		model_mixer?.update(activeDelta);
		for (const mixer of extra_bot_mixers) mixer.update(activeDelta);
		viewer_mixer?.update(activeDelta);
	}
	if (animationsActive) update_animated_preview();
	viewmodel_mixer?.update(activeDelta);
	update_viewmodel_motion(activeDelta);
	update_debug_trace_smoothing(activeDelta);
	update_extra_debug_smoothing(activeDelta);
	update_grenade_interpolation(activeDelta);
	update_explosion_effects(activeDelta);
	update_shot_effects(activeDelta);
	if (orbit.enabled) orbit.update();
	renderer.clear();
	renderer.render(scene, camera);
	if (viewmodel_root?.visible)
	{
		renderer.clearDepth();
		renderer.render(viewmodel_scene, viewmodel_camera);
	}
}

async function main()
{
	init_scene();
	init_map_worker();
	install_ui();
	install_play_controls();
	install_picking();
	animate();
	await load_preset(k_default_preset);
	run_self_checks();
	load_manifest();
	load_mirage();
}

main().catch((error) =>
{
	$("self-check").textContent = `Self-check failed: ${error.message || error}`;
	$("self-check").className = "pill bad";
});
