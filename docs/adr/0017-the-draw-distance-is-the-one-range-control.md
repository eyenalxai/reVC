# ADR 0017: The draw distance is the one range control

Date: 2026-10-16
Status: Accepted
Relates to: VICE-125 (the implementation), VICE-145 (the local cap), ADR 0013
(the ring table)

## Context

VICE-120 gave the traffic ring one table and capped it by the visible range,
but the ring's size still came from the `SpawnDistance` slider, a second control
next to the draw distance. The two overlapped and neither explained the other:
the spawn distance scaled the ring and its culls (and nothing else), the draw
distance scaled the world's LOD distance (and, through the cap alone, did not
change the ring). The owner asked for one control (ask 4).

## Decision

1. The ring request is the LOD distance over its stock value (the LOD distance
   base times DRAWDIST_DEFAULT), so the ring scales with the draw distance and
   the stock draw distance gives the stock ring. The LOD distance carries the
   FOV factor (Camera.cpp), so the ring keeps following it, and the visible cap
   keeps the ring inside min(LOD distance, far clip) with its floor.
   The ring is also capped locally: `RING_MAX_SCALE` = 2, so the front band
   stays at most 500 m (VICE-145, below).
2. The separate spawn distance slider goes. The display menu keeps the draw
   distance, the ped density and the car density.
3. The old key still loads and maps. A file that raised the spawn distance and
   never moved the draw distance gets the draw distance that asks for the old
   ring (`DRAWDIST_DEFAULT x v`, at every FOV factor, because the factor cancels
   between the two scales); a file that set the draw distance keeps it. Where
   VICE-120's visible cap binds, the mapped ring is the cap of the mapped draw
   distance, the same rule every ring obeys: the map is faithful to the request
   the old value carried, not to the capped ring of a file that had its own draw
   distance. The map is the pure `LegacyRangeDrawDistance` in TrafficRings.h,
   called from the load in re3.cpp, and its range is asserted at compile time.
   The load removes the key from the loaded structure; the file itself loses the
   key with the next save, so until that save a later load maps it again.
4. The ring reads the draw distance live. `CurrentTrafficRings` composes the LOD
   distance from `CRenderer::ms_lodDistScale` and the camera's FOV factor instead
   of reading `TheCamera.LODDistMultiplier`, because the frame order puts the
   menu (where the slider and the old-key map run) before the camera: with the
   composed value the caps of the same frame see the draw distance the menu just
   set. The caps refresh at the start of the frame's update block, after the
   menu and before the population and the traffic read them; the weather (the
   time cycle) lands later in the frame, so the far clip of the caps is one
   frame old, exactly as it is for every reader of the frame. The FOV factor is
   the camera's, so an FOV move is seen from the next frame.
5. The draw distance's bounds and the LOD distance base live in one small
   header, `DrawDistance.h`, read by the slider (Frontend.cpp), the settings
   registry (Settings.cpp), the old key map (re3.cpp), the renderer
   (Renderer.h LOD_DISTANCE) and the ring (TrafficRings.h). The registry row of
   the draw distance now clamps the value at load; it was unbounded, and a stale
   file could put it anywhere before the menu clamped it.
6. The generator ring follows the one control too (230 m and 345 m times the
   scale, so [440, 460) m at the cap): the parked cars appear with the world the
   slider draws, and locally (VICE-145). It stays under the despawn of the cars
   it makes at every scale (the static asserts and the harness carry that), so a
   parked car is never removed at its own door.
7. The population caps follow the ring scale, exactly as VICE-120 left them.

## VICE-145: the local cap, after the ring followed the draw distance all the way

The first cut of point 1 let the ring follow the draw distance up to the visible
range. At the owner's settings (draw distance 16, first person FOV 110, sunny)
that made the scale 7.9, the front band 1970 m and the despawn 2206 m. The
budgets are a fixed number of entities (`MAX_RANDOM_CARS` 1700, the ped cap
5500), so the same cars spread over a 15.3 km^2 shell: the measured 1700-car
target was never even reached (the counter sat at 171-200) and the near streets
emptied, the exact opposite of the design point. The cap keeps the ring local:

- `RING_MAX_SCALE` = 2 caps both scales, after the visible cap and its floor.
  The draw distance still drives the ring up to the cap (the scale reaches 2 at
  a draw distance of 2.4 at FOV 70); past that the world grows and the ring does
  not. Stock settings are unchanged (the scale is 1 there).
- The budgets and the rates follow the capped scale, so the target of the
  density sliders fills a 500 m ring instead of a 2 km one. At the owner's
  settings the target is 408 cars in a 0.99 km^2 ring: 414 cars/km^2, one car
  per ~10 m of street if the streets take 6% of the ground (the estimate is the
  harness's, labelled as one). The state the owner liked (the spawn distance
  slider at 4.0 on 494888ff) had a 636 m ring with 246-341 cars: the cap keeps
  the same density on a smaller ring, and `MAX_RANDOM_CARS` no longer binds.
- The invariant is untouched: the cap only lowers the scale, and every rule of
  the table (the reserve, the camera rule, the generator, the ped band) holds at
  every scale from the floor to the cap (the harness checks the whole matrix).
- Invisibility. With the cap the ring sits far inside the far clip (560 m of
  despawn against 2000 m of clear-weather clip, 1440 m of margin), so the far
  clip no longer hides a spawn. The off-screen band is created out of view, and
  the on-screen band is the original game's own arrangement, a few hundred
  metres out, where the fog and the fade distance of the world around the car do
  the hiding. The stricter blind-spot creation is VICE-127's work.
- The parked cars (the measured `NumParkedCars` 0). The generator ring follows
  the one control too (point 6), so the uncapped scale moved its band from
  [126, 146) m (494888ff, 230 x the FOV factor) to [1792, 1812) m: the player
  never enters a band 1.8 km away, so the generator never created a car. They
  were not removed: the lifetime cull of that state sat at 2206 m, above the
  band, so a car created there would have survived. The cap puts the band at
  [440, 460) m, a local band the player crosses while driving, and the parked
  cars come back.

## What VICE-120 had already done (no work here)

- `ApplyPopulationCaps` and `GetRandomCarTarget` scale with the ring scale.
- The draw distance slider refreshes the caps in its callback, and the per-frame
  refresh exists. This ADR moves that refresh to the start of the update block
  and makes the ring value live (point 4), which is what the same-frame claim
  needed.

## Consequences

- One user-visible control for the range, with a local ring: the draw distance
  moves the ring between the stock ring and the cap, and beyond the cap it moves
  the world only.
- The ring of a lowered draw distance floors at the stock ring: the stock
  numbers are the pop-in distance the game's own band carries, so the lower part
  of the slider moves the world only.
- The old key's map can raise the world's draw distance, because the ring now
  costs draw distance. The ticket documents it; the key is written no more after
  the first save that follows the load.
- A file that holds the old key never gets it written again. A session that ends
  without a save (a crash, a kill) maps the key again on the next load, which is
  the same result: the map is a function of the file.
