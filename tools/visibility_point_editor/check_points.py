"""Fail when the editor preset no longer matches the points compiled into CS2FOW."""

import json
import math
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PRESET = Path(__file__).with_name("default_sas_visibility_points.json")
SOURCE = ROOT / "src" / "core" / "visibility_sampling.cpp"


def main() -> None:
    preset = json.loads(PRESET.read_text(encoding="utf-8"))
    points = preset["points"]
    assert preset.get("version") == 1
    assert preset.get("coordinate_space") == "source_local"
    assert preset.get("model") == "ctm_sas"
    assert preset["point_count"] == len(points) == 15
    assert len({point["name"] for point in points}) == len(points)
    assert all(math.isfinite(float(point[axis])) for point in points for axis in ("x", "y", "z"))

    text = SOURCE.read_text(encoding="utf-8")
    block = text.split("constexpr std::array<body_point, 15> k_body_points {{", 1)[1].split("}};", 1)[0]
    compiled = [tuple(map(float, match)) for match in re.findall(
        r"\{\{([-+0-9.eE]+)f,\s*([-+0-9.eE]+)f,\s*([-+0-9.eE]+)f\}\}", block
    )]
    expected = [(float(point["x"]), float(point["y"]), float(point["z"])) for point in points]
    assert len(compiled) == len(expected)
    assert all(abs(left - right) < 1e-5 for a, b in zip(compiled, expected) for left, right in zip(a, b))
    print("visibility point preset matches compiled CS2FOW points")


if __name__ == "__main__":
    main()
