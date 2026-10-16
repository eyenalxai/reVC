# ADR 0013: One traffic ring table, capped by the visible range

Date: 2026-10-16
Status: Accepted
Relates to: VICE-120 (the implementation), ADR 0009 (the count follows the ring)

## Context

The spawn and despawn radii lived as macros in three files: `CarCtrl.cpp` (the
car band and the culls), `Population.cpp` (the ped ring) and `CarGen.cpp` (the
generator ring). Two numbers broke at the maximum settings, and the owner
reported empty streets there:

- The rings scale with the spawn distance slider (120 m side, 250 m front at
  1.0), but the birth cull of a fresh off-screen car used the off-screen ring
  itself (120 m, 480 m at 4.0): a placement that lands a lane offset past the
  ring is deleted on the frame it is created. The lifetime cull of the same car
  (`PossiblyRemoveVehicle`, with `EXTENDED_OFFSCREEN_DESPAWN_RANGE` on) used the
  on-screen despawn (280 m, 1120 m at 4.0), so the two disagreed.
- The visible world does not scale with the slider. The camera far clip is
  weather data (2000 m sunny, 650 m rainy, 150 m foggy) and the LOD distance is
  300 x the LOD multiplier (360 m at the default draw distance and FOV 70). At
  4.0 the front ring aims at 1000 m, past the far clip of any weather but sunny,
  and every front spawn dies there.

## Decision

1. Every spawn radius, despawn threshold, camera minimum and generator ring
   lives in one table, `src/control/TrafficRings.h`, per class (car, boat,
   generator, pedestrian). The table holds the stock radii; no file keeps a
   second copy.
2. One scale multiplies the radii:
   `scale = min(spawnDistance * fovFactor, visibleCap)`, where the visible
   range is `min(LOD distance, weather far clip)` and the cap keeps the largest
   spawn radius (the on-screen car ring) one reserve inside it. The spawn
   distance and FOV parts are the old multiplication; the cap is new.
3. The cap has a floor: the stock ring at the live FOV factor (and the stock
   off-screen band, which never took the FOV factor). The cap stops growth
   only. A ring smaller than stock would drop traffic into the player's view,
   and a despawn smaller than stock would remove the cars a wider FOV factor
   put there, including the generator's parked car at its own door.
4. Two radii never took the FOV factor and keep their own scale, the spawn
   distance alone: the off-screen car band and the top-down request. The
   camera rule (82.5 m times the FOV factor), the MARQUIS rule, the mission and
   scripted protections and the boat factors (1.5 on the water path, 1.5 on the
   despawn of an extended-range vehicle) stay.
5. The invariant, for every scale:
   `spawn radius + reserve <= that class's despawn threshold`. Each threshold
   is the stock radius or the ring plus the reserve, whichever binds, so it is
   never tighter than the radius the game always had.
6. The population caps follow the ring, not the slider: the cap and the car
   target scale with the square of the ring scale (ADR 0009's area argument),
   and they refresh when a slider moves, when the draw distance moves and once
   a frame, because the weather moves the visible range.

## The reserve is an assumption, and this is it

The reserve is the game's own 30 m step from the on-screen ring (250 m) to the
despawn (280 m) and back down to the inner edge (220 m). The birth cull does
not measure the ring, it measures `vecTargetPos -> the final position` of the
car, so the reserve must hold three things:

- the lane offset, `(lane + OneWayLaneOffset()) * LANE_WIDTH` in `CarCtrl.cpp`:
  `LANE_WIDTH` is 5 m, the shipped `paths.ipl` holds up to 4 lanes per side and
  a two-way road has an offset of 0.5 (the path nodes carry width 0), so
  `(4 - 1 + 0.5) * 5 = 17.5 m`;
- the 2 m the final position moves along the node link;
- the curve point at the placement fraction against the straight
  interpolation, the sagitta of the link, a few metres.

None of the three scales with the ring, so the reserve is a distance and not a
ratio. The harness states the assumption and prints the two derived components;
it models the error as the whole reserve, which is the worst case the game's own
numbers have always been sized for.

## Consequences

- At stock settings every spawn ring, the on-screen despawn and the camera rule
  hold their value. One row moves: the **birth cull of a fresh off-screen car,
  from 120 m to 280 m**. It has to move, because the old birth cull used the
  off-screen ring itself and killed a placement that landed a lane offset past
  it, while the lifetime cull of the same car kept it to 280 m. No car roams
  farther: the lifetime cull is unchanged, so the move only stops the deaths at
  birth. The birth cull and the lifetime cull now read the same row.
- At maximum settings the ring stops at the visible range instead of aiming
  past it. With the default draw distance the ring lands at the LOD distance
  (330 m at spawn distance 4.0); the draw distance extends it (1000 m at 16.0),
  which is what VICE-125 wires to the user control.
- A cutscene sets the FOV factor to 1.0 and drops the FOV part of the LOD
  distance. The ring therefore takes `min(spawnDistance, visibleCap)` there
  instead of `min(spawnDistance * fovFactor, visibleCap)`: at FOV 70 the two are
  the same expression and a cutscene changes nothing, and at a narrower FOV the
  ring shrinks exactly as the FOV factor shrank it before this change. Only the
  floor of the cap differs from the old code: the cutscene ring is never below
  the stock ring, where the old code could pull it below one.
- The traffic rate follows the ring scale too, so a smaller ring is not
  over-filled and a larger one is not starved.
- Dead code removed: the `#else` of `EXTENDED_OFFSCREEN_DESPAWN_RANGE` in
  `PossiblyRemoveVehicle` (compiled out in every shipped build) and the two
  table rows it fed went away; the shipped path reads the same row as the birth
  cull.
