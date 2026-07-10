# CS2FOW LOS Point Editor

Local-only editor for tuning CS2FOW target LOS points against the exported CS2 SAS model.

The editor shows:

- the local SAS GLB model
- optional USP-S, M4A1-S, and AWP previews attached to the right hand
- 8 generated collision AABB fallback points
- 15 editable body LOS points
- one dynamic weapon muzzle point when a weapon is selected

It does not load maps, BVH8 files, rays, hit triangles, or runtime worker state. Valve model exports are local assets and must not be committed.

## Export Local Assets

```powershell
python tools/visibility_point_editor/export_assets.py --game "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo"
```

This writes ignored files under:

```text
tools/visibility_point_editor/local_assets/
```

The weapon preview and muzzle point are only for tuning and visual context. Use the Weapon Preview panel to choose a weapon and adjust its local position, rotation, and scale.

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
	"points": []
}
```

Runtime integration is intentionally separate. CS2FOW will later combine these body points with generated AABB fallback corners.
