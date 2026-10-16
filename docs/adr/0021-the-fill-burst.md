# ADR 0021: The fill burst of the population and the traffic

Date: 2026-10-16
Status: Accepted
Relates to: VICE-126 (the implementation), ADR 0009 (the count follows the
ring), ADR 0013 (the ring table), ADR 0020 (the caps), VICE-119 (the asks)

## Context

The owner asked for the street to fill at once after boot, an interior exit and
a cutscene end, not by a per-frame trickle (ask 6). The trickle is the fill rate
of the table (`PED_ATTEMPTS_PER_FRAME` and `RANDOM_CAR_ATTEMPTS_PER_FRAME`, 3
times the ring scale: 6 calls a frame at the capped ring). It fills a full ring
in tens of seconds, and ADR 0020 measured what the caps alone cannot do: at the
maximum sliders the caps were reached long before them, because the fill rate
is what limits the counts.

The shipped game had two one-frame fills, both part of the same mechanism:
`CPopulation::GeneratePedsAtStartOfGame` (100 `AddToPopulation` calls in the
frame the boot countdown `m_CountDownToPedsAtStart` reached zero) and
`CCarCtrl::GenerateRandomCars` (100 `GenerateOneRandomCar` calls in the frame
`CountDownToCarsAtStart` reached zero, plus 20 frames of generator spawning).
The cars re-armed their countdown while a cutscene ran, so a cutscene end filled
the street with 100 cars; the pedestrians did not re-arm, so a cutscene end left
the crowd empty.

Two facts bound any change here:

- A car placement is the costlier of the two fills: it runs the collision
  queries of the frame (VICE-121 made them affordable, not free), builds a
  vehicle and its crew. A pedestrian placement runs a node search and the
  placement checks.
- A spawn fails when its model is not resident (`CStreaming::HasModelLoaded`):
  a burst that runs before the streaming has the models spends its attempts for
  nothing.

## Decision

1. A **fill burst** is a bounded work window: `FILL_BURST_FRAMES` (60) frames of
   `FILL_BURST_ATTEMPTS_PER_FRAME` (25) spawn attempts, or of
   `FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME` (100) while the screen hides the fill
   (the fade covers at least `FILL_BURST_HIDDEN_FADE`).
   The bounds live in `src/core/PopulationBudget.h` next to the trickle they
   replace; the state machine and its rules live in `src/core/FillBurst.h`, one
   instance per population (`CPopulation::ms_FillBurst`,
   `CCarCtrl::ms_FillBurst`). A frame of one population is not a frame of the
   other: the two windows run on their own clocks.
2. The window is a **work window, not a stopwatch**. A frame the gates stop (a
   cutscene, the pool down to its reserve, a frame the frame-time watchdog
   skipped) does not spend it and is not a stall: the burst waits for the
   streaming. A frame that runs and places nothing *is* a stall, and
   `FILL_BURST_STALL_FRAMES` (30) stalls in a row give the burst up, so a target
   that cannot be reached cannot keep a burst alive.
3. The burst **stops at the local target**: `FILL_BURST_TARGET_FRACTION` (90 %)
   of the line the trickle stops at (the area budget plus the drivers for the
   pedestrians, `CCarCtrl::GetRandomCarTarget` for the cars), checked before and
   inside the frame. A burst never runs past its target and never fills an area
   that is already full.
4. The **hidden frames** of a transition spend the large budget: the fade the
   game draws over the world covers at least `FILL_BURST_HIDDEN_FADE` (three
   quarters of the screen, `CDraw::FadeValue`), so the fill happens behind a
   dimmed frame at the ring, where the fog already hides a spawn. The fully
   black frames of a fade are that rule's own case, and its dark head and tail.
   A visible frame spends the small budget, four times the trickle at the capped
   ring and still bounded. The burst spends its window while the player can see
   the world only after a transition that has no fade (boot with the intro
   cutscene is the common one), and even then it is the fill of the transition,
   not a continuous rate.
5. **Peds and cars burst together.** The boot and restart countdowns
   (`m_CountDownToPedsAtStart`, `CountDownToCarsAtStart`) arm the burst of their
   own population where they used to make the one-frame call. `CGame::
   ArmFillBursts` arms both: the interior exit (`CGame::SetCurrArea`, the one
   writer of `currArea`, now behind the script's `COMMAND_SET_AREA_VISIBLE` and
   the restart path) and the cutscene end (`CCutsceneMgr::DeleteCutsceneData`,
   the script's `CLEAR_CUTSCENE`). The implicit re-arm of the cars while a
   cutscene runs goes away; the explicit arm at the cutscene end replaces it and
   gives the pedestrians the burst they never had.
6. The **parked cars come with the car burst**: the generator timers are bypassed
   for 20 frames (`GenerateEvenIfPlayerIsCloseCounter`, exactly what the shipped
   car burst did). The generator ring of the table still decides where a parked
   car appears, a few hundred metres out.
7. The burst runs **inside the existing generators** and behind the existing
   gates. The replay gate (`CReplay::IsPlayingBack`) arms nothing and spends
   nothing; the cutscene gate, the pool reserve and the streaming gates are the
   ones the trickle has. The models are checked once per burst frame: the
   zone's pedestrian group for the crowd, the loaded class arrays for the cars.
8. The **random stream is shared and is not reseeded**. The burst draws from the
   one global stream exactly like the trickle, so every draw it makes shifts
   every later draw of the frame and of the frames after it (pedestrian models,
   the accident rolls, the traffic route choices, the weather). The shift is the
   price of the fill and it is documented, not hidden; the car generators keep
   their own per-vehicle seeds (`m_nRouteSeed`), so the AI of an existing car is
   not shifted. The counters of the burst (the attempts and the placements) make
   the shift measurable.

## Alternatives considered

- **Many attempts in one frame** (the shipped 100 + 100 at boot, staged for
  every transition). The frame cost of 200 placements is the spike the ticket
  wants bounded, and a transition with a fade gives 1-2 s of hidden frames to
  spread the same work over. Rejected.
- **A bigger constant trickle** (say 25 calls a frame always). The owner asked
  for the fill of a transition, not for a permanently higher spawn rate: a
  bigger trickle keeps spawning into a full area, and the ring's own target says
  when to stop. Rejected.
- **A separate private random stream for the burst** (so the shift disappears).
  It is a second stream to seed, to keep deterministic and to explain, and the
  ticket says the burst stays inside the existing generators. Rejected.
- **Filling during the cutscene itself** (the world is not drawn there). The
  cutscene camera can be anywhere in the city; the fill belongs to the moment
  the player gets the world back, which is also when the streaming is asked for
  the player's zone. Rejected.

## Consequences

- After an interior exit and a cutscene end the crowd and the traffic fill at
  once: up to 6000 attempts of a window, of which the hidden frames of the
  transition carry a large part without a visible frame cost at all. The boot
  fill replaces the shipped 100 + 100 one-frame burst with the same order of
  work, spread over frames.
- The work of a burst is bounded by construction: 60 frames x 100 attempts at
  the very most, and the 90 % stop line ends it earlier. The visible frames of a
  burst add at most 25 pedestrian and 25 car attempts each, a few times the
  trickle the ring already pays for; the hidden frames add the shipped boot
  burst's own rate while the frame is not drawn. The live frame cost across a
  transition is the orchestrator's measurement (the counters of the burst are in
  `utils/caps-audit/live-counters.py`).
- The counters shift: the burst consumes random draws, so a weather change or an
  accident can land differently after a burst than it would have without one.
  Nothing is seeded differently, and a replay playback is untouched (the fill is
  gated there).
- A burst that cannot place anything ends after 30 empty frames (1 s) and hands
  the fill back to the trickle; a burst that cannot run (the streaming) waits
  for the models without spending its window.
- `CPopulation::GeneratePedsAtStartOfGame` is gone: it was the one-frame boot
  burst this ADR replaces. Its band (the shipped 10 m off-screen floor) does not
  live on: every burst uses the off-screen band of the ring table, because a
  spawn closer than that band is a pop-in on the first turn of the camera, which
  the near bubble of VICE-127 forbids.
