// Local browser editor for body points, generated axis-aligned box corners, and
// a weapon-muzzle preview. It reads ignored model assets and exports one preset;
// it does not load maps, cast runtime rays, or change plugin state.

import * as THREE from "https://esm.sh/three@0.160.0";
import {OrbitControls} from "https://esm.sh/three@0.160.0/examples/jsm/controls/OrbitControls.js";
import {TransformControls} from "https://esm.sh/three@0.160.0/examples/jsm/controls/TransformControls.js";
import {GLTFLoader} from "https://esm.sh/three@0.160.0/examples/jsm/loaders/GLTFLoader.js";

const k_source_units_per_meter = 39.3700787;
const k_default_preset = "default_sas_visibility_points.json";
const k_aabb_color = 0x007c91;
const k_body_color = 0xa96300;
const k_selected_color = 0xdf1f2d;
const k_muzzle_color = 0x92278f;
const k_aabb_dot_radius = 0.022 / 3.0;
const k_body_dot_radius = 0.03 / 3.0;
const k_selected_dot_radius = 0.044 / 3.0;
const k_muzzle_dot_radius = 0.038 / 3.0;
const k_default_weapon_grip = {x: 0.5, y: -2.5, z: -0.5, rx: 90, ry: -90, rz: 4, scale: 1};
const k_weapon_grips = {
	usp_silencer: {x: 0, y: 0, z: 0, rx: 90, ry: -90, rz: 0, scale: 1},
	m4a1_silencer: k_default_weapon_grip,
	awp: {x: -4, y: 0.75, z: 0, rx: 90, ry: -90, rz: 0, scale: 1}
};
const k_weapon_muzzle_offsets = {
	usp_silencer: {x: 18, y: 0, z: 0},
	m4a1_silencer: {x: 36, y: 0, z: 0},
	awp: {x: 52, y: 0, z: 0}
};

const $ = (id) => document.getElementById(id);
const source_to_three = (p) => new THREE.Vector3(p.y / k_source_units_per_meter, p.z / k_source_units_per_meter, p.x / k_source_units_per_meter);
const three_to_source = (p) => ({x: p.z * k_source_units_per_meter, y: p.x * k_source_units_per_meter, z: p.y * k_source_units_per_meter});
const read_number = (id) =>
{
	const value = Number($(id).value);
	return Number.isFinite(value) ? value : 0;
};
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
let manifest_models = {};
let weapon_model;
let points = [];
let default_points = [];
let selected_index = 0;
let marker_group;
let aabb_group;
let muzzle_group;
let status_extra = "";
let active_weapon_key = "";
let model_status = "Model unavailable";

function reset_camera()
{
	camera.position.set(4.2, 3.0, 6.2);
	orbit.target.set(0, 1.0, 0);
	orbit.update();
}

function degrees_to_radians(value)
{
	return value * Math.PI / 180.0;
}

function point_vec(point)
{
	return {x: Number(point.x), y: Number(point.y), z: Number(point.z)};
}

function format_number(value)
{
	return Number(value).toFixed(2).replace(/\.?0+$/, "");
}

function generated_aabb_points()
{
	const min = {x: read_number("min-x"), y: read_number("min-y"), z: read_number("min-z")};
	const max = {x: read_number("max-x"), y: read_number("max-y"), z: read_number("max-z")};
	return [
		{x: min.x, y: min.y, z: min.z}, {x: max.x, y: min.y, z: min.z},
		{x: min.x, y: max.y, z: min.z}, {x: max.x, y: max.y, z: min.z},
		{x: min.x, y: min.y, z: max.z}, {x: max.x, y: min.y, z: max.z},
		{x: min.x, y: max.y, z: max.z}, {x: max.x, y: max.y, z: max.z}
	];
}

function set_model_opacity()
{
	if (!model)
	{
		return;
	}
	const opacity = read_number("model-opacity");
	model.traverse((node) =>
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

function make_marker(point, color, radius)
{
	const material = new THREE.MeshBasicMaterial({
		color,
		transparent: true,
		opacity: read_number("point-opacity"),
		depthTest: false
	});
	const mesh = new THREE.Mesh(new THREE.SphereGeometry(radius, 16, 10), material);
	mesh.position.copy(source_to_three(point));
	mesh.renderOrder = 10;
	return mesh;
}

function make_aabb_wire(points)
{
	const edges = [
		[0, 1], [0, 2], [1, 3], [2, 3],
		[4, 5], [4, 6], [5, 7], [6, 7],
		[0, 4], [1, 5], [2, 6], [3, 7]
	];
	const vertices = [];
	for (const [a, b] of edges)
	{
		vertices.push(source_to_three(points[a]), source_to_three(points[b]));
	}
	const geometry = new THREE.BufferGeometry().setFromPoints(vertices);
	const material = new THREE.LineBasicMaterial({color: k_aabb_color, transparent: true, opacity: 0.75});
	const lines = new THREE.LineSegments(geometry, material);
	lines.renderOrder = 8;
	return lines;
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

function draw_muzzle_point()
{
	clear_group(muzzle_group);
	const offset = k_weapon_muzzle_offsets[active_weapon_key];
	if (!weapon_model || !offset)
	{
		return;
	}
	const position = weapon_model.localToWorld(source_to_three(offset).clone());
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
	transform.detach();
	clear_group(marker_group);
	clear_group(aabb_group);

	const aabb_points = generated_aabb_points();
	aabb_group.add(make_aabb_wire(aabb_points));
	for (const point of aabb_points)
	{
		aabb_group.add(make_marker(point, k_aabb_color, k_aabb_dot_radius));
	}

	for (let index = 0; index < points.length; ++index)
	{
		const point = point_vec(points[index]);
		const marker = make_marker(point, index === selected_index ? k_selected_color : k_body_color, index === selected_index ? k_selected_dot_radius : k_body_dot_radius);
		marker.userData.pointIndex = index;
		marker_group.add(marker);
		if (index === selected_index)
		{
			transform.attach(marker);
		}
	}

	if (points.length === 0)
	{
		transform.detach();
	}
	draw_muzzle_point();
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
	for (const id of ["point-name", "point-x", "point-y", "point-z"])
	{
		$(id).disabled = !point;
	}
	$("add-point").disabled = points.length >= 32;
	$("duplicate-point").disabled = !point || points.length >= 32;
	$("delete-point").disabled = !can_delete_point(points.length);
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
	$("status-body-count").textContent = points.length;
	$("status-aabb-count").textContent = generated_aabb_points().length;
	$("status-muzzle").textContent = weapon_model ? active_weapon_key : "None";
	$("status-selected").textContent = points[selected_index]?.name ?? "None";
	$("status").textContent = status_extra || (model ? "Studio ready." : "Load a local SAS model to begin.");
	$("model-status").textContent = model_status;
	$("model-status").classList.toggle("ready", Boolean(model));
}

function update_scene()
{
	selected_index = points.length ? Math.min(Math.max(selected_index, 0), points.length - 1) : -1;
	set_model_opacity();
	draw_points();
	render_point_editor();
	update_status();
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
	update_scene();
}

function apply_tools_preview(gltf)
{
	const clip = THREE.AnimationClip.findByName(gltf.animations || [], "tools_preview");
	if (!clip)
	{
		return false;
	}
	const mixer = new THREE.AnimationMixer(model);
	mixer.clipAction(clip).play();
	mixer.setTime(0);
	return true;
}

function apply_readable_materials()
{
	model.traverse((node) =>
	{
		if (!node.isMesh)
		{
			return;
		}
		node.material = new THREE.MeshStandardMaterial({
			color: 0x646a70,
			roughness: 0.82,
			metalness: 0.0
		});
	});
}

function hand_parent()
{
	return model?.getObjectByName("hand_R") || model?.getObjectByName("hand_r") || model;
}

function clear_weapon()
{
	if (!weapon_model)
	{
		return;
	}
	weapon_model.parent?.remove(weapon_model);
	weapon_model = null;
}

function update_weapon_transform()
{
	if (!weapon_model)
	{
		return;
	}
	weapon_model.position.copy(source_to_three({
		x: read_number("weapon-x"),
		y: read_number("weapon-y"),
		z: read_number("weapon-z")
	}));
	weapon_model.rotation.set(
		degrees_to_radians(read_number("weapon-rx")),
		degrees_to_radians(read_number("weapon-ry")),
		degrees_to_radians(read_number("weapon-rz"))
	);
	const scale = Math.max(0.01, read_number("weapon-scale"));
	weapon_model.scale.setScalar(scale);
	draw_muzzle_point();
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
	active_weapon_key = key;
	if (!key)
	{
		status_extra = model ? "Weapon preview cleared." : "No local SAS model loaded.";
		draw_muzzle_point();
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
	await new Promise((resolve, reject) =>
	{
		loader.load(url, (gltf) =>
		{
			weapon_model = gltf.scene;
			hand_parent().add(weapon_model);
			apply_weapon_grip(key);
			update_weapon_transform();
			status_extra = `Weapon loaded: ${key}`;
			update_status();
			resolve();
		}, undefined, reject);
	});
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
	return new Promise((resolve, reject) =>
	{
		loader.load(url, (gltf) =>
		{
			if (model)
			{
				clear_weapon();
				scene.remove(model);
			}
			model = gltf.scene;
			model.rotation.set(0, 0, 0);
			apply_readable_materials();
			scene.add(model);
			const posed = apply_tools_preview(gltf);
			set_model_opacity();
			model_status = "Model loaded";
			status_extra = `SAS loaded: ${url}${posed ? " (tools_preview pose)" : ""}`;
			update_scene();
			resolve();
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
		manifest_models = manifest.models || {};
		if (manifest.models?.ct_sas)
		{
			model_status = "Model loading";
			status_extra = "Loading local SAS GLB...";
			update_status();
			await load_model_from_url(manifest.models.ct_sas);
			return;
		}
		throw new Error("manifest has no ct_sas model");
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
}

function install_ui()
{
	for (const button of document.querySelectorAll("#load-sas, [data-load-model]"))
	{
		button.addEventListener("click", () => $("sas-file").click());
	}
	$("import-los").addEventListener("click", () => $("import-json").click());
	$("reset-camera").addEventListener("click", reset_camera);
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
	for (const id of ["weapon-x", "weapon-y", "weapon-z", "weapon-rx", "weapon-ry", "weapon-rz", "weapon-scale"])
	{
		$(id).addEventListener("input", () =>
		{
			update_weapon_transform();
			update_range_labels();
		});
	}
	$("sas-file").addEventListener("change", (event) =>
	{
		const file = event.target.files[0];
		if (file)
		{
			load_model_from_url(URL.createObjectURL(file));
		}
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

function install_picking()
{
	const raycaster = new THREE.Raycaster();
	const pointer = new THREE.Vector2();
	renderer.domElement.addEventListener("pointerdown", (event) =>
	{
		const rect = renderer.domElement.getBoundingClientRect();
		pointer.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
		pointer.y = -(((event.clientY - rect.top) / rect.height) * 2 - 1);
		raycaster.setFromCamera(pointer, camera);
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
	for (const id of ["load-sas", "import-los", "export-toggle", "points-list", "point-name", "scene-card", "points-card"])
	{
		expect(Boolean($(id)), `redesigned control: ${id}`);
	}

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
	renderer = new THREE.WebGLRenderer({canvas: $("view"), antialias: true});
	renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
	renderer.setSize(window.innerWidth, window.innerHeight);

	scene = new THREE.Scene();
	scene.background = new THREE.Color(0xeff1f1);
	camera = new THREE.PerspectiveCamera(58, window.innerWidth / window.innerHeight, 0.02, 3000);
	camera.position.set(4.2, 3.0, 6.2);

	orbit = new OrbitControls(camera, renderer.domElement);
	orbit.target.set(0, 1.0, 0);
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
		const value = three_to_source(transform.object.position);
		points[selected_index].x = value.x;
		points[selected_index].y = value.y;
		points[selected_index].z = value.z;
		render_point_editor();
		update_status();
	});
	scene.add(transform);

	loader = new GLTFLoader();
	marker_group = new THREE.Group();
	aabb_group = new THREE.Group();
	muzzle_group = new THREE.Group();
	scene.add(aabb_group, marker_group, muzzle_group);
	scene.add(new THREE.HemisphereLight(0xffffff, 0xc7cbd0, 3.0));
	scene.add(new THREE.GridHelper(4, 8, 0x9ca2a7, 0xd5d8da));

	window.addEventListener("resize", () =>
	{
		camera.aspect = window.innerWidth / window.innerHeight;
		camera.updateProjectionMatrix();
		renderer.setSize(window.innerWidth, window.innerHeight);
	});
}

function animate()
{
	requestAnimationFrame(animate);
	renderer.render(scene, camera);
}

async function main()
{
	init_scene();
	install_ui();
	install_picking();
	animate();
	await load_preset(k_default_preset);
	run_self_checks();
	load_manifest();
}

main().catch((error) =>
{
	$("self-check").textContent = `Self-check failed: ${error.message || error}`;
	$("self-check").className = "pill bad";
});
