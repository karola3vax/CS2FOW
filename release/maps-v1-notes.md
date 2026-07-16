# CS2FOW Official Maps v1

This is the independent, CI-friendly release stream for CS2FOW's ready-made official map data.

- 23 verified official map pairs (`.bvh8` and `.json`)
- CS2 build `24209309`
- BVH8 format `3`
- Bake recipe `1`
- Exact source-map fingerprints inside every JSON report
- ZIP hash and provenance in the attached manifest and checksum file

Extract the ZIP into your server's `game/csgo` directory. It contains the correct `addons/cs2fow/data/maps` path.

The `maps-v1` tag and its assets are permanent. A new `maps-v*` release will be created only when a covered official map, the covered map list, the bake recipe, or the BVH8 format changes. Normal CS2FOW releases will continue to bundle the same official-map data for manual installs.
