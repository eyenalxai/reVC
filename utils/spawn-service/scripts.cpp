/*
 * The evidence program for the script and mission creators (ticket VICE-157).
 *
 * It compiles the service's engine-free header (src/control/SpawnService.h)
 * and drives the one construction the engine's explicit kind and this program
 * share:
 *
 *	A. The car opcodes. COMMAND_CREATE_CAR's set (the model and the resolved
 *	   position) and COMMAND_CREATE_RANDOM_CAR_FOR_CAR_PARK's set (the model,
 *	   the position and the heading): the purpose (MISSION for a mission
 *	   script, SPECIAL for an idle one), the force (both opcodes), the exact
 *	   placement, the heading vector, the tag (MISSION_VEHICLE or the
 *	   traffic's) and the clear-space flag. The admission is then driven with
 *	   a full frame (the force bypass admits both) and an empty pool (the
 *	   physical last slot refuses both).
 *
 *	B. The standing ped opcodes. COMMAND_CREATE_CHAR's and
 *	   COMMAND_CREATE_RANDOM_CHAR's sets: the type, the model and the
 *	   position, mapped to the explicit ped request, admitted with the full
 *	   budgets (the force bypass) and refused by the pool and by a missing
 *	   model.
 *
 *	C. The in-car opcodes. COMMAND_CREATE_CHAR_INSIDE_CAR,
 *	   COMMAND_CREATE_CHAR_AS_PASSENGER and the two random in-car opcodes:
 *	   the seat, the model (the explicit one, or -1 for the population's own
 *	   choice), the vehicle requirement and the clear-space flag (never set
 *	   in a car).
 *
 *	D. The ledger. Every admitted request counts its purpose and its EXPLICIT
 *	   kind, every refusal its reason, and the created row the purpose. The
 *	   mission actors land in MISSION, the idle scripts' and the car park's in
 *	   SPECIAL.
 *
 * What this program cannot reach, stated plainly: the engine's creation
 * itself (SpawnService.cpp's CVehicle/CPed constructors, the world insert and
 * the same-frame ppCreated handle) is the game's, checked by the build and by
 * the live play-test; this harness drives the admission and the argument
 * mapping they call. The ground cast of a standing ped and the model's
 * centre-to-base offset are the engine's too; the harness hands the opcode's
 * resolved z in and checks that the request carries it unchanged.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 \
 *	    -o /tmp/spawn-scripts utils/spawn-service/scripts.cpp
 *	/tmp/spawn-scripts
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "SpawnService.h"

#include <math.h>
#include <stdio.h>

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
	if (fabsf(got - want) > 0.0001f) {
		g_failures++;
		printf("  FAIL: %s (want %.4f, got %.4f)\n", what, want, got);
	}
}

/* ------------------------------------------------------------------ the budgets */

/* A full frame: the car target and the car budget are at their lines and the
 * ped pool has no free spaces. The ped reserve is the third gate. */
static SSpawnExplicitFacts
FullBudget(bool poolHasSpace)
{
	SSpawnExplicitFacts facts;

	facts.budget.randomCars = 200;
	facts.budget.randomTarget = 150.0f;
	facts.budget.carsInUse = 300;
	facts.budget.carBudget = 250;
	facts.budget.pedFreeSpaces = 0;
	facts.poolHasSpace = poolHasSpace;
	return facts;
}

/* A roomy frame: every budget gate passes and the pool has room. */
static SSpawnExplicitFacts
RoomBudget(bool poolHasSpace)
{
	SSpawnExplicitFacts facts;

	facts.budget.randomCars = 10;
	facts.budget.randomTarget = 150.0f;
	facts.budget.carsInUse = 20;
	facts.budget.carBudget = 250;
	facts.budget.pedFreeSpaces = 400;
	facts.poolHasSpace = poolHasSpace;
	return facts;
}

/* The script's own ped type values (ePedType; the core takes plain ints so it
 * stays engine-free). */
#define SCRIPT_PED_CIVMALE	4
#define SCRIPT_PED_COP		6

/* ------------------------------------------------------------------ section A */

static void
SectionCarOpcodes(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnScriptCarArgs args;
	SSpawnExplicitRequest request;
	SSpawnExplicitDecision decision;
	SSpawnExplicitFacts facts;

	printf("A. the car opcodes: CREATE_CAR and the car park\n");

	/* A1: COMMAND_CREATE_CAR's set. The mission car names the mission tag,
	 * forces (a mission cannot fail on a scheduling decision) and clears
	 * the spot before the insert. */
	{
		SpawnTelemetryReset(&tel);
		args.carModel = 105;
		args.x = 12.5f;
		args.y = -3.25f;
		args.z = 8.0f;
		args.headingDegrees = 0.0f;
		args.hasHeading = false;
		args.parked = false;
		args.missionScript = true;
		SpawnScriptCarRequest(args, &request);

		CheckInt(request.purpose, SPAWN_PURPOSE_MISSION, "A1: a mission script's car counts as MISSION");
		Check(request.force, "A1: the mission car forces");
		CheckInt(request.carModel, 105, "A1: the model is the opcode's");
		CheckNear(request.x, 12.5f, "A1: the request's x is exact");
		CheckNear(request.y, -3.25f, "A1: the request's y is exact");
		CheckNear(request.z, 8.0f, "A1: the request's z is exact");
		CheckNear(request.dirX, 1.0f, "A1: no heading is the identity's forward x");
		CheckNear(request.dirY, 0.0f, "A1: no heading is the identity's forward y");
		CheckInt(request.createdBy, SPAWN_VEHICLE_TAG_MISSION, "A1: the tag is MISSION_VEHICLE");
		Check(request.clearSpace, "A1: the car clears the mission spot");
		CheckInt((int)request.lifetimeMs, 0, "A1: no lifetime asks for the grace");

		facts = FullBudget(true);
		Check(SpawnExplicitCarAdmit(request, facts, &decision, &tel), "A1: the force bypass admits a full frame");
		CheckNear(decision.x, 12.5f, "A1: the decision's x is the request's");
		CheckNear(decision.y, -3.25f, "A1: the decision's y is the request's");
		CheckNear(decision.z, 8.0f, "A1: the decision's z is the request's");
		CheckNear(decision.dirX, 1.0f, "A1: the decision's forward x is the request's");
		CheckNear(decision.dirY, 0.0f, "A1: the decision's forward y is the request's");
		CheckInt((int)decision.protectMs, NEAR_BUBBLE_GRACE_MS, "A1: the lifetime floors at the grace");
		CheckInt(decision.createdBy, SPAWN_VEHICLE_TAG_MISSION, "A1: the decision's tag is the mission's");

		/* The engine's created note runs after its constructor; mirror it. */
		SpawnNoteCreated(&tel, request.purpose);
		CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_MISSION], 1, "A1: the request counts by purpose");
		CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_EXPLICIT], 1, "A1: the request counts as EXPLICIT");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_MISSION], 1, "A1: the created row counts the purpose");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_BUDGET], 0, "A1: no budget refusal was counted");
	}

	/* A2: the idle script's car is a SPECIAL actor, not a mission's. */
	{
		SpawnTelemetryReset(&tel);
		args.missionScript = false;
		SpawnScriptCarRequest(args, &request);
		CheckInt(request.purpose, SPAWN_PURPOSE_SPECIAL, "A2: an idle script's car counts as SPECIAL");
		facts = FullBudget(true);
		Check(SpawnExplicitCarAdmit(request, facts, &decision, &tel), "A2: the idle script's car forces too");
		SpawnNoteCreated(&tel, request.purpose);
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_SPECIAL], 1, "A2: the created row counts SPECIAL");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_MISSION], 0, "A2: no mission row moved");
	}

	/* A3: COMMAND_CREATE_RANDOM_CAR_FOR_CAR_PARK's set: the heading vector,
	 * the traffic's tag and the script's force (the old cap was the pool's
	 * own number, so the force keeps the behaviour). */
	{
		SpawnTelemetryReset(&tel);
		args.carModel = 154;
		args.x = 40.0f;
		args.y = 60.0f;
		args.z = 12.0f;
		args.headingDegrees = 90.0f;
		args.hasHeading = true;
		args.parked = true;
		args.missionScript = false;
		SpawnScriptCarRequest(args, &request);

		CheckInt(request.purpose, SPAWN_PURPOSE_SPECIAL, "A3: the car park's car counts as SPECIAL");
		Check(request.force, "A3: the car park's script forces too");
		CheckInt(request.createdBy, SPAWN_VEHICLE_TAG_RANDOM, "A3: the car park's tag is the traffic's");
		CheckNear(request.dirX, 0.0f, "A3: 90 degrees is forward y");
		CheckNear(request.dirY, 1.0f, "A3: 90 degrees is forward y");
		CheckNear(request.x, 40.0f, "A3: the car park's x is exact");
		CheckNear(request.z, 12.0f, "A3: the car park's z is exact");
		Check(request.clearSpace, "A3: the car park clears the spot too");

		facts = FullBudget(true);
		Check(SpawnExplicitCarAdmit(request, facts, &decision, &tel), "A3: the force bypass admits the car park in a full frame");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_BUDGET], 0, "A3: the force leaves no budget refusal");
		CheckNear(decision.dirX, 0.0f, "A3: the decision carries the heading x");
		CheckNear(decision.dirY, 1.0f, "A3: the decision carries the heading y");
		CheckInt(decision.createdBy, SPAWN_VEHICLE_TAG_RANDOM, "A3: the decision's tag is the traffic's");
		SpawnNoteCreated(&tel, request.purpose);
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_SPECIAL], 1, "A3: the admitted car counts");
	}

	/* A4: the physical pool's last slot refuses even the force. */
	{
		SpawnTelemetryReset(&tel);
		args.missionScript = true;
		args.parked = false;
		args.hasHeading = false;
		SpawnScriptCarRequest(args, &request);
		facts = RoomBudget(false);
		Check(!SpawnExplicitCarAdmit(request, facts, &decision, &tel), "A4: an empty pool refuses the mission car");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_POOL], 1, "A4: the refusal is the pool's");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_MISSION], 0, "A4: nothing was created");
	}

	/* A5: a request without a model is a bad request, not a budget refusal. */
	{
		SpawnTelemetryReset(&tel);
		args.carModel = -1;
		SpawnScriptCarRequest(args, &request);
		facts = RoomBudget(true);
		Check(!SpawnExplicitCarAdmit(request, facts, &decision, &tel), "A5: no model refuses");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE], 1, "A5: the refusal is the no-site's");
	}
}

/* ------------------------------------------------------------------ section B */

static void
SectionStandingPeds(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnScriptPedArgs args;
	SSpawnExplicitPedRequest request;
	SSpawnExplicitFacts facts;

	printf("B. the standing ped opcodes: CREATE_CHAR and CREATE_RANDOM_CHAR\n");

	/* B1: COMMAND_CREATE_CHAR's set. */
	{
		SpawnTelemetryReset(&tel);
		args.pedType = SCRIPT_PED_CIVMALE;
		args.pedModel = 17;
		args.place = SPAWN_PED_PLACE_STANDING;
		args.x = 100.0f;
		args.y = 200.0f;
		args.z = 9.5f;
		args.missionScript = true;
		SpawnScriptPedRequest(args, &request);

		CheckInt(request.purpose, SPAWN_PURPOSE_MISSION, "B1: a mission script's ped counts as MISSION");
		Check(request.force, "B1: the mission ped forces");
		CheckInt(request.place, SPAWN_PED_PLACE_STANDING, "B1: the ped stands");
		CheckInt(request.pedType, SCRIPT_PED_CIVMALE, "B1: the ped type is the opcode's");
		CheckInt(request.pedModel, 17, "B1: the model is the opcode's");
		CheckNear(request.x, 100.0f, "B1: the request's x is exact");
		CheckNear(request.y, 200.0f, "B1: the request's y is exact");
		CheckNear(request.z, 9.5f, "B1: the request's z is exact");
		Check(request.clearSpace, "B1: the standing ped clears the mission spot");

		facts = FullBudget(true);
		Check(SpawnExplicitPedAdmit(request, facts, &tel), "B1: the force bypass admits the ped in a full frame");
		SpawnNoteCreated(&tel, request.purpose);
		CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_MISSION], 1, "B1: the request counts by purpose");
		CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_EXPLICIT], 1, "B1: the request counts as EXPLICIT");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_MISSION], 1, "B1: the created row counts the purpose");
	}

	/* B2: a cop's set: the type selects the cop constructor, the model is
	 * the cop type. The idle script's ped is SPECIAL. */
	{
		SpawnTelemetryReset(&tel);
		args.pedType = SCRIPT_PED_COP;
		args.pedModel = 1;	/* COP_STREET */
		args.missionScript = false;
		SpawnScriptPedRequest(args, &request);
		CheckInt(request.purpose, SPAWN_PURPOSE_SPECIAL, "B2: an idle script's ped counts as SPECIAL");
		CheckInt(request.pedType, SCRIPT_PED_COP, "B2: the cop type is the constructor's");
		CheckInt(request.pedModel, 1, "B2: the cop type is the model selector");
		facts = RoomBudget(true);
		Check(SpawnExplicitPedAdmit(request, facts, &tel), "B2: the roomy frame admits the cop");
		SpawnNoteCreated(&tel, request.purpose);
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_SPECIAL], 1, "B2: the SPECIAL row counts");
	}

	/* B3: the ped pool's last slot refuses even the force. */
	{
		SpawnTelemetryReset(&tel);
		args.pedType = SCRIPT_PED_CIVMALE;
		args.pedModel = 17;
		args.missionScript = true;
		SpawnScriptPedRequest(args, &request);
		facts = RoomBudget(false);
		Check(!SpawnExplicitPedAdmit(request, facts, &tel), "B3: an empty ped pool refuses");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_POOL], 1, "B3: the refusal is the pool's");
	}

	/* B4: a standing request without a model is a bad request. */
	{
		SpawnTelemetryReset(&tel);
		args.pedModel = -1;
		SpawnScriptPedRequest(args, &request);
		facts = RoomBudget(true);
		Check(!SpawnExplicitPedAdmit(request, facts, &tel), "B4: a standing ped without a model refuses");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE], 1, "B4: the refusal is the no-site's");
	}
}

/* ------------------------------------------------------------------ section C */

static void
SectionInCarPeds(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnScriptPedArgs args;
	SSpawnExplicitPedRequest request;
	SSpawnExplicitFacts facts;
	CVehicle *pVehicle = (CVehicle *)1;	/* the in-car arms only compare it */

	printf("C. the in-car ped opcodes: the seat, the model and the vehicle\n");

	/* C1: COMMAND_CREATE_CHAR_INSIDE_CAR's set: the explicit model, the
	 * driver's seat, no clear-space (the car's own place). */
	{
		SpawnTelemetryReset(&tel);
		args.pedType = SCRIPT_PED_CIVMALE;
		args.pedModel = 22;
		args.place = SPAWN_PED_PLACE_DRIVER;
		args.x = 5.0f;
		args.y = 6.0f;
		args.z = 7.0f;
		args.missionScript = true;
		SpawnScriptPedRequest(args, &request);
		request.vehicle = pVehicle;

		CheckInt(request.purpose, SPAWN_PURPOSE_MISSION, "C1: the inside-car ped counts as MISSION");
		Check(request.force, "C1: the inside-car ped forces");
		CheckInt(request.place, SPAWN_PED_PLACE_DRIVER, "C1: the place is the driver's seat");
		CheckInt(request.pedModel, 22, "C1: the explicit model is the opcode's");
		Check(!request.clearSpace, "C1: an in-car ped never clears the spot");
		facts = FullBudget(true);
		Check(SpawnExplicitPedAdmit(request, facts, &tel), "C1: the driver admits in a full frame");
		SpawnNoteCreated(&tel, request.purpose);
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_MISSION], 1, "C1: the created row counts");
	}

	/* C2: COMMAND_CREATE_CHAR_AS_PASSENGER's set. */
	{
		SpawnTelemetryReset(&tel);
		args.pedType = SCRIPT_PED_CIVMALE;
		args.pedModel = 23;
		args.place = SPAWN_PED_PLACE_PASSENGER;
		args.missionScript = false;
		SpawnScriptPedRequest(args, &request);
		request.vehicle = pVehicle;
		CheckInt(request.place, SPAWN_PED_PLACE_PASSENGER, "C2: the place is the passenger's seat");
		CheckInt(request.pedModel, 23, "C2: the explicit model is the opcode's");
		Check(!request.clearSpace, "C2: a passenger never clears the spot");
		facts = RoomBudget(true);
		Check(SpawnExplicitPedAdmit(request, facts, &tel), "C2: the passenger admits");
	}

	/* C3: the random in-car opcodes: the population's model (pedModel -1),
	 * driver and passenger. The service asks CPopulation::AddPedInCar; this
	 * program only checks that the request says so. */
	{
		SpawnTelemetryReset(&tel);
		args.pedType = SCRIPT_PED_CIVMALE;
		args.pedModel = -1;
		args.place = SPAWN_PED_PLACE_DRIVER;
		args.missionScript = true;
		SpawnScriptPedRequest(args, &request);
		request.vehicle = pVehicle;
		CheckInt(request.pedModel, -1, "C3: the random driver names no model");
		CheckInt(request.place, SPAWN_PED_PLACE_DRIVER, "C3: the random driver's place");
		facts = FullBudget(true);
		Check(SpawnExplicitPedAdmit(request, facts, &tel), "C3: the population's driver admits");

		args.place = SPAWN_PED_PLACE_PASSENGER;
		SpawnScriptPedRequest(args, &request);
		request.vehicle = pVehicle;
		CheckInt(request.pedModel, -1, "C3: the random passenger names no model");
		CheckInt(request.place, SPAWN_PED_PLACE_PASSENGER, "C3: the random passenger's place");
		Check(SpawnExplicitPedAdmit(request, FullBudget(true), &tel), "C3: the population's passenger admits");
	}

	/* C4: an in-car request without its vehicle is a bad request, with or
	 * without an explicit model. */
	{
		SpawnTelemetryReset(&tel);
		args.place = SPAWN_PED_PLACE_DRIVER;
		args.pedModel = -1;
		SpawnScriptPedRequest(args, &request);
		facts = RoomBudget(true);
		Check(!SpawnExplicitPedAdmit(request, facts, &tel), "C4: no vehicle refuses the population's driver");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE], 1, "C4: the refusal is the no-site's");

		args.pedModel = 24;
		SpawnScriptPedRequest(args, &request);
		Check(!SpawnExplicitPedAdmit(request, RoomBudget(true), &tel), "C4: no vehicle refuses the explicit driver");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE], 2, "C4: both refusals are counted");
	}
}

/* ------------------------------------------------------------------ section D */

static void
SectionLedger(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnScriptCarArgs car;
	SSpawnScriptPedArgs ped;
	SSpawnExplicitRequest carRequest;
	SSpawnExplicitPedRequest pedRequest;
	SSpawnExplicitDecision decision;

	printf("D. the ledger: the purposes, the kind, the refusals and the creations\n");

	SpawnTelemetryReset(&tel);
	car.carModel = 105;
	car.x = car.y = car.z = 0.0f;
	car.headingDegrees = 0.0f;
	car.hasHeading = false;
	car.parked = false;
	car.missionScript = true;
	SpawnScriptCarRequest(car, &carRequest);
	Check(SpawnExplicitCarAdmit(carRequest, FullBudget(true), &decision, &tel), "D1: the mission car admits");
	SpawnNoteCreated(&tel, carRequest.purpose);

	ped.pedType = SCRIPT_PED_CIVMALE;
	ped.pedModel = 17;
	ped.place = SPAWN_PED_PLACE_STANDING;
	ped.x = ped.y = ped.z = 0.0f;
	ped.missionScript = true;
	SpawnScriptPedRequest(ped, &pedRequest);
	Check(SpawnExplicitPedAdmit(pedRequest, FullBudget(true), &tel), "D1: the mission ped admits");
	SpawnNoteCreated(&tel, pedRequest.purpose);

	car.missionScript = false;
	car.parked = true;
	car.hasHeading = true;
	SpawnScriptCarRequest(car, &carRequest);
	Check(SpawnExplicitCarAdmit(carRequest, FullBudget(true), &decision, &tel), "D1: the car park's car admits");
	SpawnNoteCreated(&tel, carRequest.purpose);
	ped.missionScript = false;
	SpawnScriptPedRequest(ped, &pedRequest);
	Check(SpawnExplicitPedAdmit(pedRequest, FullBudget(true), &tel), "D1: the idle ped admits");
	SpawnNoteCreated(&tel, pedRequest.purpose);

	CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_MISSION], 2, "D1: two mission requests");
	CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_SPECIAL], 2, "D1: two special requests");
	CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_EXPLICIT], 4, "D1: all four are EXPLICIT");
	CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_ROAD_SITE], 0, "D1: no road-site row moved");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_MISSION], 2, "D1: two mission creations");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_SPECIAL], 2, "D1: two special creations");
	CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_BUDGET], 0, "D1: the force leaves no budget refusal");

	printf("  the ledger: %u mission, %u special requests; %u mission, %u special creations; "
		"%u budget refusals\n",
		tel.m_nRequestsByPurpose[SPAWN_PURPOSE_MISSION],
		tel.m_nRequestsByPurpose[SPAWN_PURPOSE_SPECIAL],
		tel.m_nCreatedByPurpose[SPAWN_PURPOSE_MISSION],
		tel.m_nCreatedByPurpose[SPAWN_PURPOSE_SPECIAL],
		tel.m_nRefusalsByReason[SPAWN_REFUSAL_BUDGET]);
}

int
main(void)
{
	printf("spawn-service script opcode harness (VICE-157)\n");
	printf("  the explicit kind's same-frame handle is the engine's ppCreated (the live play-test)\n");

	SectionCarOpcodes();
	SectionStandingPeds();
	SectionInCarPeds();
	SectionLedger();

	printf("  %lld checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
