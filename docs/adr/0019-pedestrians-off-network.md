# ADR 0019: Pedestrians off the node network, with a stray cull

Date: 2026-10-16
Status: Accepted
Relates to: VICE-122 (the implementation), VICE-119 (the program), ADR 0013 (the ped ring comes from the table)

## Context

Pedestrians exist only on the pedestrian node network: `GeneratePedCreationCoors`
picks a pair of ped nodes in the creation ring and returns a point between them.
Every area the map gives no ped nodes — the beach, the airfield, the docks, the
golf course, the parks — stays empty, while the node areas fill up (owner ask 7).

Walking is not the blocker. `FindNextNodeWandering` looks for the closest ped
node with an effectively unlimited radius (the search caps its own Manhattan
metric at 10000), and the wander state walks to the node with `Seek`. A
pedestrian created away from the network therefore walks to it on its own. What
is missing is a *placement* that is safe off the network: a node is ground a
pedestrian can stand on, so the node path gets ground, level, water, clearance
and reach for free, and a free position must earn each of them. The current
ground test (`|groundZ - pos.z| <= 3`) alone lets a roof or a ledge through.

## Decision

1. A bounded fraction of the spawns (`PED_OFF_NETWORK_FRACTION`, 0.15) takes a
   free position instead of the node path. The fraction is one named constant.
   A free position that fails a check falls back to the node path, so the share
   of off-network pedestrians never exceeds the constant and on-network spawning
   is untouched. Interiors (the mall) keep the node path; the mall pedestrians
   are stationary by design.
2. The free position lies in the same ring and the same bands as the node
   position (in view: the creation ring; out of view: the off-screen band; up to
   `PED_OFF_NETWORK_ATTEMPTS` tries), and passes these checks:
   - ground found, and within the node path's own 3 m of the level of the ring
     (`CWorld::FindGroundZFor3DCoord`, cast starting on the level itself, so a
     surface above the level is not found and cannot be picked);
   - the surface: the top surface over the position may stand no more than a
     step (`PED_OFF_NETWORK_STEP`, 0.5 m) above the ground. A roof, a canopy or
     a bridge deck stands higher;
   - the water: the ground must stand above the water line by
     `PED_OFF_NETWORK_WATER_MARGIN` (0.1 m). The line is
     `CWaterLevel::GetWaterLevelNoWaves`, so neither this answer nor the drown
     answer moves with the wave phase;
   - clearance over the feet: a sphere (`PED_OFF_NETWORK_PROBE_RADIUS`, 0.5 m,
     raised `PED_OFF_NETWORK_PROBE_HEIGHT`) against the static world
     (`CWorld::TestSphereAgainstWorld`). `IsPositionClearForPed` reads vehicles
     and pedestrians only, so this is what catches a spot inside a wall, a fence
     or a thin roof;
   - clear for a pedestrian (`CPedPlacement::IsPositionClearForPed`);
   - the network is within the walk bound (150 m in the search's own Manhattan
     metric) and on the level of the ground (5 m). A node far below the position
     means the position is a roof; the bound is what stops a pedestrian walking
     to a node across the map.
3. The stray cull: the population watches the off-network pedestrians in a
   bounded table (32 entries, 2 checks a frame) until each reaches a node
   (`m_pLastPathNode` within 8 m, which the wander state sets when `Seek`
   arrives). A stray that walks for 20 s without coming 0.25 m closer to its
   node is stuck; a stray whose feet are under the water line is in the water.
   **One rule for both counters: a stray is counted on the frame the cull
   removes it.** A stray the camera can see (`TheCamera.IsSphereVisible`,
   radius `PED_OFF_NETWORK_CULL_RADIUS`, 3 m) stays watched and is counted on a
   later frame, so a counter never carries a pedestrian that is still there.
   The stall clock only runs while the pedestrian walks with a node to head for,
   and it stands while the pedestrian is in one of its own waits (a traffic
   light, a chat, a hit wall) or stands still, so a pedestrian that waits on
   purpose is never culled.
4. A stray with no node to head for is not a stray the walk clock can measure.
   It keeps its watch for `PED_OFF_NETWORK_NO_TARGET_TIME` (60 s) and the watch
   is then given up: the slot comes back and the pedestrian is left alone. It is
   neither culled nor counted, because a pedestrian without a target is fleeing,
   following or locked, not a walk to the network.
5. A gang or couple group that is placed on a free position is part of the
   change: its own members are validated at their own spots (the same water
   line), they stay within a step of the seed (the group's own level checks),
   they are counted, and they are watched like every other off-network
   pedestrian. A pedestrian that follows another (the couple, a formation
   member) leaves the watch on its first check: the walk clock is not for it.
6. The counters live in the binary for the live measurement rig:
   `CPopulation::ms_nOffNetworkSpawns` (peds created on a free position, before
   a group's own rules drop a member), `ms_nOffNetworkStuck` and
   `ms_nOffNetworkDrowned` (removals, rule 3). They reset in
   `CPopulation::Initialise`, with the stray table, and the table holds its
   pedestrians through the game's own reference system, so a removed pedestrian
   never leaves a stale pointer.
7. A fraction of zero draws no random number, so the node path is bit-exact
   with the behaviour before this change. At 0.15 the node path keeps its rules
   and its rate; its draws shift, because the game's random number sequence is
   one stream.

## What the checks cannot tell, and what covers it

- A surface the casts find is a surface. A low platform whose top is within the
  level margin of the ring and within the node level of a node (a roof garden, a
  hillside ledge with a node at its level) still passes, and a spot under a
  canopy is rejected rather than accepted: the checks are conservative in the
  safe direction. The stray cull takes the pedestrian that cannot leave such a
  spot.
- The clearance probe uses one sphere at the feet. A thin horizontal slab above
  the sphere (a balcony over the head) is not seen; the water and the stray cull
  still cover the pedestrian.
- The water check reads the level of a block, not a depth under the waves. The
  wave phase, the shoreline wetness and the game's own drowning stay where they
  were.

## Consequences

- Empty places gain pedestrians: one appears in the creation ring and walks to
  the closest node, so the crowd flows from the empty area towards the network
  edge. The empty area keeps receiving spawns while the player is there.
- A pedestrian that walks into the water anyway is counted and culled; the
  game's own drowning handles the rest.
- The placement costs up to two vertical line casts, three block lookups and
  the clearance sphere per candidate, plus one node search per candidate that
  passes the earlier checks. The caller makes `PED_ATTEMPTS_PER_FRAME * scale`
  spawn calls a frame and the free path runs for the fraction of them, so the
  node search is paid about `PED_OFF_NETWORK_ATTEMPTS * 0.15 * PED_ATTEMPTS_PER_FRAME * scale`
  times a frame (about 9 at the maximum ring scale, 4.0), each a scan of the
  whole ped node array; a candidate rejected before the search costs none. A
  cheaper exact search needs a spatial index for the path nodes; the
  path-finding work (VICE-130) owns that, not this ticket.
- The watch is bounded and the frame cost is flat: a fixed slice of the table is
  checked, and nothing here walks the crowd.
- The fraction and the distances are guesses to tune against the owner's
  play-test, like the ring values (VICE-132).
