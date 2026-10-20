/*
 * The evidence program for VICE-172: the movement solver's harness and the
 * no-animation-lock invariant.
 *
 * It compiles and drives the game's own solver module
 * (src/peds/PlayerMoveSolver.cpp - one intent struct in, one snapshot out)
 * with no engine and no stubs: the intent and the snapshot are plain data, and
 * the module includes no engine header (the structural check below proves the
 * object file names no engine symbol).
 *
 *	A. The parity. The movement maths of the pre-rework controller
 *	   (src/peds/PlayerPed.cpp at ef9b65f598a0f09b39a7476a8aae048d827c3a5c:
 *	   MoveVelocityTowards, SteerVelocityTowards, the state rows and the
 *	   ground and the air branches of CPlayerPed::UpdatePlayerMovement) is
 *	   transcribed here as the reference, and both implementations are driven
 *	   from the same randomized intent stream (one fixed seed, 400,000 steps,
 *	   with the targeted states: no input, zero dt, the air, the steep slope,
 *	   a zero target speed). The returned velocity, the moved vector and the
 *	   state must agree bit for bit, at every step.
 *
 *	B. The invariant, structurally. The module is compiled again, alone, to
 *	   an object file, and every undefined symbol must come from the C
 *	   library or the math library. An engine read (an animation
 *	   association, the clock, the pad, the camera) would add a symbol and
 *	   fail the run. A planted translation unit that reads an engine symbol
 *	   is compiled beside it and must be refused, so the list's teeth are
 *	   checked on every run.
 *
 *	C. The invariant, behaviourally. The target speed comes from the intent
 *	   alone: the animation side of the adapter is modelled outside the
 *	   intent (the association absent in one run, present but frozen - the
 *	   classic animation lock - in the other), the two velocity series must
 *	   be bit-identical, the pinned target must be reached exactly, and a
 *	   moved goalpost (a second stream with another pinned target) must
 *	   change the result accordingly.
 *
 *	D. The determinism. The same intent stream twice gives identical
 *	   snapshots, and two solvers stepped interleaved equal the same two
 *	   stepped apart.
 *
 *	E. The tables. PlayerMoveTablesCheck holds every table to its invariants
 *	   and drives each rejection with a planted row - the slide's, the dive's
 *	   and the jump's rules included. The parity stream keeps the slide's
 *	   pair, the dive's edge and the jump's edge off: the reference maths of
 *	   VICE-172 has none of them, and each has its own section below.
 *
 *	F. The events. The event bits are VICE-173's list: every landing raises
 *	   EVENT_LANDED once, the dive's landing raises it once on its own frame,
 *	   and the ground's jump raises EVENT_JUMP once, on the frame it leaves
 *	   the ground. A press in the air raises nothing, and the state follows
 *	   the ground and the air every step.
 *
 *	G. The finiteness. Every output is finite for dt in {0, 1/50, 1/10}, the
 *	   pad's extreme (the target speed at 2 engine units) and the zero input,
 *	   on the ground, in the air, on the steep slope and in the slide.
 *
 *	H. The slide (VICE-180). The parity sweep above must never enter the
 *	   slide - the reference maths of VICE-172 has no slide, so the sweep's
 *	   intents leave the crouch edge off - and the slide is driven here by
 *	   hand instead: the entry (the sprint plus the crouch edge, in one frame,
 *	   with the entry boost above the sprint's speed), the decay (monotonic
 *	   over flat ground), the exit (below the exit speed and not above it, and
 *	   no entry below the entry speed: the hysteresis), the jump (the air
 *	   takes the slide's speed and keeps it), and the two other exits the
 *	   design names (a prone request, the feet leaving the surface).
 *
 *	H. The stance (VICE-176). The crouch row scales the target speed and the
 *	   turn rate, the stance moves are the table's rows and fire on the
 *	   edges (the Duck toggle, the sprint, the jump, the exit-vehicle press,
 *	   with the sprint's own gate on the entry), a crouch that is never
 *	   cancelled holds for a minute of steps, and VICE-178's headroom holds
 *	   every stand-up back until the body can rise. The parity stream keeps
 *	   the stance out of it: the reference maths has no stance row, so no
 *	   new edge is fed to it.
 *
 *	I. The prone (VICE-177). The prone row is the crawl - the crawl cycle's
 *	   own speed as a share of the walk's, half the standing turn rate and the
 *	   signed capsule top of VICE-178's prone set, which hangs below the
 *	   origin. The PED_PRONE edge lies the ped down from the stand and from
 *	   the crouch and only on the ground, the prone's own edge stands him up,
 *	   the Duck edge lifts him into the crouch (the low ladder is prone,
 *	   crouch, stand), the sprint, the jump and the exit-vehicle press stand
 *	   him up, and the headroom holds every one of those exits back. The
 *	   parity stream keeps the prone out of it for the same reason as the
 *	   crouch.
 *
 *	J. The dive (VICE-181). The dive is the prone action with the feet off the
 *	   ground, so its edge is the PED_PRONE edge against the grounded flag and
 *	   the dive enters from the air and never from the ground. The row keeps
 *	   the momentum the dive was entered with (a dive off a jump, a slide jump
 *	   and a fall all carry their speed through it, bit for bit with the pad
 *	   released) and steers the velocity direction at its own turn rate -
 *	   twice the air's, a small authority against the ground's 12. The body's
 *	   own yaw adds no rule: it is the engine's one (the body faces the camera
 *	   in every view and TurnTargetPed sets it outright in the first person
 *	   view), and the snapshot's spin reports the turn, so the dive spins with
 *	   the look and the solver only reads the facing in and reports the turn
 *	   out. The feet touching the ground land the dive in the slide - the
 *	   slide-out, whose own row and step bleed the speed, so no curve is
 *	   copied into the dive - and the stance rows of the touchdown put the
 *	   body in the prone on the same frame. A wall or a head hit has no row:
 *	   those triggers have no producer in the tree yet (the tests that read
 *	   them arrive with the wall run), so the dive has no exit but the ground.
 *	   The parity stream leaves the dive's edge off: the reference maths of
 *	   VICE-172 has no dive.
 *
 *	K. The jump (VICE-182). The player's jump is the input's: the press leaves
 *	   the ground state for the air on the frame it is read and reports the
 *	   jump event, so the adapter writes the vertical impulse on that frame,
 *	   not when the launch animation's finish callback used to. The impulse is
 *	   the stock callback's 8.5 (MOVE_PLAYER_JUMP_IMPULSE), the horizontal
 *	   velocity crosses the edge untouched, the landing clears the air state
 *	   and the contact and leaves no state behind, and a press in the air is
 *	   no second launch. The parity stream leaves the jump's edge off: the
 *	   reference maths of VICE-172 has no ground jump row.
 *
 * Build and run (from the repository root; the structural check drives the
 * compiler and nm itself and writes its scratch files under /tmp/opencode):
 *
 *	c++ -std=c++17 -O2 -o /tmp/opencode/movement-solver src/peds/PlayerMoveSolver.cpp utils/movement-solver/harness.cpp
 *	/tmp/opencode/movement-solver
 *
 * The exit code is 0 when every check passes.
 */

#include "../../src/peds/PlayerMoveSolver.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(a)	(sizeof(a) / sizeof((a)[0]))

/* ---------------------------------------------------------------------- *
 * the checks
 * ---------------------------------------------------------------------- */

static int g_checks;
static int g_failures;

static void
Check(bool ok, const char *what)
{
	g_checks++;
	if (!ok) {
		g_failures++;
		printf("  FAIL: %s\n", what);
	}
}

/* ---------------------------------------------------------------------- *
 * the deterministic stream
 * ---------------------------------------------------------------------- */

/*
 * The intent stream is drawn from a splitmix64 generator at one fixed seed, so
 * every run drives the same stream and a failure is reproducible. The ranges
 * cover what the adapter can hand the solver: the walk, run and sprint target
 * speeds (engine units, one unit per 1/50 second), the surface speeds, the
 * ground and the air, the steep slopes, the input and its absence, and the
 * action edges the later tickets add.
 */
#define STREAM_SEED	0x1726d4c1b3a59f77ull

static uint64_t g_seed;

static void
SetStreamSeed(uint64_t seed)
{
	g_seed = seed;
}

static uint64_t
SplitMix(void)
{
	uint64_t z = (g_seed += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

static uint32_t
RngBits(void)
{
	return (uint32_t)(SplitMix() >> 32);
}

/* A float in [low, high) */
static float
RngFloat(float low, float high)
{
	return low + (high - low) * (float)(SplitMix() >> 40) * (1.0f / 16777216.0f);
}

/* True `percent` times out of a hundred */
static bool
RngChance(int percent)
{
	return (int)(RngBits() % 100) < percent;
}

static SMoveIntent
MakeIntent(void)
{
	SMoveIntent intent;

	intent.dt = RngChance(92) ? 1.0f / 50.0f : RngFloat(0.0f, 1.0f / 10.0f);
	intent.vel = SMoveVec2{ RngFloat(-1.5f, 1.5f), RngFloat(-1.5f, 1.5f) };
	intent.surfaceSpeed = RngChance(25)
		? SMoveVec2{ RngFloat(-0.5f, 0.5f), RngFloat(-0.5f, 0.5f) }
		: SMoveVec2{ 0.0f, 0.0f };
	intent.slopeNormal = SMoveVec3{ 0.0f, 0.0f, 0.0f };
	intent.moveHeading = RngFloat(-3.2f, 3.2f);
	intent.targetSpeed = RngChance(80) ? RngFloat(0.0f, 1.0f) : 0.0f;
	intent.facing = RngFloat(-3.2f, 3.2f);
	// the header's numbers: SURFACE_TARMAC and SURFACE_PED (SurfaceTable.h)
	intent.surface = RngChance(50) ? 1 : 17;
	intent.onGround = RngChance(65);
	intent.steepCliff = RngChance(intent.onGround ? 12 : 4);
	if (intent.steepCliff)
		intent.slopeNormal = SMoveVec3{ RngFloat(-1.0f, 1.0f), RngFloat(-1.0f, 1.0f), 0.0f };
	intent.hasInput = RngChance(80);
	intent.tier = !RngChance(40) ? TIER_WALK : RngChance(50) ? TIER_RUN : TIER_SPRINT;
	/* The jump's own edge stays off: the reference maths of VICE-172 has no
	 * ground jump row, so a stream that entered the air on the press could not
	 * be compared. The jump's own checks drive the edge by hand (11, VICE-182),
	 * and the finiteness sweep sets it for its own slice. */
	intent.jumpRequested = false;
	/* The dive's own edge stays off: the reference maths of VICE-172 has no
	 * dive, so a stream that entered it could not be compared. The dive's own
	 * checks drive the edge by hand (10, VICE-181), and the finiteness sweep
	 * sets it for its own slice. */
	intent.diveRequested = false;
	/* The slide's entry pair, the prone request and the stance's edges stay off:
	 * the parity reference of VICE-172 has no slide and no stance row, so a
	 * stream that entered either could not be compared. The slide's own checks
	 * drive the pair by hand (7, VICE-180), the stance has its own section (8,
	 * VICE-176) and the dive its own (10, VICE-181), and the finiteness sweep
	 * sets them for its own slice. */
	intent.crouchRequested = false;
	intent.proneRequested = false;
	intent.sprintRequested = false;
	intent.exitVehiclePressed = false;
	intent.headroomClear = true;
	return intent;
}

/* The one intent the behavioural stream and the finiteness sweep build by
 * hand: a walk step on flat ground, its fields overwritten per case. */
static SMoveIntent
WalkIntent(float heading, float targetSpeed)
{
	SMoveIntent intent;

	intent.dt = 1.0f / 50.0f;
	intent.vel = SMoveVec2{ 0.0f, 0.0f };
	intent.surfaceSpeed = SMoveVec2{ 0.0f, 0.0f };
	intent.slopeNormal = SMoveVec3{ 0.0f, 0.0f, 0.0f };
	intent.moveHeading = heading;
	intent.targetSpeed = targetSpeed;
	intent.facing = heading;
	intent.surface = 1;	// SURFACE_TARMAC (SurfaceTable.h)
	intent.onGround = true;
	intent.steepCliff = false;
	intent.hasInput = true;
	intent.tier = TIER_WALK;
	intent.jumpRequested = false;
	intent.diveRequested = false;
	intent.crouchRequested = false;
	intent.proneRequested = false;
	intent.sprintRequested = false;
	intent.exitVehiclePressed = false;
	intent.headroomClear = true;
	return intent;
}

/* The crouch walk step of the stance section: the walk intent with the Duck
 * edge, the edge that enters the crouch (the table's row fires on it). */
static SMoveIntent
CrouchIntent(float heading, float targetSpeed)
{
	SMoveIntent intent = WalkIntent(heading, targetSpeed);
	intent.crouchRequested = true;
	return intent;
}

/* The prone step of the prone section: the walk intent with the prone edge,
 * the edge that lies the ped down on the ground (VICE-177). */
static SMoveIntent
ProneIntent(float heading, float targetSpeed)
{
	SMoveIntent intent = WalkIntent(heading, targetSpeed);
	intent.proneRequested = true;
	return intent;
}

/* ---------------------------------------------------------------------- *
 * 1. the reference: the pre-rework maths
 * ---------------------------------------------------------------------- */

/*
 * Transcribed from src/peds/PlayerPed.cpp at
 * ef9b65f598a0f09b39a7476a8aae048d827c3a5c (the commit before the movement
 * solver merge): MoveVelocityTowards (PlayerPed.cpp:80),
 * SteerVelocityTowards (PlayerPed.cpp:91), the state rows (playerMovementStates,
 * PlayerPed.cpp:73) and the ground and the air branches of
 * CPlayerPed::UpdatePlayerMovement (PlayerPed.cpp:1868-1894). The engine's Sin,
 * Cos, Atan2 and Sqrt are the float libm calls (src/math/maths.h), the same
 * ones the module calls, and the engine's Clamp (common.h:304) and
 * CGeneral::LimitRadianAngle (General.h:58) are the two functions the module
 * transcribed itself, so the comparison is bit for bit.
 *
 * Two places differ from the old controller, both documented by the module:
 * the steep slope's PushOffSteepSlope already ran in the adapter, so the
 * intent's velocity is the pushed one, and GetMoveAnimReferenceSpeed arrives as
 * the intent's targetSpeed.
 */

/* ePlayerMovementState (PlayerPed.cpp:66 at ef9b65f5) */
#define REF_PLAYERMOVE_GROUND	0
#define REF_PLAYERMOVE_AIR	1

#define REF_PI			3.14159265358979323846f

struct tRefMovementState
{
	float accel;
	float friction;
	float turnRate;
};

static const tRefMovementState refMovementStates[2] = {
	{ 20.0f / 50.0f, 20.0f / 50.0f, 12.0f },	// PLAYER_GROUND_ACCEL/FRICTION/TURN_RATE
	{ 4.0f / 50.0f, 0.0f, 1.6f },			// PLAYER_AIR_ACCEL/TURN_RATE
};

struct SRefMoveState
{
	int state;		// the old m_nMovementState
	float fStateTime;	// the solver's own, mirrored: no output in phase 0
};

static float
RefLimitRadianAngle(float angle)
{
	// CGeneral::LimitRadianAngle, with the engine's Clamp macro spelled out
	float result = angle < -25.0f ? -25.0f : angle > 25.0f ? 25.0f : angle;

	while (result >= REF_PI)
		result -= 2 * REF_PI;

	while (result < -REF_PI)
		result += 2 * REF_PI;

	return result;
}

/* MoveVelocityTowards (PlayerPed.cpp:80) */
static SMoveVec2
RefMoveVelocityTowards(const SMoveVec2 &velocity, const SMoveVec2 &target, float maxDelta)
{
	SMoveVec2 change = { target.x - velocity.x, target.y - velocity.y };
	float magnitude = sqrtf(change.x * change.x + change.y * change.y);

	if (magnitude <= maxDelta)
		return target;
	float scale = maxDelta / magnitude;
	return SMoveVec2{ velocity.x + change.x * scale, velocity.y + change.y * scale };
}

/* SteerVelocityTowards (PlayerPed.cpp:91) */
static SMoveVec2
RefSteerVelocityTowards(const SMoveVec2 &velocity, const SMoveVec2 &target, float maxTurn)
{
	float angle = atan2f(velocity.y, velocity.x);
	float targetAngle = atan2f(target.y, target.x);
	float turn = RefLimitRadianAngle(targetAngle - angle);

	if (turn > maxTurn)
		turn = maxTurn;
	else if (turn < -maxTurn)
		turn = -maxTurn;
	float newAngle = angle + turn;
	float magnitude = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);
	return SMoveVec2{ cosf(newAngle) * magnitude, sinf(newAngle) * magnitude };
}

/* The old controller's step: the same input arrives as the intent, the same
 * velocity and state come back out as the snapshot. */
static SMoveSnapshot
RefStep(SRefMoveState *pRef, const SMoveIntent &intent)
{
	SMoveSnapshot snapshot;

	/* m_nMovementState = bIsStanding ? PLAYERMOVE_GROUND : PLAYERMOVE_AIR */
	int state = intent.onGround ? REF_PLAYERMOVE_GROUND : REF_PLAYERMOVE_AIR;
	if (state != pRef->state) {
		pRef->state = state;
		pRef->fStateTime = 0.0f;
	}
	pRef->fStateTime += intent.dt;

	const tRefMovementState &row = refMovementStates[state];

	/* CVector2D velocity = CVector2D(m_vecMoveSpeed) - surfaceSpeed */
	SMoveVec2 velocity = intent.vel;
	/* float dt = CTimer::GetTimeStepInSeconds() */
	float dt = intent.dt;
	float ownSpeed = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);
	/* bool bMoving = padMoveInGameUnit > 0.0f */
	bool bMoving = intent.hasInput;
	/* float targetSpeed = bMoving ? GetMoveAnimReferenceSpeed() : 0.0f */
	float targetSpeed = bMoving ? intent.targetSpeed : 0.0f;
	/* CVector2D moveDirection(-Sin(m_fMoveHeading), Cos(m_fMoveHeading)) */
	SMoveVec2 moveDirection = { -sinf(intent.moveHeading), cosf(intent.moveHeading) };
	SMoveVec2 targetVelocity = { moveDirection.x * targetSpeed, moveDirection.y * targetSpeed };

	if (state == REF_PLAYERMOVE_AIR) {
		if (bMoving) {
			// Keep the momentum and steer toward the input
			if (ownSpeed > 0.0f)
				velocity = RefSteerVelocityTowards(velocity, moveDirection, row.turnRate * dt);
			if (ownSpeed < targetSpeed) {
				// Air control speeds the ped up to the target, never past it
				velocity = RefMoveVelocityTowards(velocity, targetVelocity, row.accel * dt);
				float newSpeed = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);
				if (newSpeed > targetSpeed) {
					float scale = targetSpeed / newSpeed;
					velocity.x *= scale;
					velocity.y *= scale;
				}
			}
		}
	} else if (intent.steepCliff) {
		// The adapter ran PushOffSteepSlope on the velocity before the step:
		// the slope owns it and the input does not touch it
	} else if (bMoving) {
		// The direction follows the input at the ground turn rate, the speed
		// follows the target at the acceleration or the friction
		if (ownSpeed > 0.0f)
			velocity = RefSteerVelocityTowards(velocity, moveDirection, row.turnRate * dt);
		float rate = ownSpeed > targetSpeed ? row.friction : row.accel;
		velocity = RefMoveVelocityTowards(velocity, targetVelocity, rate * dt);
	} else {
		velocity = RefMoveVelocityTowards(velocity, SMoveVec2{ 0.0f, 0.0f }, row.friction * dt);
	}

	/* m_vecMoveSpeed.x/y = velocity.x/y + surfaceSpeed.x/y; m_moved = velocity */
	snapshot.state = (eMoveStateId)(state == REF_PLAYERMOVE_GROUND ? MOVE_GROUND : MOVE_AIR);
	snapshot.stance = STANCE_STAND;
	snapshot.velocity = SMoveVec2{ velocity.x + intent.surfaceSpeed.x, velocity.y + intent.surfaceSpeed.y };
	snapshot.moved = velocity;
	snapshot.speed = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);
	snapshot.heading = intent.moveHeading;
	snapshot.turnScale = 1.0f;
	snapshot.capsuleHeight = 0.0f;
	snapshot.events = 0;
	snapshot.reportMoveState = 0;
	return snapshot;
}

/* The movement comparison of the parity check: the fields the pre-rework maths
 * also produced. The tables add outputs the controller never had - the event
 * bits and the reported movement state - and those are checked against the
 * tables themselves, not against a reference that cannot know them. */
static bool
SameMovement(const SMoveSnapshot &a, const SMoveSnapshot &b)
{
	return a.state == b.state && a.stance == b.stance &&
		a.velocity.x == b.velocity.x && a.velocity.y == b.velocity.y &&
		a.moved.x == b.moved.x && a.moved.y == b.moved.y &&
		a.speed == b.speed && a.heading == b.heading &&
		a.turnScale == b.turnScale && a.capsuleHeight == b.capsuleHeight;
}

/* The one snapshot comparison the determinism checks use: every field. */
static bool
SameSnapshot(const SMoveSnapshot &a, const SMoveSnapshot &b)
{
	return a.state == b.state && a.stance == b.stance &&
		a.velocity.x == b.velocity.x && a.velocity.y == b.velocity.y &&
		a.moved.x == b.moved.x && a.moved.y == b.moved.y &&
		a.speed == b.speed && a.heading == b.heading &&
		a.turnScale == b.turnScale && a.capsuleHeight == b.capsuleHeight &&
		a.events == b.events &&
		a.reportMoveState == b.reportMoveState;
}

static bool
SnapshotIsFinite(const SMoveSnapshot &snapshot)
{
	return isfinite(snapshot.velocity.x) && isfinite(snapshot.velocity.y) &&
		isfinite(snapshot.moved.x) && isfinite(snapshot.moved.y) &&
		isfinite(snapshot.speed) && isfinite(snapshot.heading) &&
		isfinite(snapshot.turnScale) && isfinite(snapshot.capsuleHeight);
}

static void
PrintIntent(const SMoveIntent &intent)
{
	printf("    intent: dt %.9g vel (%.9g, %.9g) surface (%.9g, %.9g) heading %.9g target %.9g\n",
		intent.dt, intent.vel.x, intent.vel.y,
		intent.surfaceSpeed.x, intent.surfaceSpeed.y,
		intent.moveHeading, intent.targetSpeed);
	printf("      ground %d steep %d input %d tier %d jump %d dive %d\n",
		intent.onGround, intent.steepCliff, intent.hasInput,
		intent.tier, intent.jumpRequested, intent.diveRequested);
}

static void
PrintSnapshot(const char *name, const SMoveSnapshot &snapshot)
{
	printf("    %s: state %d stance %d velocity (%.9g, %.9g) moved (%.9g, %.9g)\n",
		name, snapshot.state, snapshot.stance,
		snapshot.velocity.x, snapshot.velocity.y, snapshot.moved.x, snapshot.moved.y);
	printf("      speed %.9g heading %.9g turnScale %.9g capsule %.9g events %u report %u\n",
		snapshot.speed, snapshot.heading, snapshot.turnScale, snapshot.capsuleHeight,
		snapshot.events, snapshot.reportMoveState);
}

/* One parity step against the reference; false on a mismatch. */
static bool
StepParity(CPlayerMoveSolver *pSolver, SRefMoveState *pRef, const SMoveIntent &intent, SMoveSnapshot *pGot)
{
	SMoveSnapshot want;

	*pGot = pSolver->Step(intent);
	want = RefStep(pRef, intent);
	return SameMovement(*pGot, want);
}

/* ---------------------------------------------------------------------- *
 * 1a. the parity: the randomized sweep
 * ---------------------------------------------------------------------- */

#define PARITY_STEPS	400000

static void
CheckParitySweep(void)
{
	CPlayerMoveSolver solver;
	SRefMoveState ref = { REF_PLAYERMOVE_GROUND, 0.0f };
	SMoveVec2 liveVelocity = { 1.0f, 0.0f };	// the adapter's own velocity, read back each frame
	int mismatches = 0;
	int ground = 0, air = 0, steep = 0, idle = 0, zeroTarget = 0;

	SetStreamSeed(STREAM_SEED);
	for (int i = 0; i < PARITY_STEPS; i++) {
		SMoveIntent intent = MakeIntent();
		SMoveSnapshot got, want;

		// half the steps feed the previous velocity back, the way the adapter
		// reads the live ped each frame; the other half probes an arbitrary one
		if (i & 1)
			intent.vel = liveVelocity;
		got = solver.Step(intent);
		want = RefStep(&ref, intent);
		if (!SameMovement(got, want)) {
			if (mismatches < 3) {
				printf("  step %d:\n", i);
				PrintIntent(intent);
				PrintSnapshot("solver", got);
				PrintSnapshot("reference", want);
			}
			mismatches++;
		}
		liveVelocity = got.moved;
		if (intent.onGround)
			ground++;
		else
			air++;
		if (intent.steepCliff)
			steep++;
		if (!intent.hasInput)
			idle++;
		if (intent.targetSpeed == 0.0f)
			zeroTarget++;
	}

	Check(mismatches == 0, "the solver and the pre-rework maths agree bit for bit on every randomized step");
	printf("  %d steps (%d on the ground, %d in the air, %d on a steep slope, %d without input,\n",
		PARITY_STEPS, ground, air, steep, idle);
	printf("  %d with a zero target speed): the velocity, the moved vector and the state bit for bit\n",
		zeroTarget);
}

/* ---------------------------------------------------------------------- *
 * 1b. the parity: the targeted states
 * ---------------------------------------------------------------------- */

static void
CheckParityTargeted(void)
{
	int mismatches = 0;

	/* (a) no input: the ground friction takes the ped at rest, exactly */
	{
		CPlayerMoveSolver solver;
		SRefMoveState ref = { REF_PLAYERMOVE_GROUND, 0.0f };
		SMoveIntent intent = WalkIntent(0.9f, 0.3f);
		SMoveSnapshot got;
		float before;

		intent.hasInput = false;
		intent.vel = SMoveVec2{ 0.6f, -0.2f };
		before = sqrtf(intent.vel.x * intent.vel.x + intent.vel.y * intent.vel.y);
		if (!StepParity(&solver, &ref, intent, &got))
			mismatches++;
		Check(got.speed < before, "no input: the ground step slows the ped (the friction)");
		for (int i = 0; i < 200; i++) {
			intent.vel = got.moved;
			if (!StepParity(&solver, &ref, intent, &got))
				mismatches++;
		}
		Check(got.moved.x == 0.0f && got.moved.y == 0.0f && got.speed == 0.0f,
			"no input: the ground stream comes to rest exactly");
	}

	/* (b) zero dt: no input changes nothing, bit for bit */
	{
		CPlayerMoveSolver solver;
		SRefMoveState ref = { REF_PLAYERMOVE_GROUND, 0.0f };
		SMoveIntent intent = WalkIntent(0.4f, 0.0f);
		SMoveSnapshot got;
		SMoveVec2 before = { 0.42f, -0.13f };

		intent.dt = 0.0f;
		intent.hasInput = false;
		intent.vel = before;
		if (!StepParity(&solver, &ref, intent, &got))
			mismatches++;
		Check(got.moved.x == before.x && got.moved.y == before.y,
			"zero dt, no input: the step changes nothing, bit for bit");
	}

	/* (c) the air: no input keeps the momentum exactly, the input keeps the
	 *     momentum above the target and never speeds the ped past it below */
	{
		CPlayerMoveSolver solver;
		SRefMoveState ref = { REF_PLAYERMOVE_AIR, 0.0f };
		SMoveIntent intent = WalkIntent(1.1f, 0.3f);
		SMoveSnapshot got;
		SMoveVec2 before = { 0.8f, 0.1f };
		SMoveVec2 slow = { 0.1f, 0.05f };
		float beforeSpeed = sqrtf(before.x * before.x + before.y * before.y);

		intent.onGround = false;
		intent.hasInput = false;
		intent.vel = before;
		if (!StepParity(&solver, &ref, intent, &got))
			mismatches++;
		Check(got.moved.x == before.x && got.moved.y == before.y,
			"the air, no input: the momentum is kept, bit for bit");
		intent.hasInput = true;
		intent.vel = before;
		if (!StepParity(&solver, &ref, intent, &got))
			mismatches++;
		Check(fabsf(got.speed - beforeSpeed) < 1e-5f,
			"the air, input: the momentum above the target is kept (the air has no friction)");
		intent.vel = slow;
		if (!StepParity(&solver, &ref, intent, &got))
			mismatches++;
		Check(got.speed <= intent.targetSpeed,
			"the air, input: the air control never speeds the ped past the target");
	}

	/* (d) the steep slope: the intent's velocity passes through untouched,
	 *     whatever the input asks (the adapter already pushed the ped) */
	{
		CPlayerMoveSolver solver;
		SRefMoveState ref = { REF_PLAYERMOVE_GROUND, 0.0f };
		SMoveIntent intent = WalkIntent(2.0f, 0.5f);
		SMoveSnapshot got;
		SMoveVec2 pushed = { -0.31f, 0.87f };

		intent.steepCliff = true;
		intent.slopeNormal = SMoveVec3{ 0.6f, -0.8f, 0.0f };
		intent.vel = pushed;
		if (!StepParity(&solver, &ref, intent, &got))
			mismatches++;
		Check(got.moved.x == pushed.x && got.moved.y == pushed.y,
			"the steep slope: the pushed velocity passes through untouched");
		intent.moveHeading = -2.0f;
		intent.targetSpeed = 0.9f;
		intent.hasInput = true;
		intent.vel = pushed;
		if (!StepParity(&solver, &ref, intent, &got))
			mismatches++;
		Check(got.moved.x == pushed.x && got.moved.y == pushed.y,
			"the steep slope: the input does not steer, speed up or slow the ped");
	}

	/* (e) a zero target speed with input on the ground: the friction takes
	 *     the ped to rest, exactly, like the no-input stream */
	{
		CPlayerMoveSolver solver;
		SRefMoveState ref = { REF_PLAYERMOVE_GROUND, 0.0f };
		SMoveIntent intent = WalkIntent(0.0f, 0.0f);
		SMoveSnapshot got;

		intent.hasInput = true;
		intent.vel = SMoveVec2{ 0.33f, 0.44f };
		for (int i = 0; i < 200; i++) {
			intent.vel = got.moved;
			if (!StepParity(&solver, &ref, intent, &got))
				mismatches++;
		}
		Check(got.moved.x == 0.0f && got.moved.y == 0.0f,
			"a zero target speed: the ped comes to rest exactly, like no input");
	}

	Check(mismatches == 0, "the solver and the reference agree on the targeted states too");
}

/* ---------------------------------------------------------------------- *
 * 2a. the invariant, structurally
 * ---------------------------------------------------------------------- */

/*
 * The module compiles alone, and every undefined symbol of its object file
 * must come from the C library or the math library: a read of the engine (an
 * animation association, the clock, the pad, the camera) or of the C++ runtime
 * would add a symbol and fail the run. The planted translation unit below
 * reads an engine symbol of the shape an animation lock would add, and the
 * same listing must refuse it, so the list's teeth are checked on every run,
 * not only on the day the module was written.
 */

#define MODULE_SOURCE		"src/peds/PlayerMoveSolver.cpp"
#define MODULE_OBJECT		"/tmp/opencode/movement-solver-module.o"
#define PLANTED_SOURCE		"/tmp/opencode/movement-solver-planted.cpp"
#define PLANTED_OBJECT		"/tmp/opencode/movement-solver-planted.o"

/* The names the C library (libc) may contribute. */
static const char *aLibcSymbols[] = {
	"memset", "memcpy", "memmove", "memcmp", "abort",
	"__stack_chk_fail", "__stack_chk_guard",
};

/* The names the math library (libm) may contribute. The -O2 build folds the
 * cosf/sinf pair into glibc's sincosf, so that name is here too. */
static const char *aMathSymbols[] = {
	"sqrtf", "sinf", "cosf", "sincosf", "tanf", "atanf", "atan2f",
	"asinf", "acosf", "fabsf", "floorf", "ceilf", "truncf", "roundf",
	"powf", "expf", "logf", "fmodf",
};

static bool
NameInList(const char *name, const char *const *aList, int count)
{
	for (int i = 0; i < count; i++)
		if (strcmp(name, aList[i]) == 0)
			return true;
	return false;
}

static bool
SymbolIsAllowed(const char *name)
{
	return NameInList(name, aLibcSymbols, ARRAY_COUNT(aLibcSymbols)) ||
		NameInList(name, aMathSymbols, ARRAY_COUNT(aMathSymbols));
}

/* Run a shell command and read its output; false when it cannot run or exits
 * non-zero. */
static bool
RunCommand(const char *command, char *output, size_t outputSize)
{
	FILE *pipe = popen(command, "r");
	size_t used = 0;

	if (pipe == NULL)
		return false;
	if (outputSize > 0) {
		size_t n;
		while (used + 1 < outputSize && (n = fread(output + used, 1, outputSize - 1 - used, pipe)) > 0)
			used += n;
		output[used] = '\0';
	}
	return pclose(pipe) == 0;
}

static bool
CompileObject(const char *source, const char *object)
{
	char command[512];
	char output[512];

	snprintf(command, sizeof(command), "c++ -std=c++17 -O2 -c %s -o %s 2>&1", source, object);
	if (RunCommand(command, output, sizeof(output)))
		return true;
	printf("    the compile failed: %s\n", output);
	return false;
}

/* The undefined symbols of an object file, straight from nm. */
static int
UndefinedSymbols(const char *object, char names[][64], int maxNames)
{
	char command[512];
	char output[16384];
	char *line;
	int count = 0;

	snprintf(command, sizeof(command), "nm -u -P %s 2>&1", object);
	if (!RunCommand(command, output, sizeof(output)))
		return -1;
	for (line = strtok(output, "\n"); line != NULL; line = strtok(NULL, "\n")) {
		char name[64];
		char type;

		if (sscanf(line, "%63s %c", name, &type) == 2 && type == 'U' && count < maxNames)
			snprintf(names[count++], 64, "%s", name);
	}
	return count;
}

static void
CheckStructural(void)
{
	char names[32][64];
	char planted[32][64];
	bool bad = false;
	int count;
	int plantedCount;

	Check(CompileObject(MODULE_SOURCE, MODULE_OBJECT), "the module compiles alone to an object file");
	count = UndefinedSymbols(MODULE_OBJECT, names, ARRAY_COUNT(names));
	Check(count >= 0, "nm lists the module object's undefined symbols");
	if (count < 0)
		return;

	printf("  the module's undefined symbols (%d):", count);
	for (int i = 0; i < count; i++)
		printf(" %s", names[i]);
	printf("\n");
	for (int i = 0; i < count; i++) {
		if (!SymbolIsAllowed(names[i])) {
			bad = true;
			printf("    %s is not the C library or the math library\n", names[i]);
		}
	}
	Check(!bad, "every undefined symbol of the module comes from the C library or the math library");

	/* the planted read: an engine symbol the list must refuse */
	{
		FILE *file;

		remove(PLANTED_SOURCE);
		remove(PLANTED_OBJECT);
		file = fopen(PLANTED_SOURCE, "w");

		if (file != NULL) {
			fprintf(file,
				"/* planted by the VICE-172 harness: an engine read of the\n"
				" * shape an animation lock would add */\n"
				"namespace CTimer { float GetTimeStepInSeconds(void); }\n"
				"float PlantedEngineRead(float dt)\n"
				"{\n"
				"\treturn CTimer::GetTimeStepInSeconds() * dt;\n"
				"}\n");
			fclose(file);
		}
	}
	Check(CompileObject(PLANTED_SOURCE, PLANTED_OBJECT),
		"the planted engine read compiles to its own object file");
	plantedCount = UndefinedSymbols(PLANTED_OBJECT, planted, ARRAY_COUNT(planted));
	Check(plantedCount >= 0, "nm lists the planted object's undefined symbols");
	if (plantedCount < 0)
		return;

	{
		bool plantedSeen = false;
		bool plantedRefused = false;

		for (int i = 0; i < plantedCount; i++) {
			if (strstr(planted[i], "GetTimeStepInSeconds") != NULL) {
				plantedSeen = true;
				printf("  the planted engine read is %s: %s\n", planted[i],
					SymbolIsAllowed(planted[i])
						? "ALLOWED, the check would not fire"
						: "refused by the list, the check fires");
				if (!SymbolIsAllowed(planted[i]))
					plantedRefused = true;
			}
		}
		Check(plantedSeen, "the planted engine read appears among the undefined symbols");
		Check(plantedRefused, "the planted engine read is refused: the check fires on an engine dependency");
	}
}

/* ---------------------------------------------------------------------- *
 * 2b. the invariant, behaviourally
 * ---------------------------------------------------------------------- */

/*
 * The animation side of the adapter is modelled, not linked: the association
 * that is absent or frozen in the game is a struct the harness mutates
 * outside the intent, and the solver never sees it. The check drives walk
 * steps with the intent's target speed pinned and holds three claims:
 *
 *	- the association absent and the association present but frozen (its
 *	  clock stopped: the classic animation lock) give bit-identical series;
 *	- the pinned target is reached exactly, and the first step already
 *	  moves the ped (the input response does not wait for a cycle);
 *	- a moved goalpost - a second stream with another pinned target -
 *	  changes the series accordingly: the reached velocity is the second
 *	  target.
 */

#define BEHAVIOUR_STEPS	240

struct SAnimationModel
{
	bool present;		// the movement association exists
	bool frozen;		// its clock does not advance
	float progress;		// its own blend progress, read by nothing here
	float stubSpeed;	// what the reference speed read returns, engine units
};

static void
CheckBehavioural(void)
{
	static const float targetStub = 0.30f;		// the reference speed read of the absent association
	static const float targetGoalpost = 0.15f;	// the moved goalpost
	CPlayerMoveSolver solverAbsent, solverFrozen, solverGoalpost;
	SAnimationModel animAbsent = { false, false, 0.0f, targetStub };
	SAnimationModel animFrozen = { true, true, 0.0f, targetStub };
	SMoveVec2 liveAbsent = { 0.0f, 0.0f };
	SMoveVec2 liveFrozen = { 0.0f, 0.0f };
	SMoveVec2 liveGoalpost = { 0.0f, 0.0f };
	int diffsFrozen = 0;
	int diffsGoalpost = 0;

	for (int step = 0; step < BEHAVIOUR_STEPS; step++) {
		SMoveIntent intentAbsent, intentFrozen, intentGoalpost;

		// the association's own clock: the frozen one does not advance
		if (animAbsent.present)
			animAbsent.progress += 1.0f / 30.0f;
		if (animFrozen.present && !animFrozen.frozen)
			animFrozen.progress += 1.0f / 30.0f;

		intentAbsent = WalkIntent(0.0f, animAbsent.stubSpeed);
		intentAbsent.vel = liveAbsent;
		intentFrozen = WalkIntent(0.0f, animFrozen.stubSpeed);
		intentFrozen.vel = liveFrozen;
		intentGoalpost = WalkIntent(0.0f, targetGoalpost);
		intentGoalpost.vel = liveGoalpost;

		const SMoveSnapshot &snapshotAbsent = solverAbsent.Step(intentAbsent);
		const SMoveSnapshot &snapshotFrozen = solverFrozen.Step(intentFrozen);
		const SMoveSnapshot &snapshotGoalpost = solverGoalpost.Step(intentGoalpost);

		liveAbsent = snapshotAbsent.moved;
		liveFrozen = snapshotFrozen.moved;
		liveGoalpost = snapshotGoalpost.moved;

		if (!SameSnapshot(snapshotAbsent, snapshotFrozen))
			diffsFrozen++;
		if (!SameSnapshot(snapshotAbsent, snapshotGoalpost))
			diffsGoalpost++;
		if (step == 0) {
			Check(snapshotAbsent.speed > 0.0f,
				"the first step already moves the ped: the input response does not wait");
			Check(snapshotGoalpost.speed > 0.0f,
				"the first step already moves the ped with the moved goalpost");
		}
	}

	Check(diffsFrozen == 0, "the absent association and the frozen association give bit-identical series");
	Check(diffsGoalpost > 0, "the moved goalpost changes the series");
	Check(solverAbsent.GetSnapshot().moved.x == 0.0f &&
	      solverAbsent.GetSnapshot().moved.y == targetStub,
		"the solver reaches the pinned target exactly");
	Check(solverGoalpost.GetSnapshot().moved.x == 0.0f &&
	      solverGoalpost.GetSnapshot().moved.y == targetGoalpost,
		"the moved goalpost's own target is reached exactly");
	Check(solverAbsent.GetSnapshot().moved.y != solverGoalpost.GetSnapshot().moved.y,
		"the two streams end on different velocities, as the two targets ask");
	printf("  %d steps with the target pinned at %.9g: the absent and the frozen association agree,\n",
		BEHAVIOUR_STEPS, targetStub);
	printf("  the walk reaches the target exactly, and the goalpost at %.9g reaches that instead\n",
		targetGoalpost);
}

/* ---------------------------------------------------------------------- *
 * 3. the determinism
 * ---------------------------------------------------------------------- */

#define DETERMINISM_STEPS	4096

static SMoveIntent g_streamA[DETERMINISM_STEPS];
static SMoveIntent g_streamB[DETERMINISM_STEPS];
static SMoveSnapshot g_seriesA[DETERMINISM_STEPS];
static SMoveSnapshot g_seriesB[DETERMINISM_STEPS];

static void
CheckDeterminism(void)
{
	int diffs = 0;

	// the streams are drawn once, at two fixed seeds, and replayed below
	SetStreamSeed(STREAM_SEED ^ 0xA5A5A5A5A5A5A5A5ull);
	for (int i = 0; i < DETERMINISM_STEPS; i++)
		g_streamA[i] = MakeIntent();
	SetStreamSeed(STREAM_SEED ^ 0x5A5A5A5A5A5A5A5Aull);
	for (int i = 0; i < DETERMINISM_STEPS; i++)
		g_streamB[i] = MakeIntent();

	/* (a) the same stream twice */
	{
		CPlayerMoveSolver first, second;

		for (int i = 0; i < DETERMINISM_STEPS; i++) {
			const SMoveSnapshot &a = first.Step(g_streamA[i]);
			const SMoveSnapshot &b = second.Step(g_streamA[i]);

			if (!SameSnapshot(a, b))
				diffs++;
			g_seriesA[i] = a;
		}
		Check(diffs == 0, "the same intent stream twice gives identical snapshots, field for field");
		Check(SameSnapshot(second.GetSnapshot(), g_seriesA[DETERMINISM_STEPS - 1]),
			"GetSnapshot hands out the snapshot the last step returned");
	}

	/* (b) two solvers interleaved equal two stepped apart */
	{
		CPlayerMoveSolver interA, interB;
		CPlayerMoveSolver soloA, soloB;

		for (int i = 0; i < DETERMINISM_STEPS; i++) {
			g_seriesA[i] = interA.Step(g_streamA[i]);
			g_seriesB[i] = interB.Step(g_streamB[i]);
		}
		diffs = 0;
		for (int i = 0; i < DETERMINISM_STEPS; i++) {
			if (!SameSnapshot(soloA.Step(g_streamA[i]), g_seriesA[i]))
				diffs++;
		}
		for (int i = 0; i < DETERMINISM_STEPS; i++) {
			if (!SameSnapshot(soloB.Step(g_streamB[i]), g_seriesB[i]))
				diffs++;
		}
		Check(diffs == 0, "two solvers interleaved equal the two stepped apart, field for field");
	}

	printf("  %d steps a stream, twice: identical; the interleaved pair equals the two apart,\n",
		DETERMINISM_STEPS);
	printf("  so the solver keeps its whole state on the instance\n");
}

/* ---------------------------------------------------------------------- *
 * 4. the tables
 * ---------------------------------------------------------------------- */

/*
 * VICE-171 (movement/p0-tables) exported PlayerMoveTablesCheck; VICE-180 grew
 * the slide's rules. The check drives the module's own tables and the harness's
 * copy - the module's numbers, in a set a defect can be planted in - so every
 * rejection the check claims can be driven. PlantSet rebuilds every field, so
 * each case starts clean.
 */
static tMoveStateRow g_plantStates[MOVE_NUMSTATES];
static tMoveLadderRow g_plantLadder[TIER_NUM];
static tMoveStanceRow g_plantStances[STANCE_NUM];
static tMoveTransitionRow g_plantTransitions[8];
static tMoveStanceTransitionRow g_plantStanceTransitions[17];

/* The module's own slide, prone and dive numbers (PlayerMoveSolver.cpp), so
 * the planted set is a copy of the real one: the two speeds the slide's guards
 * name, the prone row - the crawl as a share of the walk, the half turn and
 * the signed capsule top of the set that hangs below the origin (VICE-177) -
 * and the dive's own acceleration and turn rate (VICE-181). */
#define PLANT_SLIDE_ENTRY_SPEED	0.18f
#define PLANT_SLIDE_EXIT_SPEED	0.13f
#define PRONE_SPEED_SCALE	0.4f
#define PRONE_TURN_SCALE	0.5f
#define PRONE_CAPSULE_HEIGHT	-0.44f
#define PLANT_DIVE_ACCEL	(2.0f / 50.0f)
#define PLANT_DIVE_TURN_RATE	3.2f

static SMoveTables
PlantSet(void)
{
	SMoveTables t;
	uint32_t anyState = (1u << MOVE_NUMSTATES) - 1u;

	for (int i = 0; i < MOVE_NUMSTATES; i++) {
		g_plantStates[i].accel = (i == MOVE_GROUND) ? 20.0f / 50.0f : 4.0f / 50.0f;
		g_plantStates[i].friction = (i == MOVE_GROUND) ? 20.0f / 50.0f : 0.0f;
		g_plantStates[i].turnRate = (i == MOVE_GROUND) ? 12.0f : 1.6f;
		g_plantStates[i].entryBoost = 0.0f;
		g_plantStates[i].boostDecay = 0.0f;
		g_plantStates[i].inputScale = 1.0f;
		g_plantStates[i].flags = MOVEFLAG_STEER_INPUT | ((i == MOVE_AIR) ? MOVEFLAG_KEEP_MOMENTUM : 0u);
	}
	/* the slide's row as the module carries it */
	g_plantStates[MOVE_SLIDE].accel = 2.0f / 50.0f;
	g_plantStates[MOVE_SLIDE].friction = 8.0f / 50.0f;
	g_plantStates[MOVE_SLIDE].turnRate = 16.0f;
	g_plantStates[MOVE_SLIDE].entryBoost = 2.0f / 50.0f;
	g_plantStates[MOVE_SLIDE].boostDecay = 8.0f / 50.0f;
	g_plantStates[MOVE_SLIDE].inputScale = 0.0f;
	g_plantStates[MOVE_SLIDE].flags = MOVEFLAG_STEER_INPUT;

	/* the dive's row as the module carries it (VICE-181): the low air control,
	 * no friction (the row keeps its momentum; the slide's curve does the
	 * bleeding on the landing), twice the air's turn rate and no boost */
	g_plantStates[MOVE_DIVE].accel = PLANT_DIVE_ACCEL;
	g_plantStates[MOVE_DIVE].friction = 0.0f;
	g_plantStates[MOVE_DIVE].turnRate = PLANT_DIVE_TURN_RATE;
	g_plantStates[MOVE_DIVE].entryBoost = 0.0f;
	g_plantStates[MOVE_DIVE].boostDecay = 0.0f;
	g_plantStates[MOVE_DIVE].inputScale = 1.0f;
	g_plantStates[MOVE_DIVE].flags = MOVEFLAG_STEER_INPUT | MOVEFLAG_KEEP_MOMENTUM;

	for (int i = 0; i < TIER_NUM; i++) {
		g_plantLadder[i].tier = (eMoveTier)i;
		g_plantLadder[i].stance = STANCE_STAND;
		g_plantLadder[i].stateMask = anyState;
		g_plantLadder[i].speedScale = 1.0f + (float)i;
		g_plantLadder[i].reportMoveState = MOVE_REPORT_WALK;
	}
	for (int i = 0; i < STANCE_NUM; i++) {
		g_plantStances[i].speedScale = 1.0f;
		g_plantStances[i].turnScale = 1.0f;
		g_plantStances[i].capsuleHeight = 0.0f;
	}
	/* the stance rows as the module carries them: the crouch's own numbers and
	 * the prone's signed top (VICE-176, VICE-177) */
	g_plantStances[STANCE_CROUCH].speedScale = 0.5f;
	g_plantStances[STANCE_CROUCH].turnScale = 1.25f;
	g_plantStances[STANCE_CROUCH].capsuleHeight = 0.5f;
	g_plantStances[STANCE_PRONE].speedScale = PRONE_SPEED_SCALE;
	g_plantStances[STANCE_PRONE].turnScale = PRONE_TURN_SCALE;
	g_plantStances[STANCE_PRONE].capsuleHeight = PRONE_CAPSULE_HEIGHT;
	g_plantTransitions[0].from = MOVE_GROUND; g_plantTransitions[0].trigger = TRIGGER_AIRBORNE;
	g_plantTransitions[0].to = MOVE_AIR; g_plantTransitions[0].onEntryEvent = 0;
	g_plantTransitions[0].guard = GUARD_NONE; g_plantTransitions[0].guardSpeed = 0.0f;
	g_plantTransitions[1].from = MOVE_AIR; g_plantTransitions[1].trigger = TRIGGER_LAND;
	g_plantTransitions[1].to = MOVE_GROUND; g_plantTransitions[1].onEntryEvent = EVENT_LANDED;
	g_plantTransitions[1].guard = GUARD_NONE; g_plantTransitions[1].guardSpeed = 0.0f;
	g_plantTransitions[2].from = MOVE_GROUND; g_plantTransitions[2].trigger = TRIGGER_JUMP;
	g_plantTransitions[2].to = MOVE_AIR; g_plantTransitions[2].onEntryEvent = EVENT_JUMP;
	g_plantTransitions[2].guard = GUARD_NONE; g_plantTransitions[2].guardSpeed = 0.0f;
	g_plantTransitions[3].from = MOVE_AIR; g_plantTransitions[3].trigger = TRIGGER_DIVE;
	g_plantTransitions[3].to = MOVE_DIVE; g_plantTransitions[3].onEntryEvent = 0;
	g_plantTransitions[3].guard = GUARD_NONE; g_plantTransitions[3].guardSpeed = 0.0f;
	/* the slide's rows: the entry, the jump, the prone request and the exit */
	g_plantTransitions[4].from = MOVE_GROUND; g_plantTransitions[4].trigger = TRIGGER_SLIDE;
	g_plantTransitions[4].to = MOVE_SLIDE; g_plantTransitions[4].onEntryEvent = 0;
	g_plantTransitions[4].guard = GUARD_SPEED_ABOVE; g_plantTransitions[4].guardSpeed = PLANT_SLIDE_ENTRY_SPEED;
	g_plantTransitions[5].from = MOVE_SLIDE; g_plantTransitions[5].trigger = TRIGGER_JUMP;
	g_plantTransitions[5].to = MOVE_AIR; g_plantTransitions[5].onEntryEvent = EVENT_JUMP;
	g_plantTransitions[5].guard = GUARD_NONE; g_plantTransitions[5].guardSpeed = 0.0f;
	g_plantTransitions[6].from = MOVE_SLIDE; g_plantTransitions[6].trigger = TRIGGER_LAND;
	g_plantTransitions[6].to = MOVE_GROUND; g_plantTransitions[6].onEntryEvent = 0;
	g_plantTransitions[6].guard = GUARD_SPEED_BELOW; g_plantTransitions[6].guardSpeed = PLANT_SLIDE_EXIT_SPEED;
	/* the dive's landing row (VICE-181): the entry row of the air above is the
	 * planted set's own (index 3), and this row lands the dive in the
	 * slide-out with the speed carried, no guard and no reset */
	g_plantTransitions[7].from = MOVE_DIVE; g_plantTransitions[7].trigger = TRIGGER_LAND;
	g_plantTransitions[7].to = MOVE_SLIDE; g_plantTransitions[7].onEntryEvent = EVENT_LANDED;
	g_plantTransitions[7].guard = GUARD_NONE; g_plantTransitions[7].guardSpeed = 0.0f;

	/* the stance moves of the module's own table (VICE-176, VICE-177,
	 * VICE-181), in its order: the dive's touchdown rows first in the stand's
	 * and the crouch's groups, the sprint's own gate row, the prone's entries
	 * and exits and the headroom refusals included */
	g_plantStanceTransitions[0].from = STANCE_STAND; g_plantStanceTransitions[0].trigger = TRIGGER_DIVE_LAND;
	g_plantStanceTransitions[0].to = STANCE_PRONE;
	g_plantStanceTransitions[1].from = STANCE_STAND; g_plantStanceTransitions[1].trigger = TRIGGER_SPRINT;
	g_plantStanceTransitions[1].to = STANCE_STAND;
	g_plantStanceTransitions[2].from = STANCE_STAND; g_plantStanceTransitions[2].trigger = TRIGGER_CROUCH;
	g_plantStanceTransitions[2].to = STANCE_CROUCH;
	g_plantStanceTransitions[3].from = STANCE_STAND; g_plantStanceTransitions[3].trigger = TRIGGER_PRONE;
	g_plantStanceTransitions[3].to = STANCE_PRONE;
	g_plantStanceTransitions[4].from = STANCE_CROUCH; g_plantStanceTransitions[4].trigger = TRIGGER_DIVE_LAND;
	g_plantStanceTransitions[4].to = STANCE_PRONE;
	g_plantStanceTransitions[5].from = STANCE_CROUCH; g_plantStanceTransitions[5].trigger = TRIGGER_HEADROOM_BLOCKED;
	g_plantStanceTransitions[5].to = STANCE_CROUCH;
	g_plantStanceTransitions[6].from = STANCE_CROUCH; g_plantStanceTransitions[6].trigger = TRIGGER_CROUCH;
	g_plantStanceTransitions[6].to = STANCE_STAND;
	g_plantStanceTransitions[7].from = STANCE_CROUCH; g_plantStanceTransitions[7].trigger = TRIGGER_SPRINT;
	g_plantStanceTransitions[7].to = STANCE_STAND;
	g_plantStanceTransitions[8].from = STANCE_CROUCH; g_plantStanceTransitions[8].trigger = TRIGGER_JUMP;
	g_plantStanceTransitions[8].to = STANCE_STAND;
	g_plantStanceTransitions[9].from = STANCE_CROUCH; g_plantStanceTransitions[9].trigger = TRIGGER_EXIT_VEHICLE;
	g_plantStanceTransitions[9].to = STANCE_STAND;
	g_plantStanceTransitions[10].from = STANCE_CROUCH; g_plantStanceTransitions[10].trigger = TRIGGER_PRONE;
	g_plantStanceTransitions[10].to = STANCE_PRONE;
	g_plantStanceTransitions[11].from = STANCE_PRONE; g_plantStanceTransitions[11].trigger = TRIGGER_HEADROOM_BLOCKED;
	g_plantStanceTransitions[11].to = STANCE_PRONE;
	g_plantStanceTransitions[12].from = STANCE_PRONE; g_plantStanceTransitions[12].trigger = TRIGGER_PRONE;
	g_plantStanceTransitions[12].to = STANCE_STAND;
	g_plantStanceTransitions[13].from = STANCE_PRONE; g_plantStanceTransitions[13].trigger = TRIGGER_CROUCH;
	g_plantStanceTransitions[13].to = STANCE_CROUCH;
	g_plantStanceTransitions[14].from = STANCE_PRONE; g_plantStanceTransitions[14].trigger = TRIGGER_SPRINT;
	g_plantStanceTransitions[14].to = STANCE_STAND;
	g_plantStanceTransitions[15].from = STANCE_PRONE; g_plantStanceTransitions[15].trigger = TRIGGER_JUMP;
	g_plantStanceTransitions[15].to = STANCE_STAND;
	g_plantStanceTransitions[16].from = STANCE_PRONE; g_plantStanceTransitions[16].trigger = TRIGGER_EXIT_VEHICLE;
	g_plantStanceTransitions[16].to = STANCE_STAND;

	t.states = g_plantStates; t.numStates = MOVE_NUMSTATES;
	t.ladder = g_plantLadder; t.numLadder = TIER_NUM;
	t.stances = g_plantStances; t.numStances = STANCE_NUM;
	t.transitions = g_plantTransitions; t.numTransitions = 8;
	t.stanceTransitions = g_plantStanceTransitions; t.numStanceTransitions = 17;
	return t;
}

static void
CheckTables(void)
{
	int seen[MOVE_NUMSTATES] = { 0 };
	int bad = 0;
	int offStance = 0;
	CPlayerMoveSolver solver;

	Check(MOVE_GROUND != MOVE_AIR && MOVE_AIR != MOVE_SLIDE && MOVE_GROUND != MOVE_SLIDE &&
	      MOVE_SLIDE != MOVE_DIVE && MOVE_GROUND != MOVE_DIVE && MOVE_AIR != MOVE_DIVE,
		"the design's four movement states are named and distinct");
	Check(STANCE_STAND != STANCE_CROUCH && STANCE_CROUCH != STANCE_PRONE &&
	      STANCE_STAND != STANCE_PRONE,
		"the design's three stances are named and distinct");

	SetStreamSeed(STREAM_SEED ^ 0x7AB1E5ull);
	for (int i = 0; i < 4000; i++) {
		SMoveIntent intent = MakeIntent();
		const SMoveSnapshot &snapshot = solver.Step(intent);

		if ((unsigned)snapshot.state >= MOVE_NUMSTATES) {
			bad++;
			continue;
		}
		seen[snapshot.state]++;
		if (snapshot.stance != STANCE_STAND)
			offStance++;
	}
	Check(seen[MOVE_GROUND] > 0 && seen[MOVE_AIR] > 0,
		"the phase 0 stream uses the ground and the air");
	Check(seen[MOVE_SLIDE] == 0 && seen[MOVE_DIVE] == 0,
		"the parity stream enters neither the slide (its pair is off, VICE-180), the dive (its edge is off, VICE-181) nor the jump (its edge is off, VICE-182)");
	Check(bad == 0 && offStance == 0,
		"the state is always one of the four and the stance is the one of phase 0");
	printf("  the states of the sweep: ground %d, air %d, slide %d, dive %d\n",
		seen[MOVE_GROUND], seen[MOVE_AIR], seen[MOVE_SLIDE], seen[MOVE_DIVE]);

	/* The module's tables, then the planted set (VICE-171's SMoveTables
	 * overload): every rejection the check claims is driven with a defect. */
	{
		const char *why = NULL;
		SMoveTables t = PlantSet();

		Check(PlayerMoveTablesCheck(&why), "the module's own tables hold every invariant");
		Check(PlayerMoveTablesCheck(t, &why), "the planted set holds before a defect is planted");

		#define REJECTS(plant, text) do { \
			SMoveTables broken = PlantSet(); \
			plant; \
			Check(!PlayerMoveTablesCheck(broken, &why), text); \
		} while (0)

		REJECTS(broken.numStates = MOVE_NUMSTATES - 1,
			"a state without a row is refused");
		REJECTS(g_plantStates[MOVE_AIR].friction = -1.0f,
			"a state row out of range is refused");
		REJECTS(broken.numLadder = TIER_NUM - 1,
			"a ladder without a rung is refused");
		REJECTS(g_plantLadder[TIER_RUN].tier = TIER_WALK,
			"a ladder row out of order is refused");
		REJECTS(g_plantLadder[TIER_SPRINT].speedScale = g_plantLadder[TIER_RUN].speedScale,
			"a ladder row that does not ascend is refused");
		REJECTS(g_plantLadder[TIER_WALK].stateMask = 1u << MOVE_AIR,
			"a state without a rung is refused");
		REJECTS(g_plantLadder[TIER_SPRINT].speedScale = g_plantLadder[TIER_SLIDE_ENTRY].speedScale,
			"a slide rung at or below the sprint's is refused");
		REJECTS(broken.numStances = STANCE_NUM - 1,
			"a stance without a row is refused");
		REJECTS(g_plantStances[STANCE_CROUCH].turnScale = -1.0f,
			"a stance row out of range is refused");
		REJECTS(g_plantStances[STANCE_PRONE].capsuleHeight = -1.5f,
			"a stance row whose top is under the ground is refused");
		REJECTS(g_plantTransitions[3].to = MOVE_NUMSTATES,
			"a transition to a state out of range is refused");
		REJECTS(g_plantTransitions[3].trigger = 0,
			"a transition trigger out of range is refused");
		REJECTS(g_plantTransitions[0].guard = GUARD_NUM,
			"a transition guard out of range is refused");
		REJECTS(g_plantTransitions[0].guardSpeed = 1.0f,
			"an unguarded transition with a threshold is refused");
		REJECTS(g_plantTransitions[4].guardSpeed = 0.0f,
			"a guarded transition without its threshold is refused");
		REJECTS(g_plantTransitions[4].to = MOVE_GROUND,
			"a slide without an entry row is refused");
		REJECTS(g_plantTransitions[6].guard = GUARD_NONE; g_plantTransitions[6].guardSpeed = 0.0f,
			"a slide exit without the exit speed is refused");
		REJECTS(g_plantTransitions[6].guardSpeed = PLANT_SLIDE_ENTRY_SPEED,
			"a slide exit speed at or above its entry speed is refused");
		REJECTS(g_plantTransitions[3].to = MOVE_GROUND,
			"a dive without its entry row is refused");
		REJECTS(g_plantTransitions[7].trigger = TRIGGER_AIRBORNE,
			"a dive without its landing row is refused");
		REJECTS(g_plantTransitions[7].to = MOVE_GROUND,
			"a dive whose landing is not the slide-out is refused");
		REJECTS(g_plantTransitions[2].to = MOVE_GROUND,
			"a ground jump that does not leave for the air is refused");
		REJECTS(g_plantTransitions[2].onEntryEvent = 0,
			"a ground jump that raises no jump event is refused");
		REJECTS(g_plantTransitions[2].from = MOVE_AIR,
			"a jump without its ground row is refused");
		REJECTS(g_plantStanceTransitions[3].to = STANCE_NUM,
			"a stance move to a stance out of range is refused");
		REJECTS(g_plantStanceTransitions[3].trigger = 0,
			"a stance move trigger out of range is refused");
		REJECTS(g_plantStanceTransitions[0].to = STANCE_STAND,
			"a dive whose landing does not enter the prone is refused");
	}
}

/* ---------------------------------------------------------------------- *
 * 5. the events
 * ---------------------------------------------------------------------- */

#define EVENT_STREAM_STEPS	60

/*
 * The event bits are VICE-173's list and the transition rows raise them: a
 * landing raises EVENT_LANDED once, the dive's landing raises it once too
 * (VICE-181, section 10), and the ground's jump raises EVENT_JUMP once
 * (VICE-182, section 11). No other bit fires: the wall and the head bits wait
 * for the tests that read them.
 */
static void
CheckEvents(void)
{
	uint32_t events[4];
	bool distinct = true;

	events[0] = EVENT_LANDED;
	events[1] = EVENT_WALL_CONTACT;
	events[2] = EVENT_WALL_LEFT;
	events[3] = EVENT_HEAD_HIT;
	for (int i = 0; i < 4; i++)
		for (int j = i + 1; j < 4; j++)
			if (events[i] == events[j] || (events[i] & events[j]) != 0)
				distinct = false;
	Check(distinct, "the design's four event bits are named and distinct");

	/* ground -> air -> ground, three landings in the stream */
	{
		CPlayerMoveSolver solver;
		bool wasGround = true;
		int landings = 0;
		int landingsWithTheBit = 0;
		int stateBad = 0;
		uint32_t otherBits = 0;

		for (int i = 0; i < EVENT_STREAM_STEPS; i++) {
			bool ground = !(i % 20 >= 6 && i % 20 < 10);
			SMoveIntent intent = WalkIntent(0.5f, 0.3f);

			if (ground && !wasGround)
				landings++;
			wasGround = ground;
			intent.onGround = ground;
			intent.hasInput = (i % 3) != 2;
			const SMoveSnapshot &snapshot = solver.Step(intent);
			if (snapshot.events & EVENT_LANDED)
				landingsWithTheBit++;
			otherBits |= snapshot.events & ~(uint32_t)EVENT_LANDED;
			if (snapshot.state != (ground ? MOVE_GROUND : MOVE_AIR))
				stateBad++;
		}
		Check(landings == 3, "the stream holds three landings");
		Check(stateBad == 0, "the state follows the ground and the air every step");
		Check(landingsWithTheBit == landings,
			"every landing raises the landing event exactly once");
		Check(otherBits == 0, "no other event bit is raised in phase 0");
	}

	/* The jump's own bit (VICE-182): the ground's press raises it on the step
	 * that leaves the ground, a press in the air raises nothing (the double
	 * jump is a later ticket), and the jump's landing raises the landing bit
	 * once. The full jump story is section 11. */
	{
		CPlayerMoveSolver solver;
		SMoveVec2 live = { 0.0f, 0.2f };
		int jumpEvents = 0;
		int airPressEvents = 0;
		int landings = 0;

		for (int i = 0; i < 30; i++) {
			SMoveIntent intent = WalkIntent(0.0f, 0.3f);

			intent.vel = live;
			intent.onGround = i <= 3 || i >= 22;
			intent.jumpRequested = (i == 3) || (i == 10);	/* the second press is in the air */
			const SMoveSnapshot &snapshot = solver.Step(intent);
			if (snapshot.events & EVENT_JUMP)
				jumpEvents++;
			if (i == 10 && (snapshot.events & EVENT_JUMP))
				airPressEvents++;
			if (snapshot.events & EVENT_LANDED)
				landings++;
			live = snapshot.moved;
		}
		Check(jumpEvents == 1, "the ground's press raises the jump event exactly once");
		Check(airPressEvents == 0, "a press in the air raises no jump event: no second launch");
		Check(landings == 1, "the jump's landing raises the landing event once");
	}

	/* the dive's edge is an air action (VICE-181): with the feet on the ground
	 * it changes nothing, and its own entry from the air is section 10's */
	{
		CPlayerMoveSolver quiet, edged;
		SMoveVec2 liveQuiet = { 0.7f, 0.2f };
		SMoveVec2 liveEdged = { 0.7f, 0.2f };
		int edgeDiffs = 0;

		for (int i = 0; i < EVENT_STREAM_STEPS; i++) {
			SMoveIntent intentQuiet = WalkIntent(0.25f, 0.3f);
			SMoveIntent intentEdged;

			intentQuiet.vel = liveQuiet;
			intentEdged = intentQuiet;
			intentEdged.vel = liveEdged;
			intentEdged.diveRequested = true;
			const SMoveSnapshot &snapshotQuiet = quiet.Step(intentQuiet);
			const SMoveSnapshot &snapshotEdged = edged.Step(intentEdged);
			liveQuiet = snapshotQuiet.moved;
			liveEdged = snapshotEdged.moved;
			if (!SameSnapshot(snapshotQuiet, snapshotEdged))
				edgeDiffs++;
		}
		Check(edgeDiffs == 0, "the dive's edge with the feet on the ground changes no output: the dive is an air action");
	}

	printf("  every landing raises the landing bit exactly once, the ground's press raises the jump\n");
	printf("  bit once and the air's press raises nothing; the dive's edge is an air action\n");
}

/* ---------------------------------------------------------------------- *
 * 6. the finiteness
 * ---------------------------------------------------------------------- */

#define FINITE_STEPS	100000

static void
CheckFiniteness(void)
{
	static const float aDt[3] = { 0.0f, 1.0f / 50.0f, 1.0f / 10.0f };
	static const float aTarget[4] = { 0.0f, 2.0f / 60.0f, 2.0f, 0.3f };
	CPlayerMoveSolver solver;
	int bad = 0;

	SetStreamSeed(STREAM_SEED ^ 0xF1E17Eull);
	for (int slice = 0; slice < (int)ARRAY_COUNT(aDt); slice++) {
		for (int i = 0; i < FINITE_STEPS; i++) {
			SMoveIntent intent = MakeIntent();

			intent.dt = aDt[slice];
			intent.targetSpeed = aTarget[i % (int)ARRAY_COUNT(aTarget)];
			// the pad's extreme and, every fourth step, the zero input
			intent.hasInput = (i % 4) != 3;
			intent.vel = SMoveVec2{ RngFloat(-10.0f, 10.0f), RngFloat(-10.0f, 10.0f) };
			// the slide's entry pair and the prone request, so the slide's
			// own outputs and the low stances' rows are covered too
			// (VICE-180, VICE-176, VICE-177), the dive's own edge as well
			// (VICE-181) and the jump's (VICE-182): the dive, its slide-out
			// and the jump's air are driven here
			intent.tier = (i % 4) == 0 ? TIER_SPRINT : intent.tier;
			intent.crouchRequested = (i % 4) == 0;
			intent.proneRequested = (i % 8) == 0;
			intent.diveRequested = (i % 8) == 4;
			intent.jumpRequested = (i % 8) == 2;
			const SMoveSnapshot &snapshot = solver.Step(intent);
			if (!SnapshotIsFinite(snapshot)) {
				if (bad < 3) {
					PrintIntent(intent);
					PrintSnapshot("non-finite", snapshot);
				}
				bad++;
			}
		}
	}

	Check(bad == 0, "every output is finite for dt {0, 1/50, 1/10}, the pad's extreme and the zero input");
	printf("  %d steps: dt {0, 1/50, 1/10} x the targets {0, 2/60, 2, 0.3} x the input and its\n",
		(int)ARRAY_COUNT(aDt) * FINITE_STEPS);
	printf("  absence, over the ground, the air, the steep slope, the slide, the crouch, the prone,\n");
	printf("  the dive and the jump: every velocity, moved, speed, heading, turn scale and capsule finite\n");
}

/* ---------------------------------------------------------------------- *
 * 7. the slide (VICE-180)
 * ---------------------------------------------------------------------- */

/*
 * The slide's own checks, beside the parity sweep and never inside it: the
 * reference maths of VICE-172 has no slide, so MakeIntent leaves the crouch
 * edge off and the slide is driven here by hand. The four claims the design
 * names, plus its exits:
 *
 *	* the entry: a sprint holds the shipped sprint_civi cycle's own speed
 *	  (14.668 units per second, 0.29336 engine units), and the crouch edge
 *	  enters the slide in that same frame with the entry boost, so the first
 *	  slide frame is above the sprint's speed;
 *	* the decay: the slide's speed never rises while it runs over flat ground;
 *	* the exit: the slide ends below the exit speed (0.13 engine units, the
 *	  run's own 6.39 units per second) and runs on at or above it, and the
 *	  entry refuses below the entry speed (0.18): the two thresholds differ,
 *	  which is the hysteresis the design asks for;
 *	* the jump: the jump leaves the slide for the air, the air keeps the
 *	  slide's speed and a slide jump is faster than the sprint's own speed;
 *	* the other exits: a prone request ends the slide on the ground (VICE-177
 *	  owns the prone stance), and the feet leaving the surface end it into the
 *	  fall with the speed it had.
 *
 * The tier follows the state: the slide names the ladder's entry rung even
 * though the pad still holds the sprint.
 */

#define SLIDE_SPRINT_SPEED	(14.668f / MOVE_ENGINE_UNITS_PER_SECOND)
#define SLIDE_ENTRY_SPEED	0.18f
#define SLIDE_EXIT_SPEED	0.13f

/* One step into the slide: the sprint's speed, the sprint tier and the crouch
 * edge, the entry pair in one frame. */
static SMoveSnapshot
StepIntoSlide(CPlayerMoveSolver *pSolver)
{
	SMoveIntent intent = WalkIntent(0.0f, SLIDE_SPRINT_SPEED);

	intent.tier = TIER_SPRINT;
	intent.crouchRequested = true;
	intent.vel = SMoveVec2{ 0.0f, SLIDE_SPRINT_SPEED };
	return pSolver->Step(intent);
}

/* One slide frame at a given own speed, the input held and the crouch edge off,
 * so nothing re-enters the slide. */
static SMoveSnapshot
StepSlideAt(CPlayerMoveSolver *pSolver, float speed)
{
	SMoveIntent intent = WalkIntent(0.0f, SLIDE_SPRINT_SPEED);

	intent.tier = TIER_SPRINT;
	intent.vel = SMoveVec2{ 0.0f, speed };
	return pSolver->Step(intent);
}

static void
CheckSlide(void)
{
	/* (a) the entry: the sprint settles on the shipped sprint cycle's own
	 *     speed, the crouch edge enters the slide in that frame, and the entry
	 *     boost puts the slide's first frame above the sprint's speed. */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, SLIDE_SPRINT_SPEED);
		SMoveSnapshot snapshot = solver.Step(intent);
		float sprintSpeed;

		intent.tier = TIER_SPRINT;
		for (int i = 0; i < 200; i++) {
			intent.vel = snapshot.moved;
			snapshot = solver.Step(intent);
		}
		sprintSpeed = snapshot.speed;
		Check(snapshot.state == MOVE_GROUND && fabsf(sprintSpeed - SLIDE_SPRINT_SPEED) < 1e-6f,
			"the sprint settles on the shipped sprint cycle's own speed");

		intent.crouchRequested = true;
		intent.vel = snapshot.moved;
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_SLIDE,
			"the sprint plus the crouch edge enters the slide in one frame");
		Check(snapshot.speed > sprintSpeed,
			"the entry boost puts the slide's first frame above the sprint's speed");
		Check(snapshot.tier == TIER_SLIDE_ENTRY,
			"the slide reports the ladder's entry rung, not the pad's sprint");
		Check(snapshot.reportMoveState == MOVE_REPORT_SPRINT,
			"the slide's rung reports the sprint movement state");
		printf("  the sprint holds %.5f engine units; the slide's first frame holds %.5f\n",
			sprintSpeed, snapshot.speed);
	}

	/* (b) the decay: the speed never rises over flat ground, every frame the
	 *     guard keeps in the slide runs at or above the exit speed, and the
	 *     step that ends it runs below that speed. */
	{
		CPlayerMoveSolver solver;
		SMoveSnapshot snapshot = StepIntoSlide(&solver);
		float previous = snapshot.speed;
		float guardIn = 0.0f;	// the speed the guard of the last frame kept in the slide
		float exitIn = 0.0f;	// the speed the guard of the frame that ended it read
		int rises = 0;
		int frames = 0;

		while (frames < 600 && snapshot.state == MOVE_SLIDE) {
			float own = snapshot.speed;

			snapshot = StepSlideAt(&solver, snapshot.moved.y);
			frames++;
			if (snapshot.state != MOVE_SLIDE) {
				exitIn = own;
				break;
			}
			if (snapshot.speed > previous + 1e-6f)
				rises++;
			previous = snapshot.speed;
			guardIn = own;
		}
		Check(frames < 600, "the slide reaches its exit: it does not run forever");
		Check(snapshot.state == MOVE_GROUND, "the slide ends on the ground");
		Check(rises == 0, "the slide's speed decays monotonically over flat ground");
		Check(guardIn >= SLIDE_EXIT_SPEED,
			"every frame the slide keeps runs at or above the exit speed");
		Check(exitIn < SLIDE_EXIT_SPEED,
			"the exit fires on a step below the exit speed and not above it");
		printf("  the slide ran %d frames and ended on a step of %.5f, below the exit's %.2f\n",
			frames, exitIn, SLIDE_EXIT_SPEED);
	}

	/* (c) the two thresholds: the exit fires strictly below its speed, and the
	 *     entry refuses below its own: the hysteresis band between them keeps
	 *     a ped from flapping in and out of the slide. */
	{
		CPlayerMoveSolver solver;
		SMoveSnapshot snapshot = StepIntoSlide(&solver);

		Check(snapshot.state == MOVE_SLIDE, "the entry at the sprint's speed starts the slide");
		snapshot = StepSlideAt(&solver, SLIDE_EXIT_SPEED + 0.01f);
		Check(snapshot.state == MOVE_SLIDE, "a slide above the exit speed runs on");
		snapshot = StepSlideAt(&solver, SLIDE_EXIT_SPEED);
		Check(snapshot.state == MOVE_SLIDE, "a slide at the exit speed runs on: the guard reads below it");
		snapshot = StepSlideAt(&solver, SLIDE_EXIT_SPEED - 0.01f);
		Check(snapshot.state == MOVE_GROUND, "a slide below the exit speed ends on the ground");
		Check(snapshot.stance == STANCE_CROUCH,
			"the slide's natural end leaves the ped in the crouch (VICE-192)");

		// the entry refuses inside the hysteresis band, and at any tier but
		// the sprint's
		{
			SMoveIntent intent = WalkIntent(0.0f, SLIDE_SPRINT_SPEED);

			intent.tier = TIER_SPRINT;
			intent.crouchRequested = true;
			intent.vel = SMoveVec2{ 0.0f, SLIDE_ENTRY_SPEED - 0.01f };
			snapshot = solver.Step(intent);
			Check(snapshot.state == MOVE_GROUND,
				"the crouch edge below the entry speed does not start the slide (the hysteresis)");

			intent.vel = SMoveVec2{ 0.0f, SLIDE_SPRINT_SPEED };
			intent.tier = TIER_RUN;
			snapshot = solver.Step(intent);
			Check(snapshot.state == MOVE_GROUND,
				"the crouch edge at the run tier does not start the slide: the pair needs the sprint");
		}
	}

	/* (d) the slide jump: the transition leaves for the air, no row resets the
	 *     velocity, the air keeps the slide's speed, and the jump is faster
	 *     than the sprint's own speed. */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, SLIDE_SPRINT_SPEED);
		SMoveSnapshot snapshot;
		float slideSpeed;

		snapshot = StepIntoSlide(&solver);
		snapshot = StepSlideAt(&solver, snapshot.moved.y);
		slideSpeed = snapshot.speed;
		Check(snapshot.state == MOVE_SLIDE && slideSpeed > SLIDE_SPRINT_SPEED,
			"the slide runs above the sprint's speed before the jump");

		intent.tier = TIER_SPRINT;
		intent.jumpRequested = true;
		intent.vel = SMoveVec2{ 0.0f, slideSpeed };
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_AIR, "the jump from the slide moves to the air");
		Check((snapshot.events & EVENT_JUMP) != 0, "the slide's jump reports the jump event");
		Check(fabsf(snapshot.speed - slideSpeed) < 1e-6f,
			"the slide jump carries the slide's speed: the transition resets no velocity");
		Check(snapshot.speed > SLIDE_SPRINT_SPEED, "the slide jump is faster than the sprint's speed");

		// the air's keep-momentum holds it: no input, no ground, no change
		intent.jumpRequested = false;
		intent.hasInput = false;
		intent.onGround = false;
		for (int i = 0; i < 30 && snapshot.state == MOVE_AIR; i++) {
			intent.vel = snapshot.moved;
			snapshot = solver.Step(intent);
		}
		Check(snapshot.state == MOVE_AIR && fabsf(snapshot.speed - slideSpeed) < 1e-6f,
			"the air keeps the slide jump's momentum, bit for bit");
		Check(snapshot.stance == STANCE_STAND,
			"a slide jump keeps the standing stance (VICE-192)");
	}

	/* (e) the two other exits the design names: a prone request ends the slide
	 *     on the ground and lands the ped in the prone (VICE-177's stance row
	 *     fires on the same edge), and the feet leaving the surface end it into
	 *     the fall with the speed it had. */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, SLIDE_SPRINT_SPEED);
		SMoveSnapshot snapshot = StepIntoSlide(&solver);
		float slideSpeed = snapshot.speed;

		intent.tier = TIER_SPRINT;
		intent.proneRequested = true;
		intent.vel = SMoveVec2{ 0.0f, snapshot.moved.y };
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_GROUND, "a prone request ends the slide on the ground");
		Check(snapshot.stance == STANCE_PRONE,
			"the slide's prone request lands in the prone: the stance row fires on the same edge");

		solver.Reset();
		snapshot = StepIntoSlide(&solver);
		intent.proneRequested = false;
		intent.onGround = false;
		intent.vel = SMoveVec2{ 0.0f, snapshot.moved.y };
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_AIR,
			"the feet leaving the surface end the slide into the fall");
		Check(fabsf(snapshot.speed - slideSpeed) < 1e-6f,
			"the fall takes the slide's speed on");
	}

	/* (f) the slide keeps its whole state on the instance: two solvers driven
	 *     with the same slide stream - the sprint, the entry, the decay, the
	 *     jump - agree field for field, the entry boost between frames
	 *     included. */
	{
		CPlayerMoveSolver first, second;
		SMoveIntent intent = WalkIntent(0.0f, SLIDE_SPRINT_SPEED);
		SMoveVec2 liveFirst = SMoveVec2{ 0.0f, 0.0f };
		SMoveVec2 liveSecond = SMoveVec2{ 0.0f, 0.0f };
		int diffs = 0;

		intent.tier = TIER_SPRINT;
		for (int i = 0; i < 80; i++) {
			SMoveSnapshot a, b;

			if (i > 0 && !SameSnapshot(first.GetSnapshot(), second.GetSnapshot()))
				diffs++;
			intent.crouchRequested = (i == 10);
			intent.jumpRequested = (i == 50);
			intent.onGround = i < 55;
			intent.vel = liveFirst;
			a = first.Step(intent);
			intent.vel = liveSecond;
			b = second.Step(intent);
			liveFirst = a.moved;
			liveSecond = b.moved;
		}
		Check(diffs == 0, "two solvers driven with the same slide stream agree field for field");
	}

	printf("  the slide names the entry rung, keeps the sprint's lane out of it, and ends on the\n");
	printf("  ground, the air or the prone request; authored slide content is the follow-up\n");
}

/* ---------------------------------------------------------------------- *
 * 7. the stance (VICE-176)
 * ---------------------------------------------------------------------- */

/*
 * The stance is the solver's axis with its own rows: the crouch row scales the
 * step (the target speed and the turn rate), the stance moves are rows of the
 * stance transition table and fire on the edges, and the headroom - VICE-178's
 * test, which the adapter runs and the solver only reads - holds the stand-ups
 * back. The digits below are the module's crouch row and the ground state row
 * (PlayerMoveSolver.cpp, moveStanceRows and moveStateRows); the harness pins
 * them so a table edit cannot pass unnoticed.
 */

#define CROUCH_SPEED_SCALE	0.5f	/* the crouch row: half the walk, the content's own stride share */
#define CROUCH_TURN_SCALE	1.25f	/* the crouch row: a quarter tighter than the stand */
#define CROUCH_CAPSULE_HEIGHT	0.5f	/* VICE-178's crouch set: 0.55 - 0.2 - 0.2 + 0.35, the head sphere's top */
#define GROUND_TURN_RATE	12.0f	/* the ground state row */
#define GROUND_ACCEL		20.0f / 50.0f	/* the ground state row */

/* The crouch hold of the acceptance: a crouch that is never cancelled must not
 * time out. Sixty seconds of the pad's own step. */
#define CROUCH_HOLD_STEPS	3000

/* The crouch row's values and the scaled target, one step after the edge. */
static void
CheckStanceRow(void)
{
	CPlayerMoveSolver standSolver, crouchSolver;
	SMoveIntent stand = WalkIntent(0.0f, 0.30f);
	SMoveIntent crouch = CrouchIntent(0.0f, 0.30f);
	const SMoveSnapshot &standSnap = standSolver.Step(stand);
	const SMoveSnapshot &crouchSnap = crouchSolver.Step(crouch);

	Check(standSnap.stance == STANCE_STAND, "the walk of phase 0 stands the ped");
	Check(crouchSnap.stance == STANCE_CROUCH, "the Duck edge crouches the ped in the step that reads it");
	Check(crouchSnap.turnScale == CROUCH_TURN_SCALE,
		"the crouch row's turn scale is the snapshot's turn scale");
	Check(crouchSnap.capsuleHeight == CROUCH_CAPSULE_HEIGHT,
		"the crouch row reports VICE-178's crouch capsule top");
	Check(crouchSnap.targetSpeed == CROUCH_SPEED_SCALE * standSnap.targetSpeed,
		"the crouch row scales the target speed by half");
	Check(crouchSnap.targetSpeed < standSnap.targetSpeed,
		"the crouch is slower than the stand at the same intent");

	/* the crouch walks its scaled target exactly, like the stand walks its own */
	{
		SMoveVec2 live = { 0.0f, 0.0f };
		SMoveIntent intent = WalkIntent(0.0f, 0.30f);
		const SMoveSnapshot *snap = &crouchSnap;

		for (int i = 0; i < 240; i++) {
			intent.vel = live;
			snap = &crouchSolver.Step(intent);
			live = snap->moved;
		}
		Check(snap->stance == STANCE_CROUCH, "the crouch holds over the walk");
		Check(snap->moved.x == 0.0f && snap->moved.y == CROUCH_SPEED_SCALE * 0.30f,
			"the crouch walk reaches the scaled target exactly");
	}

	printf("  the crouch row: speed scale %.9g, turn scale %.9g, capsule %.9g\n",
		CROUCH_SPEED_SCALE, CROUCH_TURN_SCALE, CROUCH_CAPSULE_HEIGHT);
}

/* The crouch row's turn rate, behaviourally: one step of the crouch is the
 * reference maths with the crouch row's rate (12 * 1.25) and its scaled target,
 * the same step of the stand is that maths with 12 and the unscaled target, and
 * the crouch's direction settles tighter. */
static void
CheckStanceTurn(void)
{
	const float dt = 1.0f / 50.0f;
	const float targetSpeed = 0.30f;
	/* the velocity points at +y; the input asks for +x: a quarter turn */
	const float heading = -1.5707963267948966f;	/* -PI / 2: the move direction is (1, 0) */
	SMoveVec2 velocity = { 0.0f, 1.0f };
	SMoveVec2 moveDirection = { -sinf(heading), cosf(heading) };
	SMoveIntent stand = WalkIntent(heading, targetSpeed);
	SMoveIntent crouch = CrouchIntent(heading, targetSpeed);
	CPlayerMoveSolver standSolver, crouchSolver;
	SMoveVec2 standGot, crouchGot, steered, standWant, crouchWant;

	stand.vel = velocity;
	crouch.vel = velocity;
	standGot = standSolver.Step(stand).moved;
	crouchGot = crouchSolver.Step(crouch).moved;

	steered = RefSteerVelocityTowards(velocity, moveDirection, GROUND_TURN_RATE * dt);
	standWant = RefMoveVelocityTowards(steered, SMoveVec2{ moveDirection.x * targetSpeed, moveDirection.y * targetSpeed },
		GROUND_ACCEL * dt);
	steered = RefSteerVelocityTowards(velocity, moveDirection, GROUND_TURN_RATE * CROUCH_TURN_SCALE * dt);
	crouchWant = RefMoveVelocityTowards(steered,
		SMoveVec2{ moveDirection.x * CROUCH_SPEED_SCALE * targetSpeed, moveDirection.y * CROUCH_SPEED_SCALE * targetSpeed },
		GROUND_ACCEL * dt);

	Check(standGot.x == standWant.x && standGot.y == standWant.y,
		"the standing step turns at the state row's own rate, bit for bit");
	Check(crouchGot.x == crouchWant.x && crouchGot.y == crouchWant.y,
		"the crouch step turns at the crouch row's rate, bit for bit");

	/* the crouch's direction is the tighter one: closer to the input after one
	 * step (both start a quarter turn away) */
	{
		float standAngle = atan2f(standGot.y, standGot.x);
		float crouchAngle = atan2f(crouchGot.y, crouchGot.x);
		float targetAngle = atan2f(moveDirection.y, moveDirection.x);
		float standGap = fabsf(RefLimitRadianAngle(targetAngle - standAngle));
		float crouchGap = fabsf(RefLimitRadianAngle(targetAngle - crouchAngle));

		Check(crouchGap < standGap, "the crouch turns tighter than the stand: its direction is closer to the input");
	}
}

/* The stance moves: the Duck edge toggles both ways, the sprint holds the
 * stance standing and stands a crouch up, the jump and the exit-vehicle press
 * stand it up, and nothing else moves the stance. */
static void
CheckStanceEdges(void)
{
	/* (a) the Duck toggle, both ways, with the holds between */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, 0.3f);

		Check(solver.Step(intent).stance == STANCE_STAND, "the stance starts standing");
		Check(solver.Step(CrouchIntent(0.0f, 0.3f)).stance == STANCE_CROUCH, "the Duck edge crouches the ped");
		Check(solver.Step(intent).stance == STANCE_CROUCH, "the crouch holds without an edge");
		Check(solver.Step(CrouchIntent(0.0f, 0.3f)).stance == STANCE_STAND, "the Duck edge stands the ped up again");
		Check(solver.Step(intent).stance == STANCE_STAND, "the stand holds without an edge");
	}

	/* (b) the sprint gates the entry and stands the crouch up */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, 0.3f);

		intent.sprintRequested = true;
		intent.crouchRequested = true;
		Check(solver.Step(intent).stance == STANCE_STAND,
			"the Duck edge inside a sprint does not crouch the ped (the sprint holds the stance standing)");
		Check(solver.Step(intent).stance == STANCE_STAND, "the stance holds standing while the sprint holds");
	}

	/* (c) the sprint, the jump and the exit-vehicle press, each from a crouch */
	{
		static const char *const names[3] = { "the sprint", "the jump", "the exit-vehicle press" };
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, 0.3f);

		for (int i = 0; i < 3; i++) {
			Check(solver.Step(CrouchIntent(0.0f, 0.3f)).stance == STANCE_CROUCH, "the Duck edge crouches the ped");
			if (i == 0)
				intent.sprintRequested = true;
			else if (i == 1)
				intent.jumpRequested = true;
			else
				intent.exitVehiclePressed = true;
			Check(solver.Step(intent).stance == STANCE_STAND, names[i]);
			Check(solver.Step(intent).stance == STANCE_STAND, names[i]);
			intent.sprintRequested = false;
			intent.jumpRequested = false;
			intent.exitVehiclePressed = false;
		}
	}

	/* (d) the crouch that is never cancelled: a minute of steps, no timeout */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, 0.3f);
		int standsAfter = 0;

		(void)solver.Step(CrouchIntent(0.0f, 0.3f));
		for (int i = 0; i < CROUCH_HOLD_STEPS; i++) {
			if (solver.Step(intent).stance != STANCE_CROUCH)
				standsAfter++;
			intent.hasInput = (i % 3) != 2;
			intent.vel = solver.GetSnapshot().moved;
		}
		Check(standsAfter == 0, "a crouch that is never cancelled holds: no duck timer, sixty seconds of steps");
		printf("  %d steps (%g s) in one crouch, no edge: the stance never moves by itself\n",
			CROUCH_HOLD_STEPS, CROUCH_HOLD_STEPS / 50.0);
	}
}

/* VICE-178's headroom: the adapter passes the world's answer in the intent, so
 * the solver never touches the world, and every stand-up the crouch has is held
 * back while the body cannot rise. */
static void
CheckStanceHeadroom(void)
{
	CPlayerMoveSolver solver;
	SMoveIntent intent = WalkIntent(0.0f, 0.3f);

	(void)solver.Step(CrouchIntent(0.0f, 0.3f));
	Check(solver.GetSnapshot().stance == STANCE_CROUCH, "the ped crouches under the ceiling");

	intent.headroomClear = false;	/* the world above the ped refuses the stand */

	intent.crouchRequested = true;
	Check(solver.Step(intent).stance == STANCE_CROUCH, "the Duck edge is refused: the crouch holds with no headroom");
	intent.crouchRequested = false;
	Check(solver.Step(intent).stance == STANCE_CROUCH, "the refusal holds over the steps after it");

	intent.sprintRequested = true;
	Check(solver.Step(intent).stance == STANCE_CROUCH, "the sprint is refused: the crouch holds with no headroom");
	intent.sprintRequested = false;

	intent.jumpRequested = true;
	Check(solver.Step(intent).stance == STANCE_CROUCH, "the jump is refused: the crouch holds with no headroom");
	intent.jumpRequested = false;

	intent.exitVehiclePressed = true;
	Check(solver.Step(intent).stance == STANCE_CROUCH, "the exit-vehicle press is refused with no headroom");
	intent.exitVehiclePressed = false;

	/* the ped walks out from under it: the same edge stands him up */
	intent.headroomClear = true;
	intent.crouchRequested = true;
	Check(solver.Step(intent).stance == STANCE_STAND, "the same Duck edge stands the ped up with the headroom clear");

	printf("  no headroom: the Duck edge, the sprint, the jump and the exit-vehicle press all hold the crouch\n");
	printf("  the headroom clear again: the same edge stands the ped up\n");
}

static void
CheckStance(void)
{
	CheckStanceRow();
	CheckStanceTurn();
	CheckStanceEdges();
	CheckStanceHeadroom();
}

/* ---------------------------------------------------------------------- *
 * 9. the prone (VICE-177)
 * ---------------------------------------------------------------------- */

/*
 * The prone is the solver's lowest stance: the PED_PRONE edge lies the ped
 * down, but only on the ground, and the row is the crawl - the crawl cycle's
 * own speed as a share of the walk's, half the standing turn rate, and the
 * signed capsule top of VICE-178's prone set, which lies below the origin
 * because the body does. The exits walk back up the ladder: the prone's own
 * edge stands the ped up, the Duck edge lifts him into the crouch, the sprint,
 * the jump and the exit-vehicle press stand him up, and every one of them is
 * held back while the body cannot rise (VICE-178's headroom). The digits below
 * are the module's prone row (PlayerMoveSolver.cpp, moveStanceRows); the
 * harness pins them so a table edit cannot pass unnoticed.
 */

/* The prone hold of the acceptance: a prone that is never cancelled must not
 * time out. Sixty seconds of the pad's own step. */
#define PRONE_HOLD_STEPS	3000

/* The prone row's values and the scaled target, one step after the edge. */
static void
CheckProneRow(void)
{
	CPlayerMoveSolver standSolver, proneSolver;
	SMoveIntent stand = WalkIntent(0.0f, 0.30f);
	SMoveIntent prone = ProneIntent(0.0f, 0.30f);
	const SMoveSnapshot &standSnap = standSolver.Step(stand);
	const SMoveSnapshot &proneSnap = proneSolver.Step(prone);

	Check(proneSnap.stance == STANCE_PRONE, "the prone edge lies the ped down in the step that reads it");
	Check(proneSnap.turnScale == PRONE_TURN_SCALE, "the prone row's turn scale is the snapshot's turn scale");
	Check(proneSnap.capsuleHeight == PRONE_CAPSULE_HEIGHT, "the prone row reports VICE-178's prone capsule top");
	Check(proneSnap.capsuleHeight < 0.0f, "the prone capsule top hangs below the origin: the body lies on the ground");
	Check(proneSnap.targetSpeed == PRONE_SPEED_SCALE * standSnap.targetSpeed,
		"the prone row scales the target speed by the crawl's own share of the walk");
	Check(proneSnap.targetSpeed < standSnap.targetSpeed, "the crawl is slower than the stand at the same intent");

	/* the crawl walks its scaled target exactly, like the stand walks its own */
	{
		SMoveVec2 live = { 0.0f, 0.0f };
		SMoveIntent intent = WalkIntent(0.0f, 0.30f);
		const SMoveSnapshot *snap = &proneSnap;

		for (int i = 0; i < 240; i++) {
			intent.vel = live;
			snap = &proneSolver.Step(intent);
			live = snap->moved;
		}
		Check(snap->stance == STANCE_PRONE, "the prone holds over the crawl");
		Check(snap->moved.x == 0.0f && snap->moved.y == PRONE_SPEED_SCALE * 0.30f,
			"the crawl reaches the scaled target exactly");
	}

	printf("  the prone row: speed scale %.9g (the crawl's 0.443 over the walk's 1.13 is 0.392, taken to the\n",
		PRONE_SPEED_SCALE);
	printf("  playback floor 0.4 so the crawl cycle plays exactly), turn scale %.9g,\n", PRONE_TURN_SCALE);
	printf("  capsule %.9g (the prone set's top: the body hangs below the origin)\n", PRONE_CAPSULE_HEIGHT);
}

/* The prone row's turn rate, behaviourally: one step of the prone is the
 * reference maths with the prone row's rate (12 * 0.5) and its scaled target,
 * the same step of the stand is that maths with 12 and the unscaled target, and
 * the prone's direction settles looser - a body that lies down drives the
 * direction slowly. */
static void
CheckProneTurn(void)
{
	const float dt = 1.0f / 50.0f;
	const float targetSpeed = 0.30f;
	/* the velocity points at +y; the input asks for +x: a quarter turn */
	const float heading = -1.5707963267948966f;	/* -PI / 2: the move direction is (1, 0) */
	SMoveVec2 velocity = { 0.0f, 1.0f };
	SMoveVec2 moveDirection = { -sinf(heading), cosf(heading) };
	SMoveIntent stand = WalkIntent(heading, targetSpeed);
	SMoveIntent prone = ProneIntent(heading, targetSpeed);
	CPlayerMoveSolver standSolver, proneSolver;
	SMoveVec2 standGot, proneGot, steered, standWant, proneWant;

	stand.vel = velocity;
	prone.vel = velocity;
	standGot = standSolver.Step(stand).moved;
	proneGot = proneSolver.Step(prone).moved;

	steered = RefSteerVelocityTowards(velocity, moveDirection, GROUND_TURN_RATE * dt);
	standWant = RefMoveVelocityTowards(steered, SMoveVec2{ moveDirection.x * targetSpeed, moveDirection.y * targetSpeed },
		GROUND_ACCEL * dt);
	steered = RefSteerVelocityTowards(velocity, moveDirection, GROUND_TURN_RATE * PRONE_TURN_SCALE * dt);
	proneWant = RefMoveVelocityTowards(steered,
		SMoveVec2{ moveDirection.x * PRONE_SPEED_SCALE * targetSpeed, moveDirection.y * PRONE_SPEED_SCALE * targetSpeed },
		GROUND_ACCEL * dt);

	Check(standGot.x == standWant.x && standGot.y == standWant.y,
		"the standing step turns at the state row's own rate, bit for bit");
	Check(proneGot.x == proneWant.x && proneGot.y == proneWant.y,
		"the prone step turns at the prone row's rate, bit for bit");

	/* the prone's direction is the looser one: further from the input after one
	 * step (both start a quarter turn away) */
	{
		float standAngle = atan2f(standGot.y, standGot.x);
		float proneAngle = atan2f(proneGot.y, proneGot.x);
		float targetAngle = atan2f(moveDirection.y, moveDirection.x);
		float standGap = fabsf(RefLimitRadianAngle(targetAngle - standAngle));
		float proneGap = fabsf(RefLimitRadianAngle(targetAngle - proneAngle));

		Check(proneGap > standGap, "the prone turns looser than the stand: its direction is further from the input");
	}
}

/* The prone's entries and exits: the edge lies the ped down from the stand and
 * from the crouch and only on the ground, the prone's own edge stands him up,
 * the Duck edge lifts him into the crouch (the low ladder is prone, crouch,
 * stand), and the sprint, the jump and the exit-vehicle press stand him up. */
static void
CheckProneEdges(void)
{
	/* (a) the entries: the edge from the stand and from the crouch, and the
	 *     step without an edge that keeps what it found */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, 0.3f);

		Check(solver.Step(intent).stance == STANCE_STAND, "the stance starts standing");
		Check(solver.Step(ProneIntent(0.0f, 0.3f)).stance == STANCE_PRONE, "the prone edge lies the ped down from the stand");
		Check(solver.Step(intent).stance == STANCE_PRONE, "the prone holds without an edge");
		Check(solver.Step(ProneIntent(0.0f, 0.3f)).stance == STANCE_STAND, "the prone edge stands the ped up again");

		solver.Reset();
		Check(solver.Step(CrouchIntent(0.0f, 0.3f)).stance == STANCE_CROUCH, "the Duck edge crouches the ped");
		Check(solver.Step(ProneIntent(0.0f, 0.3f)).stance == STANCE_PRONE, "the prone edge reaches the prone from the crouch");
	}

	/* (b) the ground: the prone action is a ground action */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = ProneIntent(0.0f, 0.3f);

		intent.onGround = false;
		Check(solver.Step(intent).stance == STANCE_STAND,
			"the prone edge with the feet off the ground does not lie the ped down");
	}

	/* (c) the low ladder: the prone's own edge stands the ped up, and the Duck
	 *     edge lifts him into the crouch */
	{
		CPlayerMoveSolver solver;

		(void)solver.Step(ProneIntent(0.0f, 0.3f));
		Check(solver.Step(CrouchIntent(0.0f, 0.3f)).stance == STANCE_CROUCH, "the Duck edge lifts the prone into the crouch");
		Check(solver.Step(CrouchIntent(0.0f, 0.3f)).stance == STANCE_STAND, "the Duck edge again stands the crouch up");
	}

	/* (d) the sprint, the jump and the exit-vehicle press, each from a fresh
	 *     prone */
	{
		static const char *const names[3] = { "the sprint", "the jump", "the exit-vehicle press" };
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, 0.3f);

		for (int i = 0; i < 3; i++) {
			Check(solver.Step(ProneIntent(0.0f, 0.3f)).stance == STANCE_PRONE, "the prone edge lies the ped down");
			if (i == 0)
				intent.sprintRequested = true;
			else if (i == 1)
				intent.jumpRequested = true;
			else
				intent.exitVehiclePressed = true;
			Check(solver.Step(intent).stance == STANCE_STAND, names[i]);
			Check(solver.Step(intent).stance == STANCE_STAND, names[i]);
			intent.sprintRequested = false;
			intent.jumpRequested = false;
			intent.exitVehiclePressed = false;
		}
	}

	/* (e) the prone that is never cancelled: a minute of steps, no timeout */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, 0.3f);
		int standsAfter = 0;

		(void)solver.Step(ProneIntent(0.0f, 0.3f));
		for (int i = 0; i < PRONE_HOLD_STEPS; i++) {
			if (solver.Step(intent).stance != STANCE_PRONE)
				standsAfter++;
			intent.hasInput = (i % 3) != 2;
			intent.vel = solver.GetSnapshot().moved;
		}
		Check(standsAfter == 0, "a prone that is never cancelled holds: sixty seconds of steps");
		printf("  %d steps (%g s) in one prone, no edge: the stance never moves by itself\n",
			PRONE_HOLD_STEPS, PRONE_HOLD_STEPS / 50.0);
	}

	/* (f) the air does not move the stance: a prone that leaves the surface
	 *     stays prone while it falls */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, 0.3f);

		(void)solver.Step(ProneIntent(0.0f, 0.3f));
		intent.onGround = false;
		intent.vel = SMoveVec2{ 0.0f, 0.05f };
		const SMoveSnapshot &snap = solver.Step(intent);
		Check(snap.state == MOVE_AIR && snap.stance == STANCE_PRONE,
			"the feet leaving the surface move the state, not the stance: the body stays prone in the fall");
	}
}

/* VICE-178's headroom over the prone (VICE-177): the adapter passes the
 * world's answer in the intent, and every exit the prone has is held back while
 * the body cannot rise - the prone's own edge, the Duck edge, the sprint, the
 * jump and the exit-vehicle press. */
static void
CheckProneHeadroom(void)
{
	CPlayerMoveSolver solver;
	SMoveIntent intent = WalkIntent(0.0f, 0.3f);

	(void)solver.Step(ProneIntent(0.0f, 0.3f));
	Check(solver.GetSnapshot().stance == STANCE_PRONE, "the ped lies prone under the ceiling");

	intent.headroomClear = false;	/* the world above the ped refuses the stand */

	intent.proneRequested = true;
	Check(solver.Step(intent).stance == STANCE_PRONE, "the prone edge is refused: the prone holds with no headroom");
	intent.proneRequested = false;
	Check(solver.Step(intent).stance == STANCE_PRONE, "the refusal holds over the steps after it");

	intent.crouchRequested = true;
	Check(solver.Step(intent).stance == STANCE_PRONE, "the Duck edge is refused: the prone holds with no headroom");
	intent.crouchRequested = false;

	intent.sprintRequested = true;
	Check(solver.Step(intent).stance == STANCE_PRONE, "the sprint is refused: the prone holds with no headroom");
	intent.sprintRequested = false;

	intent.jumpRequested = true;
	Check(solver.Step(intent).stance == STANCE_PRONE, "the jump is refused: the prone holds with no headroom");
	intent.jumpRequested = false;

	intent.exitVehiclePressed = true;
	Check(solver.Step(intent).stance == STANCE_PRONE, "the exit-vehicle press is refused with no headroom");
	intent.exitVehiclePressed = false;

	/* the ped crawls out from under it: the same edge stands him up */
	intent.headroomClear = true;
	intent.proneRequested = true;
	Check(solver.Step(intent).stance == STANCE_STAND, "the same prone edge stands the ped up with the headroom clear");

	printf("  no headroom: the prone edge, the Duck edge, the sprint, the jump and the exit-vehicle press all hold the prone\n");
	printf("  the headroom clear again: the same edge stands the ped up\n");
}

static void
CheckProne(void)
{
	CheckProneRow();
	CheckProneTurn();
	CheckProneEdges();
	CheckProneHeadroom();
}

/* ---------------------------------------------------------------------- *
 * 10. the dive (VICE-181)
 * ---------------------------------------------------------------------- */

/*
 * The dive is the prone action with the feet off the ground: its edge enters it
 * from the air alone (the adapter reads the PED_PRONE edge against the grounded
 * flag, so the two edges can never fire on one frame), its row keeps the
 * momentum it was entered with and steers the velocity direction at its own
 * small authority, the feet touching the ground land it in the slide - the
 * slide-out, whose own row and step bleed the speed, so no curve is copied into
 * the dive - and the stance rows of that touchdown put the body in the prone.
 * The body's own yaw adds no rule of its own: the engine's one rule turns the
 * body to the camera in every view (TurnTargetPed sets it outright in the first
 * person view), and the snapshot's spin is that turn - so the dive spins with
 * the look and the solver only reads the facing in and reports the turn out. A
 * wall or a head hit has no row: those triggers have no producer in the tree
 * yet (the tests that read them arrive with the wall run), so the dive's only
 * exit is the ground. The digits below are the module's dive row
 * (PlayerMoveSolver.cpp, moveStateRows); the harness pins them so a table edit
 * cannot pass unnoticed, exactly as the slide's and the prone's sections do.
 */

#define DIVE_ACCEL		(2.0f / 50.0f)	/* the dive row: half the air's own air control */
#define DIVE_TURN_RATE		3.2f		/* the dive row: twice the air's own authority */
#define AIR_TURN_RATE		1.6f		/* the air row: the authority the dive is measured against */
#define DIVE_SPRINT_SPEED	(14.668f / MOVE_ENGINE_UNITS_PER_SECOND)	/* sprint_civi, the dive's carried case */

/* The air step of the dive section: the walk intent with the feet off the
 * ground, so the dive's edge has the state it needs (VICE-181). */
static SMoveIntent
AirIntent(float heading, float targetSpeed)
{
	SMoveIntent intent = WalkIntent(heading, targetSpeed);

	intent.onGround = false;
	return intent;
}

/* One step into the dive: the fall's own frame (the row the feet leaving the
 * ground enters) and then the frame the dive's edge rides. The pad is released,
 * so the fall's velocity crosses the two frames bit for bit. */
static SMoveSnapshot
StepIntoDive(CPlayerMoveSolver *pSolver, float heading, float targetSpeed, SMoveVec2 velocity)
{
	SMoveIntent intent = AirIntent(heading, targetSpeed);

	intent.facing = heading;
	intent.hasInput = false;
	intent.vel = velocity;
	(void)pSolver->Step(intent);	/* the frame the feet leave the ground: the air's row */
	intent.diveRequested = true;
	return pSolver->Step(intent);
}

/* Put a solver in the air: the one frame the feet leaving the ground takes, the
 * row the dive's own checks read the state from. */
static void
PrimeAir(CPlayerMoveSolver *pSolver)
{
	SMoveIntent intent = AirIntent(0.0f, 0.0f);

	intent.hasInput = false;
	intent.vel = SMoveVec2{ 0.0f, 0.0f };
	(void)pSolver->Step(intent);
}

/* The entry: the air plus the dive's own edge enters the dive in one frame,
 * whatever the direction the ped falls in and whatever way it faces (the row
 * reads neither), the entry carries the fall's velocity bit for bit, the stance
 * stays where it is (the body lies down on the landing, not in the air), the
 * ground refuses the edge, and the dive holds while the feet are off the
 * ground: only the ground ends it. */
static void
CheckDiveEntry(void)
{
	/* (a) the entry, at four directions and facings, in one frame */
	for (int i = 0; i < 4; i++) {
		CPlayerMoveSolver solver;
		float heading = -3.0f + 1.9f * (float)i;
		SMoveIntent intent = AirIntent(heading, 0.3f);
		SMoveVec2 fall = SMoveVec2{ -sinf(heading) * 0.25f, cosf(heading) * 0.25f };
		SMoveSnapshot snapshot;

		/* no input, so the entry's own row does not touch the speed and the
		 * velocity that lands is bit for bit the fall's */
		intent.facing = 3.0f - 1.9f * (float)i;
		intent.hasInput = false;
		intent.vel = fall;
		snapshot = solver.Step(intent);	/* the frame the feet leave the ground: the air */
		Check(snapshot.state == MOVE_AIR, "the ped falls before the dive: the air's own frame");
		intent.diveRequested = true;
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_DIVE, "the air plus the dive edge enters the dive in one frame");
		Check(snapshot.moved.x == fall.x && snapshot.moved.y == fall.y,
			"the dive's entry carries the fall's velocity bit for bit, whatever the direction and the facing");
		Check(snapshot.stance == STANCE_STAND,
			"the dive's entry keeps the stance: the body lies down on the landing, not in the air");
	}

	/* (b) the ground: the dive is an air action, so its edge with the feet on
	 *     the ground enters nothing (the prone's own ground rule is VICE-177's) */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, 0.3f);

		intent.diveRequested = true;
		intent.vel = SMoveVec2{ 0.0f, 0.2f };
		Check(solver.Step(intent).state == MOVE_GROUND,
			"the dive's edge with the feet on the ground does not enter the dive");
	}

	/* (c) the dive holds while the feet are off the ground: a stream of falling
	 *     frames, the look and the pad moving, the jump edge pressing (the jump
	 *     is the ground's own row since VICE-182, so the press is no double
	 *     jump) - nothing moves the state but the ground */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = AirIntent(0.0f, 0.3f);
		int leaves = 0;

		(void)StepIntoDive(&solver, 0.0f, 0.3f, SMoveVec2{ 0.0f, 0.25f });
		for (int i = 0; i < 60; i++) {
			intent.facing = 0.1f * (float)i;
			intent.hasInput = (i % 3) != 2;
			intent.jumpRequested = (i % 9) == 4;
			intent.vel = solver.GetSnapshot().moved;
			if (solver.Step(intent).state != MOVE_DIVE)
				leaves++;
		}
		Check(leaves == 0, "the dive holds while the feet are off the ground: only the ground ends it");
	}
}

/* The speed carried: the dive keeps the speed it was entered with (with the pad
 * released, bit for bit; with the pad held the row's own low air control is the
 * air's), and the entry off a fall, a jump and a slide jump all carry theirs -
 * the slide jump's all the way from the slide's own speed. */
static void
CheckDiveCarriedSpeed(void)
{
	/* (a) off a fall: the velocity is the fall's, bit for bit, for the whole
	 *     dive */
	{
		CPlayerMoveSolver solver;
		SMoveSnapshot snapshot = StepIntoDive(&solver, 0.0f, 0.3f, SMoveVec2{ 0.0f, 0.25f });
		SMoveIntent intent = AirIntent(0.0f, 0.3f);
		SMoveVec2 entered = snapshot.moved;

		intent.hasInput = false;
		for (int i = 0; i < 30 && snapshot.state == MOVE_DIVE; i++) {
			intent.vel = snapshot.moved;
			snapshot = solver.Step(intent);
		}
		Check(snapshot.state == MOVE_DIVE, "the fall's dive is still diving after thirty frames");
		Check(snapshot.moved.x == entered.x && snapshot.moved.y == entered.y,
			"the dive keeps the fall's velocity bit for bit while the feet are off the ground");
	}

	/* (b) off a slide jump: the slide's speed crosses the jump, the dive and
	 *     the landing without a reset - a dive off the fastest launch there is */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = WalkIntent(0.0f, SLIDE_SPRINT_SPEED);
		SMoveSnapshot snapshot = StepIntoSlide(&solver);
		float slideSpeed;

		snapshot = StepSlideAt(&solver, snapshot.moved.y);
		slideSpeed = snapshot.speed;
		intent.tier = TIER_SPRINT;
		intent.jumpRequested = true;
		intent.vel = SMoveVec2{ 0.0f, slideSpeed };
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_AIR && snapshot.speed == slideSpeed,
			"the slide jump carries the slide's speed into the air");

		intent.jumpRequested = false;
		intent.onGround = false;
		intent.hasInput = false;
		intent.diveRequested = true;
		intent.vel = snapshot.moved;
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_DIVE && snapshot.speed == slideSpeed,
			"a dive off the slide jump carries the slide's speed through the dive");

		/* the feet touch: the slide-out's own row is what touches the speed,
		 * and its entry boost only lifts the head */
		intent.diveRequested = false;
		intent.onGround = true;
		intent.vel = snapshot.moved;
		{
			float diveSpeed = snapshot.speed;

			snapshot = solver.Step(intent);
			Check(snapshot.state == MOVE_SLIDE,
				"the slide-jump dive's landing enters the slide-out: the slide's own machinery");
			Check(snapshot.stance == STANCE_PRONE, "the slide-jump dive lands prone");
			Check(snapshot.speed >= diveSpeed,
				"the landing starts from the dive's speed: the slide's own entry boost only adds");
			Check(snapshot.speed > SLIDE_SPRINT_SPEED, "the dive-out is faster than the sprint's own speed");
		}
	}

	/* (c) the row's own numbers, against the reference maths: the dive's step is
	 *     the air's branch with the dive's acceleration and turn rate, and the
	 *     dive's authority sits between the air's small one and the ground's 12 */
	{
		const float dt = 1.0f / 50.0f;
		const float heading = -1.5707963267948966f;	/* -PI / 2: the move direction is (1, 0) */
		const float targetSpeed = 0.05f;
		SMoveVec2 velocity = { 0.0f, 0.3f };
		SMoveVec2 moveDirection = { -sinf(heading), cosf(heading) };
		CPlayerMoveSolver diveSolver, airSolver, slowSolver;
		SMoveIntent dive = AirIntent(heading, targetSpeed);
		SMoveIntent air = AirIntent(heading, targetSpeed);
		SMoveIntent slow = AirIntent(heading, targetSpeed);
		SMoveVec2 diveGot, airGot, slowGot, want;
		float diveTurn, airTurn;

		/* the solvers are in the state their row is read from first: the dive's
		 * own entry frame and the air's first frame */
		(void)StepIntoDive(&diveSolver, heading, targetSpeed, SMoveVec2{ 0.0f, 0.0f });
		(void)StepIntoDive(&slowSolver, heading, targetSpeed, SMoveVec2{ 0.0f, 0.0f });
		PrimeAir(&airSolver);

		dive.vel = velocity;
		air.vel = velocity;
		diveGot = diveSolver.Step(dive).moved;
		airGot = airSolver.Step(air).moved;

		/* the dive: the steer at the row's own rate and nothing else - the
		 * speed is above the target, so no air control runs */
		want = RefSteerVelocityTowards(velocity, moveDirection, DIVE_TURN_RATE * dt);
		Check(diveGot.x == want.x && diveGot.y == want.y,
			"the dive's step steers at the dive row's own rate, bit for bit against the reference maths");

		want = RefSteerVelocityTowards(velocity, moveDirection, AIR_TURN_RATE * dt);
		Check(airGot.x == want.x && airGot.y == want.y,
			"the air's step steers at the air row's own rate: the dive's number is the dive's own");

		/* the authority as the angle actually turned: above the air's small one,
		 * below the ground's own */
		diveTurn = fabsf(RefLimitRadianAngle(atan2f(diveGot.y, diveGot.x) - atan2f(velocity.y, velocity.x)));
		airTurn = fabsf(RefLimitRadianAngle(atan2f(airGot.y, airGot.x) - atan2f(velocity.y, velocity.x)));
		Check(diveTurn > airTurn && diveTurn < 12.0f * dt,
			"the dive's authority sits between the air's and the ground's");
		printf("  one step's turn: the dive %.6f rad, the air %.6f rad, the ground's own %.6f rad\n",
			diveTurn, airTurn, 12.0f * dt);

		/* the row's low air control: a dive slower than the target is sped up
		 * toward it at the dive's own rate, the air's branch maths bit for bit */
		{
			SMoveVec2 slowSpeed = { 0.0f, 0.02f };
			SMoveVec2 target = { moveDirection.x * targetSpeed, moveDirection.y * targetSpeed };

			slow.vel = slowSpeed;
			slowGot = slowSolver.Step(slow).moved;
			want = RefMoveVelocityTowards(RefSteerVelocityTowards(slowSpeed, moveDirection, DIVE_TURN_RATE * dt),
				target, DIVE_ACCEL * dt);
			Check(slowGot.x == want.x && slowGot.y == want.y,
				"a slow dive is pushed toward the target at the dive row's own low acceleration");
		}
	}
}

/* The yaw: the body's turn is the engine's one rule (the body faces the camera
 * in every view, and TurnTargetPed sets the yaw outright in the first person
 * view), so the dive adds no rotation rule - the snapshot's spin is the facing
 * delta of the step in every state, and a reader that adds the spins up holds
 * the body's yaw. The look's own stream here turns and wraps past PI, so the
 * delta is the wrapped one the spin is built from. */
static void
CheckDiveSpin(void)
{
	CPlayerMoveSolver diveSolver, groundSolver;
	SMoveIntent dive = AirIntent(0.0f, 0.3f);
	SMoveIntent ground = WalkIntent(0.0f, 0.3f);
	float facing = 0.0f, previous = 0.0f;
	int diveDiffs = 0, groundDiffs = 0;

	/* both solvers see the same facing stream from the same frame: the dive
	 * entered at facing 0, the ground's own first step also at facing 0 */
	dive.vel = SMoveVec2{ 0.0f, 0.25f };
	(void)StepIntoDive(&diveSolver, 0.0f, 0.3f, dive.vel);
	ground.vel = SMoveVec2{ 0.0f, 0.25f };
	(void)groundSolver.Step(ground);

	for (int i = 0; i < 60; i++) {
		float delta;

		/* the look turns at a constant rate and wraps: a body that faces the
		 * camera crosses the half turn without a snap */
		facing += 0.25f;
		if (facing > 3.0f)
			facing -= 6.28f;
		delta = RefLimitRadianAngle(facing - previous);
		previous = facing;

		dive.facing = facing;
		dive.vel = SMoveVec2{ 0.0f, 0.25f };
		if (diveSolver.Step(dive).spin != delta)
			diveDiffs++;
		ground.facing = facing;
		ground.vel = SMoveVec2{ 0.0f, 0.25f };
		if (groundSolver.Step(ground).spin != delta)
			groundDiffs++;
	}
	Check(diveDiffs == 0,
		"the dive's spin is the facing delta of the step, bit for bit: the solver reports the body's turn and adds none");
	Check(groundDiffs == 0,
		"the ground's spin is the same facing delta: the state adds no rotation rule");
	printf("  the look turned 60 frames and wrapped: the dive's spin and the ground's are the one facing delta\n");
}

/* The landing: the feet touching the ground leave the dive for the slide - the
 * slide-out, whose own row and step bleed the speed - and the stance rows of
 * the touchdown put the body in the prone on the same frame. The slide-out
 * decays with the slide's own curve, its own exit ends it and the body is prone
 * on the ground when it does. */
static void
CheckDiveLanding(void)
{
	CPlayerMoveSolver solver;
	SMoveIntent intent = AirIntent(0.0f, 0.3f);
	SMoveSnapshot snapshot = StepIntoDive(&solver, 0.0f, 0.3f, SMoveVec2{ 0.0f, DIVE_SPRINT_SPEED });
	float diveSpeed = snapshot.speed;
	float previous, exitIn = 0.0f;
	int frames = 0, rises = 0;

	Check(snapshot.state == MOVE_DIVE, "the dive holds while the feet are off the ground");
	Check(snapshot.stance == STANCE_STAND, "the dive keeps the standing stance in the air");

	/* the feet touch: the slide-out and the prone, in the one frame */
	intent.vel = snapshot.moved;
	intent.onGround = true;
	snapshot = solver.Step(intent);
	Check(snapshot.state == MOVE_SLIDE,
		"the dive's feet touching the ground land it in the slide-out: the slide's own row and step");
	Check(snapshot.stance == STANCE_PRONE,
		"the same frame's stance rows lie the body down: the dive's touchdown is the prone entry");
	Check((snapshot.events & EVENT_LANDED) != 0, "the dive's landing raises the landing event once");
	Check(snapshot.speed >= diveSpeed, "the landing starts from the speed the dive had");

	/* the slide-out: the slide's own curve, its own exit speed and the prone
	 * at the end of it */
	previous = snapshot.speed;
	while (frames < 600 && snapshot.state == MOVE_SLIDE) {
		float own = snapshot.speed;

		intent.vel = snapshot.moved;
		snapshot = solver.Step(intent);
		frames++;
		if (snapshot.state != MOVE_SLIDE) {
			exitIn = own;
			break;
		}
		if (snapshot.speed > previous + 1e-6f)
			rises++;
		previous = snapshot.speed;
	}
	Check(frames < 600, "the dive's slide-out reaches its exit: it does not run forever");
	Check(rises == 0, "the dive's slide-out decays monotonically with the slide's own curve");
	Check(exitIn < SLIDE_EXIT_SPEED, "the slide-out's exit fires below the slide's own exit speed");
	Check(snapshot.state == MOVE_GROUND, "the dive's slide-out ends on the ground");
	Check(snapshot.stance == STANCE_PRONE,
		"the body is prone when the slide-out ends: a dive reaches the prone on landing");
	printf("  the dive at %.5f landed and slid out over %d frames, ending on a step of %.5f below %.2f\n",
		diveSpeed, frames, exitIn, SLIDE_EXIT_SPEED);
}

/* The exits: the state leaves the dive on the ground alone (a wall and a head
 * hit have no rows - those triggers have no producer in the tree yet), and the
 * slide-out's own exits are the slide's: the jump leaves for the air with its
 * speed, and the prone request ends it on the ground with the prone's own
 * toggle standing the body up (VICE-177). */
static void
CheckDiveExits(void)
{
	/* (a) the jump out of the dive-out: the slide's own row, and the air keeps
	 *     the speed the slide-out had */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = AirIntent(0.0f, 0.3f);
		SMoveSnapshot snapshot = StepIntoDive(&solver, 0.0f, 0.3f, SMoveVec2{ 0.0f, DIVE_SPRINT_SPEED });
		float outSpeed;

		intent.vel = snapshot.moved;
		intent.onGround = true;
		snapshot = solver.Step(intent);
		outSpeed = snapshot.speed;
		Check(snapshot.state == MOVE_SLIDE && snapshot.stance == STANCE_PRONE,
			"the dive-out holds the body prone before the jump");

		intent.jumpRequested = true;
		intent.vel = snapshot.moved;
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_AIR, "the jump leaves the dive-out for the air");
		Check(snapshot.speed == outSpeed, "the dive-out's jump carries its speed into the air, bit for bit");
		Check(snapshot.stance == STANCE_STAND,
			"the jump stands the prone body up: the prone's own jump rule (VICE-177) applies to the dive-out too");
	}

	/* (b) the prone request: the slide-out ends on the ground and the prone's
	 *     own edge stands the body up (the toggle, VICE-177) */
	{
		CPlayerMoveSolver solver;
		SMoveIntent intent = AirIntent(0.0f, 0.3f);
		SMoveSnapshot snapshot = StepIntoDive(&solver, 0.0f, 0.3f, SMoveVec2{ 0.0f, DIVE_SPRINT_SPEED });

		intent.vel = snapshot.moved;
		intent.onGround = true;
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_SLIDE && snapshot.stance == STANCE_PRONE, "the dive-out holds the body prone");

		intent.proneRequested = true;
		intent.vel = snapshot.moved;
		snapshot = solver.Step(intent);
		Check(snapshot.state == MOVE_GROUND, "a prone request ends the dive-out on the ground");
		Check(snapshot.stance == STANCE_STAND,
			"the prone's own edge stands the body up: the request is the toggle it is on the ground");
	}
}

/* The dive's own determinism: two solvers driven with the same dive stream -
 * the entry, the turn, the landing and the slide-out - agree field for field,
 * the slide-out's entry boost between the frames included. */
static void
CheckDiveDeterminism(void)
{
	CPlayerMoveSolver first, second;
	SMoveVec2 liveFirst = SMoveVec2{ 0.0f, 0.25f };
	SMoveVec2 liveSecond = SMoveVec2{ 0.0f, 0.25f };
	int diffs = 0;

	for (int i = 0; i < DETERMINISM_STEPS; i++) {
		SMoveIntent intent = AirIntent(0.0f, 0.3f);
		SMoveSnapshot a, b;

		if (i > 0 && !SameSnapshot(first.GetSnapshot(), second.GetSnapshot()))
			diffs++;
		intent.facing = 0.07f * (float)i;
		intent.diveRequested = (i == 5);
		intent.onGround = i >= 30;
		intent.hasInput = (i % 2) == 0;
		intent.vel = liveFirst;
		a = first.Step(intent);
		intent.vel = liveSecond;
		b = second.Step(intent);
		liveFirst = a.moved;
		liveSecond = b.moved;
	}
	Check(diffs == 0, "two solvers driven with the same dive stream agree field for field");
}

static void
CheckDive(void)
{
	CheckDiveEntry();
	CheckDiveCarriedSpeed();
	CheckDiveSpin();
	CheckDiveLanding();
	CheckDiveExits();
	CheckDiveDeterminism();
}

/* ---------------------------------------------------------------------- *
 * 11. the jump (VICE-182)
 * ---------------------------------------------------------------------- */

/*
 * The jump is the input's, not the launch animation's. The stock player's
 * impulse sat in the launch animation's finish callback (CPed::FinishLaunchCB:
 * ApplyMoveForce(0, 0, 8.5) for the player), so the ped left the ground only
 * when the animation reached the callback. VICE-182 moves the impulse to the
 * adapter and gives the solver the ground's own jump row: the press leaves the
 * ground on the frame it is read, the step reports EVENT_JUMP, and the adapter
 * writes the impulse on that frame. The checks below prove the solver's half:
 * the pressed frame is the air's, the event is raised once, the impulse is the
 * stock 8.5, the landing clears the air facts and leaves no state behind, and a
 * press in the air is no second launch.
 *
 * The adapter's half is the one write at the event
 * (CPlayerPed::PlayerJumpOrDive, CPlayerPed::PlayerControlFighter):
 *
 *	ApplyMoveForce(0.0f, 0.0f, MOVE_PLAYER_JUMP_IMPULSE);
 *	bIsStanding = false;
 *	bIsInTheAir = true;
 *
 * The launch callback keeps no player impulse, so the player's jump has one
 * source.
 */

#define STOCK_PLAYER_JUMP_IMPULSE	8.5f	/* CPed::FinishLaunchCB's player branch before VICE-182 */

/* The press on the first frame leaves the ground on that frame, with the jump
 * event and the horizontal velocity untouched (the impulse is vertical and the
 * adapter's); the module's impulse is the stock callback's magnitude. */
static void
CheckJumpFrame(void)
{
	CPlayerMoveSolver solver;
	SMoveIntent intent = WalkIntent(0.0f, 0.30f);
	SMoveSnapshot snapshot;

	intent.hasInput = false;
	intent.vel = SMoveVec2{ 0.0f, 0.20f };
	snapshot = solver.Step(intent);
	Check(snapshot.state == MOVE_GROUND, "the ped stands on the ground before the jump");

	intent.jumpRequested = true;
	intent.vel = snapshot.moved;
	snapshot = solver.Step(intent);
	Check(snapshot.state == MOVE_AIR, "the jump edge leaves the ground on the frame it is pressed");
	Check((snapshot.events & EVENT_JUMP) != 0,
		"the pressed frame reports the jump event: the adapter writes the impulse there");
	Check(snapshot.moved.x == intent.vel.x && snapshot.moved.y == intent.vel.y,
		"the jump resets no velocity: the horizontal speed crosses the edge bit for bit");
	Check(MOVE_PLAYER_JUMP_IMPULSE == STOCK_PLAYER_JUMP_IMPULSE,
		"the player's jump impulse is the stock launch callback's 8.5, bit for bit");
	printf("  the jump: the pressed frame is the air's, %.1f engine units of vertical impulse, no horizontal reset\n",
		(double)MOVE_PLAYER_JUMP_IMPULSE);
}

/* The whole jump: one event on the press, the air for every frame in between,
 * one landing on the frame the feet return, and the ground state with its
 * contact after it - no air fact is left set. A press in the air is no launch:
 * the state, the velocity and the events stay the quiet stream's. */
static void
CheckJumpLanding(void)
{
	{
		CPlayerMoveSolver solver;
		SMoveVec2 live = { 0.0f, 0.20f };
		SMoveSnapshot landing = SMoveSnapshot{};
		int jumpEvents = 0;
		int jumpFrame = -1;
		int landFrame = -1;
		int airFrames = 0;
		int airBad = 0;
		int groundBad = 0;

		for (int i = 0; i < 40; i++) {
			SMoveIntent intent = WalkIntent(0.0f, 0.30f);
			bool ground = i == 0 || i >= 20;

			intent.hasInput = false;
			intent.vel = live;
			intent.onGround = ground;
			intent.jumpRequested = i == 0;
			const SMoveSnapshot &snapshot = solver.Step(intent);
			if (snapshot.events & EVENT_JUMP) {
				jumpEvents++;
				jumpFrame = i;
			}
			if (snapshot.events & EVENT_LANDED) {
				landFrame = i;
				landing = snapshot;
			}
			if (i > 0 && i < 20) {
				airFrames++;
				if (snapshot.state != MOVE_AIR || snapshot.contacts != 0)
					airBad++;
			}
			if (i >= 20 && (snapshot.state != MOVE_GROUND || (snapshot.contacts & CONTACT_GROUND) == 0))
				groundBad++;
			live = snapshot.moved;
		}
		Check(jumpEvents == 1 && jumpFrame == 0, "the jump fires once, on the input frame");
		Check(airFrames == 19 && airBad == 0,
			"every frame between the press and the landing is the air's, with no contact");
		Check(landFrame == 20, "the landing fires on the frame the feet return, once");
		Check(landing.state == MOVE_GROUND && (landing.contacts & CONTACT_GROUND) != 0,
			"the landing clears the air state and reports the ground contact");
		Check((landing.events & EVENT_JUMP) == 0,
			"the landing raises no jump event: the launch is the press's, not the ground's");
		Check(groundBad == 0, "the ground state holds after the landing: no air state is left set");
	}

	/* a press in the air: the same quiet stream, bit for bit - no second launch */
	{
		CPlayerMoveSolver quiet, edged;
		SMoveIntent intent = AirIntent(0.0f, 0.30f);
		SMoveVec2 quietLive = { 0.0f, 0.20f };
		SMoveVec2 edgedLive = { 0.0f, 0.20f };
		int diffs = 0;

		intent.hasInput = false;
		intent.vel = quietLive;
		(void)quiet.Step(intent);
		intent.vel = edgedLive;
		(void)edged.Step(intent);
		Check(quiet.GetSnapshot().state == MOVE_AIR && edged.GetSnapshot().state == MOVE_AIR,
			"the ped is in the air before the air's press");

		for (int i = 0; i < 20; i++) {
			SMoveIntent q = intent;
			SMoveIntent e = intent;
			SMoveSnapshot a, b;

			q.vel = quietLive;
			e.vel = edgedLive;
			e.jumpRequested = true;
			a = quiet.Step(q);
			b = edged.Step(e);
			if (!SameSnapshot(a, b))
				diffs++;
			quietLive = a.moved;
			edgedLive = b.moved;
		}
		Check(diffs == 0, "a press in the air changes nothing: the jump has no air row, no second launch");
	}
}

static void
CheckJump(void)
{
	CheckJumpFrame();
	CheckJumpLanding();
}

/* ---------------------------------------------------------------------- *
 * main
 * ---------------------------------------------------------------------- */

int
main(void)
{
	printf("== the movement solver (VICE-172): the harness and the no-animation-lock invariant\n\n");

	printf("== 1. the parity against the pre-rework maths (ef9b65f5)\n");
	CheckParitySweep();
	CheckParityTargeted();

	printf("\n== 2. the no-animation-lock invariant\n");
	CheckStructural();
	CheckBehavioural();

	printf("\n== 3. determinism\n");
	CheckDeterminism();

	printf("\n== 4. the tables (VICE-171): the invariants and the planted rejections\n");
	CheckTables();

	printf("\n== 5. the events (VICE-173's list, raised by VICE-171's rows)\n");
	CheckEvents();

	printf("\n== 6. finiteness\n");
	CheckFiniteness();

	printf("\n== 7. the slide (VICE-180): the entry, the curve, the exits and the jump\n");
	CheckSlide();
	printf("\n== 8. the stance (VICE-176): the crouch row, the stance moves and the headroom\n");
	CheckStance();
	printf("\n== 9. the prone (VICE-177): the crawl row, the ground entry, the low ladder and the headroom\n");
	CheckProne();
	printf("\n== 10. the dive (VICE-181): the entry, the spin, the landing and the slide-out\n");
	CheckDive();
	printf("\n== 11. the jump (VICE-182): the input frame, the impulse, the landing and the air's press\n");
	CheckJump();

	printf("\nRESULT: %s (%d checks, %d failures)\n",
		g_failures == 0 ? "every check passed" : "FAILURES", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
