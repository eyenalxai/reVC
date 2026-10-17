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
 *	E. The tables. PlayerMoveTablesCheck (VICE-171, movement/p0-tables) is
 *	   not in the tree yet; until it lands, the check holds what exists: the
 *	   design's four states and three stances are named and distinct, the
 *	   phase 0 stream uses the ground and the air, and the slide and the dive
 *	   are unreachable in phase 0.
 *
 *	F. The events. The event list is VICE-173 and the jump counter is
 *	   VICE-182; the module of phase 0 reports no edge. The check holds that
 *	   a landing in the stream raises no bit yet, that the state follows the
 *	   ground, and that the jump and dive edges change no output. It must be
 *	   extended when VICE-173 lands.
 *
 *	G. The finiteness. Every output is finite for dt in {0, 1/50, 1/10}, the
 *	   pad's extreme (the target speed at 2 engine units) and the zero input,
 *	   on the ground, in the air and on the steep slope.
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
	intent.faceCamera = RngChance(50);
	intent.jumpRequested = RngChance(10);
	intent.diveRequested = RngChance(10);
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
	intent.faceCamera = false;
	intent.jumpRequested = false;
	intent.diveRequested = false;
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
	snapshot.inCharge = true;
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
		a.turnScale == b.turnScale && a.capsuleHeight == b.capsuleHeight &&
		a.inCharge == b.inCharge;
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
		a.inCharge == b.inCharge && a.events == b.events &&
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
	printf("      ground %d steep %d input %d sprint %d faceCamera %d jump %d dive %d\n",
		intent.onGround, intent.steepCliff, intent.hasInput,
		intent.tier, intent.faceCamera, intent.jumpRequested, intent.diveRequested);
}

static void
PrintSnapshot(const char *name, const SMoveSnapshot &snapshot)
{
	printf("    %s: state %d stance %d velocity (%.9g, %.9g) moved (%.9g, %.9g)\n",
		name, snapshot.state, snapshot.stance,
		snapshot.velocity.x, snapshot.velocity.y, snapshot.moved.x, snapshot.moved.y);
	printf("      speed %.9g heading %.9g turnScale %.9g capsule %.9g inCharge %d events %u report %u\n",
		snapshot.speed, snapshot.heading, snapshot.turnScale, snapshot.capsuleHeight,
		snapshot.inCharge, snapshot.events, snapshot.reportMoveState);
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
 * VICE-171 (movement/p0-tables) will export PlayerMoveTablesCheck and this
 * check will call it. It is not in the tree yet (the module keeps its rows in
 * moveStates, a private static array), so the harness holds what exists: the
 * design's four states and three stances are named and distinct, the phase 0
 * stream reaches the ground and the air, and the slide and the dive - whose
 * rows are zero and whose transitions arrive with VICE-180/VICE-181 - are
 * unreachable in phase 0.
 */
/* The check's own table set: the module's phase 0 rows in a copy a defect can
 * be planted in. PlantSet rebuilds every field, so each case starts clean. */
static tMoveStateRow g_plantStates[MOVE_NUMSTATES];
static tMoveLadderRow g_plantLadder[TIER_NUM];
static tMoveStanceRow g_plantStances[STANCE_NUM];
static tMoveTransitionRow g_plantTransitions[4];

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
	g_plantTransitions[0].from = MOVE_GROUND; g_plantTransitions[0].trigger = TRIGGER_AIRBORNE;
	g_plantTransitions[0].to = MOVE_AIR; g_plantTransitions[0].onEntryEvent = 0;
	g_plantTransitions[1].from = MOVE_AIR; g_plantTransitions[1].trigger = TRIGGER_LAND;
	g_plantTransitions[1].to = MOVE_GROUND; g_plantTransitions[1].onEntryEvent = EVENT_LANDED;
	g_plantTransitions[2].from = MOVE_GROUND; g_plantTransitions[2].trigger = TRIGGER_JUMP;
	g_plantTransitions[2].to = MOVE_AIR; g_plantTransitions[2].onEntryEvent = 0;
	g_plantTransitions[3].from = MOVE_AIR; g_plantTransitions[3].trigger = TRIGGER_DIVE;
	g_plantTransitions[3].to = MOVE_DIVE; g_plantTransitions[3].onEntryEvent = 0;

	t.states = g_plantStates; t.numStates = MOVE_NUMSTATES;
	t.ladder = g_plantLadder; t.numLadder = TIER_NUM;
	t.stances = g_plantStances; t.numStances = STANCE_NUM;
	t.transitions = g_plantTransitions; t.numTransitions = 4;
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
		"the slide and the dive are unreachable in phase 0");
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
		REJECTS(broken.numStances = STANCE_NUM - 1,
			"a stance without a row is refused");
		REJECTS(g_plantStances[STANCE_CROUCH].turnScale = -1.0f,
			"a stance row out of range is refused");
		REJECTS(g_plantTransitions[3].to = MOVE_NUMSTATES,
			"a transition to a state out of range is refused");
		REJECTS(g_plantTransitions[3].trigger = 0,
			"a transition trigger out of range is refused");
	}
}

/* ---------------------------------------------------------------------- *
 * 5. the events
 * ---------------------------------------------------------------------- */

#define EVENT_STREAM_STEPS	60

/*
 * The event bits are VICE-173's list and VICE-171's transition rows raise them:
 * a landing raises EVENT_LANDED once, and no other bit fires in phase 0. The
 * rows that carry the jump, the dive and the wall events wait for the states
 * they move to (VICE-181, VICE-182), and the jump counter is VICE-182's.
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

	/* the jump and the dive edges: no reader in phase 0, so they change
	 * nothing (the jump counter arrives with VICE-182) */
	{
		CPlayerMoveSolver quiet, edged;
		SMoveVec2 liveQuiet = { 0.7f, 0.2f };
		SMoveVec2 liveEdged = { 0.7f, 0.2f };
		int edgeDiffs = 0;

		for (int i = 0; i < EVENT_STREAM_STEPS; i++) {
			SMoveIntent intentQuiet = WalkIntent(0.25f, 0.3f);
			SMoveIntent intentEdged;

			intentQuiet.vel = liveQuiet;
			intentQuiet.onGround = (i % 7) != 3;
			intentEdged = intentQuiet;
			intentEdged.vel = liveEdged;
			intentEdged.jumpRequested = true;
			intentEdged.diveRequested = true;
			const SMoveSnapshot &snapshotQuiet = quiet.Step(intentQuiet);
			const SMoveSnapshot &snapshotEdged = edged.Step(intentEdged);
			liveQuiet = snapshotQuiet.moved;
			liveEdged = snapshotEdged.moved;
			if (!SameSnapshot(snapshotQuiet, snapshotEdged))
				edgeDiffs++;
		}
		Check(edgeDiffs == 0, "the jump and the dive edges change no output in phase 0");
	}

	printf("  the landings in the stream raise no bit yet, and the jump counter does not exist\n");
	printf("  the jump counter and the air extensions arrive with VICE-182\n");
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
	printf("  absence: every velocity, moved, speed, heading, turn scale and capsule finite\n");
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

	printf("\nRESULT: %s (%d checks, %d failures)\n",
		g_failures == 0 ? "every check passed" : "FAILURES", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
