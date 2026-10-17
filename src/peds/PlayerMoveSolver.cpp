#include <math.h>
#include <stddef.h>

#include "PlayerMoveSolver.h"

// The player's movement tuning (VICE-171) is data: the state rows, the speed
// ladder, the stance rows and the transition table below. Step is the small
// shared machinery the rows drive, and the check at the bottom holds every
// table to its invariants for the harness (VICE-172).
//
// m_vecMoveSpeed is in engine velocity units, one unit per 1/50 second, so one
// engine unit is 50 units per second; the accelerations and the friction below
// are 20 and 4 units per second squared. The ground turns the direction almost
// as fast as the body turns toward it, so the ped keeps its speed through a
// turn and the legs stay with the movement; the air keeps the momentum and
// steers with a small authority.
//
// The rows are constexpr, so a wrong row count, an id out of range or a rung
// out of order fails the build here (the static_asserts below the tables) and
// fails PlayerMoveTablesCheck at run time, over these tables and over the ones
// the harness plants.

// One row per eMoveStateId: the controller's own tuning (CPlayerPed.cpp before
// VICE-170), unchanged - the same numbers, in the same units, with the same
// expression order in Step. The slide and the dive rows are present and their
// numbers - the entry boost, the boost decay, the friction and the turn rate -
// arrive with VICE-180 and VICE-181.
static constexpr tMoveStateRow moveStateRows[] = {
	{ 20.0f / 50.0f, 20.0f / 50.0f, 12.0f, 0.0f, 0.0f, 1.0f, MOVEFLAG_STEER_INPUT },	// MOVE_GROUND
	{ 4.0f / 50.0f, 0.0f, 1.6f, 0.0f, 0.0f, 1.0f, MOVEFLAG_STEER_INPUT | MOVEFLAG_KEEP_MOMENTUM },	// MOVE_AIR
	{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, MOVEFLAG_STEER_INPUT },	// MOVE_SLIDE: the row arrives with VICE-180
	{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, MOVEFLAG_STEER_INPUT | MOVEFLAG_KEEP_MOMENTUM },	// MOVE_DIVE: the row arrives with VICE-181
};

// The mask of every eMoveStateId bit; the ladder rows use it for the states a
// rung serves.
static constexpr uint32_t MOVE_ANY_STATE_MASK = (1u << MOVE_NUMSTATES) - 1u;

// The speed ladder: the explicit order of the strides (GLOSSARY.md): walk <
// run < sprint < the slide's entry boost < the slide jump. The speed scale is
// the rung's target speed as a scale of the walk, the ladder's anchor, and the
// rows strictly ascend. The moving rungs serve every state, because a stride
// does not change when the ped leaves the ground, dives or slides; the slide's
// two rungs serve the slide alone and wait for VICE-180.
//
// The scales are the ladder's shape, not the tuned feel: no phase-0 step
// multiplies by them (the reference-speed machinery still names the speed the
// step targets), and the reference speed machinery goes away with VICE-184,
// which makes the tier the intent's value and the ladder the source of the
// speed.
static constexpr tMoveLadderRow moveLadder[] = {
	{ TIER_WALK,		STANCE_STAND, MOVE_ANY_STATE_MASK, 1.0f, MOVE_REPORT_WALK },
	{ TIER_RUN,		STANCE_STAND, MOVE_ANY_STATE_MASK, 1.5f, MOVE_REPORT_RUN },
	{ TIER_SPRINT,		STANCE_STAND, MOVE_ANY_STATE_MASK, 2.0f, MOVE_REPORT_SPRINT },
	{ TIER_SLIDE_ENTRY,	STANCE_STAND, 1u << MOVE_SLIDE,    2.25f, MOVE_REPORT_SPRINT },
	{ TIER_SLIDE_JUMP,	STANCE_STAND, 1u << MOVE_SLIDE,    2.5f, MOVE_REPORT_SPRINT },
};

// One row per eMoveStance: what the stance scales. The stand row carries the
// phase-0 values (the turn scale 1.0 and the capsule 0.0 the snapshot
// reported); the crouch and the prone rows arrive with VICE-176 and VICE-177.
// The solver reports the capsule height and owns no collision (VICE-178).
static constexpr tMoveStanceRow moveStanceRows[] = {
	{ 1.0f, 1.0f, 0.0f },	// STANCE_STAND
	{ 0.0f, 0.0f, 0.0f },	// STANCE_CROUCH: the row arrives with VICE-176
	{ 0.0f, 0.0f, 0.0f },	// STANCE_PRONE: the row arrives with VICE-177
};

// The transition table, in priority order: Step collects the step's trigger
// bits once and the first row whose state and trigger match fires. Phase 0
// reaches the ground and the air alone - the feet leave the ground and the feet
// touch it again; the slide and the dive rows wait for VICE-180 and VICE-181,
// and the wall triggers have no state to move to yet (the wall run arrives
// later).
static constexpr tMoveTransitionRow moveTransitions[] = {
	// MOVE_GROUND: the actions first, the feet leaving the ground last
	{ MOVE_GROUND,	TRIGGER_JUMP,		MOVE_AIR,	0 },
	{ MOVE_GROUND,	TRIGGER_DIVE,		MOVE_DIVE,	0 },
	{ MOVE_GROUND,	TRIGGER_SLIDE,		MOVE_SLIDE,	0 },
	{ MOVE_GROUND,	TRIGGER_AIRBORNE,	MOVE_AIR,	0 },
	// MOVE_AIR
	{ MOVE_AIR,	TRIGGER_DIVE,		MOVE_DIVE,	0 },
	{ MOVE_AIR,	TRIGGER_LAND,		MOVE_GROUND,	EVENT_LANDED },
	// MOVE_SLIDE
	{ MOVE_SLIDE,	TRIGGER_JUMP,		MOVE_AIR,	0 },
	// The slide's exit to the ground. The guard of the row is the speed below
	// the slide's exit speed, with hysteresis so the exit cannot flap on the
	// threshold; the guard and the slide's other rows arrive with VICE-180, so
	// the row must not stand alone: the land trigger by itself would end the
	// slide at once.
	{ MOVE_SLIDE,	TRIGGER_LAND,		MOVE_GROUND,	0 },
};

// The row counts and the masks the check reads.
static constexpr int MOVE_STATE_ROW_COUNT = sizeof(moveStateRows) / sizeof(moveStateRows[0]);
static constexpr int MOVE_LADDER_ROW_COUNT = sizeof(moveLadder) / sizeof(moveLadder[0]);
static constexpr int MOVE_STANCE_ROW_COUNT = sizeof(moveStanceRows) / sizeof(moveStanceRows[0]);
static constexpr int MOVE_TRANSITION_ROW_COUNT = sizeof(moveTransitions) / sizeof(moveTransitions[0]);
static constexpr uint32_t MOVE_TRIGGER_ALL = TRIGGER_JUMP | TRIGGER_DIVE | TRIGGER_SLIDE | TRIGGER_LAND
	| TRIGGER_AIRBORNE | TRIGGER_WALL_CONTACT | TRIGGER_WALL_LEFT | TRIGGER_HEAD_HIT;

// Every transition of the table names a state and a trigger in range. The same
// rule runs at run time over a planted table (PlayerMoveTablesCheck).
static constexpr bool
MoveTransitionsInRange(const tMoveTransitionRow *rows, int count)
{
	for (int i = 0; i < count; i++) {
		int from = (int)rows[i].from;
		int to = (int)rows[i].to;
		if (from < 0 || from >= MOVE_NUMSTATES || to < 0 || to >= MOVE_NUMSTATES)
			return false;
		if (rows[i].trigger == 0 || (rows[i].trigger & ~MOVE_TRIGGER_ALL) != 0)
			return false;
	}
	return true;
}

// The compile-time half of the table check: the row counts, the ids in range
// and the ladder's order fail the build here when a row is wrong.
static_assert(MOVE_STATE_ROW_COUNT == MOVE_NUMSTATES, "one state row per eMoveStateId");
static_assert(MOVE_STANCE_ROW_COUNT == STANCE_NUM, "one stance row per eMoveStance");
static_assert(MOVE_LADDER_ROW_COUNT == TIER_NUM, "one ladder row per eMoveTier");
static_assert(moveLadder[TIER_WALK].tier == TIER_WALK && moveLadder[TIER_RUN].tier == TIER_RUN
	&& moveLadder[TIER_SPRINT].tier == TIER_SPRINT
	&& moveLadder[TIER_SLIDE_ENTRY].tier == TIER_SLIDE_ENTRY
	&& moveLadder[TIER_SLIDE_JUMP].tier == TIER_SLIDE_JUMP,
	"the ladder rows must be in the rung order");
static_assert(moveLadder[TIER_WALK].speedScale < moveLadder[TIER_RUN].speedScale
	&& moveLadder[TIER_RUN].speedScale < moveLadder[TIER_SPRINT].speedScale
	&& moveLadder[TIER_SPRINT].speedScale < moveLadder[TIER_SLIDE_ENTRY].speedScale
	&& moveLadder[TIER_SLIDE_ENTRY].speedScale < moveLadder[TIER_SLIDE_JUMP].speedScale,
	"the ladder must strictly ascend: walk < run < sprint < the slide's entry < the slide jump");
static_assert(MoveTransitionsInRange(moveTransitions, MOVE_TRANSITION_ROW_COUNT),
	"every transition must name a state and a trigger in range");

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

// The move of one step: the first row of the transition table whose state is
// the current one and whose trigger is in the step's mask. The row order is the
// priority; null when no row fires.
static const tMoveTransitionRow *
MoveTransitionFor(eMoveStateId from, uint32_t triggers)
{
	for (int i = 0; i < MOVE_TRANSITION_ROW_COUNT; i++) {
		const tMoveTransitionRow &row = moveTransitions[i];
		if (row.from == from && (row.trigger & triggers) != 0)
			return &row;
	}
	return NULL;
}

// The rung of the step: the pad's sprint is the top rung of the walk and
// everything else is the walk rung. The run rung is the blend's own tier
// (m_fMoveSpeed), which the intent does not carry yet, so no phase-0 step asks
// for it; the tier becomes the intent's value with VICE-184.
static eMoveTier
MoveTierOfIntent(const SMoveIntent &intent)
{
	return intent.sprint ? TIER_SPRINT : TIER_WALK;
}

// The ladder row of one step: the first row whose tier, state mask and stance
// are the step's. A stance without a rung of its own keeps the standing rung
// (the stance row scales the speed), and the walk rung, the ladder's anchor, is
// the last resort.
static const tMoveLadderRow &
MoveLadderRowOf(eMoveStateId state, eMoveStance stance, eMoveTier tier)
{
	uint32_t stateBit = 1u << (int)state;
	const tMoveLadderRow *standing = NULL;

	for (int i = 0; i < MOVE_LADDER_ROW_COUNT; i++) {
		const tMoveLadderRow &row = moveLadder[i];
		if (row.tier != tier || (row.stateMask & stateBit) == 0)
			continue;
		if (row.stance == stance)
			return row;
		if (row.stance == STANCE_STAND)
			standing = &row;
	}
	return standing != NULL ? *standing : moveLadder[TIER_WALK];
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
	m_Snapshot = SMoveSnapshot{};
}

// One step of the player's movement. The body is the controller's state machine
// (CPlayerPed::UpdatePlayerMovement, PlayerPed.cpp:1783-1899) with the engine
// reads replaced by the intent, the writes by the snapshot and the constants by
// the tables (VICE-171).
const SMoveSnapshot &
CPlayerMoveSolver::Step(const SMoveIntent &intent)
{
	// The trigger bits of this step, collected once (eMoveTrigger): the ground
	// fact and the action edges the intent carries. No phase-0 step sets the
	// slide, the wall or the head bits; the tickets that own those paths
	// collect them (VICE-180..VICE-182).
	uint32_t triggers = intent.onGround ? TRIGGER_LAND : TRIGGER_AIRBORNE;
	if (intent.jumpRequested)
		triggers |= TRIGGER_JUMP;
	if (intent.diveRequested)
		triggers |= TRIGGER_DIVE;

	// The move: the first transition row whose state and trigger match fires,
	// so the table's order is the priority. Phase 0 reaches the ground and the
	// air alone: the feet leave the ground and the feet touch it again.
	uint32_t events = 0;
	const tMoveTransitionRow *transition = MoveTransitionFor(m_State, triggers);
	if (transition != NULL) {
		m_State = transition->to;
		m_fStateTime = 0.0f;
		events = transition->onEntryEvent;
	}
	m_fStateTime += intent.dt;

	const tMoveStateRow &stateRow = moveStateRows[m_State];
	const tMoveStanceRow &stanceRow = moveStanceRows[m_Stance];
	// The turn rate of the step is the state's rate scaled by the stance's
	float turnRate = stateRow.turnRate * stanceRow.turnScale;

	// The ped's own velocity is the velocity without the surface it stands on
	SMoveVec2 velocity = intent.vel;
	float ownSpeed = Vec2Magnitude(velocity);

	// The ped moves at the ground speed of the animation it plays, so the legs
	// stay with the movement in every direction and with every weapon
	float targetSpeed = intent.hasInput ? intent.targetSpeed * stanceRow.speedScale * stateRow.inputScale : 0.0f;
	SMoveVec2 moveDirection = SMoveVec2{ -sinf(intent.moveHeading), cosf(intent.moveHeading) };
	SMoveVec2 targetVelocity = Vec2Scale(moveDirection, targetSpeed);

	if (stateRow.flags & MOVEFLAG_KEEP_MOMENTUM) {
		if (intent.hasInput) {
			// Keep the momentum and steer toward the input
			if ((stateRow.flags & MOVEFLAG_STEER_INPUT) && ownSpeed > 0.0f)
				velocity = SteerVelocityTowards(velocity, moveDirection, turnRate * intent.dt);
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
		if ((stateRow.flags & MOVEFLAG_STEER_INPUT) && ownSpeed > 0.0f)
			velocity = SteerVelocityTowards(velocity, moveDirection, turnRate * intent.dt);
		float rate = ownSpeed > targetSpeed ? stateRow.friction : stateRow.accel;
		velocity = MoveVelocityTowards(velocity, targetVelocity, rate * intent.dt);
	} else {
		velocity = MoveVelocityTowards(velocity, SMoveVec2{ 0.0f, 0.0f }, stateRow.friction * intent.dt);
	}

	m_Snapshot.state = m_State;
	m_Snapshot.stance = m_Stance;
	m_Snapshot.moved = velocity;
	m_Snapshot.velocity = Vec2Add(velocity, intent.surfaceSpeed);
	m_Snapshot.speed = Vec2Magnitude(velocity);
	m_Snapshot.heading = intent.moveHeading;
	m_Snapshot.turnScale = stanceRow.turnScale;
	m_Snapshot.capsuleHeight = stanceRow.capsuleHeight;
	m_Snapshot.inCharge = true;
	// The entry event of the move and the rung of the step. The event list is
	// VICE-173's, and the rung is the ladder row of the step's state, stance
	// and tier.
	m_Snapshot.events = events;
	m_Snapshot.reportMoveState = MoveLadderRowOf(m_State, m_Stance, MoveTierOfIntent(intent)).reportMoveState;

	return m_Snapshot;
}

// The failure of one rule: names the rule in `why` and fails.
static bool
MoveTablesFail(const char **why, const char *rule)
{
	*why = rule;
	return false;
}

// The table check (VICE-171): every table to its invariants. The rules are the
// ones the harness (VICE-172) needs: a state without a row, a transition to an
// id out of range, a ladder row that does not ascend and a stance row out of
// range are each named in `why` and fail.
bool
PlayerMoveTablesCheck(const SMoveTables &tables, const char **why)
{
	// A caller that does not want the reason still drives the rules
	const char *unused;
	if (why == NULL)
		why = &unused;
	*why = "";

	// Every state has a row, and every row is in range
	if (tables.numStates != MOVE_NUMSTATES)
		return MoveTablesFail(why, "a state without a row");
	for (int i = 0; i < tables.numStates; i++) {
		const tMoveStateRow &row = tables.states[i];
		if (!(row.accel >= 0.0f) || !(row.friction >= 0.0f) || !(row.turnRate >= 0.0f)
			|| !(row.entryBoost >= 0.0f) || !(row.boostDecay >= 0.0f) || !(row.inputScale >= 0.0f)
			|| (row.flags & ~(uint32_t)(MOVEFLAG_STEER_INPUT | MOVEFLAG_KEEP_MOMENTUM)) != 0)
			return MoveTablesFail(why, "a state row out of range");
	}

	// One ladder row per rung, in the rung order, every rung above the one
	// below it, and a walk rung for every state
	if (tables.numLadder != TIER_NUM)
		return MoveTablesFail(why, "a ladder without a rung");
	for (int i = 0; i < tables.numLadder; i++) {
		const tMoveLadderRow &row = tables.ladder[i];
		int tier = (int)row.tier;
		int stance = (int)row.stance;
		int report = (int)row.reportMoveState;
		if (tier < 0 || tier >= TIER_NUM)
			return MoveTablesFail(why, "a ladder row out of range");
		if (tier != i)
			return MoveTablesFail(why, "a ladder row out of order");
		if (stance < 0 || stance >= STANCE_NUM)
			return MoveTablesFail(why, "a ladder row names a stance out of range");
		if (row.stateMask == 0 || (row.stateMask & ~MOVE_ANY_STATE_MASK) != 0)
			return MoveTablesFail(why, "a ladder row names a state out of range");
		if (report < 0 || report >= MOVE_REPORT_NUM)
			return MoveTablesFail(why, "a ladder row names a report out of range");
		if (!(row.speedScale > 0.0f))
			return MoveTablesFail(why, "a ladder row out of range");
		if (i > 0 && !(tables.ladder[i - 1].speedScale < row.speedScale))
			return MoveTablesFail(why, "a ladder row that does not ascend");
	}
	for (int state = 0; state < MOVE_NUMSTATES; state++) {
		uint32_t stateBit = 1u << state;
		bool found = false;
		for (int i = 0; i < tables.numLadder; i++)
			if (tables.ladder[i].tier == TIER_WALK && (tables.ladder[i].stateMask & stateBit) != 0)
				found = true;
		if (!found)
			return MoveTablesFail(why, "a state without a rung");
	}

	// One stance row per stance, in range
	if (tables.numStances != STANCE_NUM)
		return MoveTablesFail(why, "a stance without a row");
	for (int i = 0; i < tables.numStances; i++) {
		const tMoveStanceRow &row = tables.stances[i];
		if (!(row.speedScale >= 0.0f) || !(row.turnScale >= 0.0f) || !(row.capsuleHeight >= 0.0f))
			return MoveTablesFail(why, "a stance row out of range");
	}

	// Every move names a state and a trigger in range
	for (int i = 0; i < tables.numTransitions; i++) {
		const tMoveTransitionRow &row = tables.transitions[i];
		int from = (int)row.from;
		int to = (int)row.to;
		if (from < 0 || from >= MOVE_NUMSTATES)
			return MoveTablesFail(why, "a transition from a state out of range");
		if (to < 0 || to >= MOVE_NUMSTATES)
			return MoveTablesFail(why, "a transition to a state out of range");
		if (row.trigger == 0 || (row.trigger & ~MOVE_TRIGGER_ALL) != 0)
			return MoveTablesFail(why, "a transition trigger out of range");
	}

	return true;
}

// The check over the module's own tables, the harness's entry point (VICE-172).
bool
PlayerMoveTablesCheck(const char **why)
{
	const SMoveTables tables = {
		moveStateRows, MOVE_STATE_ROW_COUNT,
		moveLadder, MOVE_LADDER_ROW_COUNT,
		moveStanceRows, MOVE_STANCE_ROW_COUNT,
		moveTransitions, MOVE_TRANSITION_ROW_COUNT,
	};
	return PlayerMoveTablesCheck(tables, why);
}
