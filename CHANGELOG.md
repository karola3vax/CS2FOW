# Changelog

## 0.2.0-preview

- Reorganized the runtime into map/game-state, worker, transmit, and automatic-baker responsibilities without intentionally changing proven visibility behavior.
- Restricted filtering to CS2's verified primary transmit list and left full-update snapshots untouched.
- Let visible enemies return through ordinary snapshots instead of waiting for CS2 to schedule a full update.
- Replaced aggressive movement preload with a 50 ms base plus half of recipient RTT, capped at 150 ms by default, and removed forced distance tiers.
- Added fixed-size `cs2fow_entity` evidence for real primary-bit clears, including direct and owner/effect-linked membership.
- Hardened player lifecycles, visual-group identity, linked entities, stale results, and fail-open resets.
- Added validated BVH8 version 3 files with streaming CRC checks and verified atomic replacement; older bakes are rejected.
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
