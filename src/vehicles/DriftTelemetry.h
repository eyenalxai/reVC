#ifndef __GTA_DRIFTTELEMETRY_H__
#define __GTA_DRIFTTELEMETRY_H__

class CAutomobile;
class CDriftControl;

// File telemetry for the drift layer (see docs/adr/0001-arcade-drift-handling.md).
// It records the state the on-screen readout shows into drift-telemetry.csv, so
// a played session can be read back. It changes no physics, and it is inert
// while gDriftDebug is false.
//
// The game thread only formats a line into a stack buffer and appends it to a
// backlog under a short mutex. One writer thread owns the file, writes the
// backlog out and flushes it. No file call ever runs on the game thread.
class CDriftTelemetry
{
public:
	// One row of the live state, at most every 50 ms of game time while the
	// drift is active or the handbrake is held. The same call emits the edge
	// rows: drift entry and exit, boost, air pause and resume, denied entry.
	static void Tick(CAutomobile *car, CDriftControl *ctrl);

	// A row with an explicit event reason, queued at once (the writer thread
	// flushes it within the flush period). The event text is a short token
	// with an optional detail; it must not contain a comma.
	static void Event(CAutomobile *car, CDriftControl *ctrl, const char *event);
};

#endif // __GTA_DRIFTTELEMETRY_H__
