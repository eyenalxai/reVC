# ADR 0016: Pedestrians off the node network, with a stray cull

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
ground test (`|groundZ - pos.z| <= 3`) alone lets a roof through.

## Decision

1. A bounded fraction of the spawns (`PED_OFF_NETWORK_FRACTION`, 0.15) takes a
   free position instead of the node path. The fraction is one named constant.
   A free position that fails a check falls back to the node path, so the share
   of off-network pedestrians never exceeds the constant and on-network spawning
   is untouched. Interiors (the mall) keep the node path; the mall pedestrians
   are stationary by design.
2. The free position lies in the same ring and the same bands as the node
   position (in view: the creation ring; out of view: the off-screen band; up to
   `PED_OFF_NETWORK_ATTEMPTS` tries), and passes five checks:
   - ground found, and within the node path's own 3 m of the level of the ring
     (`CWorld::FindGroundZFor3DCoord`, cast starting on the level itself, so a
     surface above the level is not found and cannot be picked);
   - no roof: the top surface over the position must be the ground itself (a
     cast from 50 m above; a surface above the ground is a roof or a canopy);
   - not water: the ground must stand above the water surface
     (`CWaterLevel::GetWaterLevel`);
   - clear for a pedestrian (`CPedPlacement::IsPositionClearForPed`);
   - the network is within the walk bound (150 m in the search's own Manhattan
     metric) and on the level of the ground (5 m). A node far below the position
     means the position is a roof; the bound is what stops a pedestrian walking
     to a node across the map.
3. The stray cull: the population watches the off-network pedestrians in a
   bounded table (32 entries, 2 checks a frame) until each reaches a node
   (`m_pLastPathNode` within 8 m, which the wander state sets when `Seek`
   arrives). A stray that walks for 20 s without coming 0.25 m closer to its
   node is stuck: counted and removed. A stray in the water is a bad placement:
   counted, and removed out of sight. The stall clock only runs while the
   pedestrian walks with a node to head for, and it stands while the pedestrian
   is in one of its own waits (a traffic light, a chat, a hit wall) or stands
   still, so a pedestrian that waits on purpose is never culled.
4. The cull only removes a pedestrian the camera does not see
   (`TheCamera.IsSphereVisible`), so it never makes one vanish in view.
5. Three counters live in the binary for the live measurement rig:
   `CPopulation::ms_nOffNetworkSpawns`, `ms_nOffNetworkStuck`,
   `ms_nOffNetworkDrowned`. They reset in `CPopulation::Initialise`, with the
   stray table, and the table holds its pedestrians through the game's own
   reference system, so a removed pedestrian never leaves a stale pointer.

## Consequences

- Empty places gain pedestrians: one appears in the creation ring and walks to
  the closest node, so the crowd flows from the empty area towards the network
  edge. The empty area keeps receiving spawns while the player is there.
- The counter counts the pedestrians of the open placement (`AddToPopulation`'s
  loop). The gang and couple groups are placed by their own helpers around the
  same validated position, and their members stand and chat by design, so they
  stay out of the stray table.
- A pedestrian that walks into the water despite the placement check is counted
  and culled; the game's own drowning handles the rest.
- The placement costs up to two vertical line casts and one clear check per
  candidate, plus one node search for a candidate that passes. The node path it
  replaces costs the same order, and the count per frame is fixed, so the frame
  cost stays flat and does not follow the crowd.
- The fraction and the distances are guesses to tune against the owner's
  play-test, like the ring values (VICE-132).
