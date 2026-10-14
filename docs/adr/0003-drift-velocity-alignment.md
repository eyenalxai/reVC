# ADR 0003: The drift v3 law steers the velocity as well as the nose

Date: 2026-10-15
Status: Accepted

## Context

ADR 0002 holds a target slip angle with an absolute yaw rate. The owner's
capture shows the limit of that law: the centripetal demand of a turn is the
speed times the yaw rate, so the same 1.5 rad/s yaw rate slides the car at
140 km/h but only loosens the rear at 63 km/h. At frac 0.34 of the top speed
the capture reaches 6-10 degrees of slip and the grip comes back before the
hold angle is near. One attempt at frac 0.52 did reach 43 degrees, so the
crossover sits between roughly 65 and 95 km/h.

The exits lose the angle too. The counter-yaw reverses the car's yaw rate
slowly, so the slip keeps growing for the first moments after the release
(28 to 35 degrees, 24 to 33 degrees in the capture), and the 0.3 s recovery
window ends with 20-30 degrees left. The next handbrake finds the car still
rotating.

## Decision

1. While a drift grips (ENGAGE and DRIFT), the layer applies velocity
   alignment: it steers the car's velocity towards the target slip angle, not
   only the car's nose. The alignment is a proportional controller on the slip
   error (gain 3 per second) with a capped rotation rate (120 degrees per
   second), scaled by the drift strength and the grip cut, so it fades in on
   entry and out on exit. The hold angle is now reached at street speed, where
   the tyre forces alone cannot make it. ADR 0004 adds the drift's own turn
   (the curve rate): the nose and the velocity then turn together, so this
   alignment holds the slip while the drift carries the corner.
2. The rotation never changes the length of the velocity. The component along
   the car's up axis does not turn; the rest turns around it. The drift cannot
   add or remove speed, so the speed retention of ADR 0002 still holds.
3. The exit bleeds the slip that is left towards zero with the same rotation,
   on top of the counter-yaw: gain 4 per second, capped at 90 degrees per
   second, fading with the grip ramp. The recovery window and the grip ramp
   grow with it (DRIFT_RECOVER_TIME 0.6 s, DRIFT_RELEASE_TIME 0.6 s). A release
   at 35-40 degrees reaches below 10 degrees inside the window without a sign
   flip; past about 60 degrees a wild release can still spin.
4. The handbrake turn does not use the velocity alignment. It only turns the
   car, so the angle model and the alignment stay off while it runs; the exit
   bleed still runs in its recovery, next to the counter-yaw.

## Consequences

- The slip of a drift now has three actors: the nose yaw (ADR 0002), the tyre
  forces, and the velocity alignment. The alignment carries the angle where
  the tyre forces cannot; the yaw controller still shapes the nose.
- The invariants do not move: the layer is player-only, off by default, inert
  with the feature off, surfaces and temperature still multiply on the grip,
  and AI, bikes and missions keep the original handling.
- The recovery is longer, so a release leaves the drift later than before. The
  boost charge ends at the release, so the window does not change the boost.
- The log's km/h column now multiplies the per-tick velocity by the tick rate
  (VICE-52), so the next capture can pair a real speed with its frac value.
