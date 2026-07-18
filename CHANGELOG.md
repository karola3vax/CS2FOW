# Changelog

## 0.2.4-preview

- Rebuilt against the current Metamod:Source and HL2SDK so CS2FOW commands and settings register correctly after the July 17 CS2 tooling update.
- Tightened visibility checks by reducing the upper eye origin from 24 to 16 units, AABB side/top padding from 8 to 4 units, and ping-scaled shoulder origins from `24 + 0.64 x RTT` (maximum 128) to `16 + 0.48 x RTT` (maximum 96).
- Automatically treat every other living player as an enemy when `mp_teammates_are_enemies 1` is active.
- Reduced the lifecycle fail-open window from 3 seconds to 1 second and removed the separate 1.5-second visual warmup while preserving the complete-group baseline check.
- Updated Visibility Studio with a second SAS model 256 units away and the same stationary origins, target samples, and ray count used by the runtime.
- Replaced velocity/lookahead prediction with a ping-scaled W/S/diagonal intention origin, added a permanent feet origin, and reduced rays by 37.5% to 62.5% per player pair (from 192-384 to 120-144).

## 0.2.3-preview

- Verified that CS2 build `24248951` keeps the same private runtime layout and updated the strict Windows/Linux server fingerprints.
- Rebaked `cs_shelter`, `de_boulder`, `de_eldorado`, and `de_fachwerk` after their mounted map sources changed in the same update.

## 0.2.2-preview

- Verified the private runtime layout and updated the strict Windows/Linux fingerprints for CS2 build `24209309`.
- Removed the obsolete Valve string-token database import dropped by that update.
- Rewrote the README for ordinary server owners and added new visibility, smoke, HE, and map demonstrations.

## 0.2.1-preview

- Stopped generic owner/effect links from pulling independent gameplay entities into a hidden player's visual group.
- Kept planted C4, dropped objectives, grenade projectiles, infernos, sounds, and unknown entities independent so player culling cannot hide core gameplay state.
- Kept explicit player visuals together: pawn, known carried weapons (including carried C4), wearables, and a currently carried hostage prop.
- Simplified `cs2fow_entity` evidence to direct visual-group membership.

## 0.2.0-preview

- Added default-on smoke occlusion from CS2's live voxel grid, with copied worker data and smoke-only fail-open behavior.
- Added wall-safe, configurable 2.5-second visibility channels through smoke disturbed by HE grenades.
- Fixed HE event discovery, post-initialization listener registration, and detonation-position reading without making ordinary smoke depend on HE support.
- Prevented an old HE event from clearing a smoke that detonated later by ordering both on CS2 game time.
- Matched visible smoke timing more closely by delaying initial occlusion and revealing fading smoke 0.5 seconds earlier.
- Added optional teammate visibility filtering with the same wall, smoke, prediction, and full-group rules used for enemies.
- Reorganized the runtime into map/game-state, worker, transmit, and automatic-baker responsibilities without intentionally changing proven visibility behavior.
- Updated CheckTransmit hiding to set CS2's matching `dont_transmit` bit before clearing a set primary transmit bit; missing lists fail open, while full updates and the other mask storage remain untouched.
- Bundled `sv_enable_donttransmit 0` as the compatibility default and automatically execute `cs2fow.cfg` after convar registration and at every map start; paired-list handling also supports mode `1`.
- Let visible enemies return through ordinary snapshots instead of waiting for CS2 to schedule a full update.
- Tuned movement preload to a 75 ms base plus 1.5 times recipient RTT, capped at 375 ms and 96 units per player, with a smooth 75-100 speed ramp.
- Made left/right shoulder origins scale from 24 to 128 units with recipient RTT through public tuning controls.
- Kept safe movement up to baked walls, replaced merged target boxes with separate current/future boxes, and corrected stale-result age to use snapshot capture time.
- Added fixed-size `cs2fow_entity` evidence for entity bits actually hidden by CS2FOW, including direct and owner/effect-linked membership.
- Hardened player lifecycles, visual-group identity, linked entities, stale results, and fail-open resets.
- Bound private gamedata to verified Windows and Linux server binaries and rejected unsafe player numbers before ray casting.
- Added snapshot-capture and CheckTransmit timings, full networked-edict linked-visual coverage, and accurate active-HE status wording.
- Captured bounded VRF and automatic-baker error output so failures include their useful final messages.
- Made Linux bake cancellation and timeout terminate and reap the complete baker/VRF process tree.
- Added validated BVH8 version 3 files with rooted-tree, reachability, depth, triangle, and streaming CRC checks plus verified atomic replacement; older or structurally invalid bakes are rejected.
- Restricted VPK version 2 embedded entries to the declared file-data section instead of accepting undeclared footer bytes.
- Added a machine-readable `--inspect-bvh8` command and require every official-map bake/report pair to match before packaging.
- Kept checksums from sequential platform packaging, required all three final archives, and bundled exact cgltf, ValveResourceFormat, native-library, and .NET redistribution notices.
- Prevented the LOS editor from exporting blank/duplicate names, invalid coordinates, or zero points.
- Moved all Workshop VPK discovery and extraction into the C++ baker, including the public `--list-maps` command.
- Added held-weapon muzzle sampling alongside body and axis-aligned bounding box target points.
- Split and expanded map/BVH and visibility/transmit tests, package verification, and the line-of-sight point editor checks.
- Added a plain-language code tour and corrected operator documentation.

## 0.1.2-preview

- Further hardened CheckTransmit player lifecycle checks.
- Hide pawn, current weapons, wearables, and carried hostage prop as one group.
- Preserve fail-open behavior when live player state is uncertain.

## 0.1.1-preview

- Hardened CheckTransmit against invalid indexes, stale player state, and stale weapon handles.
- Built Linux packages against SteamRT3 Sniper for CS2 server compatibility.
- Added CI checks for glibc, libstdc++, and C++ ABI requirements.

## 0.1.0-preview

First public preview of CS2FOW.

- Native Metamod plugin for server-side CS2 visibility culling.
- Offline and automatic map baker for official, custom, and Workshop maps.
- BVH8 runtime map data with AVX traversal.
- Smooth reveal envelope to reduce corner pop-in.
- Windows x86_64 and Linux x86_64 packages.
- Optional official map prebakes as a separate release asset.
