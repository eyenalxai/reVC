# ADR 0028: Pedestrian standable cells for the off-network spawns

Date: 2026-10-18
Status: Accepted
Relates to: VICE-154 (the implementation), VICE-122 and ADR 0019 (the placement this replaces), ADR 0024 (the path node grid), the frozen spawn service design (section 9), VICE-148 (the car site index, the pattern the storage follows)

## Context

ADR 0019 gave the population a bounded fraction of off-network spawns with a
per-attempt random search: a random point in the creation ring was validated
from scratch against the world (two vertical casts, the water line, a
clearance sphere, a node search), and a point that failed any check fell back
to the node path. Two costs came with it. Every spawn paid the whole
validation again, and the same ground was re-rejected over and over: a cell of
16 m with one standable spot in sixteen was hit by the random sample with the
odds of the sample, and the node reach bound (150 m, 5 m of level) kept every
area without nodes empty - the airfield, the docks, the beach beyond the
promenade - which is the ticket's complaint.

The frozen design (section 9) fixes the shape: node sites plus lazily built
standable cells, built on the main thread in a bounded queue (the
`AddModelsToRequestList` pattern), with the checks ground, water, clearance
and a new slope predicate; sites are marked unvalidated until built; the
worker never queries the world. The population's own placement takes the cell
path now; the service planner consumes the same store later.

## Decision

1. **The grid.** One 16 m cell grid over the world (250 x 250, the occupancy
   store's own geometry, its own copy): a cell is unvalidated, queued, or
   built; a built cell holds its standable points and the level it was built
   for. Storage is static: a `uint16_t` state per cell (the cache slot + 1),
   a bounded cache of 1024 built cells, a bounded queue of 512, and the
   counters - about 181 KB, no allocation (`src/peds/PedSites.h`,
   `PedSitesCore.cpp`).

2. **The predicate.** A build probes a 4 x 4 lattice of the cell (16 points,
   4 m apart) and keeps the points that pass, in this order: the ground cast
   down from the request's level finds a surface (else *no ground*); the
   ground stands above the water line plus `PED_SITE_WATER_MARGIN` (0.1 m,
   `GetWaterLevelNoWaves`, the line ADR 0019 read; else *water*); the ground
   stands within `PED_SITE_LEVEL_MARGIN` (3 m, the node path's own tolerance)
   of the level (else *level*); the top surface below the roof cast stands no
   more than `PED_SITE_STEP` (0.5 m) above the level (else *roof* - the
   VICE-122 rule, unchanged); the static clearance sphere over the feet is
   free (else *clearance*; the dynamic clearance stays in the creation loop,
   where it is fresh); and the ground's collision normal is not steeper than
   the walk limit (else *slope*).

3. **The slope predicate is new** (`PedSiteGroundIsWalkable`): the ground's
   collision normal against `PED_SITE_SLOPE_COS_MIN` (cos 45). No slope rule
   existed anywhere - the game marks its steep faces with the
   `SURFACE_STEEP_CLIFF` surface type (the physics' own reaction,
   `CPed::PushOffSteepSlope`) and a vertical cast over a cliff face answers
   happily with the face's height. The limit is a guess to tune like the ring
   values: it must accept the map's walkable terrain and reject the cliff
   faces, which stand far past it. A NaN normal rejects.

4. **The spots.** A cell keeps up to `PED_SITE_SPOTS` (4) of its passing
   probes, chosen by a farthest-point walk, so a request can snap to the
   stored point nearest the draw: the points cover the cell instead of
   clustering. A request on a built, standable cell answers with that point; a
   built cell with no point answers *dead*; an unvalidated cell is queued and
   the caller falls back to the node path for that spawn.

5. **The lazy build is bounded and on the main thread.** The queue holds the
   requested cells; the pump (`CPedSites::Pump`, from `CPopulation::Update`,
   before the spawn calls and not in an interior) takes the queued cell
   nearest the player each time and spends `PED_SITE_CELLS_PER_FRAME` (2)
   builds a frame. The cache holds 1024 built cells and evicts the entry
   farthest from the player; a full queue drops the farthest cell for a nearer
   admission and refuses a farther one. Every world query of the module runs
   in `PedSites.cpp`, on the game's thread: the planner worker will read the
   store's plain answers only (the design's worker boundary, sections 9 and
   11).

6. **The level of an entry.** The rules read the level of the request, so a
   cell's answer belongs to it: the entry stores its build level and a request
   more than the margin away treats the entry as stale - the cell is dropped
   and re-queued for the new level (the level rules re-run; the old points are
   not the ground of the new level). The caller still refuses a point too far
   from its own ring level, with the same margin.

7. **The target ratio replaces the fraction.** `PED_OFF_NETWORK_FRACTION`
   becomes `PED_OFF_NETWORK_TARGET_RATIO` (0.15): the population keeps the
   realised share of the created pedestrians (the cell spawns against the node
   path's, `ms_nOffNetworkSpawns` against `ms_nNetworkSpawns`) and asks the
   cells while the share is below the target. The share falls short only while
   the cells cannot answer (the validation bound) and never runs past the
   target; no random number is drawn for the decision, so a target of zero
   keeps the node path bit-exact. The on-network path itself is untouched: the
   node pair, the rarity dice, the width and the creation ring are the same
   code.

8. **The node reach bound is dropped from the placement.** ADR 0019's last two
   checks - a ped node within 150 m and within 5 m of the level - were the walk
   bound and the roof heuristic of the old search. The roof's own job is now
   the level and roof rules (a ground off the ring level, or a surface over it,
   means a roof directly), and the walk to the network is the wander state's
   own search, which never had a reach bound; the stray cull takes the ones
   that stall. Reachability is the service planner's contract (design section
   4), and the design's cell checks are ground, water, clearance and slope
   only. This is what lets the areas without nodes (the airfield, the far
   sand) stand on validated ground - the ticket's acceptance.

9. **The stray cull and the existing counters stand.** The off-network
   pedestrians are still watched until they reach a node, culled when they
   stall or wade, and counted (`ms_nOffNetworkSpawns`, `ms_nOffNetworkStuck`,
   `ms_nOffNetworkDrowned`); `ms_nNetworkSpawns` is new, the other half of the
   ratio. The cell counters live in the store (`SPedSiteStats`: the cells
   built, queued, dropped, evicted; the requests and their answers; the probes
   and the rejects by reason) and the engine side mirrors them into the plain
   statics the live rig reads
   (`CPedSites::ms_nCellsBuilt`, `ms_nRejectsWater`, ... in
   `utils/caps-audit/live-counters.py`).

10. **The reset.** `CPopulation::Initialise` (boot and restart) resets the
    store with the stray table: the cells belong to the game that ends with
    the call. A save load keeps the cache (the ground does not change); the
    level staleness handles a load at another level, and the eviction handles
    another place.

## What the checks cannot tell, and what covers it

- A surface the casts find is a surface. A low platform whose top is within
  the level margin and whose surface cast agrees (a roof garden, a hillside
  ledge) still passes, exactly as it did under ADR 0019; the stray cull takes
  the pedestrian that cannot leave such a spot.
- The clearance probe is one sphere at the feet: a thin slab above the sphere
  is not seen, and the dynamic clearance (a vehicle, a pedestrian) is the
  creation loop's check, not the build's.
- The reach to the network is not tested at all (rule 8): a pedestrian placed
  far from every node walks a long way, and the stray cull bounds the ones
  that stall. The service planner will own reachability; until then the
  placement prefers nothing - it takes what the cells answer.
- The build's world queries are the game's; the evidence program
  (`utils/spawn-service/ped-sites.cpp`) fills the same probe structure from a
  synthetic map and prices the store's arithmetic exactly, but not `CWorld`.

## Consequences

- The empty places gain pedestrians: the far sand and the airfield go from
  nothing (the node reach) to the cells' share (about 37..43 % of the draws in
  the harness's model, against 0 before), and the near beach and the city keep
  their coverage.
- The placement's cost moves from the per-spawn search to the per-cell build:
  a frame spends at most 2 builds x 16 probes = 32 probes = 128 world queries,
  a constant that does not move with the crowd - below the old path's hidden
  burst frame (about 375 queries) and far below a frame. The store's own
  arithmetic is tens of nanoseconds a request and about 60 ns a steady frame
  (the harness's measurements); a report into a full cache (the eviction scan)
  is the one heavier step, about 640 ns.
- The random stream shifts: the off-network decision no longer draws, and a
  cell request draws the same angle and distance the old search did. The
  wander path and the model choices that follow a spawn are therefore shifted
  as they were under ADR 0019; a target of zero keeps the node path bit-exact.
- The tunables: the target ratio, the frame's build budget, the lattice's
  density, the slope limit, the queue and the cache sizes. They are guesses to
  tune against the owner's play-test, like the ring values (VICE-132).
