/*
 * The evidence program for the parked generators (ticket VICE-155).
 *
 * It compiles the service's own engine-free header (src/control/SpawnService.h,
 * the parked path) and drives it over a synthetic generator set. No engine
 * header is included anywhere: the world work of the parked path's stages (the
 * blockage cast, the model request, the ground casts and the car, the ladder)
 * is the game's, and this program scripts the facts the engine fills
 * (SSpawnParkedFacts) - the same seam the engine side (SpawnService.cpp,
 * SpawnParkedResolveStage) fills from the world.
 *
 *	A. The target and the budget gate. ParkedCarTarget over the ring scales,
 *	   and the gate's truth table: below the target passes, at it and above
 *	   refuses BUDGET, the car budget refuses BUDGET too, and the gate runs
 *	   before any world work (a budget refusal costs no stage).
 *
 *	B. The model resolution. The fixed model passes through; a dynamic
 *	   generator reuses its stored model while it is resident; the empty
 *	   field (-1) and an evicted stored model draw a fresh one from the
 *	   policy; the re-choice stores -model and clears the decision's
 *	   colours (the old car's paint does not follow a new model); a policy
 *	   with nothing refuses READINESS and leaves the field standing; the
 *	   stages after the resolution work with the resolved model; the
 *	   readiness refusals and passes move the purpose's wait clock.
 *
 *	C. The placement gates. The blockage (OCCUPANCY), the readiness
 *	   (READINESS), the ground (NO_SITE plus the primitive's defect row) and
 *	   the bubble arm table (the ray-free arms; NONE, OCCLUDER and RAY
 *	   refuse VISIBILITY) - each gate alone, the order when several facts are
 *	   bad at once, and the world work each refusal spends.
 *
 *	D. The request and the ledger. The request rows by purpose (parked) and
 *	   kind (parked), the created row, the refusals by reason, and the
 *	   decision's contract: the parked tag, the fresh-spawn grace, the
 *	   placement the ground resolved and the generator's angle. The parked
 *	   kind carries no driver (the contract: the tag, never SetUpDriver).
 *
 *	E. The soak. A synthetic generator set of the map's own count, driven
 *	   through the parked path over a scripted world: the parked count rises
 *	   to ParkedCarTarget, never passes it (the budget gate is the cap), a
 *	   taken car drops it and the fill brings it back - the acceptance's
 *	   "the parked counts match the target in a soak". The service's ledger
 *	   is checked against the soak's own counts (created = live + taken,
 *	   requests = created + refusals, the defect row = the NO_SITE row), and
 *	   the generators' policy state (position, angle, the thresholds, the
 *	   uses) round-trips the requests untouched - the save/load rebuild's
 *	   own property (the service is stateless per generator).
 *
 * What this program cannot reach, stated plainly: the engine side
 * (SpawnService.cpp) - the casts, the model request, the vehicle and the
 * ladder - is the game's, checked by the build and by the live soak; the
 * generators' timer and range policy (CarGen.cpp) is the game's too, so the
 * soak scripts the requests it makes and the state updates it would apply,
 * in the same order (the model re-choice on every call, the colour store on a
 * creation, the blocking flag and the +4 on a blockage, the taken-car wait).
 * The grep gate (no dice and no placement decision in CarGen.cpp) is run
 * outside this program; the commands are in the ticket.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 \
 *	    -o /tmp/spawn-parked utils/spawn-service/parked.cpp
 *	/tmp/spawn-parked
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "SpawnService.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures;
static long long g_checks;

static void
Fail(const char *what)
{
	g_failures++;
	printf("  FAIL: %s\n", what);
}

static void
Check(bool ok, const char *what)
{
	g_checks++;
	if (!ok)
		Fail(what);
}

static void
CheckInt(int got, int want, const char *what)
{
	g_checks++;
	if (got != want) {
		g_failures++;
		printf("  FAIL: %s (want %d, got %d)\n", what, want, got);
	}
}

static void
CheckNear(float got, float want, const char *what)
{
	g_checks++;
	if (fabsf(got - want) > 0.001f) {
		g_failures++;
		printf("  FAIL: %s (want %f, got %f)\n", what, want, got);
	}
}

static double
NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ the script */

/*
 * What the world answers for one request. The engine's stages fill the same
 * SSpawnParkedFacts from the world (SpawnParkedResolveStage); this is the
 * seam, so every fact is scriptable per case. The stage log is the harness's
 * own: which stages ran (the order and the work a refusal spends).
 */
struct SWorld
{
	const SSpawnParkedRequest *pRequest;
	bool storedModelResident;	/* the model stage */
	bool blocked;			/* the blockage stage */
	bool modelResident;		/* the ready stage */
	bool groundFound;		/* the ground stage */
	float groundZ;
	float heightAboveRoad;
	uint8_t arm;			/* the bubble stage */

	/* the log */
	int32_t stagesRun;
	bool stageSeen[SPAWN_PARKED_STAGE_COUNT];
	int32_t modelOfLastStage;
};

static void
WorldDefaults(SWorld *w, const SSpawnParkedRequest *pRequest)
{
	w->pRequest = pRequest;
	w->storedModelResident = true;
	w->blocked = false;
	w->modelResident = true;
	w->groundFound = true;
	w->groundZ = 5.0f;
	w->heightAboveRoad = 0.5f;
	w->arm = NEAR_BUBBLE_ARM_SOFT;
	w->stagesRun = 0;
	memset(w->stageSeen, 0, sizeof(w->stageSeen));
	w->modelOfLastStage = -1;
}

static void
WorldStage(void *user, ESpawnParkedStage stage, int32_t model, SSpawnParkedFacts *facts)
{
	SWorld *w = (SWorld *)user;

	w->stagesRun++;
	w->stageSeen[stage] = true;
	switch (stage) {
	case SPAWN_PARKED_STAGE_MODEL:
		/* The stage's model argument is the request's own field (the
		 * resolution has not run yet). */
		facts->storedModelLoaded = w->storedModelResident;
		break;
	case SPAWN_PARKED_STAGE_BLOCKAGE:
		w->modelOfLastStage = model;
		facts->blocked = w->blocked;
		break;
	case SPAWN_PARKED_STAGE_READINESS:
		facts->modelLoaded = w->modelResident;
		break;
	case SPAWN_PARKED_STAGE_GROUND:
		facts->groundFound = w->groundFound;
		if (w->groundFound) {
			facts->x = w->pRequest->x;
			facts->y = w->pRequest->y;
			facts->z = w->groundZ + w->heightAboveRoad;
		}
		break;
	case SPAWN_PARKED_STAGE_BUBBLE:
		facts->arm = w->arm;
		break;
	default:
		break;
	}
}

/* The requester's dynamic model policy: the engine draws from the zone's
 * loaded class arrays (SpawnParkedChooseModel); the harness scripts it. */
struct SPolicy
{
	int32_t model;		/* -1: nothing to place */
	int32_t calls;
};

static int32_t
PolicyChoose(void *user)
{
	SPolicy *p = (SPolicy *)user;

	p->calls++;
	return p->model;
}

static SSpawnParkedRequest
MakeRequest(int32_t modelIndex)
{
	SSpawnParkedRequest r;

	r.purpose = SPAWN_PURPOSE_PARKED;
	r.modelIndex = modelIndex;
	r.x = 100.0f;
	r.y = 200.0f;
	r.z = 5.0f;
	r.angle = 90.0f;
	r.color1 = -1;
	r.color2 = -1;
	r.alarm = 0;
	r.doorlock = 0;
	return r;
}

static SSpawnParkedBudget
MakeBudget(int32_t parkedCars, int32_t parkedTarget)
{
	SSpawnParkedBudget b;

	b.parkedCars = parkedCars;
	b.parkedTarget = parkedTarget;
	b.carsInUse = parkedCars + 40;
	b.carBudget = MAX_CARS_IN_USE;
	return b;
}

/* The scripted clock: the readiness waits read it (the engine passes the
 * game's milliseconds; the soak passes its frame clock). */
static uint32_t g_nowMs = 1000;

static ESpawnRefusal
RunParkedAt(const SSpawnParkedRequest &request, const SSpawnParkedBudget &budget, uint32_t nowMs,
	SWorld *w, SPolicy *p, SSpawnParkedDecision *decision, SSpawnServiceTelemetry *tel)
{
	WorldDefaults(w, &request);
	p->calls = 0;
	return SpawnRunParked(request, budget, nowMs, PolicyChoose, p, WorldStage, w, decision, tel);
}

/* One request through the whole parked path, over the scripted world, at the
 * scripted clock. */
static ESpawnRefusal
RunParked(const SSpawnParkedRequest &request, const SSpawnParkedBudget &budget,
	SWorld *w, SPolicy *p, SSpawnParkedDecision *decision, SSpawnServiceTelemetry *tel)
{
	return RunParkedAt(request, budget, g_nowMs, w, p, decision, tel);
}

/* ------------------------------------------------------------------ section A */

static void
SectionTarget(void)
{
	SSpawnParkedRequest r = MakeRequest(400);
	SSpawnParkedBudget b;
	SSpawnParkedDecision d;
	SSpawnServiceTelemetry tel;
	SWorld w;
	SPolicy p;

	printf("A. the parked target and the budget gate\n");
	CheckInt(ParkedCarTarget(1.0f), 20, "A1: the stock ring holds 20 parked cars");
	CheckInt(ParkedCarTarget(1.5f), 45, "A2: the target follows the ring area (scale^2)");
	CheckInt(ParkedCarTarget(2.0f), 80, "A3: the capped ring holds 80");
	CheckInt(ParkedCarTarget(4.0f), NUM_CARGENS, "A4: the target stops at the generator count");
	printf("  ParkedCarTarget: 1.0x -> %d, 1.5x -> %d, 2.0x -> %d, 4.0x -> %d (NUM_CARGENS %d)\n",
		ParkedCarTarget(1.0f), ParkedCarTarget(1.5f), ParkedCarTarget(2.0f), ParkedCarTarget(4.0f), NUM_CARGENS);

	/* Below the target the path runs. */
	SpawnTelemetryReset(&tel);
	b = MakeBudget(19, 20);
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_NONE, "A5: one below the target passes");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_PARKED], 0, "A6: the core does not count the creation (the engine's own row)");

	/* At the target and above it: BUDGET, before any world work. */
	SpawnTelemetryReset(&tel);
	b = MakeBudget(20, 20);
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_BUDGET, "A7: at the target refuses BUDGET");
	CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_BUDGET], 1, "A8: the refusal is counted once");
	CheckInt(w.stagesRun, 0, "A9: the budget gate runs before any world work");

	SpawnTelemetryReset(&tel);
	b = MakeBudget(21, 20);
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_BUDGET, "A10: above the target refuses BUDGET");

	/* The car budget: NumCarsInUse at the top refuses too. */
	SpawnTelemetryReset(&tel);
	b = MakeBudget(3, 20);
	b.carsInUse = b.carBudget;
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_BUDGET, "A11: a full car budget refuses BUDGET");
	CheckInt(w.stagesRun, 0, "A12: no world work for a car-budget refusal");
}

/* ------------------------------------------------------------------ section B */

static void
SectionModel(void)
{
	SSpawnParkedRequest r;
	SSpawnParkedBudget b = MakeBudget(3, 20);
	SSpawnParkedDecision d;
	SSpawnServiceTelemetry tel;
	SWorld w;
	SPolicy p;

	printf("B. the model resolution (the dynamic policy)\n");

	/* The fixed model passes through untouched. */
	SpawnTelemetryReset(&tel);
	r = MakeRequest(400);
	p.model = 500;
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_NONE, "B1: a fixed model passes");
	CheckInt(d.modelIndex, 400, "B2: the fixed field stands");
	Check(d.modelRechosen == false, "B3: no re-choice for a fixed model");
	CheckInt(p.calls, 0, "B4: the policy is not called for a fixed model");
	CheckInt(w.modelOfLastStage, 400, "B5: the stages work with the fixed model");

	/* A dynamic generator reuses its stored model while it is resident. */
	SpawnTelemetryReset(&tel);
	r = MakeRequest(-400);
	p.model = 500;
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_NONE, "B6: a stored dynamic model passes");
	CheckInt(d.modelIndex, -400, "B7: the stored field stands");
	Check(d.modelRechosen == false, "B8: no re-choice while the stored model is resident");
	CheckInt(p.calls, 0, "B9: the policy is not called");
	CheckInt(w.modelOfLastStage, 400, "B10: the stages work with the stored model");

	/* The empty field (-1) draws a fresh model. */
	SpawnTelemetryReset(&tel);
	r = MakeRequest(-1);
	p.model = 500;
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_NONE, "B11: the empty field draws");
	CheckInt(p.calls, 1, "B12: the policy is called once");
	Check(d.modelRechosen == true, "B13: the re-choice is flagged");
	CheckInt(d.modelIndex, -500, "B14: the new field stores -model");
	CheckInt(w.modelOfLastStage, 500, "B15: the stages work with the drawn model");

	/* An evicted stored model draws too. */
	SpawnTelemetryReset(&tel);
	r = MakeRequest(-400);
	p.model = 501;
	p.calls = 0;
	WorldDefaults(&w, &r);
	w.storedModelResident = false;
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_NONE, "B16: an evicted stored model draws");
	CheckInt(p.calls, 1, "B17: the policy is called once");
	CheckInt(d.modelIndex, -501, "B18: the new field stores the drawn model");

	/* The policy with nothing to place: READINESS, the field stands. */
	SpawnTelemetryReset(&tel);
	r = MakeRequest(-1);
	p.model = -1;
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_READINESS, "B19: an empty policy refuses READINESS");
	CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_READINESS], 1, "B20: the refusal is counted once");
	Check(d.modelRechosen == false, "B21: no re-choice on the refusal");
	CheckInt(d.modelIndex, -1, "B22: the field stands on the refusal");
	CheckInt(w.stagesRun, 1, "B23: only the model stage ran");

	/* The readiness of the resolved model: the fixed path's own gate. */
	SpawnTelemetryReset(&tel);
	r = MakeRequest(400);
	p.calls = 0;
	WorldDefaults(&w, &r);
	w.modelResident = false;
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_READINESS, "B24: a non-resident model refuses READINESS");
	Check(w.stageSeen[SPAWN_PARKED_STAGE_READINESS], "B25: the readiness stage ran");
	Check(!w.stageSeen[SPAWN_PARKED_STAGE_GROUND], "B26: the ground stage did not run");

	/* The colours: the request's own ride with the decision; a re-choice
	 * clears them (the review's defect: the old car's paint must not follow
	 * a new model). */
	SpawnTelemetryReset(&tel);
	r = MakeRequest(400);
	r.color1 = 12;
	r.color2 = 34;
	p.model = 500;
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_NONE, "B27: a fixed model with colours passes");
	CheckInt(d.color1, 12, "B28: the decision carries the request's colour 1");
	CheckInt(d.color2, 34, "B29: the decision carries the request's colour 2");

	SpawnTelemetryReset(&tel);
	r = MakeRequest(-400);
	r.color1 = 12;
	r.color2 = 34;
	p.model = 500;
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_NONE, "B30: a stored model with colours passes");
	Check(d.modelRechosen == false, "B31: no re-choice");
	CheckInt(d.color1, 12, "B32: the stored model keeps the request's colours");

	SpawnTelemetryReset(&tel);
	r = MakeRequest(-1);
	r.color1 = 12;
	r.color2 = 34;
	p.model = 500;
	CheckInt((int)RunParked(r, b, &w, &p, &d, &tel), (int)SPAWN_REFUSAL_NONE, "B33: the empty field with colours draws");
	Check(d.modelRechosen == true, "B34: the re-choice is flagged");
	CheckInt(d.color1, -1, "B35: the re-choice clears colour 1");
	CheckInt(d.color2, -1, "B36: the re-choice clears colour 2");

	SpawnTelemetryReset(&tel);
	r = MakeRequest(-400);
	r.color1 = 12;
	r.color2 = 34;
	p.model = 501;
	WorldDefaults(&w, &r);
	w.storedModelResident = false;
	p.calls = 0;
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_NONE, "B37: an evicted stored model with colours draws");
	Check(d.modelRechosen == true, "B38: the re-choice is flagged");
	CheckInt(d.color1, -1, "B39: the evicted re-choice clears colour 1");
	CheckInt(d.color2, -1, "B40: the evicted re-choice clears colour 2");

	/* The readiness wait rows: a deferral opens the purpose's clock, the
	 * next pass closes it with the milliseconds spent. */
	SpawnTelemetryReset(&tel);
	r = MakeRequest(400);
	p.calls = 0;
	WorldDefaults(&w, &r);
	w.modelResident = false;
	SpawnRunParked(r, b, 1000, PolicyChoose, &p, WorldStage, &w, &d, &tel);
	CheckInt((int)tel.m_nReadinessWaits, 1, "B41: the deferral counts on the wait row");
	CheckInt((int)tel.m_aReadinessSinceMs[SPAWN_PURPOSE_PARKED], 1000, "B42: the wait clock opens at the first deferral");

	WorldDefaults(&w, &r);
	p.calls = 0;
	SpawnRunParked(r, b, 1250, PolicyChoose, &p, WorldStage, &w, &d, &tel);
	CheckInt((int)tel.m_nReadinessWaitMs, 250, "B43: the pass adds the wait's milliseconds");
	CheckInt((int)tel.m_aReadinessSinceMs[SPAWN_PURPOSE_PARKED], 0, "B44: the clock clears on the pass");
}

/* ------------------------------------------------------------------ section C */

static void
SectionGates(void)
{
	SSpawnParkedRequest r = MakeRequest(400);
	SSpawnParkedBudget b = MakeBudget(3, 20);
	SSpawnParkedDecision d;
	SSpawnServiceTelemetry tel;
	SWorld w;
	SPolicy p;
	int32_t i;

	printf("C. the placement gates (and the order)\n");

	/* The bubble arm table: the ray-free arms allow, the rest refuse. */
	{
		static const uint8_t kArms[] = {
			NEAR_BUBBLE_ARM_NONE,
			NEAR_BUBBLE_ARM_BYPASS,
			NEAR_BUBBLE_ARM_OCCLUDER,
			NEAR_BUBBLE_ARM_FADE_DISTANCE,
			NEAR_BUBBLE_ARM_SOFT,
			NEAR_BUBBLE_ARM_RAY,
			NEAR_BUBBLE_ARM_FADE,
			NEAR_BUBBLE_ARM_ONSCREEN_BAND,
		};
		static const bool kAllowed[] = {
			false, true, false, true, true, false, true, true,
		};
		static const char *kNames[] = {
			"none", "bypass", "occluder", "fade distance", "soft", "ray", "fade", "on-screen band",
		};

		for (i = 0; i < (int32_t)(sizeof(kArms) / sizeof(kArms[0])); i++) {
			char what[96];

			SpawnTelemetryReset(&tel);
			WorldDefaults(&w, &r);
			w.arm = kArms[i];
			p.calls = 0;
			sprintf(what, "C1: the %s arm answers its own row", kNames[i]);
			CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel),
				kAllowed[i] ? (int)SPAWN_REFUSAL_NONE : (int)SPAWN_REFUSAL_VISIBILITY, what);
			if (!kAllowed[i]) {
				sprintf(what, "C2: the %s arm refuses VISIBILITY once", kNames[i]);
				CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_VISIBILITY], 1, what);
			}
		}
		printf("  the arm table (rays off): bypass, fade distance, soft, fade, on-screen band allow; none, occluder, ray refuse\n");
	}

	/* The blockage: OCCUPANCY, and the ready stage did not run. */
	SpawnTelemetryReset(&tel);
	WorldDefaults(&w, &r);
	w.blocked = true;
	p.calls = 0;
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_OCCUPANCY, "C3: a blocked spot refuses OCCUPANCY");
	CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_OCCUPANCY], 1, "C4: the refusal is counted once");
	Check(w.stageSeen[SPAWN_PARKED_STAGE_BLOCKAGE], "C5: the blockage stage ran");
	Check(!w.stageSeen[SPAWN_PARKED_STAGE_READINESS], "C6: the ready stage did not run");

	/* The ground: NO_SITE and the primitive's defect row. */
	SpawnTelemetryReset(&tel);
	WorldDefaults(&w, &r);
	w.groundFound = false;
	p.calls = 0;
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_NO_SITE, "C7: no ground refuses NO_SITE");
	CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE], 1, "C8: the refusal is counted once");
	CheckInt((int)tel.m_nGroundRejections, 1, "C9: the defect row counts the ground failure");
	Check(!w.stageSeen[SPAWN_PARKED_STAGE_BUBBLE], "C10: the bubble stage did not run");

	/* The order: the first bad gate wins and the later work is not spent. */
	SpawnTelemetryReset(&tel);
	WorldDefaults(&w, &r);
	w.blocked = true;
	w.groundFound = false;
	w.arm = NEAR_BUBBLE_ARM_NONE;
	p.calls = 0;
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_OCCUPANCY, "C11: the blockage answers before the ground");

	SpawnTelemetryReset(&tel);
	WorldDefaults(&w, &r);
	w.modelResident = false;
	w.groundFound = false;
	p.calls = 0;
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_READINESS, "C12: the readiness answers before the ground");

	SpawnTelemetryReset(&tel);
	WorldDefaults(&w, &r);
	w.groundFound = false;
	w.arm = NEAR_BUBBLE_ARM_NONE;
	p.calls = 0;
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_NO_SITE, "C13: the ground answers before the bubble");

	/* The stage order is the path's own: every stage runs once, in order. */
	SpawnTelemetryReset(&tel);
	WorldDefaults(&w, &r);
	p.calls = 0;
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_NONE, "C14: the passing path creates");
	CheckInt(w.stagesRun, SPAWN_PARKED_STAGE_COUNT, "C15: every stage runs once");
	for (i = 0; i < SPAWN_PARKED_STAGE_COUNT; i++)
		Check(w.stageSeen[i], "C16: every stage ran");
}

/* ------------------------------------------------------------------ section D */

static void
SectionLedger(void)
{
	SSpawnParkedRequest r = MakeRequest(400);
	SSpawnParkedBudget b = MakeBudget(3, 20);
	SSpawnParkedDecision d;
	SSpawnServiceTelemetry tel;
	SWorld w;
	SPolicy p;

	printf("D. the decision's contract and the ledger\n");

	SpawnTelemetryReset(&tel);
	SpawnRequestBegin(&tel, SPAWN_PURPOSE_PARKED, SPAWN_KIND_PARKED);
	p.calls = 0;
	WorldDefaults(&w, &r);
	CheckInt((int)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel), (int)SPAWN_REFUSAL_NONE, "D1: the request creates");
	CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_PARKED], 1, "D2: the request counts by purpose");
	CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_PARKED], 1, "D3: the request counts by kind");

	CheckInt(d.createdBy, SPAWN_VEHICLE_TAG_PARKED, "D4: the parked tag");
	CheckInt(SPAWN_VEHICLE_TAG_PARKED, 3, "D5: the tag is the engine's PARKED_VEHICLE (3)");
	CheckInt((int)d.protectMs, (int)NEAR_BUBBLE_GRACE_MS, "D6: the fresh-spawn grace");
	CheckNear(d.x, r.x, "D7: the placement x");
	CheckNear(d.y, r.y, "D8: the placement y");
	CheckNear(d.z, w.groundZ + w.heightAboveRoad, "D9: the placement z is the ground plus the base offset");
	CheckNear(d.angle, r.angle, "D10: the generator's angle");

	/* The engine's own rows: the creation and the removal by purpose. */
	SpawnNoteCreated(&tel, SPAWN_PURPOSE_PARKED);
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_PARKED], 1, "D11: the created row counts the parked purpose");
	SpawnNoteRemoval(&tel, SPAWN_PURPOSE_PARKED);
	CheckInt((int)tel.m_nRemovedByPurpose[SPAWN_PURPOSE_PARKED], 1, "D12: the removed row counts the parked purpose");

	/* The two kinds and the parked kind's index in the envelope. */
	CheckInt(SPAWN_KIND_ROAD_SITE, 0, "D13: the road-site kind stands first");
	CheckInt(SPAWN_KIND_EXPLICIT, 1, "D14: the explicit kind stands second");
	CheckInt(SPAWN_KIND_PARKED, 2, "D15: the parked kind is the third");
}

/* ------------------------------------------------------------------ section E */

/*
 * The soak. A synthetic generator set of the map's own count (176) over a
 * 2 km map; the player stands in the middle of the dense quarter, the
 * generators inside the generator ring fill the parked target, a taken car
 * drops the count and the fill brings it back.
 *
 * The generator's policy (when it asks, the range gate, the blocking flag,
 * the timer, the taken-car wait) is the game's (CarGen.cpp); the soak scripts
 * the requests it makes and applies the same state updates, in the same
 * order, so the service's path is driven exactly as the game drives it.
 */

#define SOAK_GENERATORS	176		/* the shipped map's count */
#define SOAK_IN_BAND	60		/* the generators inside the ring (the rest are out of range) */
#define SOAK_FRAMES	2600		/* about 86 s at 30 fps (the taken-car wait is 60 s) */
#define SOAK_FRAME_MS	33
#define SOAK_TAKE_FRAME	400
#define SOAK_AWAY_FRAME	500
#define SOAK_BACK_FRAME	2500
#define SOAK_AWAY_X	5000.0f		/* the player far away: every generator is out of range */
#define SOAK_AWAY_Y	5000.0f

struct SSoakGen
{
	float x, y, z;
	float angle;
	int32_t modelIndex;		/* >= 0 fixed, < 0 dynamic */
	int16_t color1, color2;
	uint8_t alarm, doorlock;
	uint32_t timer;
	uint16_t usesRemaining;
	bool blocking;
	int32_t handle;			/* the parked car's slot, -1: none */
	bool spotBlocked;		/* the scripted world: a car stands on the spot */
	bool spotVoid;			/* the scripted world: no ground */
	bool modelEvicted;		/* the scripted world: the fixed model is not resident */
	uint8_t arm;
	bool everRechosen;
	int32_t storedModel;		/* the car whose colours the store took, -1: none */
	int16_t storedColor1, storedColor2;	/* the colours the store took */
};

struct SSoakCar
{
	float x, y, z;
	int32_t model;
	uint8_t createdBy;
	int16_t color1, color2;		/* the effective colours (the decision's or the model's own) */
	bool live;
};

static SSoakGen g_gens[SOAK_GENERATORS];
static SSoakCar g_cars[SOAK_GENERATORS + 8];	/* one car a generator, plus the taken spot's refill */
static int32_t g_numCars;		/* the slots used */
static int32_t g_numLive;		/* the cars still standing */
static int32_t g_numTaken;

/* The scripted streaming: the models the fixed generators name and the class
 * the dynamic policy draws from. 420 is deliberately never resident. */
static bool
SoakModelResident(int32_t model)
{
	return (model >= 400 && model <= 407) || (model >= 500 && model <= 503);
}

static int32_t
SoakPolicyModel(int32_t i)
{
	return 500 + (i % 4);
}

/* The model's own colours (the constructor's draw, scripted): collision-free
 * across the set's model ranges, so the soak's colour check can tell an old
 * car's paint from the model's own. */
static int16_t
SoakModelColour1(int32_t model)
{
	return (int16_t)(1000 + model);
}

static int16_t
SoakModelColour2(int32_t model)
{
	return (int16_t)(3000 + model);
}

static void
SoakBuildSet(void)
{
	static const float kTwoPi = 6.2831853f;
	int32_t i;

	memset(g_gens, 0, sizeof(g_gens));
	memset(g_cars, 0, sizeof(g_cars));
	g_numCars = 0;
	g_numLive = 0;
	g_numTaken = 0;

	for (i = 0; i < SOAK_GENERATORS; i++) {
		SSoakGen *g = &g_gens[i];

		if (i < SOAK_IN_BAND) {
			/* The dense quarter: a 60-point ring at 212..227 m around the
			 * player, inside the generator band and clear of both clips
			 * (the near clip is 210, the far ring 230): the boundary is not
			 * a float tie, so the soak reads the same on every libm. */
			float a = kTwoPi * (float)i / (float)SOAK_IN_BAND;
			float r = 212.0f + (float)(i % 4) * 5.0f;
			g->x = r * cosf(a);
			g->y = r * sinf(a);
		} else {
			/* The rest of the map: outside the band (nearer than the
			 * near clip or past the far ring). */
			float a = kTwoPi * (float)i / (float)(SOAK_GENERATORS - SOAK_IN_BAND);
			float r = (i % 2) ? 120.0f + (float)(i % 5) * 10.0f : 260.0f + (float)(i % 40) * 10.0f;
			g->x = r * cosf(a);
			g->y = r * sinf(a);
		}
		g->z = 5.0f;
		g->angle = (float)(i * 7 % 360);
		if (i % 3 == 0)
			g->modelIndex = (i % 9 == 0) ? -1 : -(400 + (i % 8));
		else
			g->modelIndex = 400 + (i % 8);
		g->color1 = (i % 4 == 0) ? -1 : (int16_t)(10 + i % 40);
		g->color2 = (i % 4 == 0) ? -1 : (int16_t)(50 + i % 40);
		g->alarm = (uint8_t)(i % 5);
		g->doorlock = (uint8_t)(i % 3);
		/* The map's generators start at different times (the load order):
		 * the fill spreads over the first three seconds. */
		g->timer = (uint32_t)((i * 41) % 3000);
		g->usesRemaining = UINT16_MAX;
		g->blocking = false;
		g->handle = -1;
		g->spotBlocked = (i % 29 == 0);
		g->spotVoid = (i % 17 == 0);
		g->modelEvicted = (i % 13 == 0);
		g->arm = (i % 11 == 0) ? NEAR_BUBBLE_ARM_OCCLUDER :
			((i % 5 == 0) ? NEAR_BUBBLE_ARM_ONSCREEN_BAND : NEAR_BUBBLE_ARM_SOFT);
		g->everRechosen = false;
		g->storedModel = -1;
		g->storedColor1 = -1;
		g->storedColor2 = -1;
	}
}

/* The world's answers for one generator, at this frame. */
static void
SoakScriptWorld(SWorld *w, const SSoakGen *g, const SSpawnParkedRequest *pRequest)
{
	WorldDefaults(w, pRequest);
	w->blocked = g->spotBlocked;
	w->groundFound = !g->spotVoid;
	w->modelResident = !g->modelEvicted;
	w->storedModelResident = g->modelIndex < -1 && SoakModelResident(-g->modelIndex);
	w->arm = g->arm;
}

/* One generator's frame, the game's own policy: the handle, the range gate,
 * the blocking flag, the timer, then one request through the parked path. */
static void
SoakGenerator(int32_t i, uint32_t now, const STrafficRings &rings,
	float playerX, float playerY, SSpawnServiceTelemetry *tel)
{
	SSoakGen *g = &g_gens[i];
	SSpawnParkedRequest request;
	SSpawnParkedDecision decision;
	SSpawnParkedBudget budget;
	SWorld w;
	SPolicy p;
	float dx, dy, dist;
	ESpawnRefusal refusal;

	if (g->handle != -1)
		return;
	if (g->usesRemaining == 0)
		return;

	dx = g->x - playerX;
	dy = g->y - playerY;
	dist = sqrtf(dx * dx + dy * dy);
	/* The range gate: outside the far ring the blocking flag clears; the
	 * near clip holds the spawn back (the force flag is not scripted). */
	if (dist >= rings.generatorFar) {
		g->blocking = false;
		return;
	}
	if (g->blocking)
		return;
	if (dist < rings.generatorNear)
		return;
	if (now < g->timer)
		return;

	request.purpose = SPAWN_PURPOSE_PARKED;
	request.modelIndex = g->modelIndex;
	request.x = g->x;
	request.y = g->y;
	request.z = g->z;
	request.angle = g->angle;
	request.color1 = g->color1;
	request.color2 = g->color2;
	request.alarm = g->alarm;
	request.doorlock = g->doorlock;

	budget.parkedCars = g_numLive;
	budget.parkedTarget = ParkedCarTarget(rings.scale);
	budget.carsInUse = g_numLive + 40;
	budget.carBudget = MAX_CARS_IN_USE;

	SoakScriptWorld(&w, g, &request);
	p.model = SoakPolicyModel(i);
	p.calls = 0;
	/* The engine's own intake rows: RequestParked makes these two calls
	 * (SpawnRequestBegin, and SpawnNoteCreated on a creation). */
	SpawnRequestBegin(tel, SPAWN_PURPOSE_PARKED, SPAWN_KIND_PARKED);
	refusal = SpawnRunParked(request, budget, now, PolicyChoose, &p, WorldStage, &w, &decision, tel);

	/* The generator's own state, the same order CarGen.cpp applies it. */
	if (decision.modelRechosen) {
		g->modelIndex = decision.modelIndex;
		g->color1 = -1;
		g->color2 = -1;
		g->everRechosen = true;
	}
	if (refusal == SPAWN_REFUSAL_OCCUPANCY) {
		g->blocking = true;
		g->timer += 4;
		return;
	}
	if (refusal != SPAWN_REFUSAL_NONE)
		return;
	SpawnNoteCreated(tel, SPAWN_PURPOSE_PARKED);

	/* The creation's effective colours: the decision's (the service applies
	 * them) or the model's own; then the dynamic generator remembers its
	 * first car's colours, read from the car the creation made (the old
	 * policy). */
	{
		int32_t model = decision.modelIndex >= 0 ? decision.modelIndex : -decision.modelIndex;

		if (decision.color1 != -1 && decision.color2 != -1) {
			g_cars[g_numCars].color1 = decision.color1;
			g_cars[g_numCars].color2 = decision.color2;
		} else {
			g_cars[g_numCars].color1 = SoakModelColour1(model);
			g_cars[g_numCars].color2 = SoakModelColour2(model);
		}
		if ((g->color1 == -1 || g->color2 == -1) && g->modelIndex < -1) {
			g->color1 = g_cars[g_numCars].color1;
			g->color2 = g_cars[g_numCars].color2;
			g->storedModel = model;
			g->storedColor1 = g_cars[g_numCars].color1;
			g->storedColor2 = g_cars[g_numCars].color2;
		}
		g_cars[g_numCars].model = model;
	}
	g->handle = g_numCars;
	if (g->usesRemaining < UINT16_MAX)
		g->usesRemaining--;
	g->timer = now + 4;
	g_cars[g_numCars].x = decision.x;
	g_cars[g_numCars].y = decision.y;
	g_cars[g_numCars].z = decision.z;
	g_cars[g_numCars].createdBy = decision.createdBy;
	g_cars[g_numCars].live = true;
	g_numCars++;
	g_numLive++;
}

/* The player takes a car (Process' own promotion: the handle clears, the
 * timer waits a minute, the flag blocks, a dynamic field resets to -1). */
static void
SoakTakeCar(int32_t i, uint32_t now)
{
	SSoakGen *g = &g_gens[i];
	int32_t slot = g->handle;

	if (slot < 0)
		return;
	g_cars[slot].live = false;
	g_numLive--;
	g_numTaken++;
	g->handle = -1;
	g->timer = now + 60000;
	g->blocking = true;
	if (g->modelIndex < 0)
		g->modelIndex = -1;
}

static void
SectionSoak(void)
{
	STrafficRingInput in;
	STrafficRings rings;
	SSpawnServiceTelemetry tel;
	SSpawnParkedBudget budget;
	SSoakGen before[SOAK_GENERATORS];
	int32_t target, frame, i;
	int32_t maxCount = 0, reachedAt = -1, refillAt = -1;
	int32_t firstTaken = -1;
	float playerX = 0.0f, playerY = 0.0f;
	uint32_t now;
	int32_t totalRefusals;

	printf("E. the soak (the parked count against the target)\n");

	in.lodDistance = RING_LOD_REFERENCE;
	in.fovFactor = 1.0f;
	in.farClip = 1500.0f;
	rings = ComputeTrafficRings(in);
	target = ParkedCarTarget(rings.scale);
	CheckInt((int)rings.scale, 1, "E1: the soak runs at the stock ring scale");

	SoakBuildSet();
	SpawnTelemetryReset(&tel);
	memcpy(before, g_gens, sizeof(before));

	for (frame = 0; frame < SOAK_FRAMES; frame++) {
		now = (uint32_t)frame * SOAK_FRAME_MS;

		/* The player: at the centre, away for the taken-car wait, back
		 * for the refill. */
		if (frame >= SOAK_AWAY_FRAME && frame < SOAK_BACK_FRAME) {
			playerX = SOAK_AWAY_X;
			playerY = SOAK_AWAY_Y;
		} else {
			playerX = 0.0f;
			playerY = 0.0f;
		}

		if (frame == SOAK_TAKE_FRAME) {
			/* The nearest car with a handle goes. */
			float best = 1e9f;
			for (i = 0; i < SOAK_IN_BAND; i++) {
				if (g_gens[i].handle < 0)
					continue;
				if (fabsf(g_gens[i].x) + fabsf(g_gens[i].y) < best) {
					best = fabsf(g_gens[i].x) + fabsf(g_gens[i].y);
					firstTaken = i;
				}
			}
			if (firstTaken >= 0)
				SoakTakeCar(firstTaken, now);
		}

		/* The round-robin of CTheCarGenerators::Process: a quarter of the
		 * generators each frame. */
		for (i = frame % 4; i < SOAK_GENERATORS; i += 4)
			SoakGenerator(i, now, rings, playerX, playerY, &tel);

		if (g_numLive > maxCount)
			maxCount = g_numLive;
		if (reachedAt < 0 && g_numLive >= target)
			reachedAt = frame;
		if (frame > SOAK_BACK_FRAME && refillAt < 0 && g_numLive >= target)
			refillAt = frame;
	}

	printf("  the target at the stock ring: %d; the generators in the ring: %d\n", target, SOAK_IN_BAND);
	printf("  the fill: %d parked cars at frame %d; the maximum over the soak: %d (the target: %d)\n",
		target, reachedAt, maxCount, target);
	printf("  the taken car: %d at frame %d; the refill after the player's return: frame %d; the final count: %d\n",
		g_numTaken, SOAK_TAKE_FRAME, refillAt, g_numLive);

	/* The acceptance: the count reaches the target and never passes it. */
	Check(reachedAt >= 0, "E2: the soak reaches the target");
	Check(maxCount <= target, "E3: the count never passes the target");
	CheckInt(g_numTaken, 1, "E4: one car was taken");
	Check(refillAt >= 0, "E5: the fill refills the taken spot");
	CheckInt(g_numLive, target, "E6: the soak ends at the target");

	/* The ledger against the soak's own counts. */
	CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_PARKED], (int)tel.m_nRequestsByKind[SPAWN_KIND_PARKED],
		"E7: every parked request counts by its kind");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_PARKED], g_numLive + g_numTaken,
		"E8: the created row is the live cars plus the taken ones");
	totalRefusals = 0;
	for (i = 1; i < SPAWN_REFUSAL_COUNT; i++)
		totalRefusals += (int)tel.m_nRefusalsByReason[i];
	CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_PARKED], (int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_PARKED] + totalRefusals,
		"E9: every request either created or refused");
	CheckInt((int)tel.m_nGroundRejections, (int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE],
		"E10: the defect row is the NO_SITE row (the ground gate)");
	Check((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_BUDGET] > 0, "E11: the surplus attempts are refused BUDGET");
	Check((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_OCCUPANCY] > 0, "E12: the blocked spots refuse OCCUPANCY");
	Check((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_VISIBILITY] > 0, "E13: the occluder arms refuse VISIBILITY");
	Check((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_READINESS] > 0, "E14: the evicted models refuse READINESS");
	printf("  the ledger: %d requests, %d created; refused: no site %d, occupancy %d, visibility %d, readiness %d, budget %d (the sum %d)\n",
		(int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_PARKED],
		(int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_PARKED],
		(int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE],
		(int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_OCCUPANCY],
		(int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_VISIBILITY],
		(int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_READINESS],
		(int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_BUDGET],
		totalRefusals);

	/* The policy state round-trips: the generator's own policy moved the
	 * model field and the colours of the re-chosen generators (the parked
	 * path's only write back), and nothing else stands moved - so the state
	 * a save carries is exactly what the requests read. */
	{
		int32_t moved = 0;
		for (i = 0; i < SOAK_GENERATORS; i++) {
			char what[96];

			sprintf(what, "E15: the generator %d position stands", i);
			CheckNear(g_gens[i].x, before[i].x, what);
			CheckNear(g_gens[i].y, before[i].y, "E16: the position stands");
			CheckNear(g_gens[i].z, before[i].z, "E17: the position stands");
			CheckNear(g_gens[i].angle, before[i].angle, "E18: the angle stands");
			CheckInt(g_gens[i].alarm, before[i].alarm, "E19: the alarm threshold stands");
			CheckInt(g_gens[i].doorlock, before[i].doorlock, "E20: the lock threshold stands");
			CheckInt(g_gens[i].usesRemaining, before[i].usesRemaining, "E21: the uses stand");
			if (g_gens[i].everRechosen)
				moved++;
		}
		printf("  the policy state: the re-choice moved %d generators' model fields; every other field stands\n", moved);
	}

	/* The colour store: every paint the store took belongs to the model of
	 * the car it came from - the review's defect (a re-choice painting the
	 * new model with the old car's colours) fails here. */
	{
		int32_t stored = 0;
		for (i = 0; i < SOAK_GENERATORS; i++) {
			if (g_gens[i].storedModel < 0)
				continue;
			stored++;
			CheckInt(g_gens[i].storedColor1, SoakModelColour1(g_gens[i].storedModel), "E22: the stored colour 1 belongs to the car's model");
			CheckInt(g_gens[i].storedColor2, SoakModelColour2(g_gens[i].storedModel), "E23: the stored colour 2 belongs to the car's model");
		}
		printf("  the colour store: %d generators remembered a car's paint; every one is its car's model's own\n", stored);
	}

	/* The save/load rebuild's own property: the parked path reads the
	 * request alone, so the same generator state answers the same - the
	 * state a save carries is everything the path reads. */
	{
		SSpawnParkedDecision d1, d2;
		SWorld w1, w2;
		SPolicy p1, p2;
		SSpawnParkedRequest r1, r2;
		int32_t slot = -1;
		int32_t holder = -1;

		for (i = 0; i < SOAK_GENERATORS; i++) {
			if (g_gens[i].handle >= 0) {
				holder = i;
				slot = g_gens[i].handle;
				break;
			}
		}
		Check(holder >= 0 && slot >= 0, "E24: a generator holds a car");
		r1 = MakeRequest(g_gens[holder].modelIndex);
		r2 = r1;
		budget = MakeBudget(g_numLive, target);
		p1.model = SoakPolicyModel(holder);
		p2.model = SoakPolicyModel(holder);
		WorldDefaults(&w1, &r1);
		WorldDefaults(&w2, &r2);
		CheckInt((int)SpawnRunParked(r1, budget, g_nowMs, PolicyChoose, &p1, WorldStage, &w1, &d1, &tel), (int)SPAWN_REFUSAL_BUDGET, "E25: the same state answers the same");
		CheckInt((int)SpawnRunParked(r2, budget, g_nowMs, PolicyChoose, &p2, WorldStage, &w2, &d2, &tel), (int)SPAWN_REFUSAL_BUDGET, "E26: the same state answers the same");
		CheckInt(d1.modelIndex, d2.modelIndex, "E27: the decisions match");
	}
}

/* ------------------------------------------------------------------ the cost */

static volatile uint64_t g_costSink;

static void
PrintCost(void)
{
	SSpawnParkedRequest r = MakeRequest(400);
	SSpawnParkedBudget b = MakeBudget(3, 20);
	SSpawnParkedDecision d;
	SSpawnServiceTelemetry tel;
	SWorld w;
	SPolicy p;
	double start, elapsed;
	int32_t iter;
	static const uint8_t kArms[] = {
		NEAR_BUBBLE_ARM_SOFT,
		NEAR_BUBBLE_ARM_ONSCREEN_BAND,
		NEAR_BUBBLE_ARM_OCCLUDER,
		NEAR_BUBBLE_ARM_FADE,
	};

	SpawnTelemetryReset(&tel);
	start = NowSeconds();
	for (iter = 0; iter < 200000; iter++) {
		/* The facts vary per call, so the run is real work and not a
		 * fold; the volatile sink keeps it. */
		r.modelIndex = 400 + (iter & 7);
		WorldDefaults(&w, &r);
		w.arm = kArms[iter & 3];
		w.blocked = (iter & 31) == 0;
		p.calls = 0;
		g_costSink += (uint64_t)(uint32_t)SpawnRunParked(r, b, g_nowMs, PolicyChoose, &p, WorldStage, &w, &d, &tel);
		g_costSink += (uint64_t)(uint32_t)d.modelIndex;
	}
	elapsed = NowSeconds() - start;
	printf("  the parked path's cost: %.0f ns a request (the scripted world; 200000 calls, the sink %llu)\n",
		elapsed * 1e9 / 200000.0, (unsigned long long)g_costSink);
}

/* ------------------------------------------------------------------ main */

int
main(void)
{
	printf("spawn-service parked harness (VICE-155)\n");

	SectionTarget();
	SectionModel();
	SectionGates();
	SectionLedger();
	SectionSoak();
	PrintCost();

	printf("  %lld checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
