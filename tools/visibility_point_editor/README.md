# CS2FOW Visibility Studio

Local-only LOS Point Editor, baked-wall simulator, and first-person CS2FOW test range. It opens directly as a compact desktop workspace: file and Preview/Edit/Play controls are in the top bar, while Points, Model, Weapon, Bounds, and Map share one inspector on the right.

The editor shows:

- local CT SAS and Phoenix GLB models
- optional USP-S, M4A1-S, and AWP previews attached to the right hand
- 8 generated axis-aligned bounding box (AABB) fallback points
- 15 editable body LOS points
- one dynamic weapon muzzle point when a weapon is selected
- a second SAS model fixed 256 units away before a map loads, then placeable in Source 2 world coordinates
- a runtime preview where all 15 body points and their rays follow the same SAS bones used by the plugin
- direct BVH8 loading with simple gray collision walls and live green/blocked-red wall rays
- a 64 Hz first-person test range with map collision, a site-defending Phoenix bot, geometry-aware smoke, HE/bullet clearing, real local CS2 sounds and particle artwork, and Real/Debug visibility

Choose the standing, walking, running, crouching, jumping, firing, or reloading pose under Model. The list follows the selected weapon, uses the matching real SAS clips, and cross-fades between them. Play keeps directional movement, draw, fire, reload, knife, and grenade actions on the third-person player instead of replacing them with a generic forward run; reload sounds follow the current CS2 clip events. Preview moves the runtime points, weapon, muzzle, and rays with the SAS bones; Edit restores the standing layout and point controls. Play starts from the placed viewer and target and writes their final positions back when you leave it. The Studio starts in Edit when your system requests reduced motion.

The Map tab loads version 3 baked visibility files (BVH8) directly in a background worker. Local Mirage loads automatically. Ancient and other maps can be selected with **Load BVH8**. Recognized Ancient, Anubis, Inferno, Mirage, Nuke, and Overpass maps start with a default spawn pair; every map still supports click or numeric placement. Toggle the filled collision walls to wireframe when you need to see through the full map.

The worker uses the same baked triangles and wall decisions as the runtime, including the ping-scaled shoulder distance and optional W/A/S/D intention origin. Play adds controlled smoke and HE inputs so you can exercise the runtime density and clearing rules. Smoke spreads only through connected baked-map space, bullets briefly carve narrow channels, and HE clears nearby cells for 2.5 seconds. The browser uses real locally exported CS2 smoke and explosion artwork, but Three.js does not execute Valve's particle engine, so the generated cloud is still a test input rather than a copy of Valve's full smoke simulation. Valve models, navigation, sounds, and particle artwork remain ignored local assets and must not be committed.

## Play controls

Click **Play**, then click the canvas to capture the mouse. Use `W/A/S/D` to move, `Shift` to walk silently, `Ctrl` to crouch, and `Space` to jump. Press `1` for your chosen M4A1-S/AWP primary, `2` for the USP-S, `3` for the Karambit, and `4` to switch between Smoke and HE grenades. Guns use their real magazine sizes and fire rates with unlimited reserve ammo, so `R` must finish a real reload before the clip refills. Hold and release left click for a full grenade throw; right click gives an underhand throw, AWP zoom, or Karambit heavy swing. `F` inspects, `J` switches first/third person, `V` switches between the honest Real view and the full Debug view, and `C` independently shows or hides Debug rays. `Escape` releases the mouse and pauses the range.

The USP-S, M4A1-S, and AWP magazine, fire-rate, and movement values come from the [CS2 weapon spreadsheet](https://docs.google.com/spreadsheets/d/11tDzUNBq9zIX6_9Rel__fdAUezAQzSnh5AVYzCP060c/edit?gid=0#gid=0). Mouse input defaults to `m_yaw 0.022` and sensitivity `0.94`. Air movement uses Source-style directional acceleration with the 30-unit air wish cap and the Studio's existing 320-unit global speed limit.

The first-person camera renders the arms and weapon separately at `viewmodel_fov 44` with offsets `2.5 / 2 / -1`. Each weapon also has an editable first-person XYZ offset. Movement adds procedural CS-style viewmodel bob. Gunfire uses locally exported casing, muzzle, tracer, and concrete-impact artwork; the Play HUD uses the current CS2 weapon icons, bullet marks fade after two seconds, and no damage is added. Positional sounds stop at 1,250 units and fade toward that edge. The wall/smoke ray decision remains the part copied from the runtime rules.

Real mode completely hides the Phoenix and its carried weapon when every runtime sample is blocked. Its positional footsteps stay audible, matching the plugin's gameplay boundary. Debug mode keeps a translucent ghost and shows the origins, targets, rays, capsules, navigation route, smoke, and final wall/smoke reason.

## Export Local Assets

Install Node.js, Python, and the .NET 10 SDK. The exporter uses pinned glTF Transform tooling through `npx` to retain the complete CS2 world-animation library once, reuse it for SAS and Phoenix, discard unused player textures, and compact the result. Its small .NET helper is pinned to ValveResourceFormat `19.2.6339` and exports the installed maps' navigation graphs. It also exports the local first-person arms, three weapons, and a small set of real weapon, grenade, and footstep sounds.

```powershell
python tools/visibility_point_editor/export_assets.py --game "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo"
```

This writes ignored files under:

```text
tools/visibility_point_editor/local_assets/
```

Each compact player file is normally about 6 MB instead of the full export's roughly 328 MB. Source fingerprints keep unchanged local exports from being rebuilt.

The weapon preview and muzzle point are only for tuning and visual context. Use the Weapon tab to choose a weapon and adjust its local position, rotation, and scale. Use the Export menu to copy or download the unchanged LOS JSON format.

## Run

```powershell
cd C:\path\to\CS2FOW
python -m http.server 8765
```

Open:

```text
http://127.0.0.1:8765/tools/visibility_point_editor/viewer.html
```

Serving the repository root lets the Studio reach `data/maps/de_mirage.bvh8` and its matching JSON report. If Mirage is not present, the rest of the editor stays available and **Load BVH8** still works.

The editor exports only the ordered body-point preset:

```json
{
	"version": 1,
	"coordinate_space": "source_local",
	"model": "ctm_sas",
	"point_count": 1,
	"points": [{"name": "head", "x": 0, "y": 0, "z": 64}]
}
```

Copy and download reject blank or duplicate names, non-finite coordinates, and presets outside the 1-32 point limit. The final point cannot be deleted.

Runtime integration is intentionally separate. CS2FOW combines these body points with generated axis-aligned bounding box corners and a muzzle sample in `visibility_sampling.cpp`; `check_points.py` verifies that the body-point order still matches. `node tools/visibility_point_editor/check_bvh8.mjs` checks the browser BVH8 parser and tracer with a small synthetic map.

`node tools/visibility_point_editor/check_fps.mjs` checks the movement constants, capsule collision, jumping, navigation, smoke, HE timing, and current runtime ray layout. The baker's optional `--studio-surfaces <file>` output binds compact per-triangle surface IDs to one exact BVH8 payload. When that sidecar is missing or rejected, Play safely uses concrete footsteps.
