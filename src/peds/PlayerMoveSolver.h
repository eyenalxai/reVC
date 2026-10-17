#ifndef __GTA_PLAYERMOVESOLVER_H__
#define __GTA_PLAYERMOVESOLVER_H__

#include <stdint.h>

#include "Stance.h"

// The movement solver of the on-foot player: the state machine that owns the
// player's horizontal velocity and the stance. The module is engine free - it
// includes no engine header - so a harness compiles the translation unit alone.
// The controller in CPlayerPed::UpdatePlayerMovement (PlayerPed.cpp) is the
// adapter: it turns the pad, the camera and the ped into one SMoveIntent, calls
// Step and writes the snapshot back (see the Movement solver in GLOSSARY.md).

// The states of the solver: one row per state in moveStates
// (PlayerMoveSolver.cpp). Phase 0 has the ground and the air; the slide and the
// dive arrive with the state table (VICE-171) and their tickets.
enum eMoveStateId
{
	MOVE_GROUND,		// the feet are on a surface
	MOVE_AIR,		// in the air: keep the momentum, steer a little
	MOVE_SLIDE,		// planned: the slide, a low friction and a high turn rate
	MOVE_DIVE,		// planned: the dive
	MOVE_NUMSTATES,
};

// The stance axis (eStance, Stance.h): the solver owns the player's stance.
// Phase 0 has the stand; the crouch and the prone arrive with the stance
// table (VICE-171) and their tickets (VICE-176, VICE-177).

// The edges one step reports. The snapshot carries the bits to the listeners
// (presentation, telemetry); the event list is VICE-173.
enum eMoveEvent
{
	EVENT_LANDED = 1,
	EVENT_WALL_CONTACT = 2,
	EVENT_WALL_LEFT = 4,
	EVENT_HEAD_HIT = 8,
};

struct SMoveVec2
{
	float x, y;
};

// Everything one step may see. The adapter reads the live ped state into this
// struct, so the solver never reads an engine value, the pad, the camera, an
// animation or the clock.
struct SMoveIntent
{
	float dt;		// seconds, the frame step (CTimer::GetTimeStepInSeconds)
	SMoveVec2 vel;		// m_vecMoveSpeed x/y minus the surface speed, read fresh every frame
	SMoveVec2 surfaceSpeed;	// the speed of the surface under the ped, so a collision impulse survives the step
	SMoveVec2 slopeNormal;	// m_vecDamageNormal while the touched surface is a steep cliff, else (0, 0)
	float moveHeading;	// m_fMoveHeading: the world heading of the input direction
	float targetSpeed;	// the reference speed of the playing movement animations, engine units
	bool onGround;		// bIsStanding
	bool steepCliff;	// the touched surface is a steep cliff with a normal
	bool hasInput;		// the pad moves the ped this frame (the controller's bMoving)
	bool sprint;		// the pad asks for the sprint; no reader in phase 0
	bool faceCamera;	// the body faces the camera; no reader in phase 0
	bool jumpRequested;	// no reader in phase 0; VICE-182
	bool diveRequested;	// no reader in phase 0; VICE-181
};

// One read of the solver state, taken by the step. This is the only read
// surface of the solver: Step returns it and GetSnapshot hands it out. A field
// phase 0 does not use yet says so and holds the neutral value.
struct SMoveSnapshot
{
	eMoveStateId state;	// the state the step solved
	eStance stance;	// the stance of the player
	SMoveVec2 velocity;	// the new m_vecMoveSpeed x/y: the own velocity plus the surface speed
	SMoveVec2 moved;	// the controller's own velocity, the new m_moved
	float speed;		// the speed of the own velocity, |moved|
	float heading;		// the world heading of the movement direction
	float turnScale;	// the stance turn scale; 1.0 while the stand is the only stance
	float capsuleHeight;	// the stance capsule height; the stance table (VICE-178) fills it
	bool inCharge;		// the controller owned the velocity this step
	uint32_t events;	// the eMoveEvent bits of this step; the event list is VICE-173
	uint8_t reportMoveState;	// the eMoveState value the solver reports; the ladder (VICE-171) fills it
};

// The movement solver of one player. The ped holds one instance; the adapter
// steps it once a frame, and only while the controller is in charge.
class CPlayerMoveSolver
{
public:
	CPlayerMoveSolver(void);

	void Reset(void);
	// Solve one step: the intent carries the live ped state and the return
	// value is the snapshot of this step.
	const SMoveSnapshot &Step(const SMoveIntent &intent);
	// The snapshot of the last step (see SMoveSnapshot).
	const SMoveSnapshot &GetSnapshot(void) const { return m_Snapshot; }

private:
	// The solver state. The readers use the snapshot (see SMoveSnapshot).
	eMoveStateId m_State;
	eStance m_Stance;
	float m_fStateTime;	// seconds spent in the current state
	SMoveSnapshot m_Snapshot;	// written only by Step
};

#endif // __GTA_PLAYERMOVESOLVER_H__
