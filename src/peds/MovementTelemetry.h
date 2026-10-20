#ifndef __GTA_MOVEMENTTELEMETRY_H__
#define __GTA_MOVEMENTTELEMETRY_H__

struct SMoveIntent;
struct SMoveSnapshot;

// File telemetry for the movement solver (see the Movement telemetry in
// GLOSSARY.md and docs/adr/0028-the-movement-solver-owns-velocity-position-and-stance.md).
// It records the state the on-screen readout shows into movement-telemetry.csv,
// so a played session can be read back and replayed into the solver harness
// (VICE-172, VICE-193). It changes no movement, and it is inert while
// gMovementDebug is false.
//
// The game thread only formats a line into a stack buffer and appends it to a
// backlog under a short mutex. One writer thread owns the file, writes the
// backlog out and flushes it. No file call ever runs on the game thread.
//
// One row carries one solver step: the snapshot the step returned and the
// intent the step consumed. The snapshot columns are the state, the stance, the
// speed and the target speed in units per second, the tier, the input direction
// in the body frame (forward and side), the body turn of the step, the contact
// bits, the surface and the slope. The intent columns are the frame step, the
// velocity, the surface speed, the heading, the facing, the target speed, the
// tier, the surface, the slope normal and the flags mask (the ground and steep
// facts and the action edges). A solver event writes one row per event (see
// eMoveEvent), a state change writes the exit row of the state it left and the
// enter row of the state it took, and a live row lands for every step. All
// rows of one step carry the same step number, so the harness steps the solver
// once per step and compares every row of it (MovementTelemetryFormat.h).
class CMovementTelemetry
{
public:
	// One row of the step, and the edge rows of this step: the state enter
	// and exit rows and the rows of the snapshot's events. The intent is the
	// one the step consumed; the rows carry it beside the snapshot so the
	// session replays into the harness.
	static void Tick(const SMoveIntent &intent, const SMoveSnapshot &snapshot);
};

#endif // __GTA_MOVEMENTTELEMETRY_H__
