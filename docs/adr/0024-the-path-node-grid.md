# ADR 0024: The path nodes are looked up through a uniform grid

Date: 2026-10-16
Status: Accepted
Relates to: VICE-130 (the implementation), VICE-119 (the program), ADR 0013 (the ring table the spawn test reads)

## Context

`CPathFind::DoPathSearch` is the pathfinding cost at high vehicle counts. A
live capture of the running game put it at 5.07 % self, and every sample of it
came from the car spawn test (`CCarCtrl::GenerateRandomCars` ->
`GenerateOneRandomCar` -> `CreateCarFromPlacement` ->
`CPathFind::TestCoorsCloseness`). The rest of the family is small: the ped
closest-node lookup 0.23 %, ped wandering 0.19 %, car AI re-pathing and ped
path searches below the noise.

A search asks `FindNodeClosestToCoors` twice, for the target node and for the
start node, and that lookup scanned every node of the type: 3,351 car nodes or
6,291 ped nodes, each with four flag tests and the same distance expression.
Measured on the real graph, the lookup pair costs 17.3 us while a whole spawn
test costs 13.9-15.9 us, so the scans are most of the search at the ring
scales the spawn path asks for.

## Decision

1. `FindNodeClosestToCoors` walks a uniform grid of the path nodes: 32 m
   cells over the bounding box of the node array, one cell of margin, built
   on the first lookup after `PreparePathData`. The lookup visits rings of
   cells around the query point instead of every node.
2. The ring walk stops when no farther node can beat or tie the closest node
   found. A node in ring `r` is at least `(r-1)` cells away, so the walk is
   bounded by `closestDist / cellSize + 1`: the division truncates and the
   `+1` keeps the ring whose `(r-1)`-cell edge can still tie, so a tie is
   never missed.
3. The candidates take the same distance expression and the same tie rule as
   the plain scan (the strict minimum; the lowest node index wins a tie), so
   the grid returns the node the scan would return. That exactness is what
   lets the change ship without a behaviour review of every caller.
4. When any node falls outside the grid the grid is not built and the lookup
   keeps the plain scan; the results are the same either way. Today's map
   spans 3,580 x 3,644 m against the grid's 4,128 m, so the fallback does not
   run on the shipped data; it is there for a map the grid cannot hold.
5. The grid costs 53 KB once, inside `ThePaths`: the cell offsets
   (129 x 129 + 1 cells of 2 B) and the node list (9,650 nodes of 2 B).
6. The grid describes the node array of the moment it was built. `Init` and
   `PreparePathData` clear the validity flag (the nodes are reset or moved)
   and the next lookup rebuilds it. Nothing else moves a node after the map
   is prepared.
7. `TestCoorsCloseness` asks for the distance only, so its search no longer
   traces a node list nobody reads, and `DoPathSearch` no longer writes to a
   caller's node list that has zero entries.

## The non-terminating search, and what stays out of scope

The search loop stops when the start node comes up. That is not guaranteed:
when a node that was already popped gets a better label, the code unlinks it
through the list pointers of its old position, which can orphan nodes that are
still queued. The start node then never comes up, and the base revision spins
in the loop for the rest of the session (a hang in the frame; the harness
reproduces it on a real node pair, start 3738 -> target 9315).

This change leaves the ring after more than a whole ring of empty buckets and
reports no route for a start that never came up. That changes behaviour only
where the base revision does not terminate; every case where the base
terminates returns the same node list, distance and verdict (the harness
checks that on stored cases).

Repairing the queue itself, so a node can never be orphaned, would change
*defined* results as well: a search would then find the shorter route where
the base returns a longer one. That wants its own change and its own evidence,
so it stays out of this ADR.

## Alternatives considered

- **A k-d tree or an octree.** Exact, but the build and the query are heavier
  than the win at this node count and cell size.
- **One cached node per cell per type.** The lookup would return a cell's
  representative, not the true closest node, and the tie rule would change.
- **Caching the last lookup by coordinates.** It helps only a repeated target
  point, not the varied start points, and a coordinate cache needs its own
  invalidation.
- **Sorting the nodes once and bisecting.** The distance metric is an L1
  metric with a weighted z, so a sort on x does not bound the closest node.

## Consequences

- The closest-node lookup pair drops from 17.3 us to 3.1 us; the spawn
  pattern from 13.9 us to 2.9 us at ring scale 1 and from 15.9 us to 4.8 us
  at scale 2; the chase pattern from 12.8 us to 2.1 us. The spawn bench
  accepts exactly the same placements in both builds.
- The grid is invisible outside `src/control/PathFind.{h,cpp}`. The only
  observable changes are speed and the non-terminating search now returning
  no route.
- A map that does not fit the 4.1 km grid runs on the plain scan until
  `PATHFIND_GRID_DIM` grows; the fallback keeps it correct, not fast.
- The harness (`tools/pathfind_check`) prints whether the grid was built, so
  an equality run cannot mistake the grid's result for the fallback's.
