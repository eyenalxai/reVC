# ADR 0027: The far traffic worker

Date: 2026-10-17
Status: Accepted
Relates to: VICE-133 (the implementation), VICE-128 (the layer), VICE-131 (the
threading spike and the recommended seam), VICE-119 (the asks), ADR 0026 (the
layer's decisions), ADR 0024 (the path graph and its grid), ADR 0025 (the near
bubble)

## Context

VICE-128 shipped the far traffic layer single-threaded with the boundary the
VICE-131 threading spike recommended (`/tmp/opencode/vice-131-spike.md` section
4): every world write of the layer is in one applicator, the simulation is
arithmetic on the layer's own state, and the events (creation, removal,
promotion, hand-over) go through the near bubble's gates. VICE-133 asks for the
simulation behind that boundary, on a worker thread.

The spike's evidence bounds the decision:

- The far layer was the only candidate with **no legacy shared state** (section
  3): the near traffic's collision, control, sync and path search all need
  conditions first (the spike's sections 2.2-2.4).
- The layer's own arithmetic is **small**: the VICE-128 harness measured the
  moved arithmetic at 20-25 ns a car and the whole frame's share at 0.4-0.6 us
  at the owner's settings (ADR 0026, "What bounds the work"). A worker therefore
  buys no frame time at today's cap; what it buys is that the layer's frame cost
  stops growing with the cap, which is what VICE-132 (raised maxima) needs.
- The layer's sim reads `CGeneral`'s shared stream at every path pick, and the
  path graph's node flags move at runtime (the script's road toggles,
  `CPathFind::SwitchRoadsOffInArea`, and a save load). A worker must read neither
  live.

## Decision

1. **What moves, and what stays.** The worker owns the per-car simulation: the
   path pick (`CCarCtrl::PickNextNodeRandomly`'s rules), the rail pose
   (`CCarCtrl::ComputeCarOnRailPose`'s rules), the path's own height, the
   heading, the car's own random stream, and the stagger's frame index. The
   applicator (`CFarTraffic::Process`, on the main thread) keeps every world
   write: the creation and the removal (through
   `CCarCtrl::NearBubbleAllowsCreation` / `NearBubbleAllowsRemoval` and the
   existing pool gates), the promotion and the hand-over, the pose application
   (`SetPosition`, the heading, `SetMoveSpeed`, `RemoveAndAdd`, `UpdateRW`,
   `UpdateRwFrame`, `UpdateClumpAlpha`), and the counters. The worker writes no
   car and no world; it publishes poses.

2. **The rules live once (CarOnRails.h).** The near traffic's on-rails car and
   the far sim drive the same arithmetic, and VICE-133 moved it into one
   engine-free header (`src/control/CarOnRails.h`) with two *views* per side: the
   graph (the nodes, the links, the connections) and the car's route state. The
   engine instantiates it from `CVehicle`/`ThePaths` (`CCarRailsEngineView`,
   CarCtrl.cpp), the worker from `SFarTrafficWorld`/`SFarSimCar`
   (`FarTrafficWorker.cpp`), and `CCurves` delegates its two formulas to the
   same header. The alternative - a second copy of the pick in the worker - was
   rejected: two copies of a 120-line state machine drift, and the near path is
   the reference for the far one.
   The port is checked bit for bit, in the sense stated: `utils/far-traffic-worker`
   carries a reference implementation of the *original* CarCtrl.cpp code
   (transcribed statement for statement from `miami`) and drives both over
   20,000 randomized car states (picks, u-turns and clamped curves included)
   *with the same draws*: the route state, the pose, the height and the heading
   agree exactly. The sweep therefore proves the branches and the arithmetic of
   the moved rules - every restriction of the pick's three rounds, the lane
   arithmetic, the curve, the height, the heading - and not a draw sequence:
   the far cars' stream is per car on purpose (Decision 4), and the near
   traffic's stream is untouched by inspection (the delegate
   `CCarCtrl::PickNextNodeRandomly` is the route-seed reseed and one call to the
   shared core).

3. **The snapshot, and the one part of the graph that moves.** The worker reads a
   plain-array copy of the graph, `SFarTrafficWorld` (the node positions, the
   first link and the link count, the node flags, the link geometry, the lane
   counts and the one-way lane offset, the connections and their links), built
   once on the main thread by `CFarTraffic::BuildWorldSnapshot` right after
   `ThePaths.PreparePathData` and `ThePaths.BuildNodeGrid` in `CGame::Initialise`.
   `ThePaths.BuildNodeGrid()` is called at the same point, deliberately: the
   grid used to be built lazily by the first `FindNodeClosestToCoors`, i.e.
   inside the first played frame (one array allocation and the node walk), and
   the ticket's contract puts the snapshot after `PreparePathData` and
   `BuildNodeGrid`. Forcing it moves that one-time cost under the loading
   screen and leaves the first placement a grid that is already there.
   The only fields that change after that are the two node flags: `CPathFind`
   carries a version (`m_nodeFlagsVersion`) that every writer of them bumps (the
   recursive `SwitchOffNodeAndNeighbours`, `MarkRoadsBetweenLevelsNodeAndNeighbours`,
   `Load`), and the applicator copies the flags over when the version moved, with
   the worker joined. A toggle therefore reaches the far cars on the next frame
   instead of within the frame; that is the one behavioural lag of the design
   and it is bounded by one frame.

4. **The randomness is the car's own.** The worker's draws come from a private
   32 bit LCG, one stream per car, seeded from the car's identity at creation
   (`FarTrafficSimRngSeed`: the pool slot and the car's two seeds). `CGeneral`
   and `myrand` are never called: the shared stream belongs to the frame thread,
   and two threads on one cursor would change both sequences (spike section
   2.1). The price is stated: a far car's route sequence is no longer a slice of
   the frame's stream, so the exact path a given far car takes differs from the
   single-threaded build (the same class of change ADR 0026 Decision 10 already
   documented for the layer's draws from the shared stream). The near traffic's
   stream is untouched: `CCarCtrl::PickNextNodeRandomly` still reseeds it from
   `m_nRouteSeed`, exactly as before.

5. **The command channel, and the lockstep.** The worker keeps its own array of
   sim cars, in the same order as the applicator's list of cars, and every list
   change queues one command: ADD (the applicator has built and seeded the car)
   or DROP (the applicator has promoted, removed or lost it; the drop takes the
   last entry's place, the applicator's own rule). Commands accumulate until a
   cycle consumes them, so a frame whose cycle the main loop never hands over
   (an early return from `Idle`) loses nothing: the next cycle applies them all,
   in order. The invariant is: after every cycle the two lists are equal,
   element for element.
   **The ring's bound is real, not assumed.** The command ring
   (`FAR_TRAFFIC_SIM_COMMANDS`, 4 x `FAR_TRAFFIC_MAX`) is far wider than one
   frame needs (one frame's worst case is a drop of every entry the layer holds
   plus the create attempts), but the window between two cycles is not one
   frame: a frame whose cycle the main loop never handed over (an early return
   from `Idle`) leaves its commands queued, and a long run of such frames could
   grow the queue without bound. The applicator therefore catches up: when the
   queue reaches half the ring, it runs the pending cycle inline
   (`FarTrafficCatchUpIfNeeded`, called from every queue mutation) and applies
   its output, so the queue drains and the two lists stay in lockstep. A
   mutation that still cannot fit the ring is *reported* (a sticky flag the
   applicator counts into `ms_nFarTrafficWorkerLost`), never silently dropped,
   and the catch-ups have their own row (`ms_nFarTrafficCatchUps`, 0 in normal
   play).
   The published poses name their entry index *and* the car's pool slot; the
   applicator checks both (and that the slot still holds a far car) before it
   writes anything, and counts a disagreement in `ms_nFarTrafficWorkerLost`
   (must stay 0). The worker's view of the world can therefore never be applied
   to the wrong car.

6. **The frame's order, and the one rule the poses rest on.** The applicator
   applies a published cycle only while the layer's list is exactly what the
   simulation left it as: the hand-over of the cull appends an entry (the end
   of the list, which moves no index), and every drop - the stale entries of
   the prune, the gate pass's switches and removals, the creations - happens
   *after* the apply, at the applicator's own hook. The review of this change
   found the first version pruning inside `AdoptOnCull`, i.e. before the
   apply: a drop swaps the last entry into the dropped one's place, so the
   previous cycle's poses (an entry index and a slot) named the wrong cars for
   a frame, and the mismatch counter - the acceptance row of the boundary -
   moved. The prune moved behind the apply and the cap reads the live count
   without moving the list (`FarTrafficCountLive`); the evidence program runs
   the frame's order in both forms and fails on the pre-fix one, so the
   ordering is a checked property and not a comment.

7. **The window.** The cycle is handed over at the end of the frame
   (`CFarTraffic::RunWorker`, called from `Idle` after the render and the swap's
   own wait) and joined at the top of the next frame (`CFarTraffic::JoinWorker`,
   the first statement of `CGame::Process`, before `CWorld::Process`), so the
   worker runs while the frame's tail and the vsync wait pass and never during
   `CWorld::Process` or the render. The applicator joins again at its top (the
   input is built only with the worker idle). A debug build asserts the window
   (`CFarTraffic::AssertWorkerIdle` at the top of `CWorld::Process` and of
   `RenderScene`); a release build compiles the asserts away.
   A frame whose cycle would do nothing (no commands and not one car of the
   stagger's share) does not wake the worker at all, so a layer that is off (the
   stock draw distance) costs the boundary nothing.

8. **The publication is one atomic flip.** Two output blocks; the worker fills
   the inactive one and flips an `std::atomic<int>` with release ordering; the
   main thread reads the active one after the join. The commands, the sim cars
   and the input are plain arrays the worker owns between the signal and the
   join; no lock is held while a cycle runs, and no allocation happens after
   `CFarTrafficWorker::Start` (the evidence program counts the allocations of a
   cycle: zero).

9. **The two decisions of the pick that leave the arithmetic.** The u-turn
   (`m_nPrevRouteNode == m_nNextRouteNode`) is a world action in the engine
   (`SetStatus(STATUS_PHYSICS)` + `SwitchVehicleToRealPhysics`); the sim flags it
   in the pose (`SFarSimPose::bUTurn`) and the applicator makes the call, in the
   same position of the update the engine made it. The clamped curve (the
   `printf("fout\n")` diagnostic of `PickNextNodeRandomly`) is counted instead of
   printed: the worker performs no I/O, and a print from a worker thread is an
   allocation and a lock. The count is `ms_nFarTrafficShortCurves` (cumulative),
   and the evidence program's sweep exercises the case.
   The alpha reveal stays on the main thread with the rest of the pose
   application: it reads the renderer's on-screen flag and writes the clump
   alpha, both world state.

10. **One frame of lag, accepted.** The cycle a frame's applicator queues is the
   one the next frame's applicator applies (the double buffer), so a far car's
   pose is one frame older than it was in the single-threaded build. The stagger
   is a third of the layer a frame; at the layer's own rates the lag is a
   sub-metre offset of a car that is hundreds of metres away, and it changes
   neither the band, nor the cap, nor the switches, nor the gates. The promotion
   distance keeps its margin (the ring's 280 m floor against the reach's 120 m)
   and the acceptance gauge `ms_nFarTrafficInReach` still must read 0.

11. **The measured split, and the mode switch.** `utils/far-traffic-worker`
    measures the real cycle and the real handshake on this machine:
    - one car's update: 37 ns (the pose alone), 231 ns with the path pick;
    - the frame's cycle at the cap (64 cars, 21.3 poses a frame): **1.0 us
      inline** (the simulation on the frame thread, ~46-48 ns a pose);
    - the boundary's handshake with the worker parked (the game's case: a whole
      frame between two signals, and the hand-over is measured on its own):
      **0.8-0.9 us a frame** (the hand-over 0.75-0.88 - the futex wake that
      starts the worker - and the join 0.03-0.06);
    - a frame with no layer work: **about 0** (the cycle is not offered, the
      worker is not woken);
    - the process's own CPU: the layer's 3-5 us a frame outside the frame's
      other work, the same whether the cycle runs inline or on the worker (the
      simulation moved threads, it did not disappear).
    So at today's cap the boundary is a wash to a small win (the runs read
    +0.00 to +0.14 us of a 33 ms frame, inside the harness's +-0.15 us
    run-to-run noise), and it wins from about **55-64 cars** up: the
    simulation's cost scales with the cap, the handshake's does not. The layer's cap at the
    owner's settings is 58 and the hard top 64, so the ticket's escape hatch
    applies as written - there is no measurable frame win at the current cap -
    and the honest reading of the measurement is that the boundary is the
    deliverable and the cap-independent frame cost is its value. The constant
    `FAR_TRAFFIC_WORKER` (`FarTraffic.h`) selects the worker (1) or the same
    cycle inline (0); the two paths are one implementation, so the switch is a
    cost decision only. The flat CPU cost of an idle worker thread is not in
    these numbers: the thread sleeps on its condition variable between cycles,
    and the layer's counters read the wake rate (`ms_nFarTrafficWorkerCycles`).
    What the harness cannot measure: the applicator's engine calls (the sector
    re-insert, the RW-frame write, the alpha ramp) are the engine's and are not
    linked there; the live rig and the owner's capture are their measurement
    (stated in the ticket's evidence list).

12. **The counters.** `ms_nFarTrafficWorkerCycles` (the cycles; the rate is the
    row over the elapsed time), `ms_nFarTrafficWorkerLost` (a command or a pose
    the cycle and the list disagreed about, and a ring that could not take a
    command: must stay 0), `ms_nFarTrafficShortCurves` (the pick's clamped
    curves) and `ms_nFarTrafficCatchUps` (the pending cycles the applicator ran
    inline because the window between two cycles grew the ring: 0 in normal
    play) join the layer's rows in `utils/caps-audit/live-counters.py`.
    `ms_nFarTrafficUpdates` still counts the poses applied (the stagger's
    work).

## Alternatives considered

- **A second copy of the pick in the worker.** The cheapest port, rejected: the
  near pick is a 120-line state machine and the far layer's own promise (ADR
  0026, "the near traffic's own pick") is that the two drive alike; two copies
  drift. The shared core with views keeps one copy and the evidence program
  proves the port.
- **A lock-free ring buffer for the commands.** The commands are a handful a
  frame and the input is only touched with the worker idle; a lock-free queue
  would be more machinery for a channel that is not contended. Rejected.
- **Sharing the applicator's array with the worker** (one array, the worker
  writing the sim fields). It saves the command channel but couples the two
  threads through one structure; the slot-keyed command list is the same
  lockstep with a boundary that can be reviewed line by line. Rejected.
- **Moving the alpha reveal to the worker.** It reads the renderer's on-screen
  flag and writes the clump alpha (world state, `librw`); the contract forbids
  both. Rejected; it stays in the applicator, at the stagger's rate.
- **A signal-and-join per frame (the worker runs inside the frame call).** It
  would meet the "never during" clause trivially and buy nothing: the frame
  thread would wait for the simulation it just handed over. Rejected; the
  end-of-frame signal with the top-of-frame join is the window with real
  overlap (the frame's tail and the swap wait).
- **A worker per layer share / a pool of workers.** The simulation is under a
  microsecond; a pool would add threads to a problem that fits in one. Rejected.
- **Predicting the next frame's time in the input** (removing the one-frame
  lag). It would have to guess the frame time; the lag is invisible at the
  layer's stagger and the guessed clock is a worse bug than the lag. Rejected.

## Consequences

- **The layer's frame cost stops growing with the cap.** The applicator's cost
  is the poses' world writes plus the gate pass; the simulation is off the frame
  thread, so VICE-132's raised maxima no longer pay per car per frame. At the
  current cap the change is a wash (0.83 us of handshake against 1.0 us of
  simulation).
- **The frame's order is a rule, and it is checked.** The apply reads the
  published poses before any drop of the frame, so the indices the simulation
  wrote cannot be moved under it; the harness runs the order and the pre-fix
  order and fails on the latter (0 mismatches against 31 in the pre-fix run of
  400 frames).
- **The boundary is auditable.** The checklist (the ticket's items, one by one)
  is in `FarTrafficWorker.h` with the code that holds each; the worker module
  includes no engine header and compiles on its own; a debug build asserts the
  window at `CWorld::Process` and `RenderScene`; the rig reads the cycles, the
  lost commands and the clamped curves.
- **The near traffic's arithmetic is one copy, not zero-risk.** The pick and the
  pose move into `CarOnRails.h` and both sides instantiate them, so the near
  path's code changed shape (the same statements behind inlined view accessors).
  The evidence program's reference implementation guards the arithmetic; the
  behavior of the near traffic is otherwise untouched (the same draws, in the
  same order, from the same stream).
- **A far car's route sequence changes** (Decision 4): the car's own stream
  replaces its slice of the frame's. Nothing in the layer's rules depends on the
  sequence.
- **The far snapshot is a copy of the graph that can go stale** for exactly one
  part (the node flags) and exactly one path: a writer that forgets to bump
  `m_nodeFlagsVersion`. The writers are four places, all in `PathFind.cpp`, and
  the version is bumped in each.
- **The snapshot's memory is a fixed cost.** `SFarTrafficWorld` is 320,012
  bytes (313 KB) of BSS: the 9,650 nodes (150 KB), the 3,500 links (82 KB) and
  the 20,400 connections and their link indices (39 KB each). It is built once
  (about 33,000 field copies at load) and never freed; it is the price of a
  worker that reads no live game memory.
- **The worker thread exists from `CGame::Initialise`** (started with the
  snapshot) and is joined in `CGame::ShutDown`; it sleeps between cycles, and a
  frame that has no work never wakes it.
- **The creation sites are the applicator's, and the boundary does not see
  them.** The layer's sites (the fog and the near site, ahead and behind - the
  three-site creation fix, merged from `miami` as `f20074f7`, with
  `FAR_TRAFFIC_SITE_MARGIN` and the refusal-by-cause counters) live in
  `FarTrafficTrySite`/`FarTrafficTryCreate`, i.e. in the applicator's creation
  path, on the main thread, on this side of the boundary. A creation queues its
  ADD (with the car's sim seed) for the worker like any other; a change to the
  sites therefore needs no change to the worker, the ring or the published
  blocks - which is what the boundary was for.
- **What this does not do**: no change to the collision solver, the scene sync
  walk or path searching's shared tables (the spike's deferred candidates, each
  with its own conditions in its section 3); no change to the layer's rules,
  band, cap, sites, switches or gates; no change to the near traffic's behavior.
