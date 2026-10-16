# ADR 0014: The draw distance is the one range control

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

1. The ring request is the LOD distance over its stock value (300 x 1.2), so the
   ring scales with the draw distance and the stock draw distance gives the
   stock ring. The LOD distance already carries the FOV factor (Camera.cpp), so
   the ring keeps following it, and the visible cap keeps the ring inside
   min(LOD distance, far clip) with its floor.
2. The separate spawn distance slider goes. The display menu keeps the draw
   distance, the ped density and the car density.
3. The old key still loads and maps. A file that raised the spawn distance and
   never moved the draw distance gets the draw distance that gives the old ring
   (`1.2 x v`, the same ring at every FOV factor, because the factor cancels); a
   file that set the draw distance keeps it. The key is dropped with the next
   save, so the map runs once. The map is the pure
   `LegacyRangeDrawDistance` in TrafficRings.h, called from the load in re3.cpp,
   and its range is asserted at compile time.
4. The draw distance's bounds move to one small header, `DrawDistance.h`, so
   the slider (Frontend.cpp), the settings registry (Settings.cpp), the old key
   map (re3.cpp) and the ring (TrafficRings.h) read the same range. The registry
   row of the draw distance now clamps the value at load; it was unbounded, and
   a stale file could put it anywhere before the menu clamped it.
5. The generator ring follows the one control too (230 m and 345 m times the
   scale): the parked cars appear with the world the slider draws. It stays
   under the despawn of the cars it makes at every scale (the static asserts and
   the harness carry that), so a parked car is never removed at its own door.
6. The population caps follow the ring scale, exactly as VICE-120 left them.

## What VICE-120 had already done (no work here)

- The draw distance slider refreshes the population caps in the same frame
  (Frontend.cpp, `MENUACTION_DRAWDIST`), and the game refreshes them once a
  frame after the time cycle, because the weather moves the visible range.
- `ApplyPopulationCaps` and `GetRandomCarTarget` scale with the ring scale, so
  the caps follow the control without a new formula.

## Consequences

- One user-visible control for the range.
- The ring of a lowered draw distance floors at the stock ring: the stock
  numbers are the pop-in distance the game's own band carries, so the lower part
  of the slider moves the world only.
- The old key's map can raise the world's draw distance, because the ring now
  costs draw distance. The ticket documents it; the key is consumed once and a
  file without the old key is untouched.
- A file that holds the old key never gets it written again.
