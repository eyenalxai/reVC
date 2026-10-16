# ADR 0008: Drifts chain through the recovery and flip on the counter-steer

Date: 2026-10-16
Status: Accepted

## Context

The owner: "issus is that i can't make two 90 degrees turns in a row at all.
and since the game has a lot of these it's a problem. i want to be able to turn
90 degrees left, then instantly 90 degrees right."

A drift only ever started from INACTIVE. After a release or a flick the car sat
in RECOVER until the grip ramp ran out (`DRIFT_RELEASE_TIME` 0.6 s), and the
RECOVER case had no entry check at all. The 811bb735 capture (738 rows, 21
attempts, 243 RECOVER rows) holds the proof: the quickest opposite-direction
chain goes from the release to the next entry in 0.84-0.95 s, which is the
recovery plus the player's input, and a 90-degree chicane needs the second
drift within about 0.1 s. A counter flick with the handbrake held ended the
drift (FLICK) instead of turning it around.

## Decision

1. The entry gate is one helper, `TryStartDrift(car, fwdSpeed, onGround,
   handbrake, fromRecover)`: handbrake, on the ground, forward motion and
   `|steer| > DRIFT_ENTRY_STEER`. The INACTIVE case and the RECOVER case both
   call it, so the gate lives once.
2. An entry from INACTIVE keeps the engage ramp: the ENGAGE phase with the
   grip cut starting at 0. An entry from RECOVER skips the ramp: it enters
   DRIFT with `m_fGripCut` at 1.0, so a chain and a flip bite at once.
3. The RECOVER case tries the entry first. When it starts, the recovery body
   (the counter-yaw, the bleed and the grip decay) is skipped for that frame,
   and the exit block at the bottom runs the new drift's control in the same
   frame.
4. Everything else keeps: the recovery still runs when the player does not
   re-enter; `m_fDriftTime` resets at each entry, so the charge is per drift
   and the tiers stay per drift; a finished drift grants its boost at its own
   RELEASE or FLICK as before, and the flip charges a new one on top of the
   stack; the handbrake-turn mode, the curve rate, the alignment, the grip
   curve, the tiers and the push stay.
5. The telemetry names the new entry: a RECOVER-to-DRIFT transition writes the
   same ENTER/TURN row as the INACTIVE-to-ENGAGE edge, so the next capture
   shows the chain.

## Consequences

- A left 90 and then an instant right 90 work: after a FLICK the flip starts
  on the next frame (20 ms at 50 fps), inside the 0.1 s the acceptance asks
  for, against the 0.84-0.95 s of the 811bb735 capture. A release and a quick
  re-press chains the same way, with no minimum gap.
- A held handbrake through a counter-steer flips the drift instead of ending
  it: the flick still ends the old drift and grants its boost, and the frame
  after the new opposite drift takes over at the full grip cut. To end a drift
  the player releases the handbrake. This refines ADR 0002 decision 1: the
  flick still unwinds the target through zero, but it is no longer the way to
  finish a drift while the handbrake is held.
- The gate cannot tell a chain from a flip and does not try to; the steer
  direction tells them apart. A chain in the same direction is a new drift
  with a fresh hold angle, not a continuation of the old one.
- A drift that the SLOW exit ended restarts as a handbrake turn when the
  player keeps the handbrake and the steering held, because a slow car with
  the handbrake and the steering is a handbrake turn (ADR 0002 decision 2).
  The player who wants the recovery releases the handbrake or the steering.
- The invariants of ADRs 0001-0007 still hold: the layer is player-only, off
  by default and inert when off, the drift never costs speed, surface adhesion
  and tyre temperature still multiply on top, and AI, bikes and missions keep
  the original handling.
- The next capture should show a second ENTER/TURN within about 0.1 s of the
  first FLICK or RELEASE, in the opposite direction after a flick. The owner
  then re-tests the chicane.
