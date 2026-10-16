# ADR 0002: The drift v2 control law holds a target slip angle

Date: 2026-10-15
Status: Accepted

## Context

ADR 0001 built the drift layer around a steering-to-angle mapping: the target
was `steer * constant`, so letting go of the stick straightened the car, and a
drift needed 30 percent of the top speed to start. The owner's verdict: v1 holds
an angle but is not arcade enough, and the map's narrow roads and straight
90-degree corners need drifts at low speed.

## Decision

1. The target slip angle is stateful. Entry sets it to a base hold angle
   (20 degrees) and steering integrates it (90 degrees per second, in drift
   units, clamped to -5..45). No input holds the angle; a hard counter flick
   unwinds it past zero and ends the drift. This is the torque-balancing model
   of the Criterion GDC 2018 talk (~21:00).
2. The entry gate is 10 percent of the top speed and the blend-out floor is
   5 percent. Below the gate, handbrake and steering make a handbrake turn: a
   hard rear grip cut with a fixed yaw kick (45 degrees per second) and no
   speed gate.
3. The rear grip is a curve of the slip angle, sticky below 5 degrees and at
   most 0.22 of the normal grip above 15 degrees, instead of a constant cut.
   Surface adhesion and tyre temperature still multiply on top. (ADR 0007
   retunes the three values: sticky below 8 degrees and at most 0.28 above 20
   degrees.)
4. The yaw authority is absolute: it never scales with speed, and the layer
   keeps writing the car's turn speed directly.
5. A finished drift stacks its boost (originally 0.05 or 0.09 of the top speed
   per tier, cap 0.25, a chain to 1.25x) on a boost level that pushes and
   drains, instead of one impulse. The numbers and the push direction were
   tuned in ADR 0005 (0.04 or 0.07 per tier, cap 0.15, a chain to 1.15x) and
   ADR 0006 (the push follows the nose at 0.5).

## Consequences

- `CVehicle::ProcessWheel` takes a `tWheelGripRequest` (adhesion scale and a
  slide flag) instead of a `driftSlide` boolean. The four near-duplicate
  rear-wheel call sites are one loop.
- `bAlreadySkidding` is per-wheel state read from the wheel, not a static that
  leaks between calls and vehicles.
- The slip-angle sign stays the code's: see `GLOSSARY.md`.
- The drift still never costs speed: a drifting wheel keeps its forward force
  and the slide gets part of the lost speed back.
- Feature-off fidelity is by construction: with the drift layer off, the wheel
  path is the original one. The skid flag stays the vanilla static, the rear
  wheel axes carry over from the left wheel to the right one exactly as before,
  and the grip request defaults to identity (scale 1.0, no slide). A drifting
  wheel skips the static entirely, so the drift never depends on it.
- This refines ADR 0001: the drift layer is three modules now (`DriftControl`
  for the model, `DriftFeedback` for the presentation, `DriftTelemetry` for
  the debug file log), and the rear grip is a slip-shaped curve instead of a
  constant cut.
- The debug log (`drift-telemetry.csv`) is written only while `[Drift] Debug`
  is on. One writer thread owns the file; the frame thread only appends to a
  buffer under a short mutex, so a capture never costs frames.
- The feature stays off by default.
