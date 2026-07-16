<div align="center">

# CS2FOW

### Server-side anti-wallhack for Counter-Strike 2 community servers

[![Version](https://img.shields.io/github/v/release/karola3vax/CS2FOW?style=for-the-badge&label=version)](https://github.com/karola3vax/CS2FOW/releases/latest) [![Downloads](https://img.shields.io/github/downloads/karola3vax/CS2FOW/total?style=for-the-badge&label=downloads)](https://github.com/karola3vax/CS2FOW/releases) [![Issues](https://img.shields.io/github/issues/karola3vax/CS2FOW?style=for-the-badge&label=issues)](https://github.com/karola3vax/CS2FOW/issues) [![License](https://img.shields.io/github/license/karola3vax/CS2FOW?style=for-the-badge&label=license)](LICENSE)

<img src="docs/ancient.gif" width="800" alt="CS2FOW hiding players behind solid map geometry on Ancient">

<img src="docs/smokeandhegrenade.gif" width="800" alt="CS2FOW hiding players behind smoke and revealing them through an HE-cleared opening">

</div>

CS2FOW stops your server from sending an enemy's live position when walls or smoke completely hide them. A wallhack cannot draw a player your server never sent.

- **It stays on your server:** CS2FOW never runs on your players' computers.
- **Players install nothing:** they join and play normally. There is no client injection or extra download.
- **When in doubt, show the player:** if data is missing, too old, or unsafe, CS2FOW steps aside instead of hiding someone by mistake. This is called failing open.

## Across different maps

Every map gets a lightweight 3D copy of its solid walls. CS2FOW uses that copy to check who can actually see whom.

<table>
<tr>
<td width="50%" align="center">
<img src="docs/cache.gif" width="100%" alt="CS2FOW operating around the Cache A site"><br>
<strong>Cache &mdash; A site</strong>
</td>
<td width="50%" align="center">
<img src="docs/dust2b.gif" width="100%" alt="CS2FOW operating around the Dust II B site"><br>
<strong>Dust II — B site</strong>
</td>
</tr>
<tr>
<td width="50%" align="center">
<img src="docs/dust2long.gif" width="100%" alt="CS2FOW operating across Dust II long sightlines"><br>
<strong>Dust II — Long</strong>
</td>
<td width="50%" align="center">
<img src="docs/mirageaside.gif" width="100%" alt="CS2FOW operating around the Mirage A site"><br>
<strong>Mirage &mdash; A site</strong>
</td>
</tr>
</table>

## FAQ

<details>
<summary><strong>What is CS2FOW?</strong></summary>

CS2FOW is an anti-wallhack plugin for Counter-Strike 2 community servers. If walls or live smoke completely hide a living player, your server can stop sending that player's live visuals to the opponent who cannot see them.

It is not a filter drawn over the screen. Everything happens on your server.

</details>

<details>
<summary><strong>Does it work in Premier or Valve matchmaking?</strong></summary>

No. You need a community or dedicated server running Metamod:Source. Only Valve could add something similar to official matchmaking.

</details>

<details>
<summary><strong>Do players install anything or risk a VAC ban?</strong></summary>

Nothing. Players join your server like normal. CS2FOW does not modify, inject into, or even run inside their CS2 client.

</details>

<details>
<summary><strong>Can a cheat bypass it?</strong></summary>

A cheat cannot unpack an exact live enemy position if your server never put it in the package. It can still listen for sounds, use teammate information, remember the last known position, or guess a common prefire spot. CS2FOW cuts off the main source used by wallhacks; it does not make every kind of cheating impossible.

</details>

<details>
<summary><strong>What exactly gets hidden?</strong></summary>

CS2FOW hides only the known visuals that travel with a living player: the player model, carried weapons, wearables, and a hostage they are currently carrying. Anything unknown or independent stays visible.

You always receive yourself, dead players, spectators, and HLTV. Teammates also stay visible by default, but you can choose to apply the same visibility check to living teammates.

</details>

<details>
<summary><strong>Can players still wallbang a hidden enemy?</strong></summary>

Yes. Hidden does not mean deleted. The player is still fully present on your server, so movement, hit registration, bullet penetration, damage, and game rules keep working normally.

</details>

<details>
<summary><strong>Does it block radar cheats or sound ESP?</strong></summary>

It helps against radar cheats that need live enemy positions. It does not silence footsteps or gunshots, hide bomb information, remove teammate knowledge, or erase every other clue the game provides.

</details>

<details>
<summary><strong>What about smokes, doors, breakables, and moving props?</strong></summary>

Smoke blocks sight too. CS2FOW copies the game's live smoke shape, which is stored as a 3D grid of tiny boxes called voxels. That lets it follow changing edges, overlapping smokes, growth, fading, and holes opened by grenades.

Doors, breakable objects, and moving props do not block CS2FOW yet. The baked map is a frozen copy of solid map geometry, so it only knows about walls that stay put.

</details>

<details>
<summary><strong>How does it avoid enemies appearing too late around corners?</strong></summary>

CS2FOW checks several body points, the corners of the player's box, and the muzzle of their held weapon. Above 75 units per second, it gradually starts looking ahead; at 100, prediction is fully active. It uses movement and the viewing player's ping to peek a little around the next corner before the client gets there. Wider shoulder checks at higher ping reduce late pop-in, and a short visibility hold stops one-tick flicker.

As soon as the background worker finds a clear view again, CS2FOW lets the player's next normal update through.

</details>

<details>
<summary><strong>Does it run expensive engine traces every tick?</strong></summary>

No. The baker turns the map's walls into a small, quick-to-search file called a BVH8. A background worker draws imaginary lines through that data, while `CheckTransmit` only picks up the finished yes-or-no answer when the server is deciding what to send.

</details>

<details>
<summary><strong>Do custom and Workshop maps work?</strong></summary>

Yes, as long as the map's physics can be baked. Your server can prepare a mounted map automatically. For a public Workshop item, the [CS2FOW Map Baker](https://cs2fow-bake-service.onrender.com/) can give you ready-to-use `.bvh8` and `.json` files.

</details>

<details>
<summary><strong>What happens when a map changes?</strong></summary>

CS2FOW checks the map file's fingerprint - its CRC and size - against the saved bake. If they do not match, it rejects the old data and keeps everyone visible. The automatic baker can then make a fresh copy.

</details>

<details>
<summary><strong>What does "fail open" mean?</strong></summary>

It means CS2FOW would rather show too much than hide the wrong player. If something is missing, old, or uncertain, your server sends the player normally.

</details>

## Quickstart

1. Install [Metamod:Source](https://www.sourcemm.net/) on your CS2 server.
2. Grab the Windows or Linux `0.2.2-preview` core ZIP from the [releases page](https://github.com/karola3vax/CS2FOW/releases).
3. Extract it into your server's `game/csgo` folder. Do not rearrange the folders inside the ZIP.
4. Start your server and load a map.
5. Type `cs2fow_status` in the server console.

The first time you load a map, `cs2fow_status` may say that an automatic bake is running. You can keep playing, and everyone stays visible until the bake finishes and passes its checks. The optional official-maps ZIP comes with ready-made data, so its included maps skip this first wait.

Your server needs an x86-64 CPU with AVX visible to the operating system. CS2FOW also checks that its private engine offsets, called gamedata, match the loaded CS2 server file by size and CRC32. If Valve ships an unknown update, CS2FOW stays off until matching gamedata is installed. That is much safer than guessing inside server memory.

## The protection boundary

CS2FOW keeps its hands off as much of the game as possible. It hides only this small visual group:

- the player pawn;
- active, last, and carried weapons, including carried C4;
- wearables;
- a currently carried hostage prop.

Everything that matters on its own stays on its own. A planted C4, dropped objective, dropped weapon, flying grenade, inferno, sound, or unknown entity does not disappear just because the player who once owned it is hidden. Your server still controls movement, collision, hit registration, damage, penetration, and game rules exactly as before.

CS2FOW does not filter HLTV, spectators, dead players, or your own player. Teammates stay visible by default. If you set `cs2fow_filter_teammates 1`, living teammates use the same visibility check, which can also remove their client-side markers and radar information while hidden.

Live smoke can block those imaginary sight lines too. By default, an HE opens a 100-unit viewing channel through affected smoke for 2.5 seconds, but only if the smoke was already there when the HE exploded. A wall still wins, and another overlapping smoke can still block the view.

## How it works

1. **Load the map:** CS2FOW finds the mounted VPK and the physics data inside it.
2. **Bake the walls:** the baker turns thousands of collision triangles into a compact, quick-to-search map called a BVH8.
3. **Take a picture:** on the game thread, CS2FOW safely copies each player's position, size, movement, view direction, ping, and held weapon. This recent picture is the snapshot.
4. **Draw sight lines:** a background worker tests eight viewing points against the player's current and predicted body, box corners, and weapon muzzle.
5. **Choose visible or hidden:** if even one line gets past both the baked walls and live smoke, the player stays visible.
6. **Control the outgoing update:** `CheckTransmit`, the server's outgoing entity list, first marks the verified `dont_transmit` bit and then removes the matching primary send bit for each hidden visual entity.

The worker gets a copy of the numbers, never live CS2 objects. In other words, it reads a photograph instead of reaching back into the moving game.

### Baked map geometry

<table>
<tr>
<td width="52%">
<p>The baker strips a map's static collision down to the walls CS2FOW needs for sight checks. Before using the result, the plugin checks the BVH8 structure, the source file's fingerprint (CRC and size), and the report details.</p>
<p>Your server can bake mounted maps automatically. You can also prepare public Workshop maps through the <a href="https://cs2fow-bake-service.onrender.com/">CS2FOW Map Baker</a>.</p>
</td>
<td width="48%" align="center">
<img src="docs/scan_cbbl.png" width="100%" alt="Static cobblestone collision mesh used by CS2FOW">
</td>
</tr>
</table>

## Operating the server

The plugin runs `cfg/cs2fow.cfg` when it loads and again whenever a map starts. Out of the box, wall and smoke filtering are on, teammate filtering is off, and Valve's `sv_enable_donttransmit 0` compatibility mode is used. CS2FOW's paired send-list handling also supports mode `1`.

Think of `cs2fow_status` as the dashboard. It tells you whether CS2FOW is active, why it stepped aside, which map bake is loaded, how long its work takes, how fresh the latest snapshot is, how many player pairs it checked, what smoke and HE handling are doing, whether teammates are filtered, and how an automatic bake is going.

If you need to see exactly which entity bits CS2FOW removed:

```text
cs2fow_debug 1              start silent evidence collection
cs2fow_entity               list buffered records, newest first
cs2fow_entity <edict>       show records for one entity index
cs2fow_entity clear         clear the evidence buffer
```

The debug buffer records only primary bits CS2FOW truly removed. Turning debug off stops collecting new evidence, but keeps what is already there until a reset or you clear it.

<details>
<summary><strong>Complete configuration reference</strong></summary>

| Setting | Default | Meaning |
| --- | ---: | --- |
| `sv_enable_donttransmit` | `0` | Use Valve's safer compatibility mode. CS2FOW also handles mode `1` correctly. |
| `cs2fow_enable` | `1` | Turn filtering on whenever all required data passes its safety checks. |
| `cs2fow_smoke_occlusion` | `1` | Let live smoke block sight. If CS2FOW cannot safely read the smoke data, smoke steps aside while wall protection keeps working. |
| `cs2fow_he_clear_radius_units` | `100` | Set how wide an HE-opened viewing channel is. Use `0` to turn HE clearing off. |
| `cs2fow_he_clear_seconds` | `2.5` | Set how long an HE-opened viewing channel lasts. Use `0` to turn HE clearing off. |
| `cs2fow_filter_teammates` | `0` | Give living teammates the same visibility checks as enemies. |
| `cs2fow_update_interval_ms` | `1` | Wait at least this many milliseconds before sending another picture of the players to the worker. |
| `cs2fow_base_lookahead_ms` | `75` | Start movement prediction this many milliseconds ahead before ping is added. |
| `cs2fow_rtt_lookahead_scale` | `1.5` | Multiply the viewing player's round-trip ping by this amount when looking ahead. |
| `cs2fow_max_lookahead_ms` | `375` | Never look farther ahead than this many milliseconds. Use `0` to turn movement prediction off. |
| `cs2fow_max_prediction_units` | `96` | Never move either player's predicted position farther than this. Use `0` to turn movement prediction off. |
| `cs2fow_shoulder_base_units` | `24` | Start the left and right viewing points this far from the player's eye. |
| `cs2fow_shoulder_rtt_scale` | `0.64` | Add this many shoulder units for each millisecond of the viewing player's round-trip ping. |
| `cs2fow_max_shoulder_units` | `128` | Never push the left and right viewing points farther out than this. |
| `cs2fow_visibility_hold_ms` | `16` | Once a player becomes visible, keep them visible for at least this long to prevent flicker. |
| `cs2fow_debug` | `0` | Save evidence about entity bits CS2FOW actually removed. It does not spam the console. |

If you are keeping an older custom config, give it a quick update. Add `sv_enable_donttransmit 0` and both HE-clearance settings. Replace the old `50`/`150` lookahead defaults, add the RTT, prediction-distance, and shoulder settings, replace `cs2fow_min_lookahead_ms` with `cs2fow_base_lookahead_ms`, and remove `cs2fow_peek_margin_units`.

Automatic baking needs permission to write into `addons/cs2fow/data/maps`. On Linux, the packaged baker and VRF program also need to stay executable.

</details>

## Honest limits

- Baked walls and live smoke can block sight. Doors, breakable objects, moving props, particles, projectiles, and other moving things cannot.
- Prediction is tuned for normal competitive and casual movement. Surf, KZ, and unusually fast boosts may still make a player appear late.
- More ping compensation reduces corner pop-in by showing players farther around corners, even while the viewing player stands still. Smoother peeks cost a little more information.
- Sounds, bomb information, teammate information, last-known positions, and other clues that are not part of the player entity remain available.
- If CS2FOW does not recognize the CS2 server file, it disables itself instead of guessing private memory locations.
- Full-update snapshots are hands-off. `CheckTransmit` also leaves both send lists alone if either required pointer is missing.
- Builds and unit tests cannot imitate a real CS2 send list or promise that an engine entity-copy crash is impossible. That still needs testing on a live server.

<details>
<summary><strong>Troubleshooting</strong></summary>

**`cs2fow_status` says AVX is missing:** your physical CPU may support AVX while the virtual machine cannot see it. Check that the host exposes both AVX and the operating system's AVX state to the guest.

**Automatic bake says permission denied on Linux:** restore execute permission, then check whether your mount or container policy blocks the files:

```sh
chmod +x game/csgo/tools/cs2fow_baker
chmod +x game/csgo/tools/vrf/linux64/Source2Viewer-CLI
```

When the automatic baker or VRF fails, the error includes the newest 8 KiB of their combined output. Cancelling a bake or hitting the timeout also stops the whole child process tree on Windows and Linux, so helpers are not left running in the background.

**The server binary does not match:** your CS2 update and CS2FOW gamedata do not belong together. Install a CS2FOW build verified for the current Valve server file. There is intentionally no "try it anyway" switch.

**A bake is rejected after a CS2 update:** Valve probably changed the map's source VPK. CS2FOW compares the saved and mounted file fingerprints, then rebakes instead of trusting an old map copy.

**You need to report a bug:** include your CS2FOW version, operating system, map, `cs2fow_status` output, nearby server logs, what the players were doing, and a short clip for visibility or pop-in problems. Those details turn "it broke" into something that can actually be reproduced.

</details>

## Developer and project links

- [Releases](https://github.com/karola3vax/CS2FOW/releases): download the Windows, Linux, and official-map packages.
- [Code tour](docs/CODE_TOUR.md): follow the architecture, threads, safety rules, and build and release steps in plain language.
- [Visibility Studio](tools/visibility_point_editor/README.md): see and tune the body, box, and muzzle points used for sight checks.
- [CS2FOW Map Baker](https://cs2fow-bake-service.onrender.com/): prepare visibility data from a public Workshop map.
- [Bake Service source](https://github.com/karola3vax/CS2FOW-Bake-Service): inspect the public baking service itself.

Manual baker examples:

```text
cs2fow_baker --game <cs2-root> --map de_dust2 --output de_dust2.bvh8
cs2fow_baker --list-maps --vpk <outer_dir.vpk>
cs2fow_baker --inspect-bvh8 <file>
cs2fow_baker --game <cs2-root> --map workshop/123/de_example --vpk <outer_dir.vpk> --output de_example.bvh8
```

Generated map files come from Counter-Strike 2 game data, so `DATA_NOTICE` covers them instead of the project's MIT code license. Core packages include the exact cgltf, ValveResourceFormat 19.2, native-library, and self-contained .NET notices under `licenses/`. The full legal details are in [LICENSE](LICENSE), [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES), and [DATA_NOTICE](DATA_NOTICE).
