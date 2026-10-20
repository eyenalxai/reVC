# ADR 0030: The spawn planner worker

Date: 2026-10-20
Status: Accepted
Relates to: VICE-160 (the implementation), VICE-150 (the planner), VICE-149
(the occupancy store), VICE-148 (the site index), VICE-133 (the far traffic
worker, the pattern), ADR 0025 (the near bubble), ADR 0027 (the far traffic
worker)

## Context

VICE-150 put the planner in one call: the offer walk, the cheap gates and the
bounded top-K rank, the deep facts (the live zone and
`CPathFind::TestCoorsCloseness`) and the attempts, on the frame thread. The
harness measured the whole plan at about 5 us a call over 197 candidates
(`utils/spawn-service/core.cpp`), of which the rank is about 1.4-2.6 us and
the rest is the offer walk, the ladder's candidates and the deep facts.

The ticket's scope is the rank: "planning leaves the frame thread". The rank
is arithmetic on plain records - the site's position, the ladder's arm, the
occupancy store's answers - and it grows with the candidate count. The deep
facts are world queries and the attempts are creations; both must stay on the
main thread. The far traffic layer's worker (ADR 0027) is the pattern: a
read-only snapshot, a double-buffered output, a command ring, one thread, and
a window between the end of `Idle` and the top of `CGame::Process`.

## Decision

1. **What moves, and what stays.** The worker owns the cheap gates (the site
   reservation read, the cell capacity, the ladder's arm) and the bounded
   top-K rank. The main thread owns the offer walk (the index's hash query and
   the ladder's candidates, the frustum and occluder facts and the ray
   budget), the reservation, the deep facts, the attempts, the counters and
   every world query. The worker writes no store, no counter and no world.

2. **One copy of the rank's rules.** The rank is `SpawnRankSites`
   (`SpawnService.h`), called by the inline planner (`SpawnPlanSites`) and by
   the worker's cycle. The tail is `SpawnPlanFromRank`, called by the inline
   planner and by the service's apply of a published plan. The worker is not a
   second implementation: it is the same two functions behind the boundary.
   The evidence program compares the two paths site by site.

3. **The site snapshot.** `CSpawnService::BuildPlannerWorker`, right after
   `CSpawnSites::Build`, copies the index's sites into the worker's snapshot
   and starts the thread. The geometry, the node pairs, the lanes and the
   rarities never move after the build; the flags move with the script's road
   toggles and a save load. `CSpawnSites::RefreshFlags` re-derives them and
   bumps its own refresh count; `RunPlannerWorker` sees the count move and
   copies the flags with the worker idle (the `FarTrafficRefreshWorldFlags`
   pattern).

4. **The occupancy copy.** The worker ranks against the frame-start store, not
   the store. `RunPlannerWorker` publishes the vehicle counts and the
   reservation table into the inactive copy and flips one atomic; the copy is
   exactly the state the applying frame starts from (the hand-over is after
   the render, and nothing writes the store until `CWorld::Process`). The
   ped counts and the handle records are not copied: the rank reads vehicles.
   `SSpawnOccupancyView` is the one read surface of the store and the copy,
   so `SiteReserved`/`CountAtPosition` are one implementation.

5. **The command ring and the pipeline.** A road-site request that does not
   need the car in the same call builds its offers and the ladder's candidates
   and queues one `SSpawnRankCommand` (`SPAWN_PLANNER_RING` = 16). At the end
   of the frame the main thread publishes the occupancy copy and hands the
   cycle over. The worker publishes one `SSpawnRankedPlan` per command, in
   order, in a double-buffered block flipped with one atomic. The join at the
   top of `CGame::Process` moves the batch into the main thread's pending
   queue; each request applies one plan at its head. A full ring (a missed
   hand-over) falls back to the inline planner and counts the fallback
   (`m_nPlannerWorkerLost`), so a request is never refused for the pipeline's
   own reason.

6. **The one-frame lag, stated.** A plan ranked on frame N-1 is applied on
   frame N: a road-site car appears one frame after its request. The request
   gates (the budget and the primary model's readiness) ran on the queue
   frame, so the apply admits at most one creation on a budget that filled in
   between; the primitive's own checks and the pool reserve stand. The
   explicit kind keeps the inline planner and the same-frame handle; a
   road-site caller that needs the handle (`ppCreated`) also keeps it. The
   parked kind, when its requester lands, is synchronous and keeps it too.

7. **The contention and the prefix bound.** The worker ranks against the
   frame-start copy; the tail re-checks the live store for each ranked
   candidate (a held reservation, a cell at its capacity, the reserve itself).
   A conflict refuses with OCCUPANCY and the tail moves to the next ranked
   candidate. The published prefix is `SPAWN_SERVICE_DEEP_CANDIDATES` (8), the
   same bound the inline planner's stage two has. A reservation conflict does
   not move the rank's other keys, so the plans and the refusal rows match
   the inline planner's for the same store state. A cell that fills after the
   publish moves the rank's occupancy key for the candidates in that cell, so
   the inline rank over the filled store orders the rest differently and may
   count a different number of cheap drops; the plans still agree. Past the
   published prefix the worker's plan is empty (the stated bound) where the
   inline planner could reach a ninth candidate; the request is refused for
   that frame. A site that was closed at the publish and opened before the
   apply cannot enter the plan (the copy is a filter); the next request picks
   it up.

8. **The write boundary's audit.** A debug build asserts the worker is idle at
   the main thread's entry points (`AssertIdle`, called by `CWorld::Process`
   and `RenderScene` through `CSpawnService::AssertPlannerWorkerIdle`), and the
   cycle hashes the site snapshot, the occupancy copy, the commands and the
   other output block before and after the rank and asserts they did not
   move. The output block it was handed is the only write. The thread, the
   mutex and the two condition variables are created once by `Start`; a cycle
   allocates nothing (the harness counts `operator new`).

9. **The telemetry keeps one owner.** The worker writes no counter. Its plan
   carries the candidate count and the cheap refusal counts; the main thread's
   apply adds them to `SSpawnServiceTelemetry` with the deep and attempt
   refusals. Three rows are appended at the end of the struct: the cycles
   (`m_nPlannerWorkerCycles`), the commands the pipeline could not hold
   (`m_nPlannerWorkerLost`, must stay 0) and the pending queue's depth at the
   join (`m_nPlannerWorkerQueueDepth`, a gauge). The live reader
   (`utils/caps-audit/live-counters.py`) pins the new offsets (232, 236, 240;
   the struct grows to 248 B with its alignment).

## Consequences

The rank's frame-thread cost is gone; the frame thread pays the occupancy
publish, the command copy and the plan's main-thread tail. The evidence
program (`utils/spawn-service/worker.cpp`) measured, over 400 scripted frames
and 190 candidates a request, on this machine:

| row | debug build | release build (pinned) |
| --- | --- | --- |
| the inline rank (`SpawnRankSites`) | 1.25 us | 1.3 us |
| the occupancy publish (227,000 B) | 5.6-6.2 us | 4.0-4.3 us |
| the command copy (20,544 B) | 0.2-0.3 us | 0.16 us |
| the hand-over + join, serialized | 459 us (the audit) | 5.9-6.3 us |
| the same cycle inline | 448 us (the audit) | 1.5-1.6 us |

The debug column is the module's write audit hashing the inputs every cycle;
the game's Release build has it off. The release column is the same program
compiled with `-DNDEBUG` and pinned to one core (`taskset -c 0`). The release
numbers are the real ones.
The honest trade: at 190 candidates the 227 KB occupancy copy costs more than
the 1.43 us rank it replaces, so the worker is not a frame-time win at the
stock density. What it buys is that the rank's cost stops growing with the
candidate count and leaves the frame thread: the frame thread's synchronous
work is the copy (constant) and the command (constant), while the rank runs
in the worker's window, where the join at the top of the next frame finds it
finished (the serialized 9.72 us is the handshake's worst case, not the
game's overlap). The copy is the price of ranking against the frame-start
state the ticket asks for; the alternative - a copy merged every 250 ms, the
occupancy store's original design note - would shrink it but reintroduce a
lag the equivalence proof would have to carry.

The deltas from the inline planner, stated:

- **the one-frame lag** (`Decision 6`) - the only behavioural delta of the
  road-site kind; the explicit kind is untouched;
- **the copy filter** (`Decision 7`) - a site closed at the publish cannot
  enter the plan even if it opens before the apply; the next request picks it
  up;
- **the cell-fill ordering** (`Decision 7`) - the tail's plans match, the
  examined prefix and the cheap-drop count can differ from the inline
  planner's over the filled store;
- **the request gates' frame** (`Decision 6`) - the budget and the model
  readiness are the queue frame's.

The evidence: `utils/spawn-service/worker.cpp` (the equivalence over 400
frames, the contention cases, the write boundary and the allocation count,
the cost), with the existing harnesses (`core.cpp`, `director.cpp` if present)
unchanged in their decisions.
