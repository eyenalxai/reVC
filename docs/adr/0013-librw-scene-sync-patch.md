# ADR 0013: The scene-graph sync fix ships as a librw patch applied at configure time

Date: 2026-10-16
Status: Accepted

## Context

The frame cost capture at max sliders (VICE-119, `perf-max.data`) shows the
librw scene-graph sync at about 14% of the frame: `rw::syncRecurse` 7.4% self,
`rw::Matrix::mult` 4.0%, `rw::Frame::syncDirty` 1.1%, plus the small helpers.
The walk visits every frame of every marked hierarchy and recomputes every LTM
(`syncRecurse` in `vendor/librw/src/frame.cpp`). reVC marks the hierarchy root
of every moving entity every frame (`CWorld::Process`, `CEntity::UpdateRwFrame`)
and of every animated clump (`RpAnimBlendClumpUpdateAnimations`), so the walk
also runs for entities whose matrices did not change this frame.

The fix is "recompute an LTM only when its inputs changed", which needs to
happen inside the walk, in librw. librw is a git submodule pinned to an upstream
commit (`0ad67ee`), so the repo has to say how the change is carried.

## Decision

1. The change ships as `patches/librw-scene-sync.patch`. The root `CMakeLists.txt`
   applies every `patches/librw-*.patch` to `vendor/librw` before librw is
   configured, skips patches that are already applied, and fails loudly when a
   patch does not apply. The submodule pointer stays at the upstream commit, so
   the license and the attribution of librw are untouched.
2. The mechanism: `rw::Matrix` gets a reserved `CHANGED` flag bit. Every writer
   that puts new values into a frame matrix sets it (`Matrix::setIdentity`,
   `Matrix::rotate/translate/scale/transform`, `Frame::cloneRecurse`,
   `FrameList_::streamRead`, and reVC's `RwMatrixUpdate` wrapper). `syncRecurse`
   recomputes a frame's LTM only when the frame is marked, when its own matrix
   was written, or when the parent's LTM changed, and it syncs the frame's
   objects only when the LTM changed or the frame has unsynced objects.
   `syncDirty` tests the hierarchy root with `ltm == matrix` (the root's LTM is a
   copy of its matrix) instead of trusting the root's mark alone.
3. reVC stops writing identical values: `CMatrix::UpdateRW` and
   `CMatrix::CopyToRwMatrix` compare first and skip the store and the
   `RwMatrixUpdate` call when nothing changed. The few places that write a frame
   matrix directly (`SimpleModelInfo`, `ClumpModelInfo`, `MloModelInfo`,
   `CPed::Render`) now call `RwMatrixUpdate` like the rest of the code.

## Alternatives considered

- **Fork pointer.** Push the librw change to a fork and move the submodule
  pointer. More moving parts than the patch: a second remote to keep alive, the
  patch content invisible in review, and a rebase chore on every librw update.
- **Change reVC only.** Make the reVC callers stop marking. This cannot be made
  correct: vehicle component frames (wheels, doors, bike forks, boat flaps) are
  written through the entity's CMatrix without a mark of their own, so the
  entity's mark is what keeps their LTMs fresh. The walk has to become exact
  instead.
- **Matrix-to-frame back pointer in librw.** Would let a write mark its own
  frame and would allow halting the walk at the root, but it changes the
  `rw::Matrix` layout, which the stream and geometry code copies as a raw 4x4
  matrix. Too invasive for this fix.
- **SIMD for `Matrix::mult`.** Checked first: the multiply is scalar today. The
  value is smaller than the walk itself and a SIMD version risks different
  rounding. The exactness change above skips whole multiplies instead.

## Consequences

- A fresh clone builds unchanged: the patch applies during configure.
- Rendering stays bit-identical: the skipped work only recomputes LTMs from the
  same inputs.
- The walk still visits every frame of a marked hierarchy. Halting earlier needs
  a frame-level "changed" signal from the component writers, which is the
  follow-up if the numbers still matter.
- A writer that mutates a frame matrix without setting `CHANGED` would leave
  that frame's LTM stale. The writers are enumerated in the ticket (VICE-129)
  and in the patch; the list is short and auditable.
