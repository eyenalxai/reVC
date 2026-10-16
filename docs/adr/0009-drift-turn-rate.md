# ADR 0009: The yaw authority is raised for tighter corners

Date: 2026-10-16
Status: Accepted

## Context

The owner's verdict on v8 (build fd4401d0): "still impossible to make tight
turns on high speeds, making two 90 degrees turns in a row still impossible",
then "we just need much more cornering. it needs to be more arcade."

The v8 capture (drift-telemetry.csv, 12:22 session, 629 rows, 15 drifts) shows
that the chain works and the corner does not. Three RECOVER-to-DRIFT chains
(release-to-entry gaps 120 / 159 / 214 ms) and one held-handbrake flip landed,
but the rotation sits pinned at the yaw clamp: mean |yaw| 83 degrees per second
over the 319 DRIFT samples, 64 percent of them at 80 degrees per second or
more, against `DRIFT_YAW_MAX` 1.5 rad/s (86 degrees per second). A 90-degree
corner therefore takes 1.05 s or more of drift, and the path radius is
v / 1.5 rad/s: about 19 m at 100 km/h and 26 m at 140 km/h. The path carry
(`DRIFT_CURVE_RATE` 60 degrees per second at the full angle) sits well below
the nose rate, so the slip has to grow to make the difference up.

## Decision

1. `DRIFT_YAW_MAX` 1.5 -> 2.6 rad/s: the nose cap is 149 degrees per second, so
   a 90-degree turn arrives in 0.60 s at the cap, and the radius at 140 km/h
   falls from about 26 m to about 15 m.
2. `DRIFT_YAW_ACCEL` 6 -> 12 rad/s^2: the nose reaches the raised cap in about
   0.22 s, so the corner bites at once instead of winding in over half a
   second.
3. `DRIFT_CURVE_RATE` 60 -> 110 degrees per second: the whole car comes around
   1.8x faster at the full angle, and the rate stays below the cap, so the nose
   and the direction of travel stay together and the slip stays near its
   target instead of growing to command a turn the curve does not carry.
4. Nothing else moves: the entry gate, the angle model, the grip curve, the
   slip cap, the alignment, the recovery, the boost, the tiers and the push
   keep their values.

## Consequences

- Corners at speed are markedly tighter: at the full angle the drift turns the
  path 1.8x faster, so a 90-degree corner costs 0.60 s of drift against 1.05 s,
  and chained left-right corners stay within reach (ADR 0008).
- The slip equilibrium shifts slightly: the curve carries more of the rotation,
  so the angle error that the yaw servo turns into extra yaw is smaller and the
  slip settles a little closer to the hold angle.
- A spin is still possible: the slip cap (`DRIFT_SLIP_CAP` 60 degrees) and the
  aligning pull keep their values, so a wild release past the cap can still
  spin the car.
- Low-speed handbrake turns are unchanged: they use the fixed `DRIFT_TURN_YAW`
  kick (45 degrees per second), never the curve rate or the raised cap.
- This tunes ADR 0002 decision 4 (the authority stays absolute, only the
  numbers move) and ADR 0004 decision 1 (the curve rate; at the default
  strength it stays under the cap, so the nose and the velocity still turn
  together).
- The invariants of ADRs 0001-0008 still hold: the layer is player-only, off by
  default and inert when off, the drift never costs speed and never drives
  slower than the default handling, and AI, bikes and missions keep the
  original handling. The boost, the grip, the tiers and the push stay exactly
  as they are.
- The next capture should show a mean |yaw| well above the 83 degrees per
  second of the v8 capture and a 90-degree corner near 0.60 s; the owner then
  re-tests the two 90s in a row.
