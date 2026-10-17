#include <math.h>

#include "PlayerMoveSolver.h"

// The player's movement tuning, moved here from the top of CPlayerPed.cpp
// unchanged. m_vecMoveSpeed is in engine velocity units, one unit per 1/50
// second, so one engine unit is 50 units per second; the accelerations and the
// friction below are 20 and 4 units per second squared. The ground turns the
// direction almost as fast as the body turns toward it, so the ped keeps its
// speed through a turn and the legs stay with the movement; the air steers with
// a small authority.
#define PLAYER_GROUND_ACCEL		(20.0f / 50.0f)
#define PLAYER_GROUND_FRICTION		(20.0f / 50.0f)
#define PLAYER_GROUND_TURN_RATE		12.0f	// radians per second
#define PLAYER_AIR_ACCEL		(4.0f / 50.0f)
#define PLAYER_AIR_TURN_RATE		1.6f	// radians per second

// The values of the one stance of phase 0. The stance table (VICE-171) replaces
// them with its stand row and carries the capsule height for VICE-178.
#define MOVE_STAND_TURN_SCALE		1.0f
#define MOVE_STAND_CAPSULE_HEIGHT	0.0f

// One row per eMoveStateId: the controller's tPlayerMovementState
// (PlayerPed.cpp) with the same rows.
struct tMoveState
{
	float accel;		// engine units per second squared
	float friction;		// engine units per second squared
	float turnRate;		// radians per second, the air state steers with it
};

static const tMoveState moveStates[MOVE_NUMSTATES] = {
	{ PLAYER_GROUND_ACCEL, PLAYER_GROUND_FRICTION, PLAYER_GROUND_TURN_RATE },	// MOVE_GROUND
	{ PLAYER_AIR_ACCEL, 0.0f, PLAYER_AIR_TURN_RATE },	// MOVE_AIR
	{ 0.0f, 0.0f, 0.0f },	// MOVE_SLIDE: the row arrives with VICE-180
	{ 0.0f, 0.0f, 0.0f },	// MOVE_DIVE: the row arrives with VICE-181
};

// The tokens of the states, the stances and the tiers, in the enum order. The
// telemetry rows and the harness print them (GetStateName and the like); a
// debug reader knows what a row says without a lookup table beside it.
static const char *moveStateNames[MOVE_NUMSTATES] = {
	"GROUND", "AIR", "SLIDE", "DIVE"
};

static const char *moveStanceNames[STANCE_NUM] = {
	"STAND", "CROUCH", "PRONE"
};

static const char *moveTierNames[TIER_NUM] = {
	"WALK", "RUN", "SPRINT"
};

// The engine does not reach this module, so the few functions the maths needs
// live here, with the engine's own implementations beside them.

// PI as the engine's PI is (common.h: #define PI (float)M_PI).
#define MOVE_PI		3.14159265358979323846f

// The engine's Clamp (common.h), in the float domain.
static float
Clamp(float value, float low, float high)
{
	return value < low ? low : value > high ? high : value;
}

// CGeneral::LimitRadianAngle (General.h), the same clamp and the same wraps.
static float
LimitRadianAngle(float angle)
{
	float result = Clamp(angle, -25.0f, 25.0f);

	while (result >= MOVE_PI)
		result -= 2 * MOVE_PI;

	while (result < -MOVE_PI)
		result += 2 * MOVE_PI;

	return result;
}

// CVector2D::Magnitude (Vector2D.h): Sqrt(x*x + y*y), and Vector2D.h's Sqrt is
// sqrtf.
static float
Vec2Magnitude(const SMoveVec2 &v)
{
	return sqrtf(v.x * v.x + v.y * v.y);
}

// The CVector2D operators of Vector2D.h.
static SMoveVec2
Vec2Add(const SMoveVec2 &left, const SMoveVec2 &right)
{
	return SMoveVec2{ left.x + right.x, left.y + right.y };
}

static SMoveVec2
Vec2Sub(const SMoveVec2 &left, const SMoveVec2 &right)
{
	return SMoveVec2{ left.x - right.x, left.y - right.y };
}

static SMoveVec2
Vec2Scale(const SMoveVec2 &v, float scale)
{
	return SMoveVec2{ v.x * scale, v.y * scale };
}

// Move a velocity toward a target, at most maxDelta units per second. The same
// maths as MoveVelocityTowards (PlayerPed.cpp:78):
//	CVector2D change = target - velocity;
//	float magnitude = change.Magnitude();
//	if (magnitude <= maxDelta)
//		return target;
//	return velocity + change * (maxDelta / magnitude);
static SMoveVec2
MoveVelocityTowards(const SMoveVec2 &velocity, const SMoveVec2 &target, float maxDelta)
{
	SMoveVec2 change = Vec2Sub(target, velocity);
	float magnitude = Vec2Magnitude(change);
	if (magnitude <= maxDelta)
		return target;
	return Vec2Add(velocity, Vec2Scale(change, maxDelta / magnitude));
}

// Turn a velocity toward a direction without changing its speed. The same
// maths as SteerVelocityTowards (PlayerPed.cpp:89):
//	float angle = Atan2(velocity.y, velocity.x);
//	float targetAngle = Atan2(target.y, target.x);
//	float turn = CGeneral::LimitRadianAngle(targetAngle - angle);
//	... clamp turn to +-maxTurn ...
//	float newAngle = angle + turn;
//	return CVector2D(Cos(newAngle), Sin(newAngle)) * velocity.Magnitude();
static SMoveVec2
SteerVelocityTowards(const SMoveVec2 &velocity, const SMoveVec2 &target, float maxTurn)
{
	float angle = atan2f(velocity.y, velocity.x);
	float targetAngle = atan2f(target.y, target.x);
	float turn = LimitRadianAngle(targetAngle - angle);
	if (turn > maxTurn)
		turn = maxTurn;
	else if (turn < -maxTurn)
		turn = -maxTurn;
	float newAngle = angle + turn;
	return Vec2Scale(SMoveVec2{ cosf(newAngle), sinf(newAngle) }, Vec2Magnitude(velocity));
}

CPlayerMoveSolver::CPlayerMoveSolver(void)
{
	Reset();
}

void
CPlayerMoveSolver::Reset(void)
{
	m_State = MOVE_GROUND;
	m_Stance = STANCE_STAND;
	m_fStateTime = 0.0f;
	m_fPrevFacing = 0.0f;
	m_bFirstStep = true;
	m_Snapshot = SMoveSnapshot{};
}

const char *
CPlayerMoveSolver::GetStateName(eMoveStateId state)
{
	if (state < 0 || state >= MOVE_NUMSTATES)
		return "?";
	return moveStateNames[state];
}

const char *
CPlayerMoveSolver::GetStanceName(eMoveStance stance)
{
	if (stance < 0 || stance >= STANCE_NUM)
		return "?";
	return moveStanceNames[stance];
}

const char *
CPlayerMoveSolver::GetTierName(eMoveTier tier)
{
	if (tier < 0 || tier >= TIER_NUM)
		return "?";
	return moveTierNames[tier];
}

// One step of the player's movement. The body is the controller's state machine
// (CPlayerPed::UpdatePlayerMovement, PlayerPed.cpp:1783-1899) with the engine
// reads replaced by the intent and the writes replaced by the snapshot.
const SMoveSnapshot &
CPlayerMoveSolver::Step(const SMoveIntent &intent)
{
	// The ground and the air own the velocity in phase 0; the state comes from
	// the world fact the adapter passes. The slide and the dive arrive with the
	// transition table (VICE-171).
	eMoveStateId state = intent.onGround ? MOVE_GROUND : MOVE_AIR;
	if (state != m_State) {
		m_State = state;
		m_fStateTime = 0.0f;
	}
	m_fStateTime += intent.dt;

	const tMoveState &stateRow = moveStates[m_State];

	// The ped's own velocity is the velocity without the surface it stands on
	SMoveVec2 velocity = intent.vel;
	float ownSpeed = Vec2Magnitude(velocity);

	// The ped moves at the ground speed of the animation it plays, so the legs
	// stay with the movement in every direction and with every weapon
	float targetSpeed = intent.hasInput ? intent.targetSpeed : 0.0f;
	SMoveVec2 moveDirection = SMoveVec2{ -sinf(intent.moveHeading), cosf(intent.moveHeading) };
	SMoveVec2 targetVelocity = Vec2Scale(moveDirection, targetSpeed);

	if (m_State == MOVE_AIR) {
		if (intent.hasInput) {
			// Keep the momentum and steer toward the input
			if (ownSpeed > 0.0f)
				velocity = SteerVelocityTowards(velocity, moveDirection, stateRow.turnRate * intent.dt);
			if (ownSpeed < targetSpeed) {
				// Air control speeds the ped up to the target, never past it
				velocity = MoveVelocityTowards(velocity, targetVelocity, stateRow.accel * intent.dt);
				float newSpeed = Vec2Magnitude(velocity);
				if (newSpeed > targetSpeed)
					velocity = Vec2Scale(velocity, targetSpeed / newSpeed);
			}
		}
	} else if (intent.steepCliff) {
		// A steep slope pushed the ped off before the step (CPed::
		// PushOffSteepSlope, run by the adapter), so the slope owns the
		// velocity: the input does not steer, speed up or slow the ped. The
		// intent's normal is for the states VICE-180 adds.
		velocity = intent.vel;
	} else if (intent.hasInput) {
		// The direction follows the input at the ground turn rate, so the legs
		// stay with the movement; the speed follows the target at the
		// acceleration or the friction.
		if (ownSpeed > 0.0f)
			velocity = SteerVelocityTowards(velocity, moveDirection, stateRow.turnRate * intent.dt);
		float rate = ownSpeed > targetSpeed ? stateRow.friction : stateRow.accel;
		velocity = MoveVelocityTowards(velocity, targetVelocity, rate * intent.dt);
	} else {
		velocity = MoveVelocityTowards(velocity, SMoveVec2{ 0.0f, 0.0f }, stateRow.friction * intent.dt);
	}

	// The facts of the step the listeners read. The spin is the body turn: the
	// facing the adapter passed less the facing of the previous step, wrapped,
	// so a reader (or the CSV replay) adds the spins up and holds the body yaw.
	float spin = 0.0f;
	if (m_bFirstStep) {
		m_bFirstStep = false;
	} else {
		spin = LimitRadianAngle(intent.facing - m_fPrevFacing);
	}
	m_fPrevFacing = intent.facing;

	// The input direction in the body frame: forward along the facing, side to
	// the ped's right. The body forward is (-sin(f), cos(f)) and its right is
	// (cos(f), sin(f)) (CMatrix::SetRotateZ), and the input direction of the
	// heading h is (-sin(h), cos(h)), so the two parts are the cosine and the
	// minus sine of the gap between the input and the body.
	SMoveVec2 facingDir = SMoveVec2{ 0.0f, 0.0f };
	if (intent.hasInput) {
		float gap = LimitRadianAngle(intent.moveHeading - intent.facing);
		facingDir = SMoveVec2{ cosf(gap), -sinf(gap) };
	}

	// The tilt of the surface the step saw, from the normal of the steep cliff:
	// the angle of the normal from the vertical, so a wall is 90 degrees, the
	// flat ground and the air are 0.
	float slope = 0.0f;
	if (intent.steepCliff) {
		float vertical = intent.slopeNormal.z;
		if (vertical < 0.0f)
			vertical = -vertical;
		slope = acosf(Clamp(vertical, 0.0f, 1.0f));
	}

	m_Snapshot.state = m_State;
	m_Snapshot.stance = m_Stance;
	m_Snapshot.moved = velocity;
	m_Snapshot.velocity = Vec2Add(velocity, intent.surfaceSpeed);
	m_Snapshot.speed = Vec2Magnitude(velocity);
	m_Snapshot.targetSpeed = targetSpeed;
	m_Snapshot.tier = intent.tier;
	m_Snapshot.facingDir = facingDir;
	m_Snapshot.spin = spin;
	// The wall and the head contacts arrive with the tests that read them
	// (VICE-178, VICE-180); the feet are the one contact of phase 0.
	m_Snapshot.contacts = intent.onGround ? CONTACT_GROUND : 0;
	m_Snapshot.slope = slope;
	m_Snapshot.surface = intent.surface;
	m_Snapshot.heading = intent.moveHeading;
	// The one stance of phase 0, and the step's own facts. The events, the
	// reported movement state and the capsule height arrive with the state
	// tables, the transition table and the stance table (VICE-171, VICE-178).
	m_Snapshot.turnScale = MOVE_STAND_TURN_SCALE;
	m_Snapshot.capsuleHeight = MOVE_STAND_CAPSULE_HEIGHT;
	m_Snapshot.inCharge = true;
	m_Snapshot.events = 0;
	m_Snapshot.reportMoveState = 0;

	return m_Snapshot;
}
