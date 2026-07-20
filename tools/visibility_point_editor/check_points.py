"""Compare the editor preset with body points compiled into CS2FOW.

This read-only build check prints success or exits with an error; it prevents an
editor-only change from silently disagreeing with runtime ray targets.
"""

import json
import math
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PRESET = Path(__file__).with_name("default_sas_visibility_points.json")
SOURCE = ROOT / "src" / "core" / "visibility_sampling.cpp"
VIEWER = Path(__file__).with_name("viewer.js")


def main() -> None:
    preset = json.loads(PRESET.read_text(encoding="utf-8"))
    points = preset["points"]
    assert preset.get("version") == 1
    assert preset.get("coordinate_space") == "source_local"
    assert preset.get("model") == "ctm_sas"
    assert preset["point_count"] == len(points) == 15
    assert all(isinstance(point.get("name"), str) and point["name"].strip() for point in points)
    assert len({point["name"] for point in points}) == len(points)
    assert all(math.isfinite(float(point[axis])) for point in points for axis in ("x", "y", "z"))

    text = SOURCE.read_text(encoding="utf-8")
    block = text.split(
        "constexpr std::array<body_point, k_visibility_body_point_count> k_body_points {{",
        1,
    )[1].split("}};", 1)[0]
    compiled = [tuple(map(float, match)) for match in re.findall(
        r"\{\{([-+0-9.eE]+)f,\s*([-+0-9.eE]+)f,\s*([-+0-9.eE]+)f\}\}", block
    )]
    expected = [(float(point["x"]), float(point["y"]), float(point["z"])) for point in points]
    assert len(compiled) == len(expected)
    assert all(abs(left - right) < 1e-5 for a, b in zip(compiled, expected) for left, right in zip(a, b))

    binding_block = text.split(
        "const std::array<visibility_body_binding, k_visibility_body_point_count> k_visibility_body_bindings {{",
        1,
    )[1].split("}};", 1)[0]
    runtime_bones = re.findall(r'\{"([^"]+)"', binding_block)
    viewer_text = VIEWER.read_text(encoding="utf-8")
    viewer_block = viewer_text.split("const k_runtime_body_bones = [", 1)[1].split("];", 1)[0]
    viewer_bones = re.findall(r'"([^"]+)"', viewer_block)
    assert viewer_bones == runtime_bones

    print("visibility point preset and Studio bones match compiled CS2FOW points")


if __name__ == "__main__":
    main()
