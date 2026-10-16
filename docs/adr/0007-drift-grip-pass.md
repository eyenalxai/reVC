# ADR 0007: The grip pass keeps more rear grip

Date: 2026-10-16
Status: Accepted

## Context

The owner's verdict on v6: "muuuuuuuuuuuuuch better. i'd like a bit more grip."

The v6 capture (build 565cf637, 465 data rows, 14 attempts, 11 boosts: six
tier 1 and five tier 2) shows where the grip is missing. The rear grip curve
sat at its floor (0.22) in 182 of the 200 drift frames, because the slide spent
its time past 15 degrees (mean |slip| 33.6 degrees): the curve saturated early
and had no grip left to give through the mid-slide. The tracking error was a
mean |slip - target| of 13.2 degrees. The exit unwound at about 80 degrees per
second while |slip| was above 5 degrees, and most releases at about 40 degrees
straightened to under 2 degrees in about half a second.

"More grip" means the tyre should bite through the mid-slide and the floor
should be higher.

## Decision

1. `DRIFT_GRIP_CUTOFF` 0.22 -> 0.28: the rear keeps more grip at a full slide.
2. `DRIFT_GRIP_STICKY` 5 -> 8 and `DRIFT_GRIP_SLIDE` 15 -> 20: the curve keeps
   more grip from 8 to 20 degrees of slip and reaches the floor later. At 10
   degrees of slip the scale goes from about 0.61 to about 0.88, and the band
   between 15 and 20 degrees no longer sits at the floor. The cutoff above is
   the main lever for the slide itself, because most drift frames are past 20
   degrees.
3. Nothing else moves: the alignment, the curve rate, the exit bleed, the
   counter-yaw, the boost tiers and the nose push stay as they are.

## Consequences

- The tyre bites through the mid-slide: the scale falls from 1.0 to 0.28 over
  8..20 degrees of slip, against 1.0 to 0.22 over 5..15 before, and the first
  part of the slide stays sticky.
- The floor is 0.28 at a full slide, about a quarter more grip than 0.22, and
  the curve reaches it later (20 degrees against 15).
- The floor now starts at 20 degrees, which is exactly the entry hold angle,
  so a fresh drift begins at the floor while the 8..20 degree band keeps the
  extra grip.
- The handbrake turn cuts to `GetMinGrip`, so a low-speed turn gets the raised
  floor too and hooks up a little harder.
- The next capture should show a lower mean |slip - target| during drifts and
  a faster slip decay after the release than the 565cf637 capture — attribute
  the gain between the raised floor and the sticky band; the owner then
  re-tests the wheel feel.
- The invariants of ADRs 0001-0006 still hold: the layer is player-only, off
  by default and inert when off, the drift never costs speed, surface adhesion
  and tyre temperature still multiply on top, and AI, bikes and missions keep
  the original handling.
- This tunes ADR 0002 decision 3: the shape of the curve (sticky below a
  threshold, a linear fall, the floor above a threshold) keeps, only the three
  values move.
