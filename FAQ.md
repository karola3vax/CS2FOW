# CS2FOW FAQ

This FAQ answers the repeated questions from Reddit, Discord, GitHub, and live
server testing.

## What is CS2FOW?

CS2FOW is a server-side anti-wallhack plugin for Counter-Strike 2 community
servers.

If an enemy is fully hidden behind solid map geometry, CS2FOW can stop sending
that enemy's live entity data to the client. If the client never receives the
hidden enemy position, a wallhack has much less useful data to draw.

## Is this a visual filter?

No.

CS2FOW does not blur, hide, or modify anything on the player's PC. It runs on
the server and controls which enemy entities are transmitted to each client.

## Does this work on Valve matchmaking?

No.

CS2FOW is a Metamod server plugin for community and dedicated servers. Players
do not install it, and it cannot be used on Valve official matchmaking unless
Valve implements something similar inside the game.

## Can players get VAC banned for joining a CS2FOW server?

No expected risk from CS2FOW itself.

It runs on the server. It does not modify client files, inject into the client,
or ask players to install anything.

## Can cheats bypass this?

Not in the usual "update the cheat" way.

If the server sends enemy positions, cheats can read them. If the server does
not send hidden enemy positions, a cheat cannot recover exact live data it never
received.

Cheats can still use weaker signals such as sound events, last-known positions,
common prefire spots, teammate information, or game sense. CS2FOW reduces the
main wallhack data source; it does not make cheating impossible.

## Does this remove a player from the server?

No.

The enemy still exists normally on the server. Hit registration, bullet
penetration, damage, movement, and game rules still happen server-side.

CS2FOW only changes whether that enemy entity is transmitted to a specific
client.

## Can I still wallbang hidden enemies?

Yes.

If you shoot through a wall or door correctly, the server can still register
the hit. Your client does not need to know the enemy's exact position for the
server to process the bullet.

## What exactly is hidden?

In the current preview, CS2FOW filters living enemy pawns and their currently
held weapons from living T/CT recipients.

It does not filter teammates, self, dead players, spectators, HLTV, projectiles,
dropped world items, particles, effects, or sounds.

## Are bots affected?

Yes, if they are living enemy pawns.

Bots are treated like normal players for visibility computation. Dead bots,
spectated players, teammates, and spectators are not filtered.

## Why do spectators still see everything?

By design.

CS2FOW filters only for living T/CT players. Dead players, spectators, and HLTV
remain unfiltered because spectator behavior is different from live gameplay
and is easier to break.

## Does it block radar cheats?

Partly.

If a radar cheat depends on enemy entity positions sent by the server, CS2FOW
reduces that data too. Hidden enemies are not transmitted, so there is less
live position data to draw.

It does not currently hide sound events, teammate information, bomb
information, or every possible world clue.

## What about sound ESP and footsteps?

Sounds are separate from player entities.

Player position is entity data. Footsteps, gunshots, and other audio cues are
sound events. CS2FOW currently focuses on entity visibility and does not
randomize or strip sounds.

Sound filtering needs careful design because the server does not know who is
cheating. Any sound change affects legitimate players too, and footsteps are a
core CS mechanic.

## What about smokes?

The current preview does not use smoke as an occluder.

CS2FOW uses static map geometry. Smokes are dynamic and need different handling.
A simple future version could approximate active smokes with volumes, but real
smoke behavior needs more research so it does not break normal gameplay.

## What about doors, breakables, props, or moving blockers?

Not covered in the current preview.

CS2FOW uses static baked map geometry. Dynamic occluders such as doors,
breakables, props, smokes, particles, and projectiles are intentionally out of
scope for now.

## What about dropped weapons?

Dropped weapons are a real information leak and should be handled later.

They are not continuous player ESP, but they can leak decision-making
information in clutches. For example, a weapon appearing, disappearing, or
moving can reveal where a player probably rotated.

The clean future fix is to visibility-filter dropped weapon entities too.

## Why did a weapon muzzle pop in around a corner?

The preview checks points around the player body, not every held weapon muzzle.

If a gun sticks around a corner before the body is visible, the weapon can pop
in when the player is transmitted. The likely fix is to add weapon muzzle or
weapon-bounds visibility samples so the player is revealed early when the held
weapon becomes visible.

## How accurate is the visibility check?

Spatially, it uses the real CS2 map physics resource, not a hand-made map.

The baker extracts static world collision triangles from the mounted map, builds
a BVH8 acceleration structure, and the runtime checks multiple observer and
target points against that geometry.

It is accurate for solid static walls, floors, corners, and normal map cover.
It is not a full simulation of every dynamic gameplay object.

## Does it cause pop-in when peeking?

The goal is early reveal, not exact last-millisecond reveal.

CS2FOW predicts using movement and ping, reveals enemies slightly before exact
visibility, and keeps revealed enemies visible briefly. This intentionally
leaks a small near-corner window to avoid late pop-in.

The preview still needs real server testing for edge cases.

## What latency does CS2FOW use?

Player network latency, not server frame time.

CS2FOW uses recipient RTT/ping plus the worker update interval to decide how
far ahead to reveal. Higher-ping players can receive a larger early-reveal
buffer, capped by configuration.

## Does higher ping make this worse?

Higher ping is handled by revealing a bit earlier.

That does not make networking perfect, but CS2FOW does not wait for the client
to say "I cleared the wall." The server already knows player positions and map
geometry.

## Does CS2FOW run visibility checks every tick?

No.

A worker thread refreshes the visibility matrix on an interval. The default is
`10ms`. `CheckTransmit` only reads the finished matrix and does not run BVH
traversal, ray math, file IO, locks, or baking.

## Is this heavy on the server?

In testing, no meaningful overhead was observed.

The expensive map work is done ahead of time or in a low-priority background
bake. Runtime checks use pre-baked BVH8 data and AVX math instead of engine
TraceRay spam.

In a tested 12v12 worst-case scenario, the old trace-based approach could hit
around `60ms`. CS2FOW averaged around `1ms`, with worst cases around `8ms`.

## What about 32-player deathmatch?

The worst case is more directed enemy pairs, especially if everyone is an
enemy.

CS2FOW was designed for that shape of work: pre-baked map data, BVH8 traversal,
AVX packet math, a worker thread, early exits, and cached triangle packets.
Server owners should still test their own maps and player counts with
`cs2fow_status`.

## Why is it fast?

Because it does not ask the engine to trace every segment.

The baker converts map geometry into a runtime-ready BVH8. At runtime, AVX
checks multiple child bounds or triangles in parallel. Most visibility checks
exit early as soon as one clear segment proves the enemy should be visible.

## Why AVX instead of AVX2?

AVX is the runtime baseline so more servers can run it.

The BVH8 format still uses eight-wide floating-point vectors. That works with
AVX. AVX2 is mostly useful for integer/vector operations that CS2FOW does not
heavily depend on in the current traversal path.

There is no scalar fallback in the preview.

## Why does my VDS say AVX is missing?

Some VDS providers hide CPU features from the virtual machine.

Even if the physical CPU supports AVX, the guest OS may not expose AVX or OS
AVX state. Check with CPU-Z, `lscpu`, or your host provider.

## What does automatic map baking do?

If the current map has no valid `.bvh8` file, CS2FOW starts the packaged baker
in the background at low priority.

The server stays playable and fail-open while baking. After the bake validates,
CS2FOW can activate during the same map session.

## Does auto-baking support Workshop maps?

Yes, if CS2 has mounted the map and the server can read the VPK.

CS2FOW resolves the mounted map VPK, including Workshop addon VPKs that contain
a nested `maps/<map>.vpk`, then bakes from that source.

## Why ship optional official map prebakes?

Convenience and hostile hosting environments.

Auto-baking should work for normal installs, but some Linux permissions, panel
setups, or VDS policies can block background executables. Optional prebakes let
server owners install official map data without running the baker on first
load.

## What happens if Valve updates a map?

CS2FOW validates the map source before using a bake.

The bake stores the source physics CRC and size. On map load, CS2FOW compares
the current map source with the baked source. If they do not match, the bake is
rejected and CS2FOW fails open until a new bake is generated.

For nested Workshop map packages, the nested map VPK source is also tracked.

## What does fail-open mean?

If CS2FOW is not sure, it shows players normally.

Missing map data, corrupt bakes, wrong CRCs, unsupported CPU features, stale
worker results, failed hooks, and baking-in-progress states should never hide
players incorrectly.

## Why did Linux report `GLIBCXX_3.4.32 not found`?

Older Linux packages were built against a newer libstdc++ than some SteamRT3
servers provide.

`v0.1.1-preview` rebuilt Linux artifacts against Valve's SteamRT3 Sniper SDK and
added ABI checks for that runtime baseline.

## Why did Linux report `automatic bake failed: Permission denied`?

The server likely could not execute the packaged baker or VRF binary.

Check executable permissions on Linux:

```sh
chmod +x game/csgo/addons/cs2fow/bin/cs2fow_baker
chmod +x game/csgo/addons/cs2fow/tools/vrf/*
```

Also check whether the hosting panel, container, mount options, or provider
policy blocks executing files from the server directory.

## What is gamedata for?

Gamedata stores small platform-sensitive offsets used by the plugin.

CS2FOW needs these for things like reading the CheckTransmit recipient slot and
resolving the entity system. If CS2 changes those layouts, gamedata may need an
update.

## What should I include in a bug report?

Include:

- CS2FOW version.
- Windows or Linux.
- Dedicated server, panel, container, or VDS provider.
- Map name.
- `cs2fow_status` output.
- Server console log around plugin load and map start.
- Whether the issue happens while alive, dead, spectating, or on bots.
- A short clip if the report is about pop-in or visibility.

