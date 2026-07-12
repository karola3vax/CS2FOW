# CS2FOW Visibility Studio

Local-only LOS Point Editor for tuning CS2FOW target samples against the exported CS2 SAS model. It opens directly as a fixed desktop workspace: scene controls are on the left, editable points are on the right, and file actions are in the top bar.

The editor shows:

- the local SAS GLB model
- optional USP-S, M4A1-S, and AWP previews attached to the right hand
- 8 generated axis-aligned bounding box (AABB) fallback points
- 15 editable body LOS points
- one dynamic weapon muzzle point when a weapon is selected

It does not load maps, bounding volume hierarchy (BVH8) files, rays, hit triangles, or runtime worker state. Valve model exports are local assets and must not be committed.

## Export Local Assets

```powershell
python tools/visibility_point_editor/export_assets.py --game "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo"
```

This writes ignored files under:

```text
tools/visibility_point_editor/local_assets/
```

The weapon preview and muzzle point are only for tuning and visual context. Use the Weapon tab to choose a weapon and adjust its local position, rotation, and scale. Use the Export menu to copy or download the unchanged LOS JSON format.

## Run

```powershell
cd tools/visibility_point_editor
python -m http.server 8765
```

Open:

```text
http://127.0.0.1:8765/viewer.html
```

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

Runtime integration is intentionally separate. CS2FOW combines these body points with generated axis-aligned bounding box corners and a muzzle sample in `visibility_sampling.cpp`; `check_points.py` verifies that the body-point order still matches.
