<div align="center">

# CS2FOW

### Server-side anti-wallhack for Counter-Strike 2 community servers

[![Version](https://img.shields.io/github/v/release/karola3vax/CS2FOW?style=for-the-badge&label=version)](https://github.com/karola3vax/CS2FOW/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/karola3vax/CS2FOW/total?style=for-the-badge&label=downloads)](https://github.com/karola3vax/CS2FOW/releases)
[![Issues](https://img.shields.io/github/issues/karola3vax/CS2FOW?style=for-the-badge&label=issues)](https://github.com/karola3vax/CS2FOW/issues)
[![License](https://img.shields.io/github/license/karola3vax/CS2FOW?style=for-the-badge&label=license)](LICENSE)

![CS2FOW demonstration](docs/CS2FOW.gif)

</div>

CS2FOW is a server-side anti-wallhack plugin for Counter-Strike 2 community servers. It withholds a living enemy's visual entities when solid map walls fully block that enemy from a living opponent.

## FAQ

### What is CS2FOW?

CS2FOW is a server-side anti-wallhack plugin for Counter-Strike 2 community servers. When solid map walls completely hide a living enemy, it can stop sending that enemy's visual entities to one opposing player.

It is not a screen filter and does not run on the player's computer.

### Does it work in Premier or Valve matchmaking?

No. It requires a community or dedicated server running Metamod:Source. Valve would need to add a similar system for official matchmaking.

### Do players install anything or risk a VAC ban?

Players install nothing. CS2FOW runs only on the server and does not modify or inject into the CS2 client.

### Can a cheat bypass it?

A cheat cannot recover an exact live enemy position that the server never sent. It can still use sound, teammate information, last-known positions, common prefire spots, and other game clues. CS2FOW reduces the main wallhack data source; it does not make all cheating impossible.

### What exactly gets hidden?

For a living enemy, CS2FOW treats the pawn, current weapons, wearables, carried hostage prop, and directly linked owner/effect entities as one visual group.

Teammates, self, dead players, spectators, and HLTV remain unfiltered.

### Can players still wallbang a hidden enemy?

Yes. The enemy remains fully present on the server. Movement, hit registration, penetration, damage, and game rules continue normally.

### Does it block radar cheats or sound ESP?

It reduces radar cheats that depend on live enemy entity positions. It does not remove footsteps, gunshots, teammate information, bomb information, or every other clue.

### What about smokes, doors, breakables, and moving props?

Smokes are visibility blockers. CS2FOW copies CS2's live smoke voxel grid and checks wall-clear rays against it, including changing edges, overlap, growth, fade, and grenade-made holes. Doors, breakables, and moving props are not blockers in the current preview because the baked map contains only static geometry.

### How does it avoid enemies appearing too late around corners?

The worker checks body points, bounding-box corners, and the held weapon muzzle. Movement prediction starts gradually above 75 units per second and is fully active at 100. It looks ahead by 75 ms plus 1.5 times the recipient's current round-trip latency, capped at 375 ms and 96 movement units per player. Left/right shoulder origins also grow with recipient RTT from 24 to 128 units by default. A 16 ms hold reduces one-tick corner flicker.

When the worker sees an enemy again, CS2FOW stops removing that enemy from normal snapshots immediately.

### Does it run expensive engine traces every tick?

No. Map collision is baked into a compact BVH8 file. A background worker casts rays against that data, while `CheckTransmit` only reads the finished answer.

### Do custom and Workshop maps work?

Yes, when their physics can be baked. The server can bake a mounted map automatically, and the separate Bake Service can prepare downloadable `.bvh8` and `.json` files from a public Workshop item.

### What happens when a map changes?

CS2FOW compares the current source CRC and size with the stored bake. A mismatched or outdated bake is rejected, the plugin shows players normally, and an automatic rebake can create current data.

### What does “fail open” mean?

If CS2FOW is missing information or is not sure that filtering is safe, it sends players normally instead of hiding them.

## Quickstart

1. Install [Metamod:Source](https://www.sourcemm.net/) on the CS2 server.
2. Download the Windows or Linux `0.2.0-preview` core ZIP from the [CS2FOW releases page](https://github.com/karola3vax/CS2FOW/releases).
3. Extract the ZIP into the server's `game/csgo` folder. Keep the folders inside the ZIP unchanged.
4. Start the server and load a map.
5. Run `cs2fow_status` in the server console.

The first load may say that an automatic bake is in progress. The server remains playable and CS2FOW shows everyone normally until that bake finishes and validates. The optional official-maps ZIP avoids this first-load wait for the maps included in it.

CS2FOW requires an x86-64 CPU with AVX support exposed to the operating system. Some virtual-server providers hide AVX even when the physical CPU supports it.

The plugin also verifies that its private gamedata matches the loaded CS2 server binary by file size and CRC32. After an unknown Valve update, CS2FOW stays disabled and sends everyone normally until updated gamedata is installed.

## What CS2FOW does

CS2FOW runs only on the server. Players install nothing.

When a living enemy is fully behind solid map geometry, CS2FOW may remove that enemy's current visual group from the primary entity-send list for one recipient. The visual group includes the pawn, weapons, wearables, a carried hostage prop, and directly linked owner/effect entities found by the plugin. The enemy still exists on the server, so movement, hit registration, damage, wall penetration, and game rules continue normally.

This removes the main live position data used by wallhacks. It does not make every form of cheating impossible: sound, teammate information, last-known positions, and other game clues still exist.

Live smoke also blocks visibility rays. An HE detonation opens a 100-unit viewing channel through an affected smoke for three seconds by default. Baked walls between the blast and sight line still block that opening, and overlapping smoke clouds are checked separately.

HLTV, spectators, dead players, and a player viewing themself are not filtered. Teammates remain unfiltered by default; `cs2fow_filter_teammates 1` applies the same visibility rules to their complete visual groups and may also remove client-side teammate markers and radar information.

## The six-step runtime flow

1. **Load map:** find the mounted map VPK and identify its physics data.
2. **Bake walls:** turn static collision triangles into a compact bounding volume hierarchy with eight children per node (BVH8).
3. **Collect player points:** copy each living player's position, movement, body bounds, eye direction, latency, and held-weapon class on the game thread.
4. **Cast rays:** a background worker checks eight safe recipient points against up to forty-eight target points. Targets include separate current and future axis-aligned bounding box corners, fifteen custom body points, and a weapon-muzzle point. Future positions stop at baked walls instead of passing through them.
5. **Decide visibility:** one open ray reveals the target. A short hold keeps a recently revealed target visible to reduce corner pop-in.
6. **Withhold hidden entities:** `CheckTransmit` reads the finished decision and clears only the verified primary transmit-list bits for a hidden target's visual group.

The worker receives copied numbers only. It never reads live CS2 objects.

## Baked map geometry

This is the simplified static collision mesh CS2FOW uses for its ray checks:

<div align="center">

![Static map geometry seen by CS2FOW](docs/scan_cbbl.png)

</div>

## Installation and configuration

The core ZIP installs these operator-facing paths:

```text
addons/cs2fow/bin/
addons/cs2fow/gamedata/cs2fow.games.txt
addons/metamod/cs2fow.vdf
cfg/cs2fow.cfg
tools/cs2fow_baker
tools/vrf/
```

Defaults in `cfg/cs2fow.cfg` are:

| Setting | Default | Meaning |
| --- | ---: | --- |
| `cs2fow_enable` | `1` | Enable filtering when all required data is valid. |
| `cs2fow_smoke_occlusion` | `1` | Use CS2's live smoke grid. Smoke alone fails open if private smoke data is unavailable. |
| `cs2fow_he_clear_radius_units` | `100` | Radius of the temporary viewing channel created by an HE. Set to `0` to disable HE clearing. |
| `cs2fow_he_clear_seconds` | `3.0` | Seconds before an HE-created viewing channel closes. Set to `0` to disable HE clearing. |
| `cs2fow_filter_teammates` | `0` | Apply the same visibility filtering to living teammates. |
| `cs2fow_update_interval_ms` | `1` | Minimum time between player snapshots sent to the worker. |
| `cs2fow_base_lookahead_ms` | `75` | Fixed movement lookahead before the RTT contribution. |
| `cs2fow_rtt_lookahead_scale` | `1.5` | Multiplier applied to the recipient's current round-trip latency. |
| `cs2fow_max_lookahead_ms` | `375` | Maximum movement/latency lookahead. Set to `0` to disable movement prediction. |
| `cs2fow_max_prediction_units` | `96` | Maximum predicted movement for each player. Set to `0` to disable movement prediction. |
| `cs2fow_shoulder_base_units` | `24` | Minimum left/right shoulder-origin distance. |
| `cs2fow_shoulder_rtt_scale` | `0.64` | Shoulder units added per millisecond of recipient RTT. |
| `cs2fow_max_shoulder_units` | `128` | Maximum left/right shoulder-origin distance. |
| `cs2fow_visibility_hold_ms` | `16` | Minimum time a newly visible pair stays visible. |
| `cs2fow_debug` | `0` | Collect real primary-list clears for later inspection. |

Existing custom configs must add the two HE-clearance controls shown above. They must also update the previous `50`/`150` lookahead defaults and add the RTT, prediction-distance, and shoulder controls. Older configs must replace `cs2fow_min_lookahead_ms` with `cs2fow_base_lookahead_ms` and remove `cs2fow_peek_margin_units`.

Automatic baking needs write access to `addons/cs2fow/data/maps`. On Linux, the packaged baker and VRF program must remain executable.

## Status and debug commands

`cs2fow_status` prints whether the plugin is active, why it is fail open when inactive, map and bake details, worker, snapshot-capture, and CheckTransmit timings, true snapshot age, pair counts, smoke and HE-event availability, active HE events, teammate-filtering state, and automatic-bake progress.

`cs2fow_debug 1` starts silent evidence collection. It adds a record only when CS2FOW found a primary transmit bit set immediately before clearing it. It does not print every clear.

Use:

```text
cs2fow_entity                 list buffered records, newest first
cs2fow_entity <edict>         show records for one entity index
cs2fow_entity clear           clear the evidence buffer
```

Records show the classname, relationship to the pawn (`direct`, `owner_link`, `effect_link`, or `owner+effect`), handles, recipients, reasons, count, and age. Turning debugging off stops new records but keeps existing ones until a map/plugin reset or `clear`.

## Fail-open behavior and known limits

“Fail open” means CS2FOW sends entities normally whenever it is unsure. Examples include missing or stale worker results, missing map data, a corrupt or outdated bake, changed map CRC/size, invalid live player state, a missing visual-group member, unavailable schema data, and unsupported CPU features.

Important limits:

- Private offsets are accepted only for the exact Windows or Linux server binary recorded in the bundled gamedata. Unknown builds disable CS2FOW instead of attempting unsafe reads.
- Visibility uses baked static map geometry. Smokes, doors, breakables, props, particles, projectiles, and other moving blockers are not occluders.
- Movement prediction targets normal competitive/casual CS2 movement. It ramps from zero between 75 and 100 horizontal units per second, caps speed at 350, and caps each player's offset at 96 units, so surf, KZ, and unusually fast boosts can appear late.
- Shoulder origins deliberately widen with recipient RTT to reduce high-ping corner pop-in. Larger values reveal enemies farther around corners, including while the recipient is stationary.
- Sound events, bomb information, teammate information, and other non-entity clues are not filtered.
- Version 2 and older BVH8 files are rejected. The plugin automatically rebakes when it can and remains fail open otherwise.
- `CheckTransmit` changes only `m_pTransmitEntity`, the verified primary send list. Full-update snapshots are never filtered.
- Builds and unit tests cannot reproduce a live CS2 transmit list or prove that a server will never hit a game-engine entity-copy crash. Live-server packet testing is a separate validation step.

## Troubleshooting

**`cs2fow_status` says AVX is missing:** check that the host exposes AVX and operating-system AVX state to the virtual machine. A physical CPU supporting AVX is not enough if the guest cannot see it.

**Automatic bake says permission denied on Linux:** restore execute permission and check mount/container policy:

```sh
chmod +x game/csgo/tools/cs2fow_baker
chmod +x game/csgo/tools/vrf/linux64/Source2Viewer-CLI
```

Automatic-baker and VRF failures include the final 8 KiB of their combined output so the useful error is available without successful-bake console spam.

**`cs2fow_status` says the server binary does not match:** install a CS2FOW build with gamedata verified for the current Valve server binary. There is no unsafe override.

**A bake is rejected after a CS2 update:** this is expected when Valve changes the source VPK. CS2FOW compares the stored source CRC and size to the mounted map and rebakes instead of trusting stale walls.

**A bug needs investigation:** include the CS2FOW version, operating system, map, `cs2fow_status` output, nearby server logs, player state (alive/dead/spectating/bot), and a short clip for visibility or pop-in issues.

## Developer links

- [Code tour](docs/CODE_TOUR.md): plain-language architecture, threads, safety rules, and build/release steps.
- [Visibility point editor](tools/visibility_point_editor/README.md): view and tune the body, bounds, and muzzle samples.
- [Bake Service](https://github.com/karola3vax/CS2FOW-Bake-Service): bounded public Workshop-map baking service.

Manual baker examples:

```text
cs2fow_baker --game <cs2-root> --map de_dust2 --output de_dust2.bvh8
cs2fow_baker --list-maps --vpk <outer_dir.vpk>
cs2fow_baker --game <cs2-root> --map workshop/123/de_example --vpk <outer_dir.vpk> --output de_example.bvh8
```

Generated map data is derived from Counter-Strike 2 game data and is covered by `DATA_NOTICE`, not the project's MIT code license. See `LICENSE`, `THIRD_PARTY_NOTICES`, and `DATA_NOTICE`.
