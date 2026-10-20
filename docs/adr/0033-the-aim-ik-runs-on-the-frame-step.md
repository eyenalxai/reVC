# ADR 0033: The aim IK runs on the frame step and on the frame's own matrices

Date: 2026-10-19
Status: Accepted
Relates to: VICE-194 (the implementation), VICE-169 (the spec's aim section),
VICE-177 (the prone aim path), ADR 0012 (one frame step), ADR 0015 (the aim
pose)

## Context

The aim IK (`CPedIK`) is the writer of the aim pose over the upper body partial:
it walks the torso, a driver's lean, the head and the arms toward the angle the
aim asks for, and it reads the body's HAnim matrix array to do it - the arm
pitch comes from the clavicle's `up`, the torso bends about the spine's `at`,
the look and the seek targets come from component positions read with
`GetComponentPosition`, `GetNodePosition` and `TransformToNode`.

Two things of the original code do not fit the fork's frame-step and snapshot
rules:

- Every limiter row advanced a fixed angle per call of `MoveLimb`, so the IK's
  angular speed followed the frame rate: at 100 fps a limb closed twice as fast
  as at 50, and it reached its limits and its `GUN_POINTED_SUCCESSFULLY` sooner.
  The movement solver's tables are per second (ADR 0031); the aim's were not.
- The matrix array is rebuilt only by `UpdateRpHAnim`. Inside
  `CPed::ProcessControl` it still held the previous frame's pose - the camera
  refreshes its target before its own reads - so the arm pitch and the IK's
  targets lagged a frame, and the aim held a pose the frame had already
  replaced.

## Decision

1. **The rates are per second.** The rows and the limiter live in an engine-free
   module (`src/peds/LimbRate.{h,cpp}`) with the step itself, `MoveLimbStep`,
   which walks a limb by the row's rate over the frame step it is given
   (seconds). The rates are the stock per-call angles times the reference frame
   rate, 50 Hz; `CPedIK::MoveLimb` hands in
   `CTimer::GetTimeStepInSeconds()`. At the reference rate a call is worth the
   stock angle to within a float ulp - the engine's fiftieth is not a binary
   fraction, so the frame step is 0.0199999996 s and a step can land a float
   ulp under the stock one - and at any other rate the limb closes on its
   target in the same time instead of at a speed that follows the display.
   The limits are the stock ones: a frame rate cannot move a limit.
2. **The limiter is engine free, so the harness compiles it alone.**
   `utils/aim-ik-rate/rate.cpp` holds the rows to the stock values, drives the
   game's limiter beside the transcribed stock one at the reference rate
   (420,000 randomized steps: the same state, the angles within a float ulp,
   every status difference gone one frame later on the same target, the same
   settled angle), and shows the frame-rate independence and the limits.
3. **The ped refreshes its own matrices before the aim and the look.** At the
   seam where `ProcessControl` enters the aim and the look, and only for the
   peds that will read (`bIsAimingGun`, `bIsRestoringGun`, `bIsLooking`,
   `bIsRestoringLook`), the ped runs the camera's own recipe -
   `GetMatrix().UpdateRW(); UpdateRwFrame(); UpdateRpHAnim();` - so the read
   sees this frame's pose. The peds that do not aim or look pay nothing.
4. **A target is read where it is.** The look and the seek target belong to
   another entity, whose own frame owns its pose and whose own `ProcessControl`
   may not have run yet this frame; rebuilding another body's matrices from the
   reader would read a pose that body is still updating, and it would pay a
   hierarchy rebuild for every ped that looks at anything. The one-frame
   offset of a target's position is not the aim's own state - the aim's
   direction reads are its own body's, and they are current now.
5. **The IK's upright-body assumptions stay.** The clavicle `up.z` pitch and
   the spine yaw reads assume a standing body, and the arm clamps are anchored
   to the standing bind pose; on a prone body they are noise. That is a
   separate decision: the player's prone path already drops the arm IK
   (VICE-177), the torso and head keep their correction, and the corrected
   prone IK the spec names is not this ticket.

## Considered alternatives

- **Keep the per-call rates and scale by `CTimer::GetTimeStep()` (the
   drift-layer pattern).** Rejected: the ticket asks for the honest
   per-second unit the movement tables use, so one unit serves both layers and
  the stock values stay readable as "per call at 50 fps".
- **Refresh every ped's matrices every frame in `ProcessControl`.** Rejected:
  a hierarchy rebuild per ped per frame for reads that are presentation (the
  footstep positions, the blood particles); the aim and the look are the
  readers where a stale pose changes the result.
- **Refresh the look or seek target's body from the reader.** Rejected:
  decision 4.
- **Fix the upright-body assumptions here.** Rejected: decision 5; the prone
  and slide aim poses are their own work.

## Consequences

- The aim closes on its target at the same angular speed at any frame rate, so
  the aim no longer changes with the display; the stock aim of a 50 fps frame
  is reproduced to within a float ulp.
- The arm pitch, the torso bend axis, the neck yaw and the aim's own component
  reads are from the frame they run in.
- The limiter is data in an engine-free module with a harness over it; a row
  that loses its unit or its stock value fails the harness.
