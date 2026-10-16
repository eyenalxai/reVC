# ADR 0017: The draw distance is the one range control

Date: 2026-10-16
Status: Accepted
Relates to: VICE-125 (the implementation), ADR 0013 (the ring table)

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
   scale): the parked cars appear with the world the slider draws. It stays
   under the despawn of the cars it makes at every scale (the static asserts and
   the harness carry that), so a parked car is never removed at its own door.
7. The population caps follow the ring scale, exactly as VICE-120 left them.

## What VICE-120 had already done (no work here)

- `ApplyPopulationCaps` and `GetRandomCarTarget` scale with the ring scale.
- The draw distance slider refreshes the caps in its callback, and the per-frame
  refresh exists. This ADR moves that refresh to the start of the update block
  and makes the ring value live (point 4), which is what the same-frame claim
  needed.

## Consequences

- One user-visible control for the range.
- The ring of a lowered draw distance floors at the stock ring: the stock
  numbers are the pop-in distance the game's own band carries, so the lower part
  of the slider moves the world only.
- The old key's map can raise the world's draw distance, because the ring now
  costs draw distance. The ticket documents it; the key is written no more after
  the first save that follows the load.
- A file that holds the old key never gets it written again. A session that ends
  without a save (a crash, a kill) maps the key again on the next load, which is
  the same result: the map is a function of the file.
