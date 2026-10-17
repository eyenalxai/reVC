#ifndef __GTA_MOVEMENTTELEMETRY_H__
#define __GTA_MOVEMENTTELEMETRY_H__

struct SMoveSnapshot;

// File telemetry for the movement solver (see the Movement telemetry in
// GLOSSARY.md and docs/adr/0028-the-movement-solver-owns-velocity-position-and-stance.md).
// It records the state the on-screen readout shows into movement-telemetry.csv,
// so a played session can be read back and replayed into the solver harness
// (VICE-172). It changes no movement, and it is inert while gMovementDebug is
// false.
//
// The game thread only formats a line into a stack buffer and appends it to a
// backlog under a short mutex. One writer thread owns the file, writes the
// backlog out and flushes it. No file call ever runs on the game thread.
//
// The rows come from the movement snapshot alone: the game millisecond, the
// state, the stance, the speed and the target speed in units per second, the
// tier, the input direction in the body frame (forward and side), the body turn
// of the step, the surface and the slope, and an event token. A solver event
// writes one row per event (see eMoveEvent), a state change writes the exit row
// of the state it left and the enter row of the state it took, and a live row
// lands every 50 ms of game time, the rate the drift telemetry uses.
class CMovementTelemetry
{
public:
	// One row of the live state, at most every 50 ms of game time, and the
	// edge rows of this step: the state enter and exit rows and the rows of
	// the snapshot's events.
	static void Tick(const SMoveSnapshot &snapshot);
};

#endif // __GTA_MOVEMENTTELEMETRY_H__
