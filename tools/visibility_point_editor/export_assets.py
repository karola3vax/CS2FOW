"""Export ignored local SAS/weapon model assets for the line-of-sight editor.

The script reads a user-supplied CS2 installation through ValveResourceFormat,
writes only local_assets/, and never changes runtime visibility points.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


VIEWER = Path(__file__).resolve().parent
ROOT = VIEWER.parents[1]
LOCAL_ASSETS = VIEWER / "local_assets"

MODELS = {
    "ct_sas": "agents/models/ctm_sas/ctm_sas.vmdl_c",
    "t_phoenix": "agents/models/tm_phoenix/tm_phoenix.vmdl_c",
    "viewmodel_arms": "weapons/models/shared/arms/weapon_arms.vmdl_c",
    "usp_silencer": "weapons/models/usp_silencer/weapon_pist_usp_silencer.vmdl_c",
    "m4a1_silencer": "weapons/models/m4a1_silencer/weapon_rif_m4a1_silencer.vmdl_c",
    "awp": "weapons/models/awp/weapon_snip_awp.vmdl_c",
    "knife": "weapons/models/knife/knife_karambit/weapon_knife_karambit.vmdl_c",
    "smokegrenade": "weapons/models/grenade/smokegrenade/weapon_smokegrenade.vmdl_c",
    "hegrenade": "weapons/models/grenade/hegrenade/weapon_hegrenade.vmdl_c",
    "casing_pistol": "weapons/models/shared/shells/deagle/deagle_casing.vmdl_c",
    "casing_rifle": "weapons/models/shared/shells/ak47/ak47_casing.vmdl_c",
    "casing_awp": "weapons/models/shared/shells/awp/awp_casing.vmdl_c",
}

PARTICLE_TEXTURES = {
    # These are the textures referenced by CS2's current VPCFs. The browser
    # cannot execute Source 2 particle operators, but it can render their real
    # local artwork instead of invented circles and cubes.
    "smoke_voxel": "materials/particle/smoke/smokeburst/smokeloop_i_0_sc_hardedge.vtex_c",
    "he_flame": "materials/particle/explosion_blasts/flame/explosion_blast_01_flame.vtex_c",
    "he_smoke": "materials/particle/explosion_blasts/smoke/explosion_blast_01_smoke.vtex_c",
    "he_flare": "materials/particle/particle_flares/particle_flare_001.vtex_c",
    "he_sparks": "materials/particle/sparks/sparks.vtex_c",
    "tracer": "materials/effects/spark.vtex_c",
    "bullet_hole": "materials/decals/concrete/bullethole_concrete_1_color_psd_5d8baebd.vtex_c",
}

MATERIAL_TEXTURES = {
    "ct_sas_sleeve": "characters/models/ctm_sas/sleeve/materials/sleeve_sas_color_psd_b45d8ff2.vtex_c",
}

HUD_ICONS = {
    "usp_silencer": "panorama/images/icons/equipment/usp_silencer.vsvg_c",
    "m4a1_silencer": "panorama/images/icons/equipment/m4a1_silencer.vsvg_c",
    "awp": "panorama/images/icons/equipment/awp.vsvg_c",
    "knife": "panorama/images/icons/equipment/knife_karambit.vsvg_c",
    "smokegrenade": "panorama/images/icons/equipment/smokegrenade.vsvg_c",
    "hegrenade": "panorama/images/icons/equipment/hegrenade.vsvg_c",
}

SOUNDS = {
    "usp_fire": [f"sounds/weapons/usp/usp_0{index}.vsnd_c" for index in range(1, 4)],
    "usp_draw": ["sounds/weapons/usp/usp_draw.vsnd_c"],
    "m4_fire": ["sounds/weapons/m4a1/m4a1_silencer_01.vsnd_c"],
    "m4_draw": ["sounds/weapons/m4a1/m4a1_draw.vsnd_c"],
    "awp_fire": [f"sounds/weapons/awp/awp_0{index}.vsnd_c" for index in range(1, 3)],
    "awp_draw": ["sounds/weapons/awp/awp_draw.vsnd_c"],
    "knife_draw": ["sounds/weapons/knife/knife_deploy1.vsnd_c"],
    "knife_slash": ["sounds/weapons/knife/knife_slash1.vsnd_c", "sounds/weapons/knife/knife_slash2.vsnd_c"],
    "bullet_impact": [f"sounds/physics/concrete/concrete_impact_bullet{index}.vsnd_c" for index in range(1, 6)],
    "usp_reload_out": ["sounds/weapons/usp/usp_clipout_01.vsnd_c"],
    "usp_reload_in": ["sounds/weapons/usp/usp_clipin_02.vsnd_c"],
    "usp_reload_addammo": ["sounds/weapons/usp/usp_addammo_01.vsnd_c"],
    "usp_reload_slideback": ["sounds/weapons/usp/usp_slideback_01.vsnd_c"],
    "usp_reload_slide": ["sounds/weapons/usp/usp_sliderelease_01.vsnd_c"],
    "m4_reload_out": ["sounds/weapons/m4a1/m4a1_clipout.vsnd_c"],
    "m4_reload_in": ["sounds/weapons/m4a1/m4a1_clipin.vsnd_c"],
    "m4_reload_addammo": ["sounds/weapons/m4a1/m4a1_addammo_01.vsnd_c"],
    "m4_reload_hit": ["sounds/weapons/m4a1/m4a1_cliphit.vsnd_c"],
    "awp_reload_out": ["sounds/weapons/awp/awp_clipout.vsnd_c"],
    "awp_reload_in": ["sounds/weapons/awp/awp_clipin.vsnd_c"],
    "awp_reload_hit": ["sounds/weapons/awp/awp_cliphit.vsnd_c"],
    "awp_reload_bolt_back": ["sounds/weapons/awp/awp_boltback.vsnd_c"],
    "awp_reload_bolt_forward": ["sounds/weapons/awp/awp_boltforward.vsnd_c"],
    "grenade_throw": ["sounds/weapons/hegrenade/grenade_throw.vsnd_c"],
    "he_draw": ["sounds/weapons/hegrenade/he_draw.vsnd_c"],
    "he_pin": ["sounds/weapons/hegrenade/pinpull.vsnd_c"],
    "he_bounce": ["sounds/weapons/hegrenade/he_bounce-1.vsnd_c"],
    "smoke_draw": ["sounds/weapons/smokegrenade/smokegrenade_draw.vsnd_c"],
    "smoke_pin": ["sounds/weapons/smokegrenade/pinpull.vsnd_c"],
    "smoke_bounce": ["sounds/weapons/smokegrenade/grenade_hit1.vsnd_c"],
    "smoke_emit": ["sounds/weapons/smokegrenade/smoke_emit.vsnd_c"],
    "he_detonate": [
        "sounds/weapons/hegrenade/hegrenade_detonate_02.vsnd_c",
        "sounds/weapons/hegrenade/hegrenade_detonate_03.vsnd_c",
    ],
    "ct_concrete": [f"sounds/player/footsteps/concrete_ct_{index:02}.vsnd_c" for index in range(1, 18)],
    "t_concrete": [f"sounds/player/footsteps/concrete_ct_{index:02}.vsnd_c" for index in range(1, 18)],
    "dirt": [f"sounds/player/footsteps/dirt_{index:02}.vsnd_c" for index in range(1, 15)],
    "metal": [f"sounds/player/footsteps/metal_auto_{index:02}.vsnd_c" for index in range(1, 7)],
    "wood": [f"sounds/player/footsteps/wood_{index:02}.vsnd_c" for index in range(1, 16)],
    "carpet": [f"sounds/player/footsteps/carpet_{index:02}.vsnd_c" for index in (1, 3, 4, 5, 8, 9, 11, 14, 15, 16, 17, 18, 19, 20)],
    "ladder": [f"sounds/player/footsteps/ladder_wood_{index:02}.vsnd_c" for index in range(1, 15)],
}

VIEWMODEL_ANIMATIONS = {
    "usp_silencer": {
        "draw": "animation/anims/viewmodel/pistol/_default_pistol/draw_silenced_pistol.vnmclip_c",
        "idle": "animation/anims/viewmodel/pistol/_default_pistol/idle_pistol.vnmclip_c",
        "fire": "animation/anims/viewmodel/pistol/_default_pistol/shoot1_pistol.vnmclip_c",
        "reload": "animation/anims/viewmodel/pistol/_default_pistol/reload_pistol.vnmclip_c",
        "inspect": "animation/anims/viewmodel/pistol/_default_pistol/lookat01_pistol.vnmclip_c",
    },
    "m4a1_silencer": {
        "draw": "animation/anims/viewmodel/rifle/rifle_m4a4/draw_m4a4.vnmclip_c",
        "idle": "animation/anims/viewmodel/rifle/rifle_m4a4/idle_m4a4.vnmclip_c",
        "fire": "animation/anims/viewmodel/rifle/rifle_m4a4/shoot1_m4a4.vnmclip_c",
        "reload": "animation/anims/viewmodel/rifle/rifle_m4a4/reload_m4a4.vnmclip_c",
        "inspect": "animation/anims/viewmodel/rifle/rifle_m4a4/lookat01_m4a4.vnmclip_c",
    },
    "awp": {
        "draw": "animation/anims/viewmodel/rifle/rifle_awp/draw_awp.vnmclip_c",
        "idle": "animation/anims/viewmodel/rifle/rifle_awp/idle_awp.vnmclip_c",
        "fire": "animation/anims/viewmodel/rifle/rifle_awp/shoot1_awp.vnmclip_c",
        "reload": "animation/anims/viewmodel/rifle/rifle_awp/reload_awp.vnmclip_c",
        "inspect": "animation/anims/viewmodel/rifle/rifle_awp/lookat01_awp.vnmclip_c",
    },
    "knife": {
        "draw": "animation/anims/viewmodel/knife/knife_karambit/draw_karambit.vnmclip_c",
        "idle": "animation/anims/viewmodel/knife/knife_karambit/idle1_karambit.vnmclip_c",
        "fire": "animation/anims/viewmodel/knife/knife_karambit/light_miss1_karambit.vnmclip_c",
        "fire2": "animation/anims/viewmodel/knife/knife_karambit/heavy_miss1_karambit.vnmclip_c",
        "inspect": "animation/anims/viewmodel/knife/knife_karambit/lookat01_karambit.vnmclip_c",
    },
    "smokegrenade": {
        "draw": "animation/anims/viewmodel/grenade/grenade_smokegrenade/draw_smoke.vnmclip_c",
        "idle": "animation/anims/viewmodel/grenade/grenade_smokegrenade/idle_smoke.vnmclip_c",
        "inspect": "animation/anims/viewmodel/grenade/grenade_smokegrenade/lookat01_smoke.vnmclip_c",
        "throw": "animation/anims/viewmodel/grenade/grenade_smokegrenade/throw_overhand_smoke.vnmclip_c",
    },
    "hegrenade": {
        "draw": "animation/anims/viewmodel/grenade/grenade_hegrenade/draw_hegrenade.vnmclip_c",
        "idle": "animation/anims/viewmodel/grenade/grenade_hegrenade/idle_hegrenade.vnmclip_c",
        "inspect": "animation/anims/viewmodel/grenade/grenade_hegrenade/lookat01_hegrenade.vnmclip_c",
        "throw": "animation/anims/viewmodel/grenade/grenade_hegrenade/throw_overhand_hegrenade.vnmclip_c",
    },
}

PLAYER_MODELS = {"ct_sas", "t_phoenix"}

GLTF_TRANSFORM = "@gltf-transform/cli@4.4.1"
STUDIO_ANIMATIONS = {
    "tools_preview",
    *(
        f"animation/anims/world/{kind}/_default_{kind}/{motion}_{suffix}"
        for kind, suffix in (("knife", "knife"), ("pistol", "pistol"), ("rifle", "rifle"))
        for motion in (
            "idle",
            "walk_n",
            "walk_w",
            "walk_e",
            "walk_s",
            "run_n",
            "run_w",
            "run_e",
            "run_s",
            "idle_crouch",
            "crouch_n",
            "jump_n",
        )
    ),
    "animation/anims/world/pistol/pistol_usp/shoot_usp",
    "animation/anims/world/pistol/pistol_usp/reload_usp",
    "animation/anims/world/rifle/rifle_m4a1_silencer/shoot_m4a1s",
    "animation/anims/world/rifle/rifle_m4a1_silencer/reload_m4a1s",
    "animation/anims/world/rifle/rifle_awp/shoot_awp",
    "animation/anims/world/rifle/rifle_awp/reload_awp",
    "animation/anims/world/pistol/pistol_usp/draw_usp",
    "animation/anims/world/rifle/rifle_m4a1_silencer/draw_m4a1s",
    "animation/anims/world/rifle/rifle_awp/draw_awp",
    "animation/anims/world/knife/default_ct/draw_default_ct",
    "animation/anims/world/knife/_default_knife/frontswing_knife",
    "animation/anims/world/knife/_default_knife/frontstab_knife",
    "animation/anims/world/grenade/_default_grenade/idle_grenade",
    "animation/anims/world/grenade/_default_grenade/idle_crouch_grenade",
    "animation/anims/world/grenade/_default_grenade/draw_grenade",
    "animation/anims/world/grenade/_default_grenade/pullpin_grenade",
    "animation/anims/world/grenade/_default_grenade/throw_overhand_grenade",
    "animation/anims/world/grenade/_default_grenade/throw_underhand_grenade",
}


def write_filtered_glb(source: Path, output: Path, keep_all_animations: bool) -> None:
    data = source.read_bytes()
    if len(data) < 20 or data[:4] != b"glTF" or struct.unpack_from("<I", data, 4)[0] != 2:
        raise RuntimeError(f"unsupported GLB: {source}")

    chunks: list[tuple[int, bytes]] = []
    offset = 12
    while offset + 8 <= len(data):
        length, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        chunks.append((kind, data[offset : offset + length]))
        offset += length
    if offset != len(data):
        raise RuntimeError(f"truncated GLB: {source}")

    json_index = next((index for index, (kind, _) in enumerate(chunks) if kind == 0x4E4F534A), None)
    if json_index is None:
        raise RuntimeError(f"GLB has no JSON chunk: {source}")
    document = json.loads(chunks[json_index][1].rstrip(b" \0"))
    animations = {animation.get("name"): animation for animation in document.get("animations", [])}
    missing = sorted(STUDIO_ANIMATIONS - animations.keys())
    if missing:
        raise RuntimeError(f"SAS export is missing Studio animations: {', '.join(missing)}")
    if not keep_all_animations:
        document["animations"] = [animations[name] for name in sorted(STUDIO_ANIMATIONS)]

    # The Studio replaces SAS materials with one readable gray material.
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            primitive.pop("material", None)
    for key in ("materials", "textures", "images", "samplers"):
        document.pop(key, None)

    payload = json.dumps(document, separators=(",", ":")).encode("utf-8")
    payload += b" " * (-len(payload) % 4)
    chunks[json_index] = (0x4E4F534A, payload)
    total = 12 + sum(8 + len(chunk) for _, chunk in chunks)
    with output.open("wb") as stream:
        stream.write(struct.pack("<4sII", b"glTF", 2, total))
        for kind, chunk in chunks:
            stream.write(struct.pack("<II", len(chunk), kind))
            stream.write(chunk)


def slim_sas_model(source: Path, keep_all_animations: bool) -> Path:
    npx = shutil.which("npx.cmd" if sys.platform.startswith("win") else "npx")
    if not npx:
        raise RuntimeError("Node.js/npx is required to compact the local SAS Studio model")

    source_size = source.stat().st_size
    output = source.with_name(f"{source.stem}_studio.glb")
    with tempfile.TemporaryDirectory(prefix="cs2fow-studio-") as directory:
        temporary = Path(directory)
        filtered = temporary / "filtered.glb"
        pruned = temporary / "pruned.glb"
        resampled = temporary / "studio.glb"
        write_filtered_glb(source, filtered, keep_all_animations)
        for args in (
            ("prune", filtered, pruned, "--keep-attributes", "true", "--keep-indices", "true"),
            ("resample", pruned, resampled),
        ):
            result = subprocess.run(
                [npx, "--yes", GLTF_TRANSFORM, *(str(value) for value in args)],
                text=True,
                capture_output=True,
            )
            if result.returncode != 0:
                raise RuntimeError(f"glTF Transform failed\n{result.stdout}\n{result.stderr}")
        shutil.move(resampled, output)
    if not output.is_file() or output.stat().st_size >= source_size:
        raise RuntimeError("compacted SAS model was not smaller than the original")
    source.unlink(missing_ok=True)
    for texture in source.parent.glob("*.png"):
        texture.unlink()
    return output


def default_vrf() -> Path:
    if sys.platform.startswith("win"):
        return ROOT / "tools" / "vrf" / "win64" / "Source2Viewer-CLI.exe"
    return ROOT / "tools" / "vrf" / "linux64" / "Source2Viewer-CLI"


def run_vrf(vrf: Path, game: Path, resource: str, output: Path, animations: bool) -> subprocess.CompletedProcess[str]:
    args = [
        str(vrf),
        "-i",
        str(game / "pak01_dir.vpk"),
        "-o",
        str(output),
        "--decompile",
        "--vpk_filepath",
        resource,
        "--gltf_export_format",
        "glb",
        "--gltf_export_materials",
    ]
    if animations:
        args.append("--gltf_export_animations")
    return subprocess.run(args, text=True, capture_output=True)


def export_model(vrf: Path, game: Path, key: str, resource: str) -> str:
    output = LOCAL_ASSETS / key
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)

    result = run_vrf(vrf, game, resource, output, True)
    if result.returncode != 0:
        result = run_vrf(vrf, game, resource, output, False)
    if result.returncode != 0:
        raise RuntimeError(f"VRF failed for {resource}\n{result.stdout}\n{result.stderr}")

    glbs = sorted(output.rglob("*.glb"))
    if not glbs:
        raise RuntimeError(f"VRF did not export a GLB for {resource}")
    model = slim_sas_model(glbs[0], key == "ct_sas") if key in PLAYER_MODELS else glbs[0]
    return model.relative_to(VIEWER).as_posix()


def export_sound(vrf: Path, game: Path, key: str, resource: str) -> str:
    output = LOCAL_ASSETS / "sounds" / key
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(vrf), "-i", str(game / "pak01_dir.vpk"), "-o", str(output), "--decompile", "--vpk_filepath", resource],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"VRF failed for {resource}\n{result.stdout}\n{result.stderr}")
    audio = sorted(path for pattern in ("*.wav", "*.mp3", "*.ogg") for path in output.rglob(pattern))
    if not audio:
        raise RuntimeError(f"VRF did not export browser audio for {resource}")
    return audio[0].relative_to(VIEWER).as_posix()


def export_hud_icon(vrf: Path, game: Path, key: str, resource: str) -> str:
    output = LOCAL_ASSETS / "icons" / key
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(vrf), "-i", str(game / "pak01_dir.vpk"), "-o", str(output), "--decompile", "--vpk_filepath", resource],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"VRF failed for {resource}\n{result.stdout}\n{result.stderr}")
    icons = sorted(output.rglob("*.svg"))
    if not icons:
        raise RuntimeError(f"VRF did not export an SVG for {resource}")
    return icons[0].relative_to(VIEWER).as_posix()


def export_sounds(vrf: Path, game: Path, key: str, resources: list[str]) -> list[str]:
    return [export_sound(vrf, game, f"{key}_{index}", resource) for index, resource in enumerate(resources)]


def frame_number(path: Path) -> tuple[int, ...]:
    numbers = []
    for part in path.stem.replace("_seq", "_").split("_"):
        if part.isdigit():
            numbers.append(int(part))
    return tuple(numbers)


def export_particle_texture(vrf: Path, game: Path, key: str, resource: str) -> list[str]:
    output = LOCAL_ASSETS / "particles" / key
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(vrf), "-i", str(game / "pak01_dir.vpk"), "-o", str(output), "--decompile", "--vpk_filepath", resource],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"VRF failed for {resource}\n{result.stdout}\n{result.stderr}")
    frames = sorted(output.rglob("*.png"), key=lambda path: (frame_number(path), path.name))
    sequence_zero = [path for path in frames if "_seq0_" in path.name]
    if sequence_zero:
        frames = sequence_zero
    limit = 64 if key == "smoke_voxel" else 16
    if len(frames) > limit:
        frames = [frames[round(index * (len(frames) - 1) / (limit - 1))] for index in range(limit)]
    if not frames:
        raise RuntimeError(f"VRF did not export browser texture frames for {resource}")
    return [path.relative_to(VIEWER).as_posix() for path in frames]


def export_material_texture(vrf: Path, game: Path, key: str, resource: str) -> str:
    output = LOCAL_ASSETS / "materials" / key
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(vrf), "-i", str(game / "pak01_dir.vpk"), "-o", str(output), "--decompile", "--vpk_filepath", resource],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"VRF failed for {resource}\n{result.stdout}\n{result.stderr}")
    textures = sorted(output.rglob("*.png"))
    if not textures:
        raise RuntimeError(f"VRF did not export a browser texture for {resource}")
    return textures[0].relative_to(VIEWER).as_posix()


def export_viewmodel_animation(vrf: Path, game: Path, key: str, action: str, resource: str) -> str:
    output = LOCAL_ASSETS / "animations" / key / action
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)
    result = run_vrf(vrf, game, resource, output, False)
    if result.returncode != 0:
        raise RuntimeError(f"VRF failed for {resource}\n{result.stdout}\n{result.stderr}")
    clips = sorted(output.rglob("*.glb"))
    if not clips:
        raise RuntimeError(f"VRF did not export a GLB for {resource}")
    return clips[0].relative_to(VIEWER).as_posix()


def source_fingerprint(game: Path, vrf: Path) -> str:
    digest = hashlib.sha256()
    for path in [game / "pak01_dir.vpk", vrf, *sorted((game / "maps").glob("*.vpk"))]:
        stat = path.stat()
        digest.update(str(path.resolve()).encode())
        digest.update(f"\0{stat.st_size}\0{stat.st_mtime_ns}\n".encode())
    digest.update(json.dumps(MODELS, sort_keys=True).encode())
    digest.update(json.dumps(SOUNDS, sort_keys=True).encode())
    digest.update(json.dumps(VIEWMODEL_ANIMATIONS, sort_keys=True).encode())
    digest.update(json.dumps(PARTICLE_TEXTURES, sort_keys=True).encode())
    digest.update(json.dumps(MATERIAL_TEXTURES, sort_keys=True).encode())
    digest.update(json.dumps(HUD_ICONS, sort_keys=True).encode())
    digest.update(json.dumps({"required": sorted(STUDIO_ANIMATIONS), "ct_library": "all"}).encode())
    return digest.hexdigest()


def read_bombsite_objectives(path: Path) -> dict[str, list[float]]:
    if not path.is_file():
        return {}
    objectives: dict[str, list[float]] = {}
    for block in re.split(r"\r?\n====\d+====\r?\n", path.read_text(encoding="utf-8", errors="replace")):
        if 'classname                      "env_cs_place"' not in block:
            continue
        place = re.search(r'place_name\s+"([^"]+)"', block)
        origin = re.search(r'origin\s+\[\s*([-+\d.eE]+),\s*([-+\d.eE]+),\s*([-+\d.eE]+)\s*\]', block)
        if not place or not origin:
            continue
        normalized = re.sub(r"[^a-z]", "", place.group(1).lower())
        key = "b" if normalized == "bombsiteb" else "a" if normalized == "bombsitea" else ""
        if key:
            objectives[key] = [float(origin.group(index)) for index in range(1, 4)]
    return objectives


def export_nav_graphs(vrf: Path, game: Path) -> dict[str, str]:
    maps_output = LOCAL_ASSETS / "maps"
    maps_output.mkdir(parents=True, exist_ok=True)
    project = VIEWER / "nav_exporter" / "NavExporter.csproj"
    if not project.is_file() or not shutil.which("dotnet"):
        print("warning: dotnet nav exporter unavailable; BVH fallback roaming will remain available", file=sys.stderr)
        return {}

    exported: dict[str, str] = {}
    baked_maps = {path.stem for path in (ROOT / "data" / "maps").glob("*.bvh8")}
    for map_vpk in sorted((game / "maps").glob("*.vpk")):
        map_name = map_vpk.stem
        if map_name not in baked_maps:
            continue
        with tempfile.TemporaryDirectory(prefix=f"cs2fow-nav-{map_name}-") as directory:
            result = subprocess.run(
                [str(vrf), "-i", str(map_vpk), "-o", directory, "--decompile", "--vpk_filepath", f"maps/{map_name}.nav"],
                text=True,
                capture_output=True,
            )
            nav = Path(directory) / "maps" / f"{map_name}.nav"
            if result.returncode != 0 or not nav.is_file():
                print(f"warning: {map_name} has no readable nav file", file=sys.stderr)
                continue
            subprocess.run(
                [str(vrf), "-i", str(map_vpk), "-o", directory, "--decompile", "--vpk_filepath", f"maps/{map_name}/entities/default_ents.vents_c"],
                text=True,
                capture_output=True,
            )
            output = maps_output / f"{map_name}.nav.json"
            result = subprocess.run(
                ["dotnet", "run", "--project", str(project), "-c", "Release", "--no-restore", "--", str(nav), map_name, str(output)],
                text=True,
                capture_output=True,
            )
            if result.returncode != 0:
                raise RuntimeError(f"nav export failed for {map_name}\n{result.stdout}\n{result.stderr}")
            document = json.loads(output.read_text(encoding="utf-8"))
            document["objectives"] = read_bombsite_objectives(Path(directory) / "maps" / map_name / "entities" / "default_ents.vents")
            output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
            exported[map_name] = output.relative_to(VIEWER).as_posix()
    return exported


def main() -> int:
    parser = argparse.ArgumentParser(description="Export the local CS2 SAS GLB for the CS2FOW LOS point editor.")
    parser.add_argument("--game", required=True, type=Path, help="Path to game/csgo")
    parser.add_argument("--vrf", type=Path, default=default_vrf(), help="Path to Source2Viewer-CLI")
    args = parser.parse_args()

    if not (args.game / "pak01_dir.vpk").is_file():
        raise SystemExit(f"missing pak01_dir.vpk under {args.game}")
    if not args.vrf.is_file():
        raise SystemExit(f"missing VRF CLI: {args.vrf}")

    LOCAL_ASSETS.mkdir(parents=True, exist_ok=True)
    fingerprint = source_fingerprint(args.game, args.vrf)
    manifest_path = LOCAL_ASSETS / "manifest.json"
    if manifest_path.is_file():
        previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        previous_paths = [
            *previous.get("models", {}).values(),
            *(path for value in previous.get("sounds", {}).values() for path in (value if isinstance(value, list) else [value])),
            *previous.get("maps", {}).values(),
            *(path for actions in previous.get("animations", {}).values() for path in actions.values()),
            *(path for frames in previous.get("particles", {}).values() for path in frames),
            *previous.get("materials", {}).values(),
            *previous.get("icons", {}).values(),
        ]
        if previous.get("version", 0) >= 11 and previous.get("source_fingerprint") == fingerprint and all(
            (VIEWER / path).is_file()
            for path in previous_paths
        ):
            print(f"local Studio assets are current: {manifest_path}")
            return 0

    manifest = {"version": 11, "game": str(args.game), "source_fingerprint": fingerprint, "models": {}, "sounds": {}, "animations": {}, "particles": {}, "materials": {}, "icons": {}, "maps": {}, "resources": {"models": MODELS, "sounds": SOUNDS, "animations": VIEWMODEL_ANIMATIONS, "particles": PARTICLE_TEXTURES, "materials": MATERIAL_TEXTURES, "icons": HUD_ICONS}}
    previous_current = "previous" in locals() and previous.get("source_fingerprint") == fingerprint
    for key, resource in MODELS.items():
        previous_path = previous.get("models", {}).get(key) if previous_current else None
        if previous_path and (VIEWER / previous_path).is_file():
            manifest["models"][key] = previous_path
        else:
            manifest["models"][key] = export_model(args.vrf, args.game, key, resource)
    for key, resources in SOUNDS.items():
        manifest["sounds"][key] = export_sounds(args.vrf, args.game, key, resources)
    for key, actions in VIEWMODEL_ANIMATIONS.items():
        manifest["animations"][key] = {
            action: export_viewmodel_animation(args.vrf, args.game, key, action, resource)
            for action, resource in actions.items()
        }
    for key, resource in PARTICLE_TEXTURES.items():
        manifest["particles"][key] = export_particle_texture(args.vrf, args.game, key, resource)
    for key, resource in MATERIAL_TEXTURES.items():
        manifest["materials"][key] = export_material_texture(args.vrf, args.game, key, resource)
    for key, resource in HUD_ICONS.items():
        manifest["icons"][key] = export_hud_icon(args.vrf, args.game, key, resource)
    previous_maps = previous.get("maps", {}) if "previous" in locals() else {}
    previous_version = previous.get("version", 0) if "previous" in locals() else 0
    manifest["maps"] = previous_maps if previous_version >= 6 and previous_maps and all((VIEWER / path).is_file() for path in previous_maps.values()) else export_nav_graphs(args.vrf, args.game)

    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
