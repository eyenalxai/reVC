# ADR 0014: The scene-graph sync fix ships as a librw patch applied at configure time

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
   configured, skips patches that are already applied, and fails loudly (with
   the tool's message) when a patch does not apply. The submodule pointer stays
   at the upstream commit, so the license and the attribution of librw are
   untouched.
2. The mechanism: `rw::Matrix` gets a reserved `CHANGED` flag bit. A writer that
   puts new values into a frame matrix sets it. `syncRecurse` recomputes a
   frame's LTM only when the frame is marked, when its own matrix was written,
   or when the parent's LTM changed, and it syncs the frame's objects only when
   the LTM changed or the frame has unsynced objects. `syncDirty` tests the
   hierarchy root with `ltm == matrix` (the root's LTM is a copy of its matrix)
   instead of trusting the root's mark alone.
3. reVC stops writing identical values: `CMatrix::UpdateRW` and
   `CMatrix::CopyToRwMatrix` compare first (one shared `ValuesMatch` helper) and
   skip the store and the `RwMatrixUpdate` call when nothing changed.
   `RwMatrixUpdate` is where the `CHANGED` bit is set; it is compiled in only
   when the patch is in effect (`REVC_LIBRW_PATCH_APPLIED`, driven by
   `-DREVC_LIBRW_PATCHED`), and without the patch the walk is the old one and
   needs no flag.

### Who writes a frame matrix, and who needs the flag

Non-root frames: a descendant frame's LTM is only recomputed when the walk
learns that its matrix was written, so every writer of a *child* frame sets the
flag. Those writers are `FrameUpdate.cpp`'s animation callbacks (through
`RwMatrixUpdate`), the entity and vehicle-component writes through
`CMatrix::UpdateRW`/`CopyToRwMatrix`, and librw's own frame operations
(`Frame::rotate/translate/scale/transform/setIdentity`), plus `cloneRecurse` and
`FrameList_::streamRead`, which install a whole hierarchy.

Root frames: no flag is needed. `syncDirty` compares the root's LTM with its
matrix and refreshes it whenever the values differ, and the root's LTM is by
construction a copy of its matrix. The direct `*RwFrameGetMatrix(frame) = ...`
writers that hit a hierarchy root are:

- `Boat.cpp:887`, `Heli.cpp:659`, `Automobile.cpp:5432` (a fresh frame for a
  detached wheel/panel atomic; `RpAtomicSetFrame` marks that hierarchy),
- `SimpleModelInfo.cpp:48` and `ClumpModelInfo.cpp:54` (the same pattern for a
  model instance root),
- `Ped.cpp:5140/5147` (the weapon and minigun atomic's own frame, marked by the
  `RwFrameUpdateObjects` on the next line),
- `custompipes.cpp:79-81` (a fresh camera frame, marked by `setFrame` and
  `addCamera`; the values differ from the identity LTM) and `custompipes.cpp:133`
  (`Frame::transform`, which sets the flag itself),
- `Replay.cpp:1109-1113` (the main camera's root frame; `CCamera::Process` marks
  that frame every frame, see `Camera.cpp:601/612`).

`Ped.cpp`, `SimpleModelInfo.cpp`, `ClumpModelInfo.cpp` and `MloModelInfo.cpp`
call `RwMatrixUpdate` as well, even though the sites are roots: it keeps the
"written" bookkeeping in one place, and it clears the identity/type flags the
same way every other write does.

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
- **SIMD for `Matrix::mult`.** Checked first: the multiply is scalar today
  (36 `mulss` + 27 `addss` in the shipped binary). The value is smaller than the
  walk itself and a SIMD version risks different rounding. The exactness change
  above skips whole multiplies instead.

## Scope

The patch is applied by the CMake build of the vendored submodule. Two build
paths do not run it and keep the old, correct-but-slower walk:

- the Conan build (`conanfile.py` sets `REVC_VENDORED_LIBRW=False` and uses an
  external librw),
- the Windows premake projects (`premake5.lua`, premake nightly builds).

An external librw that carries the patch can set `${PROJECT}_LIBRW_PATCHED=ON`
to turn the reVC side on.

## Consequences

- A fresh clone builds unchanged: the patch applies during configure, and
  `-DREVC_LIBRW_PATCH=OFF` builds too (the flag use in `src/fakerw/fake.cpp` is
  compiled only with the patch).
- Rendering stays bit-identical: the skipped work only recomputes LTMs from the
  same inputs. A 20000-operation fuzz check (random writes, marks and lazy
  syncs) finds no LTM difference against a naive full recompute.
- The walk still visits every frame of a marked hierarchy. Halting earlier needs
  a frame-level "changed" signal from the component writers, which is the
  follow-up if the numbers still matter.
- A writer that mutates a *child* frame matrix without `RwMatrixUpdate` one of
  the librw operations would leave that frame's LTM stale. The list above is
  the complete inventory of the direct writers in the tree.
