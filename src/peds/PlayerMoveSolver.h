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
//
// The feel is data (VICE-171): one row per state (moveStateRows), one row per
// rung of the speed ladder (moveLadder), one row per stance (moveStanceRows)
// and the transition rows (moveTransitions), all in PlayerMoveSolver.cpp. Step
// is the small shared machinery the rows drive, and PlayerMoveTablesCheck holds
// the tables to their invariants for the harness (VICE-172).

// The engine velocity unit: one unit of m_vecMoveSpeed per 1/50 second, so one
// unit is 50 units per second. The reference speed of the adapter and the
// telemetry readout convert with it (PlayerPed.cpp, MovementTelemetry.cpp).
#define MOVE_ENGINE_UNITS_PER_SECOND	50.0f

// The states of the solver: one row per state in moveStateRows
// (PlayerMoveSolver.cpp). Phase 0 reaches the ground and the air; the slide and
// the dive rows are present and their numbers arrive with VICE-180 and
// VICE-181.
enum eMoveStateId
{
	MOVE_GROUND,		// the feet are on a surface
	MOVE_AIR,		// in the air: keep the momentum, steer a little
	MOVE_SLIDE,		// planned: the slide, a low friction and a high turn rate
	MOVE_DIVE,		// planned: the dive
	MOVE_NUMSTATES,
};

// The stance axis is eStance (Stance.h): the solver owns the player's stance.
// The stand and the crouch rows carry their values; the prone row arrives with
// VICE-177. The stance rows scale the step (moveStanceRows) and the stance
// moves are rows of their own table (moveStanceTransitions), scanned like the
// state ones.

// The rungs of the speed ladder (GLOSSARY.md), the explicit order of the
// strides: walk < run < sprint < the slide's entry boost < the slide jump. One
// row per rung in moveLadder, in this order, and the rows strictly ascend; only
// the rungs the phase-0 states reach are reachable in the step (the slide's two
// rungs wait for VICE-180).
enum eMoveTier
{
	TIER_WALK,
	TIER_RUN,
	TIER_SPRINT,
	TIER_SLIDE_ENTRY,
	TIER_SLIDE_JUMP,
	TIER_NUM,
};

// The eMoveState values the ladder reports (Ped.h). The module is engine free,
// so the values are mirrored here; PlayerPed.cpp, which sees both enums, holds
// the mirror to eMoveState with a static_assert.
enum eMoveReport
{
	MOVE_REPORT_NONE = 0,	// PEDMOVE_NONE
	MOVE_REPORT_STILL,	// PEDMOVE_STILL
	MOVE_REPORT_WALK,	// PEDMOVE_WALK
	MOVE_REPORT_JOG,	// PEDMOVE_JOG
	MOVE_REPORT_RUN,	// PEDMOVE_RUN
	MOVE_REPORT_SPRINT,	// PEDMOVE_SPRINT
	MOVE_REPORT_THROWN,	// PEDMOVE_THROWN
	MOVE_REPORT_NUM,
};

// The triggers of the transition table: the bits one step collects once and
// the rows then scan in their priority order. Phase 0 fires the ground fact
// alone; the action edges and the wall triggers arrive with the states they
// move to (VICE-180..VICE-182). The stance rows (VICE-176) read the same mask:
// TRIGGER_JUMP serves the jump's own edge and the stance's stand-up alike, and
// the Duck edge, the sprint and the exit-vehicle press are the stance's.
enum eMoveTrigger
{
	TRIGGER_JUMP = 1,		// the jump action edge
	TRIGGER_DIVE = 2,		// the dive action edge
	TRIGGER_SLIDE = 4,		// the slide action edge
	TRIGGER_LAND = 8,		// the feet are on a surface this step
	TRIGGER_AIRBORNE = 16,		// the feet left it this step
	TRIGGER_WALL_CONTACT = 32,	// the wall contact edge
	TRIGGER_WALL_LEFT = 64,		// the wall left edge
	TRIGGER_HEAD_HIT = 128,		// the head hit edge
	TRIGGER_CROUCH = 256,		// the Duck action edge: the stance toggles
	TRIGGER_SPRINT = 512,		// the pad's sprint: it stands the crouch up
	TRIGGER_EXIT_VEHICLE = 1024,	// the exit-vehicle press: it stands the crouch up
	TRIGGER_HEADROOM_BLOCKED = 2048,	// the body cannot rise here: every stand-up holds (VICE-178's test)
};

// The flags of one state row: what the shared step does for the state.
enum eMoveStateFlag
{
	MOVEFLAG_STEER_INPUT = 1,	// the input turns the velocity direction
	MOVEFLAG_KEEP_MOMENTUM = 2,	// without input the state keeps its velocity
};

// The edges one step reports (the snapshot's events). The bit a transition row
// carries is set on the entry; the event list itself is VICE-173's.
enum eMoveEvent
{
	EVENT_LANDED = 1,
	EVENT_WALL_CONTACT = 2,
	EVENT_WALL_LEFT = 4,
	EVENT_HEAD_HIT = 8,
	EVENT_JUMP = 16,	// the air extensions number theirs (JUMP2, JUMP3, ...)
};

// The live contact facts of one step: what the body touches right now. The
// events above are the edges of these bits; the wall and head bits arrive with
// the tickets that add the wall and head tests (VICE-178, VICE-180).
enum eMoveContact
{
	CONTACT_GROUND = 1,	// the feet rest on a surface
	CONTACT_WALL = 2,	// the body presses against a wall or a solid
	CONTACT_HEAD = 4,	// the head touches an obstacle above
};

// The row of the state table: what one eMoveStateId moves with. The ground and
// the air rows carry the controller's own constants, unchanged, and the slide
// and the dive rows wait for VICE-180 and VICE-181.
struct tMoveStateRow
{
	float accel;		// engine units per second squared
	float friction;		// engine units per second squared
	float turnRate;		// radians per second
	float entryBoost;	// engine units per second, added when the state is entered
	float boostDecay;	// engine units per second squared, the entry boost's decay
	float inputScale;	// the scale of the input's target speed
	uint32_t flags;		// eMoveStateFlag bits
};

// The row of the speed ladder: one rung the step can name. The speed scale is
// the rung's target speed as a scale of the walk, the ladder's anchor, and the
// rows strictly ascend in the ladder's order. The phase-0 step reports the
// rung's eMoveState; the rung's speed is VICE-184's (the reference-speed
// machinery still names the speed the step targets, so no phase-0 step
// multiplies by the scale).
struct tMoveLadderRow
{
	eMoveTier tier;			// the rung
	eStance stance;		// the stance the rung belongs to
	uint32_t stateMask;		// the eMoveStateId bits the rung serves
	float speedScale;		// the rung's target speed, a scale of the walk
	eMoveReport reportMoveState;	// the eMoveState value the rung reports
};

// The row of the stance table: what one eStance scales. The stand and crouch
// rows carry their values; the prone row arrives with VICE-177. The capsule
// height is what the snapshot reports only: the collision stays the engine's,
// and VICE-178 fills the heights.
struct tMoveStanceRow
{
	float speedScale;	// scales the step's target speed
	float turnScale;	// scales the step's turn rate
	float capsuleHeight;	// the capsule height the solver reports, engine units
};

// The row of the transition table: one move of the state machine. Step collects
// its trigger bits once and scans the rows in their order; the first row whose
// state and trigger match fires, so the row order is the priority.
struct tMoveTransitionRow
{
	eMoveStateId from;	// the state the move leaves
	uint32_t trigger;	// the eMoveTrigger bit that fires it
	eMoveStateId to;	// the state the move enters
	uint32_t onEntryEvent;	// the eMoveEvent bits set on the entry
};

// The row of the stance transition table (VICE-176): one move of the stance
// axis. The shape and the rules are the state row's - the same trigger mask,
// scanned in the row order, so a stance move is a row and never a branch - and
// a stance move carries no event: the stance is reported in every snapshot
// instead (see SMoveSnapshot).
struct tMoveStanceTransitionRow
{
	eStance from;		// the stance the move leaves
	uint32_t trigger;	// the eMoveTrigger bit that fires it
	eStance to;		// the stance the move enters
};

// One table set: the five tables the check reads. The module's own set lives in
// PlayerMoveSolver.cpp; the harness (VICE-172) builds one of its own to prove a
// rejection.
struct SMoveTables
{
	const tMoveStateRow *states;
	int numStates;
	const tMoveLadderRow *ladder;
	int numLadder;
	const tMoveStanceRow *stances;
	int numStances;
	const tMoveTransitionRow *transitions;
	int numTransitions;
	const tMoveStanceTransitionRow *stanceTransitions;
	int numStanceTransitions;
};

struct SMoveVec2
{
	float x, y;
};

struct SMoveVec3
{
	float x, y, z;
};

// Everything one step may see. The adapter reads the live ped state into this
// struct, so the solver never reads an engine value, the pad, the camera, an
// animation or the clock.
struct SMoveIntent
{
	float dt;		// seconds, the frame step (CTimer::GetTimeStepInSeconds)
	SMoveVec2 vel;		// m_vecMoveSpeed x/y minus the surface speed, read fresh every frame
	SMoveVec2 surfaceSpeed;	// the speed of the surface under the ped, so a collision impulse survives the step
	SMoveVec3 slopeNormal;	// m_vecDamageNormal while the touched surface is a steep cliff, else (0, 0, 0)
	float moveHeading;	// m_fMoveHeading: the world heading of the input direction
	float facing;		// m_fRotationCur: the body heading, the frame the facing relative direction is measured in
	float targetSpeed;	// the reference speed of the playing movement animations, engine units
	uint8_t surface;	// m_nSurfaceTouched: the eSurfaceType the ped touched (SurfaceTable.h)
	bool onGround;		// bIsStanding
	bool steepCliff;	// the touched surface is a steep cliff with a normal
	bool hasInput;		// the pad moves the ped this frame (the controller's bMoving)
	eMoveTier tier;		// the rung of the ladder the pad asks for
	bool faceCamera;	// the body faces the camera; no reader in phase 0
	bool jumpRequested;	// the jump press: the jump action edge (VICE-182) and the stance's stand-up (VICE-176)
	bool diveRequested;	// no reader in phase 0; VICE-181
	// The stance request and the one world fact the stance exits need
	// (VICE-176). The adapter reads them; the solver never touches the world.
	bool crouchRequested;	// the Duck edge: the crouch stance toggles
	bool sprintRequested;	// the pad's sprint: the sprint stands the crouch up
	bool exitVehiclePressed;	// the exit-vehicle press: it stands the crouch up (the stock toggle rule)
	bool headroomClear;	// CPed::IsHeadroomClear (VICE-178): the body can rise here
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
	float speed;		// the speed of the own velocity, |moved|, engine units
	float targetSpeed;	// the speed the step targeted, engine units
	eMoveTier tier;		// the tier the step moved at
	SMoveVec2 facingDir;	// the input direction in the body frame: x forward, y side; (0, 0) with no input
	float spin;		// the body turn of the step: the facing delta, radians, signed
	uint32_t contacts;	// the eMoveContact bits of the step
	float slope;		// the tilt of the slope the step saw from the horizontal, radians; 0 while flat
	uint8_t surface;	// the eSurfaceType the step touched (SurfaceTable.h)
	float heading;		// the world heading of the movement direction
	float turnScale;	// the stance turn scale
	float capsuleHeight;	// the stance capsule height; the stand row reports 0 until VICE-178
	bool inCharge;		// the controller owned the velocity this step
	uint32_t events;	// the eMoveEvent bits of this step; the event list is VICE-173
	uint8_t reportMoveState;	// the eMoveState value the ladder's rung reports
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
	// The tokens of the enums, for the debug readout and the telemetry rows
	// (MovementTelemetry.cpp). The harness prints them too.
	static const char *GetStateName(eMoveStateId state);
	static const char *GetStanceName(eStance stance);
	static const char *GetTierName(eMoveTier tier);

private:
	// The solver state. The readers use the snapshot (see SMoveSnapshot).
	eMoveStateId m_State;
	eStance m_Stance;
	float m_fStateTime;	// seconds spent in the current state
	float m_fPrevFacing;	// the body facing of the previous step, so the spin survives
	bool m_bFirstStep;	// no previous facing to take the spin from yet
	SMoveSnapshot m_Snapshot;	// written only by Step
};

// The table check (VICE-171, PlayerMoveSolver.cpp): every state has a row,
// every transition names a state in range, the ladder ascends, the stance
// rows are in range and every stance move names a stance and a trigger. `why`
// names the broken rule when the check fails and may be null; true means every
// table holds. The first reads the module's own tables (the harness's entry
// point, VICE-172), the second the set the caller passes, so the harness can
// drive each rejection with a planted row.
bool PlayerMoveTablesCheck(const char **why);
bool PlayerMoveTablesCheck(const SMoveTables &tables, const char **why);

#endif // __GTA_PLAYERMOVESOLVER_H__
