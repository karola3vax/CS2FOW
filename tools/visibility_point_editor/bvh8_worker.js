import {Bvh8Map, Bvh8SurfaceMap, trace_runtime_rays} from "./bvh8.js";
import {FpsSimulation, FPS_TICK_RATE} from "./fps_runtime.js";

let map = null;
let cachedPackets = null;
let simulation = null;
let simulationTimer = null;
let simulationPaused = true;

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
			transfer.push(visibility.origins.buffer, visibility.targets.buffer, visibility.blocked.buffer);
			if (visibility.traversal) transfer.push(visibility.traversal.triangles.buffer);
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
			const loaded = new Bvh8Map(message.buffer);
			const positions = loaded.triangle_positions(message.unitsPerMeter);
			map = loaded;
			cachedPackets = null;
			self.postMessage({type: "loaded", id: message.id, metadata: map.metadata, positions}, [positions.buffer]);
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
			const results = targetSets.map((targets, index) => trace_runtime_rays(map, message.viewer, targets, caches[index]));
			cachedPackets = results.map((result) => result.cache);
			const transfer = [];
			for (let index = 0; index < results.length; ++index)
				transfer.push(results[index].origins.buffer, targetSets[index].buffer, results[index].blocked.buffer);
			self.postMessage({
				type: "traced",
				id: message.id,
				results: results.map((result, index) => ({origins: result.origins, targets: targetSets[index], blocked: result.blocked,
					clearCount: result.clearCount, visible: result.visible}))
			}, transfer);
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
			simulationPaused = false;
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
			simulation.set_targets(message.targets);
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
