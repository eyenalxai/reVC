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

1. A **fill burst** is a bounded work window: `FILL_BURST_FRAMES` (60) frames
   that each spend `FILL_BURST_ATTEMPTS_PER_FRAME` (25) spawn attempts, or
   `FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME` (100) while the screen hides the fill
   (the fade covers at least `FILL_BURST_HIDDEN_FADE`). The bounds live in
   `src/core/PopulationBudget.h` next to the trickle they replace; the state
   machine and its rules live in `src/core/FillBurst.h`, one instance per
   population (`CPopulation::ms_FillBurst`, `CCarCtrl::ms_FillBurst`). A frame
   of one population is not a frame of the other: the two windows run on their
   own clocks.
2. **The contract is one number, for every run**: a burst asks for at most
   `FILL_BURST_FRAMES x FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME` attempts (6000).
   A frame that spends its attempts spends a window frame, placing or not; a
   frame that cannot spend them (the models of the zone are not resident) does
   not spend the window, and it counts against the same give-up bound. A frame
   that ends without a placement is a stall, and `FILL_BURST_STALL_FRAMES` (30)
   frames in a row without one give the burst up and hand the fill back to the
   trickle. So neither an unreachable target nor a streaming that never arrives
   can keep a burst alive, and no run can spend more than the hidden window.
3. The burst **stops at the local target**: `FILL_BURST_TARGET_FRACTION` (90 %)
   of the line the trickle stops at (the area budget plus the drivers for the
   pedestrians, `CCarCtrl::GetRandomCarTarget` for the cars), checked before the
   frame and every attempt inside it. The pedestrian burst recomputes the totals
   every attempt, exactly as the shipped start-of-game burst did, so the check
   and the gate inside `AddToPopulation` read what the frame has placed so far,
   and the overshoot of the line is one placement (a group of 6 at the very
   most). The pool is re-read every `FILL_BURST_POOL_CHECK_ATTEMPTS` attempts,
   so a frame can pass it by one step of the loop (24 pedestrians at the very
   most) before it stops.
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
   ArmFillBursts` arms both: the interior exit (`CGame::SetCurrArea`) and the
   cutscene end (`CCutsceneMgr::DeleteCutsceneData`, the script's
   `CLEAR_CUTSCENE`). The implicit re-arm of the cars while a cutscene runs goes
   away; the explicit arm at the cutscene end replaces it and gives the
   pedestrians the burst they never had.

   `CGame::SetCurrArea` is the writer of the area changes of the script and the
   restart, not the only writer of `CGame::currArea`: the boot
   (`CGame::Initialise`), the reset (`ReInitGameObjectVariables`), the save load
   (`CGenericGameStorage` reads the field out of the save file) and the replay
   player (`CReplay::Process` and its restore) set the field directly, and none
   of them arms a burst. A loaded save that restores an interior therefore arms
   nothing: the exit from that interior is the moment the street appears, and
   that exit comes through `SetCurrArea`.
6. The **parked cars come with the car arms the shipped game had**: the
   countdowns (boot, restart) and the cutscene end bypass the timer of every
   generator for 20 frames (`GenerateEvenIfPlayerIsCloseCounter`), exactly what
   the shipped burst of those paths did. The interior exit does not ask for it:
   that trigger is new, and bypassing the timers there would be a change of the
   parked cars the ticket never asked for. The bypass is the timer only, never
   the ring: a generator still places its car inside its band of the table, a
   few hundred metres out (`CCarGenerator::CheckIfWithinRangeOfAnyPlayers`), so
   a parked car never appears at the player.
7. The burst runs **inside the existing generators** and behind the existing
   gates. The replay gate (`CReplay::IsPlayingBack`) arms nothing and spends
   nothing; the cutscene gate, the pool reserve and the streaming gates are the
   ones the trickle has. The models are checked once per burst frame: the
   zone's pedestrian group for the crowd, the loaded class arrays for the cars.
8. The **random stream is shared and is not reseeded by the burst**. The burst
   draws from the one global stream exactly like the trickle. What the shift
   reaches, accurately:

   - the car AI does **not** stay shifted: it reseeds the global stream from its
     own `m_nRouteSeed` at every path pick (`CCarCtrl::PickNextNodeRandomly` and
     the three other sites), so the next path pick of a car that has a route
     seed draws from its own seed again;
   - the draws that do follow the shared stream past a burst are the pedestrian
     models, the pedestrian AI (the wander path of `PedAI.cpp`), the script
     randomness (`COMMAND_GENERATE_RANDOM_FLOAT` of `Script.cpp`), the accident
     rolls and the weather.

   The shift is the price of the fill and it is documented, not hidden. The
   counters of the burst (the attempts and the placements) make it measurable.

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
- **Decrementing the window only on the frames that place something.** It makes
  the wall clock of a run unbounded in the attempts: 60 placing frames with up
  to 29 stalls between each is about 177,000 attempts, not 6000. Rejected; the
  window is spent by any frame that spends attempts.

## Consequences

- After an interior exit and a cutscene end the crowd and the traffic fill at
  once: at most 6000 attempts of a window each, of which the hidden frames of
  the transition carry a large part without a visible frame cost at all. The
  boot fill replaces the shipped 100 + 100 one-frame burst with the same order
  of work, spread over frames.
- **The work of a burst is bounded by construction**: 60 frames x 100 attempts
  at the very most, and the 90 % stop line ends it earlier. The wall clock is
  not constructional: a frame the caller's gates stop (a cutscene, the pool
  down to its reserve, a frame the frame-time watchdog skipped) is not consulted
  at all, and a frame the streaming stops spends no window frame (it counts
  against the give-up bound), so the streaming, the pool and a cutscene stretch
  the frames a run needs. The visible frames of a burst add at most 25
  pedestrian and 25 car attempts each, a few times the trickle the ring already
  pays for; the hidden frames add the shipped boot burst's own rate while the
  frame is not drawn.
- **The acceptance, restated.** What is guaranteed: the burst asks for at most
  6000 attempts, it stops at 90 % of the local target, and it gives up after 30
  frames without a placement. What is *not* guaranteed by construction: the
  1-2 s of the acceptance. At the capped ring the car target is 408 (90 % is
  368) and the window is 60 *spent* frames, so a transition with a long fade can
  reach it and a transition without one cannot; the gates stretch the wall clock
  further. The live numbers (the counter timeline, the frame time) come from the
  orchestrator after the merge, with `utils/caps-audit/live-counters.py` and the
  perf recipe of VICE-119.
- The counters shift: the burst consumes random draws of the shared stream, so
  a weather change or an accident can land differently after a burst than it
  would have without one. The car AI is not affected (it reseeds per path pick),
  and a replay playback is untouched (the fill is gated there).
- A burst that cannot place anything ends after 30 frames without a placement
  (1 s of frames that spend attempts, or of streaming waits) and hands the fill
  back to the trickle; a burst that cannot run (the streaming) waits, bounded,
  without spending its window.
- The pool arithmetic of `PopulationBudget.h` follows the burst: the widest step
  of a burst loop between two pool reads is `FILL_BURST_POOL_CHECK_ATTEMPTS`
  attempts, so at most 24 on-foot pedestrians (a mall group reaches 6) or 16 car
  crew members, both far inside `PED_POOL_RESERVE` (332). The point of the
  reserve is unchanged: it holds the frame of a fill and the one-off paths. The
  number ADR 0020 put on that frame (about 212 slots: 120 pedestrians and 92
  crew members, the trickle of the ring top of its day) is superseded by this
  step; the reserve itself stands.
- `CPopulation::GeneratePedsAtStartOfGame` is gone: it was the one-frame boot
  burst this ADR replaces. Its band (the shipped 10 m off-screen floor) does not
  live on: every burst uses the off-screen band of the ring table, because a
  spawn closer than that band is a pop-in on the first turn of the camera, which
  the near bubble of VICE-127 forbids.
