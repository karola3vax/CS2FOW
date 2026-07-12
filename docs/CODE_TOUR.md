# CS2FOW code tour

This guide follows CS2FOW in the same order that a person experiences it:

**Load map -> bake walls -> collect player points -> cast rays -> decide visibility -> withhold hidden entities**

It explains the intent of the code. The engine and file-format details are still in the source files beside the checks they protect.

## Plain-language glossary

**Recipient:** the player whose next network snapshot CS2 is building.

**Target:** another player whom the recipient might or might not be able to see.

**Visual group:** every networked entity CS2FOW treats as the visible body of one target: pawn, weapons, wearables, carried-hostage prop, and accepted owner/effect-linked entities.

**Bake:** the `.bvh8` file made from a map's static collision triangles. Baking moves expensive map preparation out of normal play.

**Bounding volume hierarchy with eight children per node (BVH8):** a tree of boxes that quickly skips most triangles when a ray crosses the map.

**Axis-aligned bounding box (AABB):** the simple box around a player. Its eight corners are target samples.

**Valve package (VPK):** the archive format containing CS2 maps and resources.

**Snapshot:** copied player numbers given to the background worker. It is not a CS2 network snapshot.

**Visibility matrix:** a recipient-by-target table of visible/hidden decisions published by the worker.

**CheckTransmit:** the CS2 server step that decides which entity bits are present in one recipient's outgoing snapshot.

**Primary and `dont_transmit` lists:** paired entity-bit lists. Hiding an entity means marking it `dont_transmit` before removing it from the primary send list.

**Full update:** a refresh chosen by CS2 that sends a recipient complete entity state. CS2FOW recognizes it but never requests it.

**Quarantine:** a short record of a previously hidden visual group. It prevents old linked entities from escaping during an uncertain group change.

**Handle and edict:** a handle identifies one particular lifetime of an entity; an edict is its network-list index. Checking both lifetime and index helps avoid acting on a recycled entity.

**CRC:** a checksum used to catch changed or damaged VPK entries and bake payloads.

**Fail open:** show/send the target normally when any required fact is missing, invalid, changed, or stale.

## Folder map

| Path | Job |
| --- | --- |
| `src/plugin/plugin.cpp` | Load/unload the plugin, register commands, execute its config, react to maps and frames, load valid bakes, and coordinate the other parts. |
| `src/plugin/game_state.cpp` | Read live CS2 players and visual groups on the game thread, then make copied worker snapshots. |
| `src/plugin/visibility_worker.*` | Own the background thread, replace pending work with the newest snapshot, cast rays, and publish results. |
| `src/plugin/transmit.cpp` | Apply lifecycle rules and visibility results to the paired primary/`dont_transmit` lists; keep quarantine and debug evidence state. |
| `src/plugin/automatic_baker.*` | Run and monitor the external baker without blocking the game thread. |
| `src/core/bvh8.cpp` | Traverse an in-memory BVH8 and answer whether a line segment hits a triangle. |
| `src/core/bvh8_format.cpp` | Validate, read, verify, and safely replace BVH8 version 3 files. |
| `src/core/builder.*` | Turn accepted triangles into BVH8 nodes and triangle packets. |
| `src/core/visibility_sampling.*` | Build recipient/target points, movement prediction, held-weapon muzzle lengths, and lookahead. |
| `src/core/vpk.*` | Parse VPK versions 1/2, list entries, extract them, and verify their CRCs. |
| `src/core/map_source.*` | Find direct or nested map physics sources and validate safe map subpaths. |
| `src/core/lifecycle_guard.h` | Fixed-size rules for player lifetimes, pair warmup, visual-group identity, and quarantine. |
| `src/core/transmit_masks.h` | Parse gamedata numbers, read the private full-update flag, and perform the paired withhold operation. |
| `src/core/transmit_debug.h` | Aggregate entity bits actually hidden by CS2FOW without allocating in `CheckTransmit`. |
| `src/core/subprocess.*` | Start external tools with argument lists, timeouts, cancellation, and captured output. |
| `src/baker/` | Command-line bake sequence and physics-GLB import. |
| `tests/` | Small assert-based tests grouped into map/BVH and visibility/transmit responsibilities. |
| `tools/visibility_point_editor/` | Local browser tool for the LOS body, AABB, and muzzle samples only. |
| `cfg/`, `gamedata/`, `data/` | Shipped settings, platform offsets, and optional map bakes. |

## Bake flow

1. `src/baker/main.cpp` checks the command arguments and safe map name. `--list-maps --vpk` stops after validated, sorted VPK discovery.
2. `find_map_source` opens the outer VPK. A direct `maps/<map>/world_physics.vmdl_c` wins. If it is absent, `maps/<map>.vpk` is the fallback.
3. `src/core/vpk.cpp` checks the VPK header, tree bounds, entry terminators, preload data, embedded/numbered archive ranges, and CRC before trusting extracted bytes. Version 2 embedded entries must stay inside its declared file-data section even when footer bytes follow it.
4. For a nested map, the C++ baker extracts the nested VPK into a temporary directory. Python and the web service do not understand or patch VPK/BVH details.
5. ValveResourceFormat exports the chosen `world_physics.vmdl_c` as a physics GLB. `glb_import.cpp` reads geometry groups and keeps the collision surfaces accepted by the bake recipe.
6. `builder.cpp` packs the accepted triangles into eight-wide packets and builds the BVH8 tree.
7. `bvh8_format.cpp` writes a version 3 file beside the destination. It reloads and verifies one rooted tree, unique reachable nodes/packets, depth, triangle totals, and payload CRC before atomically replacing the destination. A bad write leaves the previous valid bake in place.
8. The baker writes a matching `.json` report with source checksums and geometry counts. `--debug-obj` optionally writes accepted triangles for tools such as MeshLab.

The version 3 header is 256 bytes and records recipe version 1. Loading rejects unknown flags, nonzero reserved bytes, unsafe names, non-finite or reversed bounds, impossible counts, noncanonical offsets, wrong exact file size, and bad CRC before the data can become active.

## Map-load flow

After registering convars during plugin load, and again at every map start, CS2FOW asks the server to execute `cfg/cs2fow.cfg`. The bundled file sets `sv_enable_donttransmit 0`; administrators can choose mode `1` because the transmit code maintains the paired lists required by that mode.

1. The Metamod map callback or game-frame check notices a new map.
2. `change_map` stops the old worker and automatic baker, clears old map/transmit state, then asks the CS2 filesystem for mounted map-VPK candidates.
3. `find_map_source` records the selected outer/nested source entry, CRC, and size.
4. `load_bvh8` validates the installed bake. `load_map_bake` also requires the map name, source kind, CRC, and size to match the currently mounted source.
5. A matching bake starts the visibility worker. A missing, old, damaged, or mismatched bake starts the low-priority external baker when its tools are present.
6. While baking, or after any failure, `disabled_reason_` keeps transmit filtering off. A finished automatic bake is accepted only if the mounted source is still the same.

This is why a Valve map update cannot silently reuse old wall geometry.

## Game-state and worker flow

The game thread runs `hook_game_frame`. At most once per configured interval (default `1 ms`), `capture`:

1. reads controllers and pawns through resolved schema fields;
2. rejects HLTV, invalid controller/pawn links, spawning/dead players, non-T/CT teams, invalid bounds, and uncertain lifecycles;
3. copies origin, velocity, eye position/yaw, bounds, round-trip latency, team, pawn index, and held-weapon muzzle class;
4. builds/checks visual groups for lifecycle identity, but never gives live engine pointers to the worker; and
5. submits a plain copied `visibility_snapshot` with a rising sequence number.

`visibility_worker::submit` stores only the newest pending snapshot. Work does not form a backlog. The worker wakes, takes ownership of that copy, and computes a new result.

For each eligible living pair the worker:

- makes eight recipient origins: current/predicted eye, RTT-scaled left/right shoulders, predicted shoulders, and current/predicted upward points;
- makes target samples from padded AABB corners, fifteen tuned body points, and a held-weapon muzzle point;
- clips movement at baked walls and adds separate current/future boxes, body points, and muzzle points when useful movement remains;
- casts at most `8 x 48 = 384` rays, testing baked walls first and then copied live smoke, and stopping at the first open ray;
- lets an HE clear only smoke that already existed when the detonation was recorded on the same game clock;
- first tries the triangle packet that blocked the same pair's earlier ray, then traverses the BVH8 if needed; and
- holds a newly open pair visible for `cs2fow_visibility_hold_ms`.

The finished immutable result contains its sequence, capture/completion times, recipient lookahead, copied player identity, visibility matrix, timing, and pair counts. Publishing swaps a shared result; it never exposes a half-written matrix.

## CheckTransmit flow

`hook_check_transmit` is deliberately conservative:

1. Return without changes if CS2FOW is disabled, the map is not active, inputs are invalid, or the latest worker result is missing/stale.
2. Lock `transmit_state_mutex_`. This protects lifecycle, pair-baseline, quarantined-group, and debug state shared with game-frame capture and console commands. Ray traversal and file work never run under this lock.
3. First scan the recipients for CS2 full updates. For those recipients, clear stored hidden groups, but do not alter that full-update snapshot.
4. Re-read live recipient/target lifecycles and visual groups. Any mismatch with the copied worker player fails open.
5. Skip self, invalid players, and full-update snapshots. Skip teammates only when optional teammate filtering is disabled.
6. Require a stable player pair, a warmup period, and evidence that a complete current visual group was previously sent on an older worker sequence before the pair is allowed to hide.
7. When hidden, store the exact visual group. For each member whose primary bit is set, set the matching bit through the existing second `CCheckTransmitInfo` pointer, locally treated as `dont_transmit`, and only then clear the primary bit.
8. If either paired-list pointer is unavailable, change neither list and fail open. If a primary bit is already clear, leave both bits alone.
9. If rays later say visible, stop withholding the current group and let ordinary snapshots handle it; CS2FOW does not wait for or request a full update.
10. When a current group cannot be rebuilt, a still-valid quarantined old group may be withheld briefly through the same paired operation. Invalid handles/indexes are skipped rather than guessed.

Those are the only two lists CS2FOW changes. Full-update snapshots, `+16` out-of-PVS updates, and `+24` HLTV storage are untouched. Valve mode `0` uses its compatibility behavior; mode `1` consumes the explicit `dont_transmit` information maintained by the same code.

The primary `IsBitSet` check always runs because only set bits may enter the paired operation. When `cs2fow_debug` is off, clearing skips classname lookup, record search, and record update. When it is on, evidence is recorded only for a primary bit that CS2FOW actually clears. The 256-record fixed array deduplicates by entity handle, source pawn, and membership relationship; it aggregates recipients/reasons/counts without heap allocation in the hook.

## Thread and data ownership

| Thread/caller | May read live CS2 objects? | Owns or changes | Coordination |
| --- | --- | --- | --- |
| Game thread | Yes | Map state, schema reads, copied player snapshots, visual-group lifecycle state | Uses `transmit_state_mutex_` when capture touches transmit lifecycle state. |
| Visibility worker | No | One taken snapshot, ray caches, reveal holds, worker statistics, next result | `mutex_` protects pending work; `stats_mutex_` protects statistics; published result is shared immutably. |
| CheckTransmit hook | Yes, only for validation/group resolution | Paired primary/`dont_transmit` bits and transmit lifecycle/quarantine/debug state | Holds `transmit_state_mutex_`; does no BVH traversal, file I/O, process work, or heap allocation. |
| Automatic-baker thread | No live engine objects | External process and one completion record | Receives copied paths/map-source metadata; its own mutex protects status/completion. |
| Console commands | No direct player traversal | Read status or read/clear debug records | Debug commands use `transmit_state_mutex_`. |

The BVH8 data is loaded before the worker starts and remains unchanged until that worker is stopped. That gives the worker a stable read-only map tree.

## Safety rules and resets

- Missing, invalid, changed, or stale information always fails open.
- Full-update snapshots are never filtered.
- Only set primary bits and their matching verified `dont_transmit` bits are changed; either missing pointer fails open.
- The worker receives copied data and never dereferences engine objects.
- CheckTransmit uses fixed-size visual groups, caches, and debug records; it performs no heap allocation.
- Player/visual-group lifetime changes reset pair baselines and create a warmup instead of hiding immediately.
- Enabling/disabling filtering resets lifecycle, pair, hidden-group, and auxiliary-entity state but preserves collected debug evidence.
- A map change, level shutdown, or normal plugin-state reset also clears debug evidence.
- Worker start resets pending/published work, cached blocking packets, reveal holds, and timing/pair statistics.
- Automatic-baker stop cancels/joins its task and terminates its full baker/VRF process tree before old map state is discarded.

## Where to make common changes

| Change | Start here | Keep in mind |
| --- | --- | --- |
| Body, AABB, lookahead, or muzzle sampling | `src/core/visibility_sampling.cpp` | Keep `tools/visibility_point_editor/default_sas_visibility_points.json` and its check in sync for body points. |
| Player/schema field capture | `src/plugin/game_state.cpp` | Live engine reads remain on the game thread and uncertainty fails open. |
| Ray scheduling, caches, or reveal hold | `src/plugin/visibility_worker.cpp` | Worker input must stay pointer-free copied data. |
| Which target entities form a visual group | `collect_player_visual_group` in `game_state.cpp` | Fixed capacity, full-group validation, handles, and lifecycle identity protect transmit safety. |
| Withholding rules or evidence | `src/plugin/transmit.cpp` | Set `dont_transmit` before clearing a set primary bit; no filtering on full updates; no allocation in the hook. |
| VPK compatibility | `src/core/vpk.cpp` and `map_source.cpp` | Check every range/CRC and preserve direct-over-nested precedence. |
| BVH traversal math | `src/core/bvh8.cpp` | Tests cover open/blocked rays and packet caching. |
| BVH file layout | `src/core/bvh8_format.cpp` and `bvh8.h` | Validate before allocation and keep replacement atomic. |
| Physics filtering/build recipe | `src/baker/glb_import.cpp`, `src/core/builder.cpp` | Recipe changes require an intentional format/recipe decision and new bakes. |
| Operator settings/commands | `plugin.cpp`, `cfg/cs2fow.cfg`, `README.md` | Preserve the `cs2fow_*` public names. |

## Build, test, package, and release

The local default dependency layout is next to the repository:

```text
references/ambuild
references/metamod-source
references/hl2sdk-cs2
```

The exact CI commits are in `.github/workflows/build.yml`.

Windows from a developer command prompt:

```powershell
$env:PYTHONPATH = "..\references\ambuild"
New-Item -ItemType Directory -Force build | Out-Null
Set-Location build
python ..\configure.py
python -c "from ambuild2.run import cli_run; cli_run()"
Set-Location ..
.\build\cs2fow_tests\windows-x86_64\cs2fow_tests.exe
python tools\visibility_point_editor\check_points.py
python -m unittest -v tests\test_package.py
node --check tools\visibility_point_editor\viewer.js
python package.py windows-x86_64
```

Linux must be built in Valve's pinned Steam Runtime 3 Sniper SDK container. The CI workflow configures `build-linux`, runs the same tests, checks the highest required `GLIBC`, `GLIBCXX`, and `CXXABI` versions, and then runs:

```sh
python3 package.py linux-x86_64
```

For an official-map bundle, place each matching `.bvh8` and `.json` report under `data/maps`, then run:

```sh
python package.py official-maps
```

`package.py` takes the version from top-level `VERSION`. For every official map it asks `cs2fow_baker --inspect-bvh8` to fully validate the bake, then requires the JSON report's map, source, and geometry metadata to match. It also checks required license files, duplicate/unsafe ZIP entries, ZIP integrity, Linux executable modes, and retains checksums for every current-version archive already built. A full three-target run refuses incomplete final output.

Before preparing a release:

1. update `VERSION` and `CHANGELOG.md` together;
2. run Windows and Steam Runtime 3 Linux builds/tests;
3. run the point-editor checks;
4. bake and validate the intended maps from a recorded CS2 build;
5. build all three ZIPs and verify their SHA-256 values;
6. record build ID, bake recipe, fixed map list, and checksums in the release manifest; and
7. draft release notes that clearly separate automated proof from live-server validation.

Creating a tag, publishing a GitHub release, uploading archives, or deploying the Bake Service is a separate human approval step.
