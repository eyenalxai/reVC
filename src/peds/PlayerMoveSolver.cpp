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
// steers with a small authority; the slide (VICE-180) starts from a sprint plus
// the crouch press, decays over its friction curve and steers hard while it
// lasts.
//
// The rows are constexpr, so a wrong row count, an id out of range or a rung
// out of order fails the build here (the static_asserts below the tables) and
// fails PlayerMoveTablesCheck at run time, over these tables and over the ones
// the harness plants.

// The tokens of the enums, for the debug readout and the telemetry rows. The
// module is engine free, so the names live here.
static const char *moveStateNames[MOVE_NUMSTATES] = {
	"GROUND", "AIR", "SLIDE", "DIVE"
};

static const char *moveStanceNames[STANCE_NUM] = {
	"STAND", "CROUCH", "PRONE"
};

static const char *moveTierNames[TIER_NUM] = {
	"WALK", "RUN", "SPRINT", "SLIDE_ENTRY", "SLIDE_JUMP"
};

// One row per eMoveStateId. The ground and the air rows carry the controller's
// own tuning (CPlayerPed.cpp before VICE-170), unchanged - the same numbers, in
// the same units, with the same expression order in Step. The slide row is
// VICE-180's, tuned against the shipped movement cycles (anim/ped.ifp:
// walk_civi 1.13, run_civi 6.39 and sprint_civi 14.67 units per second, so the
// walk is 0.0227, the run 0.128 and the sprint 0.293 engine units):
//	* the acceleration is low: the slide takes no speed from the input (the
//	  input scale is zero), so only the friction and the slope change its speed,
//	* the friction bleeds the speed over the slide: 8 units per second squared
//	  takes the entry's 0.33 engine units down to the exit's 0.13 in about a
//	  second, which is the slide's whole length,
//	* the turn rate is high: the slide steers, and 16 radians per second is the
//	  ground's own 12 with the authority a slide needs,
//	* the entry boost lifts the speed 2 units per second over the sprint's - the
//	  ladder's entry rung over its sprint rung - and the boost's own decay
//	  spends the extra speed in a quarter of a second, so the slide's head is
//	  steep and its tail is the friction alone,
//	* the input scale is zero: the input steers the slide and adds no speed, so
//	  the friction always wins and the slide reaches its exit.
static constexpr tMoveStateRow moveStateRows[] = {
	{ 20.0f / 50.0f, 20.0f / 50.0f, 12.0f, 0.0f, 0.0f, 1.0f, MOVEFLAG_STEER_INPUT },	// MOVE_GROUND
	{ 4.0f / 50.0f, 0.0f, 1.6f, 0.0f, 0.0f, 1.0f, MOVEFLAG_STEER_INPUT | MOVEFLAG_KEEP_MOMENTUM },	// MOVE_AIR
	{ 2.0f / 50.0f, 8.0f / 50.0f, 16.0f, 2.0f / 50.0f, 8.0f / 50.0f, 0.0f, MOVEFLAG_STEER_INPUT },	// MOVE_SLIDE
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
// two rungs serve the slide alone: the slide reports the entry rung whatever
// the pad asks (VICE-180, MoveTierOfIntent), and the slide-jump rung is the
// report of the jump out of the slide, which arrives with VICE-184.
//
// The scales are the ladder's shape, not the tuned feel: no step multiplies by
// them (the reference-speed machinery still names the speed the step targets),
// and the reference speed machinery goes away with VICE-184, which makes the
// tier the intent's value and the ladder the source of the speed.
static constexpr tMoveLadderRow moveLadder[] = {
	{ TIER_WALK,		STANCE_STAND, MOVE_ANY_STATE_MASK, 1.0f, MOVE_REPORT_WALK },
	{ TIER_RUN,		STANCE_STAND, MOVE_ANY_STATE_MASK, 1.5f, MOVE_REPORT_RUN },
	{ TIER_SPRINT,		STANCE_STAND, MOVE_ANY_STATE_MASK, 2.0f, MOVE_REPORT_SPRINT },
	{ TIER_SLIDE_ENTRY,	STANCE_STAND, 1u << MOVE_SLIDE,    2.25f, MOVE_REPORT_SPRINT },
	{ TIER_SLIDE_JUMP,	STANCE_STAND, 1u << MOVE_SLIDE,    2.5f, MOVE_REPORT_SPRINT },
};

// The crawl's own speed (VICE-187): the content's crawl_fwd carries 0.443 units
// per second, and the walk cycle the ladder is anchored on (walk_civi) carries
// 1.13, so the crawl is 0.443 / 1.13 = 0.392 of the walk. The row takes the
// playback floor instead - PLAYER_ANIM_SPEED_MIN is 0.4 (PlayerPed.cpp) - so
// the scaler plays the crawl cycle exactly: the ratio it asks for is the row's
// own scale, and at the floor the cycle keeps its feet planted instead of
// running 2 percent fast. While the crawl content plays, today's reference
// speed is the crawl's own, so the body crawls at the row's share of it (the
// crouch's own shape, its stride share); VICE-184 makes the ladder the source
// of the speed, and the share is then the only multiplier, which lands the
// crawl on 0.4 of the content's 0.443.
#define MOVE_PRONE_SPEED_SCALE		0.4f

// The turn of a body that lies down: half the standing rate. The crouch's own
// 1.25 is the opposite case - a crouch steers tighter than a stand.
#define MOVE_PRONE_TURN_SCALE		0.5f

// The top of VICE-178's prone set, in the ped's own frame: the lying body's
// four 0.3 spheres put its top at 2 * 0.3 above the ground, and the origin sits
// FEET_OFFSET above the ground, so the top is 0.6 - 1.04 = -0.44 and the set
// hangs below the origin (PED_PRONE_BODY_TOP, TempColModels.cpp).
#define MOVE_PRONE_CAPSULE_HEIGHT	-0.44f

// FEET_OFFSET (common.h): the engine puts the ped origin this far above the
// feet in every stance, so the ground under the body is this far below the
// origin. The stance rows' rule reads it - a posture's top may hang below the
// origin, the way the prone body does, but never under the ground - and the
// step never touches it.
#define MOVE_FEET_OFFSET		1.04f

// One row per eStance: what the stance scales. The stand row carries the
// phase-0 values (the turn scale 1.0 and the capsule 0.0 the snapshot
// reported, which the parity harness pins). The crouch row is the slow
// shuffle: half the walk - the stride share the crouch content is authored
// at - the turn a quarter tighter than the stand, and the capsule at the top
// of VICE-178's crouch set (its head sphere: 0.55 - 0.2 - 0.2 + 0.35). The
// prone row is the crawl: the crawl cycle's own speed as a share of the
// walk's, the turn at half the standing rate - a body that lies down drives
// the direction slowly - and the capsule at the top of VICE-178's prone set,
// which hangs below the origin. The solver reports the capsule height and owns
// no collision (VICE-178).
static constexpr tMoveStanceRow moveStanceRows[] = {
	{ 1.0f, 1.0f, 0.0f },			// STANCE_STAND
	{ 0.5f, 1.25f, 0.5f },			// STANCE_CROUCH
	{ MOVE_PRONE_SPEED_SCALE, MOVE_PRONE_TURN_SCALE, MOVE_PRONE_CAPSULE_HEIGHT },	// STANCE_PRONE
};

// The slide's two speeds (VICE-180), engine units: a sprint holds 0.29336
// (sprint_civi, 14.668 units per second) and the fixed camera plays the sprint
// animation at 0.7, 0.205, so the entry sits below both, and the exit sits at
// the run's own speed (run_civi, 6.39 units per second, 0.128): the slide ends
// when the ped is no faster than a run. The exit is below the entry - the
// hysteresis that keeps a slide from ending on the frame it starts - and the
// table check holds the two in that order.
#define MOVE_SLIDE_ENTRY_SPEED	0.18f
#define MOVE_SLIDE_EXIT_SPEED	0.13f

// The part of the free fall that runs along a slope, in engine units: the
// engine's own gravity (0.008 units per 1/50 second squared, the ground row's
// 20 units per second squared). The downhill direction of a slope is the
// horizontal part of its surface normal, which the intent carries, so a slide
// down a slope gains speed and one up it loses faster than the friction.
#define MOVE_SLIDE_SLOPE_ACCEL	(20.0f / 50.0f)

// The transition table, in priority order: Step collects the step's trigger
// bits once and the first row whose state, trigger and guard match fires.
// VICE-180 adds the slide: the sprint's crouch press enters it, the jump, a
// prone request, the feet leaving the surface and the exit speed end it. The
// rows that carry the jump from the ground, the dive and the wall run wait for
// their tickets (VICE-182, VICE-181, the wall run).
static constexpr tMoveTransitionRow moveTransitions[] = {
	// MOVE_GROUND: the slide's entry, one frame: the crouch press at the
	// sprint tier, at or above the speed a sprint holds. The entry boost then
	// lifts the speed over the sprint's in the same frame (see Step).
	{ MOVE_GROUND,	TRIGGER_SLIDE,		MOVE_SLIDE,	0, GUARD_SPEED_ABOVE, MOVE_SLIDE_ENTRY_SPEED },
	// MOVE_GROUND: the feet leaving the ground. The jump and the dive rows
	// arrive with the tickets that own the impulse and the state (VICE-182,
	// VICE-181): the ground moves on the world fact alone, exactly as the
	// controller did.
	{ MOVE_GROUND,	TRIGGER_AIRBORNE,	MOVE_AIR,	0, GUARD_NONE, 0.0f },
	// MOVE_AIR: the feet touching the ground again
	{ MOVE_AIR,	TRIGGER_LAND,		MOVE_GROUND,	EVENT_LANDED, GUARD_NONE, 0.0f },
	// MOVE_SLIDE: the jump leaves the slide for the air and keeps the slide's
	// speed: no row resets a velocity, and the air row's keep-momentum holds
	// what the slide had, so a slide jump is faster than a sprint jump.
	{ MOVE_SLIDE,	TRIGGER_JUMP,		MOVE_AIR,	EVENT_JUMP, GUARD_NONE, 0.0f },
	// MOVE_SLIDE: a prone request ends the slide on the ground, and the stance
	// table lands the ped in the prone on the same edge (VICE-177): this row
	// only leaves the slide.
	{ MOVE_SLIDE,	TRIGGER_PRONE,		MOVE_GROUND,	0, GUARD_NONE, 0.0f },
	// MOVE_SLIDE: the feet leaving the surface end the slide into the fall,
	// with the speed the slide had.
	{ MOVE_SLIDE,	TRIGGER_AIRBORNE,	MOVE_AIR,	0, GUARD_NONE, 0.0f },
	// MOVE_SLIDE: the friction has bled the speed to the exit speed, so the
	// slide ends on the ground - in the crouch when the ped holds the crouch
	// stance (VICE-176's), else standing. The exit threshold is below the
	// entry's: the guard cannot flap on its own entry frame.
	{ MOVE_SLIDE,	TRIGGER_LAND,		MOVE_GROUND,	0, GUARD_SPEED_BELOW, MOVE_SLIDE_EXIT_SPEED },
};

// The stance transition table (VICE-176, VICE-177), in priority order like the
// state one: the same trigger mask, the first row whose stance and trigger
// matches fires. The stance is the solver's, so its moves are rows: the Duck
// edge crouches the ped and stands him up again, the prone action lies him
// down and stands him up again, the sprint, the jump press and the exit-vehicle
// press stand him up, and every one of those stand-ups is held back while the
// body cannot rise - VICE-178's headroom test sits on top of the stand-up
// rows, so a low body under a low ceiling stays down. The sprint's own gate is
// a row too: the sprint holds the stance standing, so a Duck edge inside a
// sprint does not crouch.
static constexpr tMoveStanceTransitionRow moveStanceTransitions[] = {
	// STANCE_STAND: the sprint owns the body, so a Duck edge inside a sprint
	// does not crouch the ped (the stock gate); otherwise the Duck edge
	// crouches him and the prone action lies him down
	{ STANCE_STAND,		TRIGGER_SPRINT,			STANCE_STAND },
	{ STANCE_STAND,		TRIGGER_CROUCH,			STANCE_CROUCH },
	{ STANCE_STAND,		TRIGGER_PRONE,			STANCE_PRONE },
	// STANCE_CROUCH: no headroom, the crouch holds; with it, the crouch's own
	// edge stands the ped up, the sprint, the jump and the exit-vehicle press
	// stand him up, and the prone action lies him down - the prone is reachable
	// from the crouch as well as from the stand
	{ STANCE_CROUCH,	TRIGGER_HEADROOM_BLOCKED,	STANCE_CROUCH },
	{ STANCE_CROUCH,	TRIGGER_CROUCH,			STANCE_STAND },
	{ STANCE_CROUCH,	TRIGGER_SPRINT,			STANCE_STAND },
	{ STANCE_CROUCH,	TRIGGER_JUMP,			STANCE_STAND },
	{ STANCE_CROUCH,	TRIGGER_EXIT_VEHICLE,		STANCE_STAND },
	{ STANCE_CROUCH,	TRIGGER_PRONE,			STANCE_PRONE },
	// STANCE_PRONE: no headroom, the prone holds - the refusal sits above every
	// exit, so a body under a ceiling stays down; with it, the prone action
	// stands the ped up, the Duck edge lifts him into the crouch (so the low
	// ladder is prone, crouch, stand), and the sprint, the jump and the
	// exit-vehicle press stand him up
	{ STANCE_PRONE,		TRIGGER_HEADROOM_BLOCKED,	STANCE_PRONE },
	{ STANCE_PRONE,		TRIGGER_PRONE,			STANCE_STAND },
	{ STANCE_PRONE,		TRIGGER_CROUCH,			STANCE_CROUCH },
	{ STANCE_PRONE,		TRIGGER_SPRINT,			STANCE_STAND },
	{ STANCE_PRONE,		TRIGGER_JUMP,			STANCE_STAND },
	{ STANCE_PRONE,		TRIGGER_EXIT_VEHICLE,		STANCE_STAND },
};

// The row counts and the masks the check reads.
static constexpr int MOVE_STATE_ROW_COUNT = sizeof(moveStateRows) / sizeof(moveStateRows[0]);
static constexpr int MOVE_LADDER_ROW_COUNT = sizeof(moveLadder) / sizeof(moveLadder[0]);
static constexpr int MOVE_STANCE_ROW_COUNT = sizeof(moveStanceRows) / sizeof(moveStanceRows[0]);
static constexpr int MOVE_TRANSITION_ROW_COUNT = sizeof(moveTransitions) / sizeof(moveTransitions[0]);
static constexpr int MOVE_STANCE_TRANSITION_ROW_COUNT =
	sizeof(moveStanceTransitions) / sizeof(moveStanceTransitions[0]);
static constexpr uint32_t MOVE_TRIGGER_ALL = TRIGGER_JUMP | TRIGGER_DIVE | TRIGGER_SLIDE | TRIGGER_LAND
	| TRIGGER_AIRBORNE | TRIGGER_WALL_CONTACT | TRIGGER_WALL_LEFT | TRIGGER_HEAD_HIT
	| TRIGGER_CROUCH | TRIGGER_SPRINT | TRIGGER_EXIT_VEHICLE | TRIGGER_HEADROOM_BLOCKED
	| TRIGGER_PRONE;

// Every transition of the table names a state and a trigger in range, and its
// guard is one of the guards: an unguarded row carries no threshold and a
// guarded one carries a positive speed. The same rule runs at run time over a
// planted table (PlayerMoveTablesCheck).
static constexpr bool
MoveTransitionsInRange(const tMoveTransitionRow *rows, int count)
{
	for (int i = 0; i < count; i++) {
		int from = (int)rows[i].from;
		int to = (int)rows[i].to;
		int guard = (int)rows[i].guard;
		if (from < 0 || from >= MOVE_NUMSTATES || to < 0 || to >= MOVE_NUMSTATES)
			return false;
		if (rows[i].trigger == 0 || (rows[i].trigger & ~MOVE_TRIGGER_ALL) != 0)
			return false;
		if (guard < 0 || guard >= GUARD_NUM)
			return false;
		if (guard == GUARD_NONE && rows[i].guardSpeed != 0.0f)
			return false;
		if (guard != GUARD_NONE && !(rows[i].guardSpeed > 0.0f))
			return false;
	}
	return true;
}

// Every stance move names a stance and a trigger in range (VICE-176); the same
// rule runs at run time over a planted table.
static constexpr bool
MoveStanceTransitionsInRange(const tMoveStanceTransitionRow *rows, int count)
{
	for (int i = 0; i < count; i++) {
		int from = (int)rows[i].from;
		int to = (int)rows[i].to;
		if (from < 0 || from >= STANCE_NUM || to < 0 || to >= STANCE_NUM)
			return false;
		if (rows[i].trigger == 0 || (rows[i].trigger & ~MOVE_TRIGGER_ALL) != 0)
			return false;
	}
	return true;
}

// The compile-time half of the table check: the row counts, the ids in range
// and the ladder's order fail the build here when a row is wrong.
static_assert(MOVE_STATE_ROW_COUNT == MOVE_NUMSTATES, "one state row per eMoveStateId");
static_assert(MOVE_STANCE_ROW_COUNT == STANCE_NUM, "one stance row per eStance");
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
	"every transition must name a state, a trigger and a guard in range, with a threshold when it is guarded");
static_assert(MoveStanceTransitionsInRange(moveStanceTransitions, MOVE_STANCE_TRANSITION_ROW_COUNT),
	"every stance move must name a stance and a trigger in range");

// The slide's own compile-time rules (VICE-180): the exit speed below the entry
// speed (the hysteresis the design asks for) and the slide's rungs above the
// sprint's. PlayerMoveTablesCheck holds the same rules over a planted set at
// run time.
static_assert(MOVE_SLIDE_EXIT_SPEED < MOVE_SLIDE_ENTRY_SPEED,
	"the slide's exit speed must sit below its entry speed");
static_assert(moveLadder[TIER_SPRINT].speedScale < moveLadder[TIER_SLIDE_ENTRY].speedScale
	&& moveLadder[TIER_SPRINT].speedScale < moveLadder[TIER_SLIDE_JUMP].speedScale,
	"the slide's rungs must sit above the sprint's");

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
// the current one, whose trigger is in the step's mask and whose guard holds
// over the ped's own speed. The row order is the priority, so a closed guard
// lets the next row of the same trigger fire; null when no row fires.
static const tMoveTransitionRow *
MoveTransitionFor(eMoveStateId from, uint32_t triggers, float ownSpeed)
{
	for (int i = 0; i < MOVE_TRANSITION_ROW_COUNT; i++) {
		const tMoveTransitionRow &row = moveTransitions[i];

		if (row.from != from || (row.trigger & triggers) == 0)
			continue;
		if (row.guard == GUARD_SPEED_ABOVE && !(ownSpeed > row.guardSpeed))
			continue;
		if (row.guard == GUARD_SPEED_BELOW && !(ownSpeed < row.guardSpeed))
			continue;
		return &row;
	}
	return NULL;
}

// The stance move of one step: the first row of the stance transition table
// whose stance is the current one and whose trigger is in the step's mask. The
// rule is the state one, over its own rows (VICE-176); null when no row fires.
static const tMoveStanceTransitionRow *
MoveStanceTransitionFor(eStance from, uint32_t triggers)
{
	for (int i = 0; i < MOVE_STANCE_TRANSITION_ROW_COUNT; i++) {
		const tMoveStanceTransitionRow &row = moveStanceTransitions[i];
		if (row.from == from && (row.trigger & triggers) != 0)
			return &row;
	}
	return NULL;
}

// The rung of the step. The slide moves at its entry rung whatever the pad
// asks: the ladder names the slide's own speed, not the sprint that fed it, and
// there is no sprint tier inside the slide (VICE-180). Every other state keeps
// the rung the adapter names from the pad ramp - the tier becomes the intent's
// own value and the ladder the source of the speeds with VICE-184.
static eMoveTier
MoveTierOfIntent(eMoveStateId state, const SMoveIntent &intent)
{
	if (state == MOVE_SLIDE)
		return TIER_SLIDE_ENTRY;
	return intent.tier;
}

// The ladder row of one step: the first row whose tier, state mask and stance
// are the step's. A stance without a rung of its own keeps the standing rung
// (the stance row scales the speed), and the walk rung, the ladder's anchor, is
// the last resort.
static const tMoveLadderRow &
MoveLadderRowOf(eMoveStateId state, eStance stance, eMoveTier tier)
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
	m_fPrevFacing = 0.0f;
	m_fEntryBoost = 0.0f;
	m_bFirstStep = true;
	m_Snapshot = SMoveSnapshot{};
}

// One step of the player's movement. The body is the controller's state machine
// (CPlayerPed::UpdatePlayerMovement, PlayerPed.cpp:1783-1899) with the engine
// reads replaced by the intent, the writes by the snapshot and the constants by
// the tables (VICE-171), plus the slide's own step (VICE-180).
const SMoveSnapshot &
CPlayerMoveSolver::Step(const SMoveIntent &intent)
{
	// The ped's own velocity is the velocity without the surface it stands on,
	// and its speed is the fact the guarded rows read (the slide's entry and
	// exit speeds, VICE-180).
	SMoveVec2 velocity = intent.vel;
	float ownSpeed = Vec2Magnitude(velocity);

	// The trigger bits of this step, collected once (eMoveTrigger): the ground
	// fact, the action edges the intent carries, and the slide's entry pair -
	// the crouch press at the sprint tier, the two facts in one frame
	// (VICE-180). The wall and the head bits arrive with the tests that read
	// them.
	uint32_t triggers = intent.onGround ? TRIGGER_LAND : TRIGGER_AIRBORNE;
	if (intent.jumpRequested)
		triggers |= TRIGGER_JUMP;
	if (intent.diveRequested)
		triggers |= TRIGGER_DIVE;
	// The stance triggers (VICE-176): the Duck edge, the sprint and the
	// exit-vehicle press are the stance's own, and the headroom is the one
	// world fact of the step - the adapter asked VICE-178's test, so the
	// solver reads a fact and never the world.
	if (intent.crouchRequested)
		triggers |= TRIGGER_CROUCH;
	if (intent.sprintRequested)
		triggers |= TRIGGER_SPRINT;
	if (intent.exitVehiclePressed)
		triggers |= TRIGGER_EXIT_VEHICLE;
	if (!intent.headroomClear)
		triggers |= TRIGGER_HEADROOM_BLOCKED;
	if (intent.crouchRequested && intent.tier == TIER_SPRINT)
		triggers |= TRIGGER_SLIDE;
	// The prone action is a ground action (VICE-177): the body lies down on a
	// surface, so with the feet off the ground the edge does nothing. The
	// slide's own request already stands on the ground.
	if (intent.proneRequested && intent.onGround)
		triggers |= TRIGGER_PRONE;

	// The slide's entry consumes the crouch edge: the one press cannot both
	// start the slide and toggle the crouch stance (the slide is a state of its
	// own, and its exit below the exit speed is what puts the ped in the crouch)
	if ((triggers & TRIGGER_SLIDE) != 0)
		triggers &= ~TRIGGER_CROUCH;

	// The move: the first transition row whose state, trigger and guard match
	// fires, so the table's order is the priority and the thresholds live in the
	// rows. The slide reaches the ground and the air, and the slide leaves them
	// (VICE-180).
	uint32_t events = 0;
	const tMoveTransitionRow *transition = MoveTransitionFor(m_State, triggers, ownSpeed);
	if (transition != NULL) {
		m_State = transition->to;
		// The old state's boost goes with it: a boost belongs to the frame the
		// state that carries it was entered.
		m_fEntryBoost = 0.0f;
		events = transition->onEntryEvent;
	}

	// The stance move: the same scan over the stance's own rows, on the same
	// mask, before the rows are read, so a stance the step enters already
	// scales the step. Nothing else is touched: the stance carries no event and
	// the movement state's own timer is not the stance's.
	const tMoveStanceTransitionRow *stanceMove = MoveStanceTransitionFor(m_Stance, triggers);
	if (stanceMove != NULL)
		m_Stance = stanceMove->to;

	const tMoveStateRow &stateRow = moveStateRows[m_State];
	const tMoveStanceRow &stanceRow = moveStanceRows[m_Stance];
	// The turn rate of the step is the state's rate scaled by the stance's
	float turnRate = stateRow.turnRate * stanceRow.turnScale;

	// A state entered with an entry boost takes it in the frame it starts, along
	// the direction the ped already moves: the slide's entry lifts the speed
	// over the sprint's before the friction touches it (VICE-180). The boost
	// then decays on the row's own rate, inside the state's own step.
	if (transition != NULL && stateRow.entryBoost > 0.0f && ownSpeed > 0.0f) {
		m_fEntryBoost = stateRow.entryBoost;
		velocity = Vec2Scale(velocity, (ownSpeed + stateRow.entryBoost) / ownSpeed);
		ownSpeed = Vec2Magnitude(velocity);
	}

	// The ped moves at the ground speed of the animation it plays, so the legs
	// stay with the movement in every direction and with every weapon
	float targetSpeed = intent.hasInput ? intent.targetSpeed * stanceRow.speedScale * stateRow.inputScale : 0.0f;
	SMoveVec2 moveDirection = SMoveVec2{ -sinf(intent.moveHeading), cosf(intent.moveHeading) };
	SMoveVec2 targetVelocity = Vec2Scale(moveDirection, targetSpeed);

	if (m_State == MOVE_SLIDE) {
		// The slide (VICE-180). The friction curve bleeds the speed the entry
		// boost has just lifted over the sprint's: the boost's own decay rides
		// on the friction while any of the boost is left, so the slide's head is
		// steep and its tail is the friction alone. The slope the ped slides on
		// accelerates it along the horizontal part of the surface normal - the
		// downhill direction, the field the intent already carries - and the
		// input steers it at the row's turn rate without changing its speed. The
		// row's input scale is zero, so the input adds no speed: the friction
		// always wins and the slide reaches its exit.
		float curve = stateRow.friction;
		if (m_fEntryBoost > 0.0f) {
			curve += stateRow.boostDecay;
			m_fEntryBoost -= stateRow.boostDecay * intent.dt;
			if (m_fEntryBoost < 0.0f)
				m_fEntryBoost = 0.0f;
		}
		float rate = ownSpeed > targetSpeed ? curve : stateRow.accel;
		velocity = MoveVelocityTowards(velocity, targetVelocity, rate * intent.dt);
		if (intent.steepCliff) {
			SMoveVec2 downhill = SMoveVec2{ intent.slopeNormal.x, intent.slopeNormal.y };
			velocity = Vec2Add(velocity, Vec2Scale(downhill, MOVE_SLIDE_SLOPE_ACCEL * intent.dt));
		}
		// The steering keeps the length: only the curve above changes the speed
		if ((stateRow.flags & MOVEFLAG_STEER_INPUT) && intent.hasInput && ownSpeed > 0.0f)
			velocity = SteerVelocityTowards(velocity, moveDirection, turnRate * intent.dt);
	} else if (stateRow.flags & MOVEFLAG_KEEP_MOMENTUM) {
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
		// intent's normal is the slide's own (VICE-180).
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

	// The facts of the step the listeners read. The spin is the body turn: the
	// facing the adapter passed less the facing of the previous step, wrapped,
	// so a reader (or the CSV replay) adds the spins up and holds the body yaw.
	float spin = 0.0f;
	if (m_bFirstStep)
		m_bFirstStep = false;
	else
		spin = LimitRadianAngle(intent.facing - m_fPrevFacing);
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

	const eMoveTier tier = MoveTierOfIntent(m_State, intent);

	m_Snapshot.state = m_State;
	m_Snapshot.stance = m_Stance;
	m_Snapshot.moved = velocity;
	m_Snapshot.velocity = Vec2Add(velocity, intent.surfaceSpeed);
	m_Snapshot.speed = Vec2Magnitude(velocity);
	m_Snapshot.targetSpeed = targetSpeed;
	m_Snapshot.tier = tier;
	m_Snapshot.facingDir = facingDir;
	m_Snapshot.spin = spin;
	// The contacts of the step: the feet on the surface, which is the slide's
	// own contact too (the slide never leaves the ground on its own). The wall
	// and the head bits arrive with the tests that read them.
	m_Snapshot.contacts = intent.onGround ? CONTACT_GROUND : 0;
	m_Snapshot.slope = slope;
	m_Snapshot.surface = intent.surface;
	m_Snapshot.heading = intent.moveHeading;
	m_Snapshot.turnScale = stanceRow.turnScale;
	m_Snapshot.capsuleHeight = stanceRow.capsuleHeight;
	// The entry event of the move and the rung of the step.
	m_Snapshot.events = events;
	m_Snapshot.reportMoveState = MoveLadderRowOf(m_State, m_Stance, tier).reportMoveState;

	return m_Snapshot;
}

// The failure of one rule: names the rule in `why` and fails.
static bool
MoveTablesFail(const char **why, const char *rule)
{
	*why = rule;
	return false;
}

// The table check (VICE-171, VICE-180): every table to its invariants. The
// rules are the ones the harness (VICE-172) needs: a state without a row, a
// transition to an id out of range or to a guard out of range, a ladder row
// that does not ascend, a stance row out of range or one whose capsule top
// sinks under the ground (VICE-177: the height is signed, the prone body lies
// below the origin), and the slide's own rules - an entry row, an exit guard
// that names the exit speed, the exit speed below the entry's, and the slide's
// rungs above the sprint's (VICE-180).
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

	// One ladder row per rung, in the rung order, the slide's rungs above the
	// sprint's, every rung above the one below it, and a walk rung for every
	// state
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
	}
	// The slide's two rungs sit above the sprint's (VICE-180): the slide is the
	// fastest stride and the jump out of it is the fastest of all. The rule is
	// named here, ahead of the general ascend below, because the slide's own
	// ladder is what the top of it is for.
	if (!(tables.ladder[TIER_SLIDE_ENTRY].speedScale > tables.ladder[TIER_SPRINT].speedScale)
		|| !(tables.ladder[TIER_SLIDE_JUMP].speedScale > tables.ladder[TIER_SPRINT].speedScale))
		return MoveTablesFail(why, "a slide rung at or below the sprint's");
	for (int i = 1; i < tables.numLadder; i++) {
		if (!(tables.ladder[i - 1].speedScale < tables.ladder[i].speedScale))
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

	// One stance row per stance, in range. The capsule height is signed, the
	// way the collision sets are: the prone body lies below the origin. The one
	// rule that holds for every posture is that its top stays above the ground
	// under the feet (TempColModels.cpp authors the sets in the origin frame,
	// whose ground is FEET_OFFSET below).
	if (tables.numStances != STANCE_NUM)
		return MoveTablesFail(why, "a stance without a row");
	for (int i = 0; i < tables.numStances; i++) {
		const tMoveStanceRow &row = tables.stances[i];
		if (!(row.speedScale >= 0.0f) || !(row.turnScale >= 0.0f))
			return MoveTablesFail(why, "a stance row out of range");
		if (!(row.capsuleHeight > -MOVE_FEET_OFFSET))
			return MoveTablesFail(why, "a stance row whose top is under the ground");
	}

	// Every move names a state and a trigger in range, and its guard and
	// threshold are the row's own
	for (int i = 0; i < tables.numTransitions; i++) {
		const tMoveTransitionRow &row = tables.transitions[i];
		int from = (int)row.from;
		int to = (int)row.to;
		int guard = (int)row.guard;
		if (from < 0 || from >= MOVE_NUMSTATES)
			return MoveTablesFail(why, "a transition from a state out of range");
		if (to < 0 || to >= MOVE_NUMSTATES)
			return MoveTablesFail(why, "a transition to a state out of range");
		if (row.trigger == 0 || (row.trigger & ~MOVE_TRIGGER_ALL) != 0)
			return MoveTablesFail(why, "a transition trigger out of range");
		if (guard < 0 || guard >= GUARD_NUM)
			return MoveTablesFail(why, "a transition guard out of range");
		if (guard == GUARD_NONE && row.guardSpeed != 0.0f)
			return MoveTablesFail(why, "an unguarded transition with a threshold");
		if (guard != GUARD_NONE && !(row.guardSpeed > 0.0f))
			return MoveTablesFail(why, "a guarded transition without its threshold");
	}

	// The slide's rows (VICE-180): an entry row exists, the exit guard names
	// the exit speed, and the exit threshold sits below every entry threshold.
	// That order is the hysteresis the design asks for: a slide that has just
	// started - at or above its entry speed - cannot end on its entry frame,
	// and a ped that sits on the exit threshold cannot flap the state, because
	// leaving needs the slow speed and entering needs the fast one.
	{
		bool hasEntry = false;
		bool hasExit = false;
		float exitSpeed = 0.0f;

		for (int i = 0; i < tables.numTransitions; i++) {
			const tMoveTransitionRow &row = tables.transitions[i];
			if (row.to == MOVE_SLIDE)
				hasEntry = true;
			if (row.from == MOVE_SLIDE && row.guard == GUARD_SPEED_BELOW) {
				hasExit = true;
				exitSpeed = row.guardSpeed;
			}
		}
		if (!hasEntry)
			return MoveTablesFail(why, "a slide without an entry row");
		if (!hasExit || !(exitSpeed > 0.0f))
			return MoveTablesFail(why, "a slide exit without the exit speed");
		for (int i = 0; i < tables.numTransitions; i++) {
			const tMoveTransitionRow &row = tables.transitions[i];
			if (row.to != MOVE_SLIDE || row.guard != GUARD_SPEED_ABOVE)
				continue;
			if (!(exitSpeed < row.guardSpeed))
				return MoveTablesFail(why, "a slide whose exit speed is not below its entry speed");
		}
	}

	// Every stance move names a stance and a trigger in range (VICE-176)
	for (int i = 0; i < tables.numStanceTransitions; i++) {
		const tMoveStanceTransitionRow &row = tables.stanceTransitions[i];
		int from = (int)row.from;
		int to = (int)row.to;
		if (from < 0 || from >= STANCE_NUM)
			return MoveTablesFail(why, "a stance move from a stance out of range");
		if (to < 0 || to >= STANCE_NUM)
			return MoveTablesFail(why, "a stance move to a stance out of range");
		if (row.trigger == 0 || (row.trigger & ~MOVE_TRIGGER_ALL) != 0)
			return MoveTablesFail(why, "a stance move trigger out of range");
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
		moveStanceTransitions, MOVE_STANCE_TRANSITION_ROW_COUNT,
	};
	return PlayerMoveTablesCheck(tables, why);
}

const char *
CPlayerMoveSolver::GetStateName(eMoveStateId state)
{
	if (state < 0 || state >= MOVE_NUMSTATES)
		return "?";
	return moveStateNames[state];
}

const char *
CPlayerMoveSolver::GetStanceName(eStance stance)
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
