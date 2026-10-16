# ADR 0005: The boost pushes along the travel, gently

Date: 2026-10-16
Status: Accepted

## Context

ADR 0002 stacks a boost when a drift ends, and ADR 0004 made the drift turn the
path. The owner's verdict on v4: "feels a lot better now! and what's more
important is that now when i crash it feels like skill issue sometimes! but
cars feels too fast after boosts and also it feels like you have to hold
handbreak for too long for boost to activate, and also car feels very hard to
control after boost."

The v4 capture (build 5b71887d, 739 rows, 23 attempts, 22 releases) shows both
halves of the complaint. Ten releases pay a boost, all tier 1: only holds of
0.60 s or more clear the 0.6 s tier, and the other twelve releases sit at
0.12-0.57 s. The push is 0.8 of the top speed per second, about 4 g, along the
car's nose. The release rows still carry 37-80 degrees of slip, and over the
173 frames with a boost running the mean slip is 34 degrees: at that angle
about 0.56 of a nose push lands across the direction of travel, so the boost
steered the car sideways while the exit settled. The drain (0.04 per second)
also spreads the tier 1 bonus over 1.25 s.

## Decision

1. The tiers are easier to earn: `DRIFT_TIER1_TIME` 0.35 s and
   `DRIFT_TIER2_TIME` 0.8 s. A normal corner drift pays tier 1; a long drift
   pays tier 2. In the capture, 17 of the 22 releases would clear 0.35 s
   against 10 that cleared 0.6 s, and 2 would reach tier 2 against none.
2. The tiers and the stack are smaller: `DRIFT_BOOST_TIER1` 0.04,
   `DRIFT_BOOST_TIER2` 0.07, `DRIFT_BOOST_MAX` 0.15. A chain tops out at
   1.15x the top speed, not 1.25x.
3. The push follows the direction of travel, not the nose: it takes the
   horizontal part of `m_vecMoveSpeed`, normalises it (the nose is the
   fallback when the travel is too small to give a direction), and pushes
   along it. The headroom is measured on the total speed,
   `push = Min(fMaxVelocity*(1 + level) - speed, DRIFT_BOOST_PUSH*
   fMaxVelocity*dt)`, so the level still means a fraction of the top speed.
   `DRIFT_BOOST_PUSH` is 0.35, about 1.8 g against about 4 g. (ADR 0006
   reverts this push: it runs along the nose again at 0.5, and the headroom
   is measured on the forward speed.)
4. `DRIFT_BOOST_DRAIN` is 0.08: the tier 1 bonus is spent in 0.5 s and the
   tier 2 bonus in 0.875 s, against 1.25 s and 2.25 s.

## Consequences

- The boost adds speed without steering the car: a push along the travel
  cannot change the direction of travel, whatever the slip is when it lands.
  The exit settles on the path the car already had, and the sideways dart of
  a nose push at slip is gone.
- The cap is a whole-car speed cap now: the push keeps working until the
  total speed reaches `fMaxVelocity*(1 + level)`, so a chain stops at 1.15x
  the top speed whatever the slip is.
- The charge needs 0.35 s (tier 1) or 0.8 s (tier 2) of real slide, so a
  corner drift pays without a long hold. The charge rules, the entry gate,
  the curve rate, the alignment and the recovery are unchanged.
- ADR 0006 reverts the push direction and the cap metric of the first two
  bullets: the nose push and the forward-speed headroom are back, because the
  travel push made the car read as sliding on ice.
- The drift never costs speed: the push only adds, the rotation helper is
  untouched, and the invariants of ADRs 0001-0004 still hold: player only,
  off by default, inert when off, surfaces and temperature unchanged, and
  AI, bikes and missions keep the original handling.
- The next capture should show tier 2 boosts, and each level spent in about
  0.5 s for tier 1 and 0.9 s for tier 2; the owner re-tests the wheel feel.
- This tunes ADR 0002 decision 5: the stack, the drain and the push of that
  decision keep their shape, and its numbers move here.
