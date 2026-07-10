from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


VIEWER = Path(__file__).resolve().parent
ROOT = VIEWER.parents[1]
LOCAL_ASSETS = VIEWER / "local_assets"

MODELS = {
    "ct_sas": "agents/models/ctm_sas/ctm_sas.vmdl_c",
    "usp_silencer": "weapons/models/usp_silencer/weapon_pist_usp_silencer.vmdl_c",
    "m4a1_silencer": "weapons/models/m4a1_silencer/weapon_rif_m4a1_silencer.vmdl_c",
    "awp": "weapons/models/awp/weapon_snip_awp.vmdl_c",
}


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
    return glbs[0].relative_to(VIEWER).as_posix()


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
    manifest = {"game": str(args.game), "models": {}, "resources": MODELS}
    for key, resource in MODELS.items():
        manifest["models"][key] = export_model(args.vrf, args.game, key, resource)

    (LOCAL_ASSETS / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {LOCAL_ASSETS / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
