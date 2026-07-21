import {Bvh8Map, Bvh8SurfaceMap} from "./bvh8.js";
import {FpsSimulation, FPS_TICK_RATE, trace_capsule_target} from "./fps_runtime.js";

let map = null;
let cachedPackets = null;
let simulation = null;
let simulationTimer = null;
let simulationPaused = true;
let unitsPerMeter = 1;
const MAX_RENDER_TRIANGLES = 300000;

function stop_simulation()
{
	if (simulationTimer !== null) clearInterval(simulationTimer);
	simulationTimer = null;
	simulation = null;
	simulationPaused = true;
}

function publish_simulation()
{
	if (!simulation || simulationPaused) return;
	try
	{
		const state = simulation.step();
		const transfer = [];
		for (const visibility of state.visibilities)
		{
			transfer.push(visibility.origins.buffer, visibility.rays.buffer, visibility.blocked.buffer);
			if (visibility.traversal)
			{
				visibility.traversal.positions = map.triangle_positions_for(visibility.traversal.triangles, unitsPerMeter);
				transfer.push(visibility.traversal.triangles.buffer, visibility.traversal.positions.buffer);
			}
		}
		for (const event of state.events)
		{
			if (event.cells) transfer.push(event.cells.buffer);
		}
		self.postMessage({type: "play-state", state}, transfer);
	}
	catch (error)
	{
		stop_simulation();
		self.postMessage({type: "error", operation: "play", message: error.message || String(error)});
	}
}

self.addEventListener("message", (event) =>
{
	const message = event.data;
	try
	{
		if (message.type === "load")
		{
			stop_simulation();
			const loaded = new Bvh8Map(message.buffer);
			unitsPerMeter = message.unitsPerMeter;
			const positions = loaded.triangle_positions(unitsPerMeter, MAX_RENDER_TRIANGLES);
			map = loaded;
			cachedPackets = null;
			self.postMessage({type: "loaded", id: message.id,
				metadata: {...map.metadata, renderedTriangleCount: positions.length / 9}, positions}, [positions.buffer]);
		}
		else if (message.type === "load-surfaces" && map)
		{
			map.surfaceMap = new Bvh8SurfaceMap(message.buffer, map.metadata);
			self.postMessage({type: "surfaces-loaded", metadata: map.surfaceMap.metadata});
		}
		else if (message.type === "trace" && map)
		{
			const targetSets = message.targetSets || [message.targets];
			const caches = Array.isArray(cachedPackets) ? cachedPackets : [];
			const deadline = (globalThis.performance?.now?.() ?? Date.now()) + 75;
			const results = targetSets.map((target, index) => trace_capsule_target(map, message.viewer, target,
				{cache: caches[index], deadline, debug: true}));
			cachedPackets = results.map((result) => result.cache);
			const transfer = [];
			for (const result of results)
			{
				delete result.cache;
				transfer.push(result.origins.buffer, result.rays.buffer, result.blocked.buffer);
			}
			self.postMessage({
				type: "traced",
				id: message.id, results
			}, transfer);
		}
		else if (message.type === "pick" && map)
		{
			const hit = map.segment_hit(message.origin, message.target);
			self.postMessage({type: "picked", id: message.id, mode: message.mode,
				mapId: message.mapId, point: hit?.point || null});
		}
		else if (message.type === "clear")
		{
			stop_simulation();
			map = null;
			cachedPackets = null;
		}
		else if (message.type === "play-start" && map)
		{
			stop_simulation();
			simulation = new FpsSimulation(map, message.settings);
			simulationPaused = Boolean(message.paused);
			simulationTimer = setInterval(publish_simulation, 1000 / FPS_TICK_RATE);
			self.postMessage({type: "play-started"});
		}
		else if (message.type === "play-stop")
		{
			stop_simulation();
		}
		else if (message.type === "play-pause" && simulation)
		{
			simulationPaused = Boolean(message.paused);
		}
		else if (message.type === "play-input" && simulation)
		{
			simulation.set_input(message.buttons || {});
		}
		else if (message.type === "play-look" && simulation)
		{
			simulation.set_look(message.yaw, message.pitch);
		}
		else if (message.type === "play-targets" && simulation)
		{
			simulation.set_targets(message.targetSets || message.targets);
		}
		else if (message.type === "play-debug" && simulation)
		{
			simulation.set_debug(message.enabled);
		}
		else if (message.type === "play-traversal" && simulation)
		{
			simulation.request_traversal();
		}
		else if (message.type === "play-speed" && simulation)
		{
			simulation.set_player_speed(message.value);
		}
		else if (message.type === "play-throw" && simulation)
		{
			simulation.throw_grenade(message.kind, message.origin, message.direction, message.speed);
		}
		else if (message.type === "play-shot" && simulation)
		{
			simulation.fire_visual(message.direction);
		}
	}
	catch (error)
	{
		self.postMessage({type: "error", operation: message.type, id: message.id, message: error.message || String(error)});
	}
});
