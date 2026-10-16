# ADR 0016: The collision model owns its triangle planes, basis and tree

Date: 2026-10-16
Status: Accepted

## Context

`CCollision::ProcessColModels` is the top frame cost at high entity counts. It
tested every triangle of model B for every pair and every frame
(`TestSphereTriangle` 12-21 % self, with `CrossProduct` and `DistToLine` under
it). Two more costs sat on the same path:

* The triangle planes lived in a 50-entry global LRU keyed by the model. As
  soon as 50 other models were used, a model's planes were freed and rebuilt
  (`CColTrianglePlane::Set` 2.3 % self).
* Every call recomputed the triangle's in-plane basis (a normalized edge, a
  cross product and the third vertex's 2D coordinates) for every sphere it
  tested.

## Decision

1. A collision model owns its triangle planes for the life of its geometry.
   The LRU is gone. Planes, basis and acceleration tree are freed in
   `RemoveCollisionVolumes` (streaming unload), in `CColModel::operator=`
   (a copy replaces the geometry) and in `CFileLoader::LoadCollisionModel`
   (an in-place reload overwrites the geometry).
2. Each model caches the per-triangle basis (36 B) lazily, on its first
   collision.
3. Each model lazily builds an acceleration tree over contiguous, ascending
   runs of its triangles. The broad phase walks it in ascending triangle
   index order and skips the subtrees the dilated sphere cannot reach, so
   the candidate list is exactly the list the full scan would build, in the
   same order. `ProcessColModels` keeps the strict minimum distance and keeps
   the first candidate on ties, so that order is part of the result.
4. The per-triangle caches are bounded by `COLTRI_CACHE_MAX_BYTES` (64 MiB).
   Past it a new model keeps its planes but loses the basis and the tree,
   which gives the same result at the pre-optimization speed. The measured
   resident set of the whole game content is about 17 MB, so the bound is a
   safety net, not a budget.

## Alternatives considered

- **Keep the LRU and only add the tree.** The rebuild traffic stays, and a
  model cannot keep a cache without keeping a link in a global list.
- **Sort the candidates by distance.** The tie rule would change: equal
  distances would pick another triangle's surface, which is a visible
  behaviour change.
- **A global side table for the basis.** It needs its own invalidation on
  reload and unload, which owning it removes.

## Consequences

- Memory per resident model grows by about 68 B/triangle (20 planes, 36
  basis, 12 tree). It is freed when the geometry goes away; the whole game's
  collision data measured 249,854 resident triangles and 17 MB of cache.
- A model whose triangles cannot be bounded conservatively (degenerate or
  non-unit plane normal, non-finite vertices) runs without a tree. The
  results are the same as before.
- The VU path keeps its own compressed planes and does not allocate the
  basis. `MoveColModelMemory` relocates all three pointers.
- The candidate-order rule and the per-model caches are invisible outside
  `src/collision/`; the only observable change is speed.
