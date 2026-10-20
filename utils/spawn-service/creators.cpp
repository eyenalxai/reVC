/*
 * The evidence program for the remaining direct creators (ticket VICE-158).
 *
 * It compiles the service's engine-free header (src/control/SpawnService.h)
 * and drives the exact request each converted creator makes over a scripted
 * world:
 *
 *	A. The set pieces. The cop chase car (MISSION, forced, the ground
 *	   policy's suspension height) and the cop on foot (MISSION, forced, the
 *	   exact resolved spot).
 *
 *	B. The roadblocks. The blockade cars and their cops (POLICE, forced: the
 *	   wanted response cannot fail on a budget; the car's tilt check stays
 *	   the caller's post-creation work).
 *
 *	C. The garages, the cheat, the taxi shortcut, the RC car and the PedAI
 *	   self-car. The garage's stored car is SPECIAL and exact; the cheat is
 *	   SPECIAL, exact and the traffic's tag; the taxi is SPECIAL, exact,
 *	   clears the mission spot and creates with the traffic's tag (the driver
 *	   contract needs RANDOM before the mission tag); the RC car is SPECIAL,
 *	   the model's base height over the ground and MISSION_VEHICLE; the
 *	   self-car is SCENARIO and exact.
 *
 *	D. The dead pedestrian of AddDeadPedInFrontOfCar (SPECIAL, the exact
 *	   caller position; the population's primitive gives the civilian its
 *	   own weapon policy).
 *
 *	E. The admission and the placement. The force bypass over a full budget
 *	   (the car target and budget at their lines, the ped reserve spent), the
 *	   physical pool's last slot (POOL, even for force), a bad model (NO_SITE)
 *	   and the tag/lifetime/clear-space contract. The ground policies: the
 *	   exact position, the base distance and the suspension height, with the
 *	   missing-ground fallback.
 *
 *	F. The ledger and the same-frame handle. Every request counts its purpose
 *	   and the EXPLICIT kind, every creation its purpose, every refusal its
 *	   reason. The run returns the created actor in the same call: the
 *	   handle is the create callback's token, the place callback has already
 *	   run when the caller reads it.
 *
 * What this program cannot reach, stated plainly: the engine's constructors,
 * the world casts and the world insert are the game's (SpawnService.cpp and
 * the creators' post-creation setup), checked by the build and by the live
 * soak. The harness scripts the facts those callbacks resolve: the ground
 * under the spot, the created model's height and the created actor. The
 * creators' own guards (the set piece's collision, the roadblock's tilt, the
 * garage's model residency, the taxi's model residency) are the callers' and
 * run after the handle; the harness checks the request they build.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 \
 *	    -o /tmp/spawn-creators utils/spawn-service/creators.cpp
 *	/tmp/spawn-creators
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "SpawnService.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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
		printf("  FAIL: %s (want %.4f, got %.4f)\n", what, want, got);
	}
}

/* --- The scripted world -------------------------------------------------- */

/* The model and ped numbers only have to be non-negative and distinct here:
 * the service never interprets them (the engine's constructors do). The
 * comments name the creators' own models. */
#define HARNESS_MODEL_POLICE	150	/* MI_POLICE */
#define HARNESS_MODEL_ROADBLOCK	151	/* the wanted tier's car */
#define HARNESS_MODEL_GARAGE	152	/* a stored garage car */
#define HARNESS_MODEL_CHEAT	153	/* a cheat's car */
#define HARNESS_MODEL_TAXI	154	/* MI_KAUFMAN */
#define HARNESS_MODEL_REMOTE	155	/* the RC car */
#define HARNESS_MODEL_SELFCAR	156	/* a PedAI self-car */
#define HARNESS_MODEL_DEADPED	7	/* MI_MALE01 */
#define HARNESS_PEDTYPE_CIVMALE	4	/* ePedType's civilian male */
#define HARNESS_PEDTYPE_COP	6	/* ePedType's cop */
#define HARNESS_COP_STREET	0	/* eCopType's street cop */

/* The tokens are opaque: the core only stores and compares them; the engine
 * would return real pool objects. */
#define CAR_TOKEN	((CVehicle *)0x1000)
#define PED_TOKEN	((CPed *)0x2000)

struct ScriptWorld
{
	SSpawnExplicitWorld carWorld;
	CVehicle *carToken;
	CPed *pedToken;
	SSpawnExplicitRequest lastRequest;
	SSpawnExplicitDecision lastDecision;
	SSpawnExplicitPedRequest lastPedRequest;
	int createdCars;
	int placedCars;
	int createdPeds;
	bool placeSawToken;
};

static void
ScriptWorldInit(ScriptWorld *world, bool groundFound, float groundZ, float heightAboveRoad)
{
	memset(world, 0, sizeof(*world));
	world->carWorld.groundFound = groundFound;
	world->carWorld.groundZ = groundZ;
	world->carWorld.heightAboveRoad = heightAboveRoad;
	world->carToken = CAR_TOKEN;
	world->pedToken = PED_TOKEN;
}

static CVehicle *
ScriptCreateCar(void *user, const SSpawnExplicitRequest &request, SSpawnExplicitWorld *worldFacts,
	ESpawnRefusal *refusal)
{
	ScriptWorld *world = (ScriptWorld *)user;

	(void)refusal;
	world->createdCars++;
	world->lastRequest = request;
	*worldFacts = world->carWorld;
	return world->carToken;
}

static void
ScriptPlaceCar(void *user, CVehicle *pVehicle, const SSpawnExplicitDecision &decision)
{
	ScriptWorld *world = (ScriptWorld *)user;

	world->placedCars++;
	world->lastDecision = decision;
	world->placeSawToken = pVehicle == world->carToken;
}

static CPed *
ScriptCreatePed(void *user, const SSpawnExplicitPedRequest &request, ESpawnRefusal *refusal)
{
	ScriptWorld *world = (ScriptWorld *)user;

	(void)refusal;
	world->createdPeds++;
	world->lastPedRequest = request;
	return world->pedToken;
}

/* --- The frames ---------------------------------------------------------- */

/* A full frame: the car target and the car budget are at their lines and the
 * ped pool has no free spaces. The force bypass must step over all of it. */
static SSpawnExplicitFacts
FullFacts(bool poolHasSpace)
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
RoomFacts(bool poolHasSpace)
{
	SSpawnExplicitFacts facts;

	facts.budget.randomCars = 10;
	facts.budget.randomTarget = 150.0f;
	facts.budget.carsInUse = 20;
	facts.budget.carBudget = 250;
	facts.budget.pedFreeSpaces = 40;
	facts.poolHasSpace = poolHasSpace;
	return facts;
}

/* --- The request builders (the creators' own argument sets) -------------- */

static SSpawnExplicitRequest
CarRequest(ESpawnPurpose purpose, bool force, int model, float x, float y, float z,
	float dirX, float dirY, uint8_t tag, uint8_t ground, float probeZ, float noHitZ,
	bool clearSpace, bool groundDummies = false)
{
	SSpawnExplicitRequest request;

	memset(&request, 0, sizeof(request));
	request.purpose = purpose;
	request.force = force;
	request.carModel = model;
	request.x = x;
	request.y = y;
	request.z = z;
	request.dirX = dirX;
	request.dirY = dirY;
	request.lifetimeMs = 0;
	request.createdBy = tag;
	request.ground = ground;
	request.groundProbeZ = probeZ;
	request.groundNoHitZ = noHitZ;
	request.groundDummies = groundDummies;
	request.clearSpace = clearSpace;
	return request;
}

static SSpawnExplicitPedRequest
PedRequest(ESpawnPurpose purpose, bool force, uint8_t place, int pedType, int pedModel,
	float x, float y, float z, float heading, uint8_t createdBy, bool missionFlags,
	CVehicle *vehicle)
{
	SSpawnExplicitPedRequest request;

	memset(&request, 0, sizeof(request));
	request.purpose = purpose;
	request.force = force;
	request.place = place;
	request.pedType = pedType;
	request.pedModel = pedModel;
	request.modifier = 0;
	request.x = x;
	request.y = y;
	request.z = z;
	request.heading = heading;
	request.createdBy = createdBy;
	request.missionFlags = missionFlags;
	request.vehicle = vehicle;
	return request;
}

/* --- The converted creators ---------------------------------------------- */

/*
 * Every row is the request its call site builds: the purpose, the model, the
 * placement, the tag, the ground policy and the force. The comment names the
 * site and the ticket's purpose rule.
 */
struct SCreatorCar
{
	const char *name;
	ESpawnPurpose purpose;
	int model;
	float x, y, z;
	float dirX, dirY;
	uint8_t tag;
	uint8_t ground;
	float probeZ, noHitZ;
	bool clearSpace;
	bool groundDummies;
};

static const SCreatorCar aCreatorCars[] = {
	/* SetPieces::TryToGenerateCopCar: a scenario actor, the ticket's
	 * MISSION rule; the ground policy reads the constructor's suspension
	 * height after the cast from the old 1000 m probe. */
	{ "set piece cop car", SPAWN_PURPOSE_MISSION, HARNESS_MODEL_POLICE,
	  10.0f, 20.0f, 1000.0f, 0.6f, 0.8f, 0, SPAWN_EXPLICIT_GROUND_HEIGHT, 1000.0f, 1000.0f, false, true },
	/* RoadBlocks::CreateRoadBlockBetween2Points: the police/wanted response;
	 * the matrix's final position and heading, exact (the tilt is applied
	 * after the handle). */
	{ "roadblock car", SPAWN_PURPOSE_POLICE, HARNESS_MODEL_ROADBLOCK,
	  -30.5f, 44.25f, 11.75f, 0.0f, 1.0f, 0, SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, false, false },
	/* CStoredCar::RestoreCar: a special actor, the stored position and
	 * heading. */
	{ "garage stored car", SPAWN_PURPOSE_SPECIAL, HARNESS_MODEL_GARAGE,
	  120.0f, -80.0f, 9.5f, -1.0f, 0.0f, 0, SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, false, false },
	/* Cheats::VehicleCheat: a special actor, the node and the 200 degree
	 * heading, the traffic's tag. */
	{ "cheat car", SPAWN_PURPOSE_SPECIAL, HARNESS_MODEL_CHEAT,
	  -400.0f, 300.0f, 15.0f, -0.9396926f, -0.3420201f, 0, SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, false, false },
	/* CGameLogic::UpdateShortCut: a special actor; the mission spot is
	 * cleared before the insert and the traffic's tag lets SetUpDriver
	 * work (the mission tag follows the driver). */
	{ "taxi shortcut", SPAWN_PURPOSE_SPECIAL, HARNESS_MODEL_TAXI,
	  700.0f, -200.0f, 8.0f, 1.0f, 0.0f, 0, SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, true, false },
	/* CRemote::GivePlayerRemoteControlledCar: a special actor, the model's
	 * base height over the ground, the mission tag. */
	{ "rc car", SPAWN_PURPOSE_SPECIAL, HARNESS_MODEL_REMOTE,
	  -100.0f, -100.0f, 20.0f, 0.0f, 1.0f, SPAWN_VEHICLE_TAG_MISSION, SPAWN_EXPLICIT_GROUND_BASE, 22.0f, 0.0f, false, false },
	/* CPed::ProcessObjective's self-car: a scenario actor at the node, four
	 * metres up, the 200 degree heading. */
	{ "pedai self-car", SPAWN_PURPOSE_SCENARIO, HARNESS_MODEL_SELFCAR,
	  250.0f, 250.0f, 34.0f, -0.9396926f, -0.3420201f, 0, SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, false, false },
};

struct SCreatorPed
{
	const char *name;
	ESpawnPurpose purpose;
	int pedType;
	int pedModel;
	float x, y, z;
	float heading;
};

static const SCreatorPed aCreatorPeds[] = {
	/* CRoadBlocks::GenerateRoadBlockCopsForCar: the police/wanted response,
	 * the exact sidewalk spot, facing the road. */
	{ "roadblock cop", SPAWN_PURPOSE_POLICE, HARNESS_PEDTYPE_COP, HARNESS_COP_STREET,
	  12.0f, -8.0f, 10.9f, -1.5707963f },
	/* CSetPiece::TryToGenerateCopPed: a scenario actor, the ticket's MISSION
	 * rule, the exact resolved spot. */
	{ "set piece cop", SPAWN_PURPOSE_MISSION, HARNESS_PEDTYPE_COP, HARNESS_COP_STREET,
	  60.0f, 70.0f, 11.4f, 0.0f },
	/* CPopulation::AddDeadPedInFrontOfCar: a special actor, the caller's
	 * exact position (the old AddPed call passed it unchanged). */
	{ "dead pedestrian", SPAWN_PURPOSE_SPECIAL, HARNESS_PEDTYPE_CIVMALE, HARNESS_MODEL_DEADPED,
	  5.0f, 6.0f, 7.5f, 0.0f },
};

/* --- A. The converted car creators --------------------------------------- */

static void
TestCreatorCars(void)
{
	printf("A. the converted car creators\n");
	for (int i = 0; i < (int)(sizeof(aCreatorCars) / sizeof(aCreatorCars[0])); i++) {
		const SCreatorCar &row = aCreatorCars[i];
		SSpawnServiceTelemetry tel;
		ScriptWorld world;
		SSpawnExplicitRequest request;
		CVehicle *handle = 0;
		bool ok;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, true, 12.5f, 0.7f);
		request = CarRequest(row.purpose, true, row.model, row.x, row.y, row.z,
			row.dirX, row.dirY, row.tag, row.ground, row.probeZ, row.noHitZ, row.clearSpace,
			row.groundDummies);
		ok = SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel);
		Check(ok, row.name);
		Check(handle == CAR_TOKEN, row.name);
		Check(world.placeSawToken, row.name);
		CheckInt(world.createdCars, 1, row.name);
		CheckInt(world.placedCars, 1, row.name);
		CheckInt(world.lastRequest.carModel, row.model, row.name);
		CheckNear(world.lastRequest.x, row.x, row.name);
		CheckNear(world.lastRequest.y, row.y, row.name);
		CheckNear(world.lastRequest.z, row.z, row.name);
		CheckInt(world.lastRequest.purpose, row.purpose, row.name);
		Check(world.lastRequest.force, row.name);
		CheckInt(world.lastRequest.createdBy, row.tag, row.name);
		CheckInt(world.lastRequest.ground, row.ground, row.name);
		Check(world.lastRequest.clearSpace == row.clearSpace, row.name);
		Check(world.lastRequest.groundDummies == row.groundDummies, row.name);
		/* The decision: the exact position or the scripted ground plus the
		 * model's own height; the heading normalized; the grace floor; the
		 * normalized tag. */
		CheckNear(world.lastDecision.x, row.x, row.name);
		CheckNear(world.lastDecision.y, row.y, row.name);
		if (row.ground == SPAWN_EXPLICIT_GROUND_EXACT)
			CheckNear(world.lastDecision.z, row.z, row.name);
		else
			CheckNear(world.lastDecision.z, 12.5f + 0.7f, row.name);
		{
			float len = sqrtf(row.dirX * row.dirX + row.dirY * row.dirY);
			if (len > 0.0001f) {
				CheckNear(world.lastDecision.dirX, row.dirX / len, row.name);
				CheckNear(world.lastDecision.dirY, row.dirY / len, row.name);
			} else {
				CheckNear(world.lastDecision.dirX, 1.0f, row.name);
				CheckNear(world.lastDecision.dirY, 0.0f, row.name);
			}
		}
		CheckInt(world.lastDecision.createdBy, row.tag ? row.tag : SPAWN_VEHICLE_TAG_RANDOM, row.name);
		CheckInt((int)world.lastDecision.protectMs, (int)NEAR_BUBBLE_GRACE_MS, row.name);
		Check(world.lastDecision.clearSpace == row.clearSpace, row.name);
		/* The ledger: one request by purpose and by EXPLICIT kind, one
		 * creation by purpose, no refusal. */
		CheckInt((int)tel.m_nRequestsByPurpose[row.purpose], 1, row.name);
		CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_EXPLICIT], 1, row.name);
		CheckInt((int)tel.m_nCreatedByPurpose[row.purpose], 1, row.name);
		for (int r = 1; r < SPAWN_REFUSAL_COUNT; r++)
			CheckInt((int)tel.m_nRefusalsByReason[r], 0, row.name);
	}
}

/* --- B. The set piece's and the RC car's ground policies ----------------- */

static void
TestGroundPolicies(void)
{
	printf("B. the ground policies\n");

	/* The set piece: the suspension height over the ground; a miss keeps the
	 * request's z. */
	{
		SSpawnExplicitRequest request = CarRequest(SPAWN_PURPOSE_MISSION, true,
			HARNESS_MODEL_POLICE, 10.0f, 20.0f, 1000.0f, 1.0f, 0.0f, 0,
			SPAWN_EXPLICIT_GROUND_HEIGHT, 1000.0f, 1000.0f, false, true);
		ScriptWorld world;
		SSpawnServiceTelemetry tel;
		CVehicle *handle;
		bool ok;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, true, 30.0f, 0.6f);
		handle = 0;
		ok = SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel);
		Check(ok, "the set piece's ground hit admits");
		CheckNear(world.lastDecision.z, 30.6f, "the set piece lands on the suspension height");

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.6f);
		handle = 0;
		ok = SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel);
		Check(ok, "the set piece's ground miss admits");
		CheckNear(world.lastDecision.z, 1000.0f + 0.6f, "the set piece's miss keeps the request z plus the height");
	}

	/* The RC car: the model's base distance over the ground; a miss uses the
	 * requested no-hit base. */
	{
		SSpawnExplicitRequest request = CarRequest(SPAWN_PURPOSE_SPECIAL, true,
			HARNESS_MODEL_REMOTE, -100.0f, -100.0f, 20.0f, 1.0f, 0.0f,
			SPAWN_VEHICLE_TAG_MISSION, SPAWN_EXPLICIT_GROUND_BASE, 22.0f, 0.0f, false);
		ScriptWorld world;
		SSpawnServiceTelemetry tel;
		CVehicle *handle;
		bool ok;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, true, 19.5f, 1.2f);
		handle = 0;
		ok = SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel);
		Check(ok, "the RC car's ground hit admits");
		CheckNear(world.lastDecision.z, 20.7f, "the RC car lands on the model's base height");

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 1.2f);
		handle = 0;
		ok = SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel);
		Check(ok, "the RC car's ground miss admits");
		CheckNear(world.lastDecision.z, 1.2f, "the RC car's miss uses the no-hit base plus the height");
	}

	/* The exact policy ignores the world's ground and keeps the caller's z. */
	{
		SSpawnExplicitRequest request = CarRequest(SPAWN_PURPOSE_SPECIAL, true,
			HARNESS_MODEL_GARAGE, 1.0f, 2.0f, 3.0f, 1.0f, 0.0f, 0,
			SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, false);
		ScriptWorld world;
		SSpawnServiceTelemetry tel;
		CVehicle *handle;
		bool ok;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, true, 50.0f, 2.0f);
		handle = 0;
		ok = SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel);
		Check(ok, "the exact policy admits");
		CheckNear(world.lastDecision.z, 3.0f, "the exact policy keeps the caller's z");
	}

	/* The heading normalization and the default heading. */
	{
		SSpawnExplicitRequest request = CarRequest(SPAWN_PURPOSE_SPECIAL, true,
			HARNESS_MODEL_CHEAT, 0.0f, 0.0f, 0.0f, 3.0f, 4.0f, 0,
			SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, false);
		ScriptWorld world;
		SSpawnServiceTelemetry tel;
		CVehicle *handle;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel), "the heading admits");
		CheckNear(world.lastDecision.dirX, 0.6f, "the heading is normalized x");
		CheckNear(world.lastDecision.dirY, 0.8f, "the heading is normalized y");

		request.dirX = 0.0f;
		request.dirY = 0.0f;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel), "the empty heading admits");
		CheckNear(world.lastDecision.dirX, 1.0f, "the empty heading defaults to x");
		CheckNear(world.lastDecision.dirY, 0.0f, "the empty heading defaults to y");
	}

	/* The lifetime floor: a requested lifetime above the grace stands. */
	{
		SSpawnExplicitRequest request = CarRequest(SPAWN_PURPOSE_SPECIAL, true,
			HARNESS_MODEL_TAXI, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0,
			SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, true);
		ScriptWorld world;
		SSpawnServiceTelemetry tel;
		CVehicle *handle;

		request.lifetimeMs = 30000;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel), "the long lifetime admits");
		CheckInt((int)world.lastDecision.protectMs, 30000, "the requested lifetime stands above the grace");
	}
}

/* --- C. The converted pedestrian creators -------------------------------- */

static void
TestCreatorPeds(void)
{
	printf("C. the converted pedestrian creators\n");
	for (int i = 0; i < (int)(sizeof(aCreatorPeds) / sizeof(aCreatorPeds[0])); i++) {
		const SCreatorPed &row = aCreatorPeds[i];
		SSpawnServiceTelemetry tel;
		ScriptWorld world;
		SSpawnExplicitPedRequest request;
		CPed *handle = 0;
		bool ok;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		request = PedRequest(row.purpose, true, SPAWN_PED_PLACE_STANDING, row.pedType,
			row.pedModel, row.x, row.y, row.z, row.heading, 0, false, 0);
		ok = SpawnRunExplicitPed(request, RoomFacts(true), ScriptCreatePed, &world, &handle, &tel);
		Check(ok, row.name);
		Check(handle == PED_TOKEN, row.name);
		CheckInt(world.createdPeds, 1, row.name);
		CheckInt(world.lastPedRequest.pedType, row.pedType, row.name);
		CheckInt(world.lastPedRequest.pedModel, row.pedModel, row.name);
		CheckNear(world.lastPedRequest.x, row.x, row.name);
		CheckNear(world.lastPedRequest.y, row.y, row.name);
		CheckNear(world.lastPedRequest.z, row.z, row.name);
		CheckNear(world.lastPedRequest.heading, row.heading, row.name);
		CheckInt(world.lastPedRequest.purpose, row.purpose, row.name);
		Check(world.lastPedRequest.force, row.name);
		CheckInt(world.lastPedRequest.createdBy, 0, row.name);
		Check(!world.lastPedRequest.missionFlags, row.name);
		CheckInt((int)tel.m_nRequestsByPurpose[row.purpose], 1, row.name);
		CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_EXPLICIT], 1, row.name);
		CheckInt((int)tel.m_nCreatedByPurpose[row.purpose], 1, row.name);
		for (int r = 1; r < SPAWN_REFUSAL_COUNT; r++)
			CheckInt((int)tel.m_nRefusalsByReason[r], 0, row.name);
	}
}

/* --- D. The admission: the force bypass, the pool and the bad requests --- */

static void
TestAdmission(void)
{
	printf("D. the admission\n");

	/* The car arm. */
	{
		SSpawnExplicitRequest request = CarRequest(SPAWN_PURPOSE_MISSION, true,
			HARNESS_MODEL_POLICE, 1.0f, 2.0f, 3.0f, 1.0f, 0.0f, 0,
			SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, false);
		ScriptWorld world;
		SSpawnServiceTelemetry tel;
		CVehicle *handle;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(SpawnRunExplicit(request, FullFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel), "the force bypass clears a full budget");
		Check(handle == CAR_TOKEN, "the force bypass returns the handle");

		request.force = false;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(!SpawnRunExplicit(request, FullFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel), "a non-forced request refuses the full budget");
		Check(handle == 0, "a refused request has no handle");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_BUDGET], 1, "the budget row");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_MISSION], 0, "no created row on a refusal");

		request.force = true;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(!SpawnRunExplicit(request, FullFacts(false), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel), "the physical pool refuses even a forced request");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_POOL], 1, "the pool row");

		request.carModel = -1;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(!SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar, &world,
			&handle, &tel), "a request without a model refuses");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE], 1, "the no-site row");
	}

	/* The ped arm. */
	{
		SSpawnExplicitPedRequest request = PedRequest(SPAWN_PURPOSE_POLICE, true,
			SPAWN_PED_PLACE_STANDING, HARNESS_PEDTYPE_COP, HARNESS_COP_STREET,
			1.0f, 2.0f, 3.0f, 0.0f, 0, false, 0);
		ScriptWorld world;
		SSpawnServiceTelemetry tel;
		CPed *handle;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(SpawnRunExplicitPed(request, FullFacts(true), ScriptCreatePed, &world, &handle, &tel),
			"the force bypass clears a full budget for a ped");
		Check(handle == PED_TOKEN, "the ped's force bypass returns the handle");

		request.force = false;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(!SpawnRunExplicitPed(request, FullFacts(true), ScriptCreatePed, &world, &handle, &tel),
			"a non-forced ped refuses the full budget");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_BUDGET], 1, "the ped budget row");

		request.force = true;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(!SpawnRunExplicitPed(request, FullFacts(false), ScriptCreatePed, &world, &handle, &tel),
			"the physical ped pool refuses even a forced request");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_POOL], 1, "the ped pool row");

		request.pedModel = -1;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(!SpawnRunExplicitPed(request, RoomFacts(true), ScriptCreatePed, &world, &handle, &tel),
			"a standing ped without a model refuses");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE], 1, "the ped no-site row");

		request.place = SPAWN_PED_PLACE_DRIVER;
		request.pedModel = 1;
		request.vehicle = 0;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(!SpawnRunExplicitPed(request, RoomFacts(true), ScriptCreatePed, &world, &handle, &tel),
			"an in-car ped without its vehicle refuses");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE], 1, "the in-car no-site row");

		request.vehicle = (CVehicle *)0x3000;
		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		handle = 0;
		Check(SpawnRunExplicitPed(request, RoomFacts(true), ScriptCreatePed, &world, &handle, &tel),
			"an in-car ped with its vehicle admits");
		Check(handle == PED_TOKEN, "the in-car ped's same-frame handle");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_POLICE], 1, "the in-car ped's created row");
	}
}

/* --- E. The ledger and the soak ------------------------------------------ */

static void
TestSoak(void)
{
	printf("E. the ledger and the soak\n");
	{
		SSpawnServiceTelemetry tel;
		ScriptWorld world;
		int created = 0;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, true, 10.0f, 0.5f);
		for (int i = 0; i < 1000; i++) {
			const SCreatorCar &row = aCreatorCars[i % (sizeof(aCreatorCars) / sizeof(aCreatorCars[0]))];
			SSpawnExplicitRequest request = CarRequest(row.purpose, true, row.model,
				row.x, row.y, row.z, row.dirX, row.dirY, row.tag, row.ground,
				row.probeZ, row.noHitZ, row.clearSpace);
			CVehicle *handle = 0;

			if (SpawnRunExplicit(request, RoomFacts(true), ScriptCreateCar, ScriptPlaceCar,
				&world, &handle, &tel))
				created++;
		}
		CheckInt(created, 1000, "the soak creates every car");
		CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_EXPLICIT], 1000, "the soak's kind row");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_MISSION], 143, "the soak's mission rows");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_POLICE], 143, "the soak's police rows");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_SPECIAL], 572, "the soak's special rows");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_SCENARIO], 142, "the soak's scenario rows");
		for (int r = 1; r < SPAWN_REFUSAL_COUNT; r++)
			CheckInt((int)tel.m_nRefusalsByReason[r], 0, "the soak's refusal rows");
	}
	{
		SSpawnServiceTelemetry tel;
		ScriptWorld world;
		int admitted = 0, refused = 0;

		SpawnTelemetryReset(&tel);
		ScriptWorldInit(&world, false, 0.0f, 0.0f);
		for (int i = 0; i < 300; i++) {
			SSpawnExplicitRequest request = CarRequest(SPAWN_PURPOSE_SPECIAL, true,
				HARNESS_MODEL_GARAGE, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0,
				SPAWN_EXPLICIT_GROUND_EXACT, 0.0f, 0.0f, false);
			CVehicle *handle = 0;
			bool pool = (i % 3) != 0;

			if (SpawnRunExplicit(request, RoomFacts(pool), ScriptCreateCar, ScriptPlaceCar,
				&world, &handle, &tel))
				admitted++;
			else
				refused++;
		}
		CheckInt(admitted, 200, "the mixed soak admits two thirds");
		CheckInt(refused, 100, "the mixed soak refuses one third");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_SPECIAL], admitted, "the mixed soak's created row");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_POOL], refused, "the mixed soak's pool row");
		CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_SPECIAL], admitted + refused,
			"the mixed soak's request row is created plus refused");
	}
}

int
main(void)
{
	printf("the remaining direct creators (VICE-158)\n\n");
	TestCreatorCars();
	TestGroundPolicies();
	TestCreatorPeds();
	TestAdmission();
	TestSoak();
	printf("\n%lld checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
