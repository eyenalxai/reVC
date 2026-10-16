# ADR 0013: One traffic ring table, clamped to the visible range

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
  300 x the LOD multiplier (360 m at the default draw distance). At 4.0 the
  front ring aims at 1000 m, past the far clip of any weather but sunny, and
  every front spawn dies there.

## Decision

1. Every spawn radius, despawn threshold, camera minimum and generator ring
   lives in one table, `src/control/TrafficRings.h`, per class (car, boat,
   generator, pedestrian). The table holds the stock radii; no file keeps a
   second copy.
2. One scale multiplies the radii:
   `scale = min(spawnDistance * fovFactor, visibleCap)`, where the visible
   range is `min(LOD distance, weather far clip)` and the cap keeps the largest
   spawn radius (the on-screen car ring) one reserve inside it, but never below
   the stock scale. The spawn distance and FOV parts are the old multiplication;
   the cap is new, and it only stops growth: a ring that aims past the visible
   range only wastes spawns, while a ring smaller than stock would drop traffic
   into the player's view. At stock settings (spawn distance 1.0, FOV 70) the
   cap does not bind and every radius keeps its stock value.
3. The invariant, for every scale:
   `spawn radius + reserve <= that class's despawn threshold`. Each threshold
   is the stock radius or the ring plus the reserve, whichever binds, so it is
   never tighter than the radius the game always had. The reserve is the game's
   own 30 m step from the on-screen ring (250 m) to the despawn (280 m) and
   back to the inner edge (220 m); it is a distance, not a ratio, because the
   lane offset and the curve placement of a fresh car do not shrink with the
   ring. The birth cull and the lifetime cull read the same row, so they can no
   longer disagree.
4. The boat factors stay: 1.5 on the water path ring and 1.5 on the despawn of
   an extended-range vehicle, so the boat geometry keeps its reserve. The 82.5 m
   camera rule, the MARQUIS rule, the top-down branch and the mission and
   scripted protections stay.
5. The population caps follow the ring, not the slider: the cap and the car
   target scale with the square of the ring scale (ADR 0009's area argument),
   and they refresh when a slider moves, when the draw distance moves and once
   a frame, because the weather moves the visible range.

## Consequences

- At stock settings every spawn ring, the on-screen despawn and the camera rule
  are unchanged. One row moves: the off-screen despawn takes the reserve over
  the side ring (120 m -> 150 m), which only keeps a fresh spawn alive; the
  lifetime cull already kept cars to 280 m.
- At maximum settings the ring stops at the visible range instead of aiming
  past it. With the default draw distance the ring lands at the LOD distance
  (330 m at spawn distance 4.0); the draw distance extends it (1000 m at 16.0),
  which is what VICE-125 wires to the user control.
- The cap only stops growth: it never shrinks the ring below the stock ring.
  In fog (the smallest far clip, 150 m) the ring stays stock and full: the
  front ring is off screen there, survives its birth frame and drives into
  view, which is what the game always did. A ring smaller than stock would
  drop traffic into the player's view for no gain.
- The traffic rate follows the ring scale too, so a smaller ring is not
  over-filled and a larger one is not starved.
- The upper part of the clamp is bounded by the LOD distance. A cutscene drops
  the FOV part of the LOD multiplier, and with it the rings; the rings followed
  that drop before this change too, so a cutscene behaves as it always did.
