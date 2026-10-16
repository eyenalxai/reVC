# ADR 0004: The drift turns the path, not only the angle

Date: 2026-10-15
Status: Accepted

## Context

ADR 0003 aligns the direction of travel with the target slip angle, so the
angle builds at street speed. The owner's verdict on that build: "when you're
drifting, car turns, but where you're going barely changes. car rotates, but it
does not turn."

The v3 capture (build 4764f157, 585 rows) shows the parked slide. The slip
reaches the target and stops: 184 DRIFT rows, 67 of them with the error under 5
degrees, 21 of those at a yaw under 10 degrees per second, and a mean yaw over
the DRIFT rows near 3 degrees per second. Both controllers command `gain *
(target - slip)`, so at the target both commands fall to zero and the car
slides straight with the nose parked at the drift angle. The capture pays for
it: 19 attempts and only 2 boosts, because the releases come at 0.24-0.57 s and
miss the 0.6 s tier. Holding the angle is not enough: the path must turn.

## Decision

1. The drift gets its own turn rate, the curve rate: `DRIFT_CURVE_RATE` 60
   degrees per second at the full angle, signed by the entry direction and
   scaled by `Abs(target slip) / DRIFT_ANGLE_MAX`, by `gDriftStrength` and by
   `m_fGripCut`. A small target angle turns gently, a full angle turns hard,
   and the rate fades in and out with the grip like the alignment. One helper,
   `CDriftControl::GetCurveRate`, serves both call sites.
2. The yaw controller carries it: `yawTargetRate = curve +
   DRIFT_YAW_GAIN*gDriftAssist*(target - slip)`, with the existing slip-cap
   align term after it. At the target the yaw command is the curve rate, not
   zero. The yaw servo still ramps the nose onto the command with
   `DRIFT_YAW_ACCEL`.
3. The velocity alignment carries the same rate: `AlignVelocity` rotates the
   velocity by `curve * dt` with the existing `RotateVelocity` helper, on top
   of the existing error correction. Nose and direction of travel turn
   together, so the slip still sits at the target while the whole car works
   through the corner. No second rotation helper.

## Consequences

- The path curves at `radius = speed / rate`: at the full 60 degrees per
  second, about 16 m at 60 km/h and 32 m at 120 km/h. The corner tightens as
  the player winds the target angle down.
- The curve rate is capped at the yaw authority (`DRIFT_YAW_MAX`), so a
  strength setting above about 1.4 cannot make the velocity outrun the nose;
  the nose and the velocity stay equal and the slip keeps its target. (ADR
  0009 raises the curve rate to 110 degrees per second at the full angle, so
  the crossing moves to about 1.35.)
- The angle still shapes the turn: the curve rate follows the target angle, so
  a small angle turns gently, and a counter-steered drift near zero target
  turns almost straight. The handbrake turn keeps its fixed yaw kick and never
  uses the curve rate.
- The speed stays untouched: the curve rate is a feedforward for a rotation,
  and the rotation helper preserves the velocity length by construction. The
  boost, the recovery bleed and the counter-yaw are unchanged.
- The invariants do not move: player only, off by default, inert when off,
  surfaces and temperature still multiply on the grip, and AI, bikes and
  missions keep the original handling.
- This extends ADR 0003: its velocity alignment still holds the slip at the
  target, and only its angle-only statement about the direction of travel is
  replaced. The next capture should show, in a held drift, the slip inside
  about 10 degrees of the target and a yaw near the curve rate instead of a
  collapse to zero; a release at 0.6 s or later still comes back with a boost
  (the tiers were shortened in ADR 0005).
