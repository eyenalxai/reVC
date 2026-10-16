# ADR 0020: The caps between the target and the pool

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
pushed the on-foot count down), 0..12 parked cars, 57..65 vehicle models
resident, ped pool 5,417/8,192, vehicle pool 153/2,560, dummy pool 2,338/2,800,
visible entities 361..1,233 of 4,000.

## Decision

1. The budgets of the population and the traffic live in one table,
   `src/core/PopulationBudget.h`: the density units, the caps (the hard tops),
   the fill rates and the streaming model budget, with the pool each cap
   protects and the pool arithmetic that ties them together. The compile-time
   invariants of the table are checked there. The fill rates are in the table
   because the cap a rate cannot fill is as wrong as a cap the pool cannot
   hold; the header says so.
2. The pedestrian cap is the count ON FOOT. The drivers and the passengers of
   the cars are added on top of it where the count is compared with the cap
   (`AddToPopulation` adds the drivers it counts in `ms_nTotalPeds` back; the
   passengers stay out of `ms_nTotalPeds` entirely). A full street no longer
   eats the crowd on the sidewalk.
3. `MAX_RANDOM_CARS` holds the whole car budget (`MAX_CARS_IN_USE`, 2,400). The
   total budget is the real cap: `NumCarsInUse` (the random, parked, mission,
   law and emergency cars) is checked in `GenerateOneRandomCar`,
   `GenerateEmergencyServicesCar`, `CarGen.cpp` and the cop ped path, so the
   random target can aim at the budget without blocking the other cars.
   A law-enforcer random car increments `NumLawEnforcerCars` and `NumRandomCars`
   both, so the sum counts it twice and the budget blocks one car early per live
   law car (the spawner keeps at most 3, other paths add a few; measured 5..6).
   The double count is documented, not changed: it errs on the safe side.
4. The parked cars follow the ring area like every other count
   (`ParkedCarTarget`: 20 at the stock ring, the map's generator count at the
   top). They stay inside the same car budget.
5. The pool is the hard resource behind the budget, and the budget alone cannot
   guarantee it: the passengers are a random draw (p = 0.125 per seat), a frame
   reads its budget once, and the paths outside the budgets (scripts, the police
   crews) create pedestrians too. The line is therefore held by the creation
   paths, each of which refuses to create when the free ped pool spaces are
   below `PED_POOL_RESERVE` (332): `CPopulation::Update` and
   `GeneratePedsAtStartOfGame` for the on-foot crowd,
   `CCarCtrl::GenerateRandomCars` for the cars (and so their drivers),
   `CreateCarFromPlacement` for the passengers of a car. The worst frame burst
   is about 212 (120 on-foot pedestrians and 92 car crew members); the rest of
   the reserve is for the one-off paths.
   `CPopulation::RemovePedsIfThePoolGetsFull` stays as the valve behind them.
   The passenger allowance `MAX_PED_PASSENGERS` (400) caps the passengers the
   traffic counts: they are the ones `ms_nTotalPeds` subtracts, so the pool
   arithmetic must carry them. Every other occupant is already inside the lines
   above — the police crews count as cops in `ms_nTotalPeds`, and the script
   passengers count like any pedestrian.
6. The ped pool arithmetic, with the driver bound being the vehicle pool (a car
   of the pool can hold a driver; a parked one can be stolen):
   on foot 4,900 + drivers 2,560 + passengers 400 + reserve 332 = 8,192.
7. The per-frame attempts stay as they are: they are the fill rate, not a cap.
   The audit did not raise them; VICE-126 (burst fill) owns the fill time.
8. The streaming budget is documented, not changed. `MAXVEHICLESLOADED` (90)
   is above every model the population can ask for: the class arrays hold 56
   models in the shipped data, the population chooses 14 more outside them (7
   police models, 2 emergency, 5 gang cars), 70 in total, of the 107 vehicle
   rows (51 are "ignore"; the specials are read out of the data by
   `utils/caps-audit`). `desiredNumVehiclesLoaded` is the memory formula capped
   at 90. Measured live: 57..65 resident. The pools VICE-134 resized hold their
   measured demand (alpha entity list 7,848 of 9,000 nodes, building list 3,132
   of 4,000, dummy pool 2,336 + 460 of 2,800; the audit re-reads them).

## Consequences

- The maximum sliders now ask for 4,900 on-foot pedestrians AND up to 2,400
  cars; the pool holds 4,900 + 2,560 drivers + 400 passengers + 332 reserve =
  8,192. The on-foot top is below the old 5,500 cap, because the old cap
  included the drivers: it is the same pool spent on a crowd instead of a crowd
  plus cars.
- The 2,400 car budget is the honest car target. The old 1,700 was not.
- The random car target is 2,400, but the live count stays near 200 at the
  maximum sliders: the caps are not what limits it. Fill rate and the ring
  geometry are (VICE-126, VICE-127), and this audit says so with numbers.
- The parked cars can be up to the generator count (185; the map has 176), but
  at the maximum ring the generator holds its car in a 20 m band at the far ring
  (1,792..1,812 m), and the parked count measured 0..12. The parked density the
  owner expects is a tuning question (VICE-132): the generator band, not the
  cap, decides it at a wide ring. The cap no longer binds before the band does.
- The audit's own cost: it reads the ped pool flags (8,192 bytes) once a frame
  in `CPopulation::Update`, once a frame in `GenerateRandomCars` and once per
  car with passengers; measured 0.65 us a scan on this machine, so about 1.3 us
  a frame while the car loop creates nothing and up to 16 us in the frame that
  creates its 23 cars. The program measures this itself, so the number cannot
  go stale.
- The three earlier numbers of the program (1,632 random cars, 5,440
  pedestrians, 2,400 cars) are superseded: the ring scale moved (ADR 0017), and
  the ped number is now the on-foot count with the drivers on top.
