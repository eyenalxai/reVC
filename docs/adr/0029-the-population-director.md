# ADR 0029: The population director apportions the global budgets

Date: 2026-10-19
Status: Accepted
Relates to: VICE-152 (the implementation), ADR 0009 (the count follows the
ring), ADR 0013 (one traffic ring table), ADR 0017 (the draw distance is the
one range control), ADR 0021 (the fill burst), VICE-158 (the covered fill),
VICE-190 (the far layer's migration)

## Context

The global car target (`CCarCtrl::GetRandomCarTarget`) and the pedestrian
count target (`CPopulation`'s own line) are totals. The ambient trickle spent
them wherever the ring's dice fell, so the areas the dice missed stayed dry for
as long as the player was there: the owner's "a packed street, then a
completely empty road". The service (VICE-150) made placement legal and
planned; it did not make the *distribution* consistent. The far layer
(VICE-128) also created and removed cars outside any ledger class of its own.

## Decision

1. The map is covered by **density cells** of 128 m (`SpawnDirector.h`): 32 x
   32 over the world's 4000 x 4000 m. The cells hold **weights** - the live
   info zone's density, the player's `m_fRoadDensity` and the view - and the
   weights are apportioned to the existing global targets by the
   largest-remainder method over integer targets. The per-cell target is
   `total x weight / sum(weights)`, clamped to the cell's **lane capacity**
   (the site index's travelled lanes times a cars-a-lane or peds-a-lane
   constant). The cell targets' sum is the global target, exactly: the engine
   passes the targets in (`CCarCtrl::GetRandomCarTarget`, the population's
   count target), so the director never recomputes them and the caps hold by
   construction. A total above the sum of the ceilings leaves the sum short
   and is counted as a breach, never hidden.
2. The **floor** is one per cell that carries a street (or a water lane) and a
   target: the "no street goes dry" line the consistency metric measures.
3. The maintenance tick runs once a frame in the ambient path. The deficient
   cells are ordered by the largest deficit fraction first (the fair share:
   the far cells are not starved by the near ones the flow keeps re-opening),
   then by proximity; the visibility is the gate, not the order (a cell whose
   centre the ladder refuses is never served, and the service's plan ranks the
   sites inside a served cell by the same arms). A frame with no deficit hands
   nothing out, so the trickle stops at the targets; the rate itself stays the
   ring's.
4. The request's **target** sits one ring behind the served cell along the
   player's travel, and its cone is the half-plane ahead of that target. The
   reason is the primitive's speed test: it refuses a car that drives away
   from the request's target relative to the player's own speed, so with the
   player moving the sites the test can accept are exactly the sites ahead of
   the target along the travel. The served cell's whole block sits there, and
   the plan's visibility rank then decides which of its sites may be used.
5. The over-target cells **release** cars at their far edge through the near
   cull's own gate (hidden, past the grace, outside the bubble): the slack is
   the flow's jitter while the area is below its budget, and any surplus once
   the area is at or above it, because then the budget itself is the ceiling
   the dry cells wait behind.
6. The **consistency metric** is the per-cell minimum over a drive plus the
   clock of each cell's stay below its floor; a stay past the fill bound
   (10 s, derived from the frame's own rate: the 408-car budget refills in
   2.3 s and the bound is four times that) is a defect. The per-cell minimum
   is the before/after number: a cell the old dice missed shows 0.
7. The **water cells** carry their own share of the car total (a boat is a
   random car), and a request aimed at a water cell asks the service for a
   site that may be water.
8. The modes are per frame: the map, the interior (the car target is zero - no
   road planning inside interiors - and the ped target is the population's
   flat interior budget) and off-map (no active set).
9. The **far layer** is counted in the service's ledger under its own class,
   `SPAWN_PURPOSE_FAR`, for its creations and removals until its own migration
   (VICE-190). The layer is not a requester yet.
10. The slow-frame pedestrian watchdog of `CGame::Process` stays: it is the
    frame's guard against filling the crowd on a frame that is already late,
    and the director's own tick is arithmetic over tens of cells.

## Consequences

- The ambient path's placement policy changes: the camera and ring policy of
  the old call (the front and side bands, the angle limits, the top-down
  branch) dies with the ticket, and the director's cells replace it. The
  service's gates still decide every creation, so no car appears where it
  could not before; the *choice* of where the traffic is needed moves from
  the frame's dice to the cells' deficits.
- The targets follow the player's cell, the mode and the sliders: the refresh
  runs when the player's cell or the mode moves, every second, or when a
  slider moves the target by a twentieth. A zone change or a day/night blend
  lands with the next refresh.
- The addons: the car track's cells are the traffic's own; the ped track's
  targets are the director's now, but its requests stay the population's
  trickle until the pedestrian requester migrates, so the ped cells' metric
  will show the ring's own placement until then (the design's later ticket).
- The evidence: `utils/spawn-service/director.cpp` drives a scripted route
  over a synthetic city of the shipped shape (the real site index, the real
  service core, the real director), once with the director and once with the
  ring dice. The after drive ends with no defect and no starved cell (the
  per-cell minima 0/2/10: min, median, max); the ring drive ends with 112
  defects and 17 starved cells. The target sums equal the budgets at every
  refresh (408 cars, 514 peds at the owner's sliders in the harness), and the
  caps are never exceeded.
- The engine's live rows: the service's conservation pair carries the car
  track; the director's own `SSpawnDirectorTelemetry` the ped track, the
  consistency rows and the request/release rows; the live reader reads both
  structs at the offsets the static asserts pin.
