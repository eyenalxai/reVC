# ADR 0018: The caps between the target and the pool

Date: 2026-10-16
Status: Accepted
Relates to: VICE-123 (the implementation), ADR 0009 (the count follows the ring),
ADR 0013 (the ring table), VICE-134 (the pools it resized), VICE-119 (the asks)

## Context

The owner asked for a lot of cars and pedestrians at the maximum sliders (asks 3
and 5). The density formulas scale with the ring area (ADR 0009) and the ring
follows the draw distance (ADR 0017), so at the maximum sliders the formulas ask
for about 21,000 pedestrians and 6,300 random cars at the sunny ring. The pools
hold 8,192 pedestrians and 2,560 vehicles. Between a formula and a pool sit the
caps, and the audit found three that did not do what their name said:

- `MAX_PEDS_IN_USE` (5,500) was described as the on-foot top, but the check in
  `AddToPopulation` compared `ms_nTotalPeds` against it, and `ms_nTotalPeds`
  counts the drivers. Every car the owner added removed a pedestrian from the
  crowd: at 2,400 cars the on-foot count stopped at 3,100.
- `MAX_RANDOM_CARS` (1,700) was left over from the spawn-distance era, when the
  formula asked for 1,632 at the top. With the ring scale it asks for 6,300, and
  the 1,700 clamp was the first thing that bound — below the car budget of 2,400
  and far below the pool of 2,560. Nothing in the game said why the target
  stopped there.
- The generator parked-car cap (20) was a constant while every other count
  followed the ring. It is also counted in the car budget, so it trades with the
  random traffic rather than with the pool.

Measured at the maximum sliders in sunny weather, before this change: 149..201
random cars against the 1,700 target and the 2,400 budget, 2,251..5,500
pedestrians against the 5,500 cap (the cap was reached; the drivers inside it
pushed the on-foot count down), 0..7 parked cars, 57..63 vehicle models
resident, ped pool 5,417/8,192, vehicle pool 153/2,560, dummy pool 2,338/2,800,
visible entities 361..1,233 of 4,000.

## Decision

1. The caps of the population and the traffic live in one table,
   `src/core/PopulationCaps.h`, with the pool each one protects and the pool
   arithmetic that ties them together. The compile-time invariants of the table
   are checked there: the ped line and the pools cannot drift apart silently.
2. The pedestrian cap is the count ON FOOT. The drivers and the passengers of
   the cars are added on top of it where the count is compared with the cap
   (`AddToPopulation` adds the drivers it counts in `ms_nTotalPeds` back; the
   passengers stay out of `ms_nTotalPeds` entirely). A full street no longer
   eats the crowd on the sidewalk.
3. `MAX_RANDOM_CARS` holds the whole car budget (`MAX_CARS_IN_USE`, 2,400). The
   total budget is the real cap: `GenerateOneRandomCar` stops when the random,
   parked, mission, law and emergency cars together reach it, so the random
   target can aim at the budget without blocking the other cars.
4. The parked cars follow the ring area like every other count
   (`ParkedCarTarget`: 20 at the stock ring, the map's generator count at the
   top). They stay inside the same car budget.
5. The pool is the hard resource behind the budget, and the budget alone cannot
   guarantee it: the passengers are a random draw (p = 0.125 per seat), and the
   budget of a frame is read once. Every creation path the budgets drive
   therefore refuses to create when the free ped pool spaces fall below the
   reserve (292): the on-foot loop, the car loop (which covers the drivers) and
   the passenger loop. The one-off paths outside the budgets (a script car, the
   helicopter's swat, the rope) are what the reserve is for.
6. The per-frame attempts stay as they are: they are the fill rate, not a cap.
   The audit did not raise them; VICE-126 (burst fill) owns the fill time.
7. The streaming budget is documented, not changed. `MAXVEHICLESLOADED` (90 held
   resident, `desiredNumVehiclesLoaded` 90 on a large-memory machine) is above
   every model the population can ask for: 56 population models from the 11
   class arrays plus about 15 police and gang models, out of the 107 vehicle
   models in the shipped data (51 are the "ignore" class the population never
   chooses). Measured live: 57..63 resident. The pools VICE-134 resized hold
   their measured demand (alpha entity list 7,848 of 9,000 nodes, building list
   3,132 of 4,000, dummy pool 2,336 + 460 of 2,800; this audit re-reads them).

## Consequences

- The maximum sliders now ask for 5,100 on-foot pedestrians AND up to 2,400
  cars; the pool holds 5,100 + 2,400 drivers + 400 passengers + 292 reserve =
  8,192. The on-foot top is below the old 5,500 cap, because the old cap
  included the drivers: it is the same pool spent on a crowd instead of a crowd
  plus cars.
- The 2,400 car budget is the honest car target. The old 1,700 was not.
- The random car target is 2,400, but the live count stays near 200 at the
  maximum sliders: the caps are not what limits it. Fill rate and the ring
  geometry are (VICE-126, VICE-127), and this audit says so with numbers.
- The parked cars can be up to the generator count (185; the map has 176), but
  at the maximum ring the generator holds its car in a 20 m band at the far ring
  (1,792..1,812 m), and the parked count measured 0..7. The parked density the
  owner expects is a tuning question (VICE-132): the generator band, not the
  cap, decides it at a wide ring. The cap no longer binds before the band does.
- The three earlier numbers of the program (1,632 random cars, 5,440
  pedestrians, 2,400 cars) are superseded: the ring scale moved (ADR 0017), and
  the ped number is now the on-foot count with the drivers on top.
