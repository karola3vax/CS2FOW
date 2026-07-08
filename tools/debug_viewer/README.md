# CS2FOW Debug Viewer

Local-only viewer for CS2FOW visibility sampling. It renders exported CS2 player models as reference, then overlays the collision box, sample points, prediction envelope, 160 visibility segments, and optional `.bvh8` blocking.

Valve model exports are local assets. Do not commit them.

## Export Local Models

```powershell
python tools/debug_viewer/export_assets.py --game "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo"
```

This writes ignored files under:

```text
tools/debug_viewer/local_assets/
```

## Run

```powershell
cd tools/debug_viewer
python -m http.server 8765
```

Open:

```text
http://127.0.0.1:8765/viewer.html
```

Use the BVH8 file picker to load a bake such as:

```text
data/maps/de_mirage.bvh8
```

Use the optional Map OBJ picker for visual context. The OBJ must come from a baker debug export, for example:

```powershell
tools\cs2fow_baker.exe --game "C:\Users\karola3vax\Desktop\server" --map de_mirage --debug-obj "%TEMP%\de_mirage.obj"
```

The OBJ is only drawn as a transparent wireframe. The loaded `.bvh8` still decides which rays are blocked.

The viewer uses the current CS2FOW constants: `200ms + ping * 2`, `500ms` cap, speed-stepped `160u` maximum peek margin, `12u` target inflation, `24u` observer shoulder offset from yaw, and `24u` vertical observer offset.
