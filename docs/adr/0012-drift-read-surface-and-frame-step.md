# ADR 0012: The drift state has one read surface and one frame step

Date: 2026-10-16
Status: Accepted

## Context

The drift layer kept its state in public members, and the feedback and the
telemetry modules read them directly (VICE-103). Every readout had to know the
members, and the debug readout, the telemetry row and the feedback could drift
apart.

The feedback step, `CDriftFeedback::Update`, ran twice a frame: the chase
camera and the car's `PreRender` both called it, and a frame guard inside the
module made the second call a no-op. Which call stepped the layer, and when the
readers saw the value, was not written down.

## Decision

1. The drift state is private. `CDriftControl::tSnapshot` (`GetSnapshot`) is
   the one read surface: the phase, the active and handbrake-turn flags, the
   slip angle, the target, the yaw rate, the grip cut, the grip scale, the
   charge, the boost and the entry speed, taken at the point of the read. The
   feedback, the telemetry and the on-screen debug readout all format from it.
2. `CDriftFeedback::Update` runs once a frame from `CGame::Process`
   (`src/core/Game.cpp`), between `CWorld::Process()` and the camera block,
   and only for the player's car (`IsDriftCar`). The frame guard is gone: the
   one caller steps once a frame.
3. The seam keeps the timing of the original code. The drift model state is
   written by the physics step inside `CWorld::Process()` and does not change
   again until the next frame, so the step computes the same values the camera
   always saw; the camera reads the strength of the same frame, and the
   `PreRender` readers (the lean, the skid marks, the smoke) read it after the
   step in the same frame.
4. The module keeps its own enable check. `Update` zeroes the strength while
   `gDriftEnabled` is off, and the call site does not test the flag, so a
   mid-drift toggle-off still clears the lean and the skid marks.

## Consequences

- The drift state cannot be read member by member outside the drift layer.
- The step has one owner and one point in the frame: the strength no longer
  depends on which camera or which render path runs.
- No physics value and no telemetry value changes; the feedback contributes
  exactly what it did.
- This refines ADR 0001: the layer stays player-only and off by default; only
  its read surface and its step are defined here.
