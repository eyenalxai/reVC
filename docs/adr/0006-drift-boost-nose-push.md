# ADR 0006: The boost pulls the car straight again

Date: 2026-10-16
Status: Accepted

## Context

ADR 0005 pushed the boost along the direction of travel. The owner's verdict:
"got way worse. [...] car feels like it's on ice."

The v5 capture (build 21c40787, 323 rows, 9 attempts, 8 boosts: five tier 1
and three tier 2, so the new tiers work) shows the force level is not the
problem: v5 delivers about the same total speed gain as v4 (about 7 km/h
against 9), only split differently. The ice comes from the push direction. In
v4 the push ran along the car's nose, and at the 30-40 degrees of slip the
release rows carry, the part of that push across the slide worked against it:
it pulled the car straight and the driver felt grip. A push along the travel
has no straightening part at all, and it keeps feeding the lateral share of
the velocity the exit is trying to scrub, so the slide lives longer and the
car reads as sliding on ice during and after the boost. In the v5 capture the
eight boost windows start at 35-59 degrees of slip, and four of them still
carry 17-48 degrees when the boost runs out.

## Decision

1. `ApplyBoost` pushes along `car->GetForward()` again, and the headroom is
   measured on the forward speed, as in v4: `fwdSpeed =
   DotProduct(car->m_vecMoveSpeed, car->GetForward())`, `push =
   Min(fMaxVelocity*(1 + level) - fwdSpeed, DRIFT_BOOST_PUSH*
   fMaxVelocity*dt)`. The drain shape and the level rules are unchanged.
2. `DRIFT_BOOST_PUSH` is 0.5, about 2.6 g with the straightening pull. v4 was
   0.8 and felt too fast; v5 was 0.35 along the travel and felt like ice.
3. Every v5 tuning that the owner did not complain about stays:
   `DRIFT_TIER1_TIME` 0.35, `DRIFT_TIER2_TIME` 0.8, `DRIFT_BOOST_TIER1` 0.04,
   `DRIFT_BOOST_TIER2` 0.07, `DRIFT_BOOST_MAX` 0.15, `DRIFT_BOOST_DRAIN` 0.08.

## Consequences

- The straightening returns: at slip the nose push has a sideways part that
  works against the slide, so the slip decays through the boost frames and
  the car feels planted instead of on ice.
- The headroom reads on the forward speed, so the total speed can pass
  `fMaxVelocity*(1 + level)` for a moment while a slide is scrubbed. That is
  accepted and recorded.
- The drift never costs speed, and the charge, the exit rules, the curve
  rate, the alignment and the recovery are unchanged. The invariants of ADRs
  0001-0005 still hold: player only, off by default, inert when off, surfaces
  and temperature unchanged, and AI, bikes and missions keep the original
  handling.
- The next capture should show the slip decaying faster through the boost
  frames than in the v5 capture, with the speed still rising without a jump.
  The owner then re-tests the wheel feel.
- This reverts ADR 0005 decision 3: its travel push and its total-speed
  headroom are replaced, and its note records the revert. The tiers, the
  stack and the drain of that decision keep their shape and their numbers.
