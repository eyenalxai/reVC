#include "common.h"

#include "CarCtrl.h"

#include "ResponderPolicy.h"

#include "Accident.h"
#include "Automobile.h"
#include "Bike.h"
#include "Camera.h"
#include "CarAI.h"
#include "CarGen.h"
#include "CarOnRails.h"
#include "Cranes.h"
#include "Curves.h"
#include "CutsceneMgr.h"
#include "Gangs.h"
#include "Game.h"
#include "Garages.h"
#include "General.h"
#include "IniFile.h"
#include "ModelIndices.h"
#include "PathFind.h"
#include "Ped.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Population.h"
#include "Wanted.h"
#include "Pools.h"
#include "Renderer.h"
#include "RoadBlocks.h"
#include "Timer.h"
#include "TrafficLights.h"
#include "TrafficRings.h"
#include "FillBurst.h"
#include "SpawnOccupancy.h"
#include "Streaming.h"
#include "VisibilityPlugins.h"
#include "Vehicle.h"
#include "Fire.h"
#include "WaterLevel.h"
#include "World.h"
#include "Zones.h"

#define DISTANCE_TO_SPAWN_ROADBLOCK_PEDS (51.0f)
#define DISTANCE_TO_SCAN_FOR_DANGER (14.0f)
#define DISTANCE_TO_SCAN_FOR_PED_DANGER (11.0f)
#define SAFE_DISTANCE_TO_PED (3.0f)
#define INFINITE_Z (1000000000.0f)

#define VEHICLE_HEIGHT_DIFF_TO_CONSIDER_WEAVING (4.0f)
#define PED_HEIGHT_DIFF_TO_CONSIDER_WEAVING (4.0f)
#define OBJECT_HEIGHT_DIFF_TO_CONSIDER_WEAVING (8.0f)
#define WIDTH_COEF_TO_WEAVE_SAFELY (1.2f)
#define OBJECT_WIDTH_TO_WEAVE (0.3f)
#define PED_WIDTH_TO_WEAVE (0.8f)

/* The path pick's own numbers and its direction test moved into
 * CarOnRails.h with the pick itself (VICE-133): the far layer's simulation runs
 * the same rules on a worker thread and cannot call CCarCtrl. */

#define DISTANCE_TO_SWITCH_FROM_BLOCK_TO_STOP (5.0f)
#define DISTANCE_TO_SWITCH_FROM_STOP_TO_BLOCK (10.0f)
#define MAX_SPEED_TO_ACCOUNT_IN_INTERCEPTING (0.13f)
#define DISTANCE_TO_NEXT_NODE_TO_CONSIDER_SLOWING_DOWN (40.0f)
#define MAX_ANGLE_TO_STEER_AT_HIGH_SPEED (0.2f)
#define MIN_SPEED_TO_START_LIMITING_STEER (0.45f)
#define DISTANCE_TO_NEXT_NODE_TO_SELECT_NEW (5.0f)
#define DISTANCE_TO_FACING_NEXT_NODE_TO_SELECT_NEW (8.0f)
#define DEFAULT_MAX_STEER_ANGLE (0.5f)
#define MIN_LOWERING_SPEED_COEFFICIENT (0.4f)
#define MAX_ANGLE_FOR_SPEED_LIMITING (1.2f)
#define MIN_ANGLE_FOR_SPEED_LIMITING (0.4f)
#define MIN_ANGLE_FOR_SPEED_LIMITING_BETWEEN_NODES (0.1f)
#define MIN_ANGLE_TO_APPLY_HANDBRAKE (0.7f)
#define MIN_SPEED_TO_APPLY_HANDBRAKE (0.3f)

#define PROBABILITY_OF_DEAD_PED_ACCIDENT (0.005f)
#define DISTANCE_BETWEEN_CAR_AND_DEAD_PED (6.0f)
#define PROBABILITY_OF_PASSENGER_IN_VEHICLE (0.125f)

bool CCarCtrl::bMadDriversCheat;
int CCarCtrl::NumLawEnforcerCars;
int CCarCtrl::NumAmbulancesOnDuty;
int CCarCtrl::NumFiretrucksOnDuty;
bool CCarCtrl::bCarsGeneratedAroundCamera;
float CCarCtrl::CarDensityMultiplier = 1.0f;
int32 CCarCtrl::NumMissionCars;
int32 CCarCtrl::NumRandomCars;
int32 CCarCtrl::NumParkedCars;
int32 CCarCtrl::NumPermanentCars;
int8 CCarCtrl::CountDownToCarsAtStart;
int32 CCarCtrl::MaxNumberOfCarsInUse = DEFAULT_MAX_NUMBER_OF_CARS;
uint32 CCarCtrl::LastTimeLawEnforcerCreated;
uint32 CCarCtrl::LastTimeFireTruckCreated;
uint32 CCarCtrl::LastTimeAmbulanceCreated;
int32 CCarCtrl::MiamiViceCycle;
uint32 CCarCtrl::LastTimeMiamiViceGenerated;
int32 CCarCtrl::TotalNumOfCarsOfRating[TOTAL_CUSTOM_CLASSES];
int32 CCarCtrl::CarArrays[TOTAL_CUSTOM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];
int32 CCarCtrl::NumRequestsOfCarRating[TOTAL_CUSTOM_CLASSES];
int32 CCarCtrl::NumOfLoadedCarsOfRating[TOTAL_CUSTOM_CLASSES];
int32 CCarCtrl::CarFreqArrays[TOTAL_CUSTOM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];
int32 CCarCtrl::LoadedCarsArray[TOTAL_CUSTOM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];
CVehicle* apCarsToKeep[MAX_CARS_TO_KEEP];
uint32 aCarsToKeepTime[MAX_CARS_TO_KEEP];

/* The fill burst (VICE-126): the state of the window and the counters the live
 * rig reads (utils/caps-audit/live-counters.py). The "total" pair accumulates
 * since boot (the last Init or ReInit), so the boot burst stays readable after
 * a later arming resets the window rows; the four "ended" counters count the
 * burst runs by the reason they ended (VICE-146): the target's stop line, the
 * full car budget, the give-up bound and the spent window. */
SFillBurst CCarCtrl::ms_FillBurst;
uint32 CCarCtrl::ms_nFillBurstAttempts;
uint32 CCarCtrl::ms_nFillBurstPlaced;
uint32 CCarCtrl::ms_nFillBurstTotalAttempts;
uint32 CCarCtrl::ms_nFillBurstTotalPlaced;
uint32 CCarCtrl::ms_nFillBurstEndedLine;
uint32 CCarCtrl::ms_nFillBurstEndedBudget;
uint32 CCarCtrl::ms_nFillBurstEndedStalls;
uint32 CCarCtrl::ms_nFillBurstEndedWindow;
float CCarCtrl::ms_fFillBurstTarget;

float
CCarCtrl::GetRandomCarTarget()
{
	// The ring area grows with the square of the ring scale, so the target scales with it too.
	return Min(RANDOM_CARS_PER_INI_UNIT * CarDensityMultiplier * CIniFile::CarNumberMultiplier * SQR(CurrentTrafficRings().scale), (float)MAX_RANDOM_CARS);
}

int32
CCarCtrl::NumCarsInUse(void)
{
	// Every car the car budget counts: the random ones (law enforcers included:
	// a police random car is counted both here and in NumLawEnforcerCars), the
	// parked ones, the mission and script cars and the emergency services.
	return NumFiretrucksOnDuty + NumAmbulancesOnDuty + NumParkedCars + NumMissionCars
		+ NumLawEnforcerCars + NumRandomCars;
}

void
CCarCtrl::GenerateRandomCars()
{
	/* The near bubble begins its frame here, before any gate can run: the
	 * line-of-sight budget of the frame and the target of the fill
	 * (VICE-127), through the service's API (VICE-150). The cutscene gate
	 * below must not leave a stale frame. */
	CSpawnService::BeginFrame();
	/* One table read per frame: the rate that fills the ring follows the ring
	 * scale, exactly as the count target does. */
	const STrafficRings rings = CurrentTrafficRings();
	/* The fill waits out a cutscene here, and the burst arms at the end of one
	 * (CCutsceneMgr::DeleteCutsceneData, VICE-126). */
	if (CCutsceneMgr::IsRunning())
		return;
	/* A car brings a driver and can bring passengers, both of them pedestrians
	 * of the same pool as the crowd (the emergency services of the frame
	 * included). When the pool is down to its reserve, no car is created: the
	 * crew of one more frame cannot take the pool the pedestrians need
	 * (PopulationBudget.h, PED_POOL_RESERVE). The check is one pool scan a frame,
	 * not one per call. */
	if (CPools::GetPedPool()->GetNoOfFreeSpaces() < PED_POOL_RESERVE)
		return;
	/* The police are their own requester (VICE-156): the wanted system's
	 * stars and caps decide, and the request goes to the service even when
	 * the ambient car target is full. */
	GeneratePoliceCars();
	if (NumRandomCars < MAX_RANDOM_CARS){
		if (CountDownToCarsAtStart != 0) {
			// The countdown lets the world settle after boot and after a
			// restart; its end arms the fill burst (VICE-126), with the parked
			// cars, exactly as the shipped burst of that path did.
			if (--CountDownToCarsAtStart == 0)
				ArmFillBurst(true);
		} else if (FillBurstIsRunning(&ms_FillBurst)) {
			SpendFillBurstFrame();
		} else {
			for (int i = 0; i < RANDOM_CAR_ATTEMPTS_PER_FRAME * rings.scale; i++)
				GenerateOneRandomCar();
		}
	}
	/* Approximately once per 4 seconds. */
	if ((CTimer::GetTimeInMilliseconds() & 0xFFFFF000) != (CTimer::GetPreviousTimeInMilliseconds() & 0xFFFFF000))
		GenerateEmergencyServicesCar();
}

/* Arm the car fill burst (VICE-126): a transition has happened and the traffic
 * may fill at once. `withParkedCars` bypasses the timer of every car generator
 * for 20 frames, exactly what the shipped burst of the boot, the restart and
 * the cutscene end did. The interior exit does not ask for it: that trigger is
 * new, and it would be an unasked change of the parked cars. The bypass is the
 * timer only, never the ring: a generator still places its car inside its band
 * of the table, a few hundred metres out (CCarGenerator::
 * CheckIfWithinRangeOfAnyPlayers), so a parked car never appears at the player. */
void
CCarCtrl::ArmFillBurst(bool withParkedCars)
{
	FillBurstArm(&ms_FillBurst);
	ms_nFillBurstAttempts = 0;
	ms_nFillBurstPlaced = 0;
	ms_fFillBurstTarget = 0.0f;
	/* The totals since boot and the end reasons are not reset here: they are
	 * the boot burst's own evidence after a later arming (VICE-146). */
	if (withParkedCars)
		CTheCarGenerators::GenerateEvenIfPlayerIsCloseCounter = 20;
}

/* The streaming gate of the car burst (VICE-126): a random car call chooses
 * from the loaded class arrays and gives up when every rating array is empty
 * (ChooseModel, ChooseCarModel), so a burst that runs before the streaming has
 * a car model spends its window for nothing. True while at least one rating
 * array holds a model. */
static bool
CarModelsAreResident(void)
{
	for (int i = 0; i < CCarCtrl::TOTAL_CUSTOM_CLASSES; i++)
		if (CCarCtrl::NumOfLoadedCarsOfRating[i] > 0)
			return true;
	return false;
}

/* One frame of the car fill burst (VICE-126). The attempts come from the burst
 * table: the whole budget while the screen hides the fill, the small one while
 * the player can see it. The stop line is re-read every attempt (the random car
 * count is live, and one placement is one car), the pool every
 * FILL_BURST_POOL_CHECK_ATTEMPTS attempts. A car placement runs the collision
 * queries of the frame (VICE-121), so the budget is the work bound of the
 * burst. */
void
CCarCtrl::SpendFillBurstFrame(void)
{
	/* The stop lines: the random car target of the ring, the line
	 * GenerateOneRandomCar stops at, and the car budget behind it. A burst never
	 * runs past either, and the end counter says which one bound. */
	ms_fFillBurstTarget = GetRandomCarTarget();
	/* The boot hazard (VICE-146): before the camera and the ring are live the
	 * target and the car budget line are 0, and the stop line would burn the
	 * whole burst on a target that cannot be filled. The frame waits instead,
	 * in the same bounded way the streaming wait uses. */
	if (ms_fFillBurstTarget <= 0.0f || MaxNumberOfCarsInUse <= 0) {
		FillBurstSpendFrame(&ms_FillBurst, false, false);
		return;
	}
	/* The two stop lines are counted apart: the target's line says the area is
	 * full, the budget's says the car budget is (the two mean different
	 * things to the orchestrator, so the reason rows do not conflate them). */
	if (FillBurstTargetReached((float)NumRandomCars, ms_fFillBurstTarget)) {
		ms_nFillBurstEndedLine++;
		FillBurstDisarm(&ms_FillBurst);
		return;
	}
	if (NumCarsInUse() >= MaxNumberOfCarsInUse) {
		ms_nFillBurstEndedBudget++;
		FillBurstDisarm(&ms_FillBurst);
		return;
	}
	/* The streaming: the frame waits for a car model instead of spending the
	 * window on attempts that cannot choose one. The wait does not spend the
	 * window; it counts against the give-up bound of the helper. */
	if (!CarModelsAreResident()) {
		FillBurstSpendFrame(&ms_FillBurst, false, false);
		return;
	}

	int attempts = FillBurstAttempts(&ms_FillBurst, FillBurstIsHidden());
	int32 placed = 0;
	for (int i = 0; i < attempts; i++) {
		// The stop line of this attempt, then the pool of the step: a car
		// brings its crew (a driver and up to three passengers), so a frame
		// that finds the pool down to its reserve stops here
		// (PopulationBudget.h, PED_POOL_RESERVE). The attempt counter follows
		// the attempts the frame makes.
		if (FillBurstTargetReached((float)NumRandomCars, ms_fFillBurstTarget))
			break;
		if (i % FILL_BURST_POOL_CHECK_ATTEMPTS == 0 &&
			CPools::GetPedPool()->GetNoOfFreeSpaces() < PED_POOL_RESERVE)
			break;
		ms_nFillBurstAttempts++;
		ms_nFillBurstTotalAttempts++;
		int32 before = NumRandomCars;
		GenerateOneRandomCar();
		if (NumRandomCars > before)
			placed++;
	}

	ms_nFillBurstPlaced += placed;
	ms_nFillBurstTotalPlaced += placed;
	EFillBurstEnded ended = FillBurstSpendFrame(&ms_FillBurst, true, placed > 0);
	if (ended == FILL_BURST_ENDED_STALLS)
		ms_nFillBurstEndedStalls++;
	else if (ended == FILL_BURST_ENDED_WINDOW)
		ms_nFillBurstEndedWindow++;
}

/* The spawn ring of one traffic call (VICE-150, shared with the police request
 * in VICE-156): the camera and the player vehicle's speed choose the cone, the
 * band and the collision test. The ambient traffic and the police request both
 * read this, so a police car arrives by the ring rule of the traffic around
 * it. */
struct STrafficSpawnRing
{
	float frontX, frontY;
	float preferredDistance;
	float angleLimit;
	bool invertAngleLimitTest;
	bool testForCollision;
};

static void
ChooseTrafficSpawnRing(const STrafficRings &rings, STrafficSpawnRing *pRing)
{
	CVehicle *pPlayerVehicle = FindPlayerVehicle();
	CVector2D vecPlayerVehicleSpeed;
	float fPlayerVehicleSpeed = 0.0f;

	if (pPlayerVehicle) {
		vecPlayerVehicleSpeed = FindPlayerVehicle()->GetMoveSpeed();
		fPlayerVehicleSpeed = vecPlayerVehicleSpeed.Magnitude();
	}
	if (TheCamera.GetForward().z < -0.9f) {
		/* Player uses topdown camera. */
		/* Spawn essentially anywhere. */
		pRing->frontX = pRing->frontY = 0.707f; /* 45 degrees */
		pRing->angleLimit = -1.0f;
		pRing->invertAngleLimitTest = true;
		pRing->preferredDistance = rings.carSideSpawn + 15.0f;
		/* BUG: testForCollision not initialized in original game. */
		pRing->testForCollision = false;
	} else if (!pPlayerVehicle) {
		/* Player is not in vehicle. */
		pRing->testForCollision = true;
		pRing->frontX = TheCamera.CamFrontXNorm;
		pRing->frontY = TheCamera.CamFrontYNorm;
		switch (CTimer::GetFrameCounter() & 1) {
		case 0:
			/* Spawn a vehicle relatively far away from player. */
			/* Forward to his current direction (camera direction). */
			pRing->angleLimit = 0.707f; /* 45 degrees */
			pRing->invertAngleLimitTest = true;
			pRing->preferredDistance = rings.carOnScreenSpawnMax;
			break;
		case 1:
			/* Spawn a vehicle close to player to his side. */
			/* Kinda not within camera angle. */
			pRing->angleLimit = 0.707f; /* 45 degrees */
			pRing->invertAngleLimitTest = false;
			pRing->preferredDistance = rings.carSideSpawn;
			break;
		}
	} else if (fPlayerVehicleSpeed > 0.4f) { /* 72 km/h */
		/* Player is moving fast in vehicle */
		/* Prefer spawning vehicles very far away from him. */
		pRing->frontX = vecPlayerVehicleSpeed.x / fPlayerVehicleSpeed;
		pRing->frontY = vecPlayerVehicleSpeed.y / fPlayerVehicleSpeed;
		pRing->testForCollision = false;
		switch (CTimer::GetFrameCounter() & 3) {
		case 0:
		case 1:
			/* Spawn a vehicle in a very narrow gap in front of a player */
			pRing->angleLimit = 0.85f; /* approx 30 degrees */
			pRing->invertAngleLimitTest = true;
			pRing->preferredDistance = rings.carOnScreenSpawnMax;
			break;
		case 2:
			/* Spawn a vehicle relatively far away from player. */
			/* Forward to his current direction (camera direction). */
			pRing->angleLimit = 0.707f; /* 45 degrees */
			pRing->invertAngleLimitTest = true;
			pRing->preferredDistance = rings.carOnScreenSpawnMax;
			break;
		case 3:
			/* Spawn a vehicle close to player to his side. */
			/* Kinda not within camera angle. */
			pRing->angleLimit = 0.707f; /* 45 degrees */
			pRing->invertAngleLimitTest = false;
			pRing->preferredDistance = rings.carSideSpawn;
			break;
		}
	} else if (fPlayerVehicleSpeed > 0.1f) { /* 18 km/h */
		/* Player is moving moderately fast in vehicle */
		/* Spawn more vehicles to player's side. */
		pRing->frontX = vecPlayerVehicleSpeed.x / fPlayerVehicleSpeed;
		pRing->frontY = vecPlayerVehicleSpeed.y / fPlayerVehicleSpeed;
		pRing->testForCollision = false;
		switch (CTimer::GetFrameCounter() & 3) {
		case 0:
			/* Spawn a vehicle in a very narrow gap in front of a player */
			pRing->angleLimit = 0.85f; /* approx 30 degrees */
			pRing->invertAngleLimitTest = true;
			pRing->preferredDistance = rings.carOnScreenSpawnMax;
			break;
		case 1:
			/* Spawn a vehicle relatively far away from player. */
			/* Forward to his current direction (camera direction). */
			pRing->angleLimit = 0.707f; /* 45 degrees */
			pRing->invertAngleLimitTest = true;
			pRing->preferredDistance = rings.carOnScreenSpawnMax;
			break;
		case 2:
		case 3:
			/* Spawn a vehicle close to player to his side. */
			/* Kinda not within camera angle. */
			pRing->angleLimit = 0.707f; /* 45 degrees */
			pRing->invertAngleLimitTest = false;
			pRing->preferredDistance = rings.carSideSpawn;
			break;
		}
	} else {
		/* Player is in vehicle but moving very slow. */
		/* Then use camera direction instead of vehicle direction. */
		pRing->testForCollision = true;
		pRing->frontX = TheCamera.CamFrontXNorm;
		pRing->frontY = TheCamera.CamFrontYNorm;
		switch (CTimer::GetFrameCounter() & 1) {
		case 0:
			/* Spawn a vehicle relatively far away from player. */
			/* Forward to his current direction (camera direction). */
			pRing->angleLimit = 0.707f; /* 45 degrees */
			pRing->invertAngleLimitTest = true;
			pRing->preferredDistance = rings.carOnScreenSpawnMax;
			break;
		case 1:
			/* Spawn a vehicle close to player to his side. */
			/* Kinda not within camera angle. */
			pRing->angleLimit = 0.707f; /* 45 degrees */
			pRing->invertAngleLimitTest = false;
			pRing->preferredDistance = rings.carSideSpawn;
			break;
		}
	}
}

void
CCarCtrl::GenerateOneRandomCar()
{
	CPlayerInfo* pPlayer = &CWorld::Players[CWorld::PlayerInFocus];
	CVector vecTargetPos = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	CVector2D vecPlayerSpeed = FindPlayerSpeed();
	CZoneInfo zone;
	CTheZones::GetZoneInfoForTimeOfDay(&vecTargetPos, &zone);
	float trafficDensity = pPlayer->m_fRoadDensity * zone.carDensity;
	pPlayer->m_nTrafficMultiplier = trafficDensity;
	/* One table read per call: the spawn rings, the birth culls and the
	 * camera rule of this call all come from it. */
	const STrafficRings rings = CurrentTrafficRings();
	/* The budget gates of this call moved into the service (VICE-150): one
	 * request, one gate (the car target, the car budget, the ped pool's
	 * reserve). What stays here is the requester's policy: the zone's model
	 * dice and the ring the call aims at. The police are their own requester
	 * (VICE-156, GeneratePoliceCars): an ambient zone may still choose a cop
	 * class below one star, and with any wanted level the ambient traffic
	 * leaves the police to the wanted system. */
	CWanted* pWanted = pPlayer->m_pPed->m_pWanted;
	int carClass;
	int carModel;
	carModel = ChooseModel(&zone, &carClass);
	if (carModel == -1 || (carClass == COPS && pWanted->GetWantedLevel() >= 1))
		/* In particular it means that cop cars never spawn if player has wanted level of 1. */
		return;

	STrafficSpawnRing ring;
	ChooseTrafficSpawnRing(rings, &ring);

	/* The request (VICE-150): the service plans the site - the index's
	 * candidates, the live zone, the occupancy, the visibility ladder and
	 * the reachability - and the primitive creates the car there. The
	 * class and the model above are the requester's policy; the water
	 * fallback (the boat model, the cop boat) is the service's per-site
	 * resolution, kept from this call.
	 * The stream shift: the removed rarity draw and the moved budget gate
	 * consume the shared dice differently (the model choice now rolls
	 * before the gate), exactly as the fill burst's own shift (ADR 0021). */
	SSpawnCarRequest request;
	request.purpose = SPAWN_PURPOSE_AMBIENT;
	request.force = false;
	request.priority = false;
	request.allowBoats = true;
	request.requireReach = true;
	request.carClass = carClass;
	request.carModel = carModel;
	request.targetX = vecTargetPos.x;
	request.targetY = vecTargetPos.y;
	request.targetZ = vecTargetPos.z;
	request.centreX = vecTargetPos.x;
	request.centreY = vecTargetPos.y;
	request.centreZ = vecTargetPos.z;
	request.playerSpeedX = vecPlayerSpeed.x;
	request.playerSpeedY = vecPlayerSpeed.y;
	request.testForCollision = ring.testForCollision;
	request.spawnDist = ring.preferredDistance;
	request.dirX = ring.frontX;
	request.dirY = ring.frontY;
	request.angleLimit = ring.angleLimit;
	request.forward = ring.invertAngleLimitTest;
	request.allowDisabled = carClass == COPS && pWanted->GetWantedLevel() >= 1;
	request.responder = SPAWN_RESPONDER_NONE;
	request.destinationX = 0.0f;
	request.destinationY = 0.0f;
	request.destinationZ = 0.0f;
	CSpawnService::RequestCar(request, nil);
}

/*
 * The police requester (VICE-156). The wanted system stays the policy owner:
 * its stars, its law-enforcer and cop caps and its model tiers
 * (ChoosePoliceCarModel) decide whether and what, and PoliceRequestGate holds
 * the old branch's cadence. The service plans the site on the traffic ring
 * around the player and creates the car; the primitive keeps the pursuit
 * state (the wanted-level mission and speed) and the occupants. Boats are
 * allowed, so a water site of the request takes the Predator.
 *
 * The old code took this branch inside GenerateOneRandomCar, so a full ambient
 * car target stopped the police as well. The request is now its own call with
 * the priority flag: the ambient fill target is not a police cap, while the
 * car budget, the pool and the wanted caps still hold.
 */
void
CCarCtrl::GeneratePoliceCars(void)
{
	CPlayerInfo* pPlayer = &CWorld::Players[CWorld::PlayerInFocus];
	CVector vecTargetPos;
	CVector2D vecPlayerSpeed;
	CWanted* pWanted;
	const STrafficRings rings = CurrentTrafficRings();
	SPoliceFacts facts;
	STrafficSpawnRing ring;
	SSpawnCarRequest request;

	/* The frame may run before the player exists (the boot countdown); no
	 * player, no wanted system, no request. */
	if (pPlayer->m_pPed == nil || pPlayer->m_pPed->m_pWanted == nil)
		return;
	pWanted = pPlayer->m_pPed->m_pWanted;

	facts.wantedLevel = pWanted->GetWantedLevel();
	facts.lawEnforcers = NumLawEnforcerCars;
	facts.maxVehicles = pWanted->m_MaximumLawEnforcerVehicles;
	facts.currentCops = pWanted->m_CurrentCops;
	facts.maxCops = pWanted->m_MaxCops;
	facts.interior = CGame::IsInInterior();
	facts.nowMs = CTimer::GetTimeInMilliseconds();
	facts.lastCreatedMs = LastTimeLawEnforcerCreated;
	if (PoliceRequestGate(facts) != POLICE_GATE_ALLOWED)
		return;

	vecTargetPos = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	vecPlayerSpeed = FindPlayerSpeed();
	ChooseTrafficSpawnRing(rings, &ring);
	PoliceFillRequest(ChoosePoliceCarModel(), COPS, vecTargetPos.x, vecTargetPos.y, vecTargetPos.z,
		vecPlayerSpeed.x, vecPlayerSpeed.y, ring.preferredDistance, ring.frontX, ring.frontY,
		ring.angleLimit, ring.invertAngleLimitTest, ring.testForCollision, &request);
	CSpawnService::RequestCar(request, nil);
}

/* The creation primitive (VICE-150). A car of the traffic spawner, of the
 * spawn service's road-site plan, or - with SPAWN_TIER_FAR - a car of the far
 * traffic layer (VICE-128). The keep list of the design: the route and the
 * lane state, the ground and the water height, the physics and the status,
 * the occupants with the caps and reserves, the final validation and the
 * counters. The strips: the rarity dice (the planner's weight replaced it)
 * and the camera decisions (the ladder above owns visibility; the top-down
 * flag is gone). The tier passes in: the far layer's cars skip the near
 * ring's birth cull, leave in the far state and skip the occupants. The
 * refusal is one of ESpawnRefusal, which the service counts by reason;
 * `ppCreated` (optional) receives the car. The driver contract: SetUpDriver
 * needs RANDOM_VEHICLE, so the near tier creates with it (the far promotion
 * sets the tag before its own call). `pArm` (optional) receives the arm the
 * creation gate resolved at the final position (the covered-fill row's own
 * answer). */
ESpawnRefusal
CCarCtrl::CreateCarFromPlacement(const SSpawnPlacement& placement, int carClass, int carModel, const CVector& vecTargetPos, const CVector2D& vecPlayerSpeed, bool testForCollision, ESpawnTier tier, CVehicle **ppCreated, ENearBubbleArm *pArm)
{
	bool bFarTraffic = tier == SPAWN_TIER_FAR;
	CPathNode* pCurNode = &ThePaths.m_pathNodes[placement.node1];
	CPathNode* pNextNode = &ThePaths.m_pathNodes[placement.node2];
	bool bBoatGenerated = pCurNode->bWaterPath;
	/* The birth culls below read the table of this call, exactly like the call
	 * that asked for the placement. */
	const STrafficRings rings = CurrentTrafficRings();
	if (bBoatGenerated && (pCurNode->bOnlySmallBoats || pNextNode->bOnlySmallBoats) && BoatWithTallMast(carModel))
		return SPAWN_REFUSAL_NO_SITE;
	if (pCurNode->numLinks == 1)
		/* Do not create vehicle if there is nowhere to go. */
		return SPAWN_REFUSAL_NO_SITE;
	int16 idInNode = 0;
	int16 linkId;
	/* A planned site carries the car path LINK id itself (SSpawnSite::link,
	 * the value m_carPathLinks is indexed with; the directed connection slot
	 * is a different number and only the reservations and the site id use
	 * it); a raw placement (the far layer's collector) resolves the link by
	 * its node-pair lookup. */
	if (placement.link >= 0) {
		linkId = placement.link;
	} else {
		while (idInNode < pCurNode->numLinks &&
			ThePaths.ConnectedNode(idInNode + pCurNode->firstLink) != placement.node2)
			idInNode++;
		linkId = ThePaths.m_carPathConnections[idInNode + pCurNode->firstLink];
	}
	assert(linkId >= 0 && linkId < NUM_CARPATHLINKS);
	CCarPathLink* pPathLink = &ThePaths.m_carPathLinks[linkId];
	int16 lanesOnCurrentRoad = pPathLink->pathNodeIndex == placement.node2 ? pPathLink->numLeftLanes : pPathLink->numRightLanes;
	if (lanesOnCurrentRoad == 0)
		/* Not spawning vehicle if road is one way and intended direction is opposide to that way. */
		return SPAWN_REFUSAL_NO_SITE;
	int16 lane = SpawnLaneFromDraw(lanesOnCurrentRoad, CGeneral::GetRandomNumber());
	int16 nextDirection = SpawnTravelDirection(placement.node1, placement.node2);
	float nextLinkForwardX = nextDirection * pPathLink->GetDirX();
	float nextLinkForwardY = nextDirection * pPathLink->GetDirY();
	/* Test the occupancy of the lane the vehicle is created in, not of the road
	 * centre: that position is what the vehicle will occupy. Only vehicles block
	 * the placement, because sidewalk peds stand 5-10 m from the road centre and
	 * used to reject about half of the placements. */
	float laneOffset = (lane + pPathLink->OneWayLaneOffset()) * LANE_WIDTH;
	CVector lanePosition(
		placement.x + laneOffset * nextLinkForwardY,
		placement.y - laneOffset * nextLinkForwardX,
		placement.z);
	int16 colliding;
	CWorld::FindObjectsKindaColliding(lanePosition, bBoatGenerated ? 40.0f : 10.0f, true, &colliding, 2, nil, false, true, false, false, false);
	if (colliding)
		/* If something is already present in spawn position, try the next placement. */
		return SPAWN_REFUSAL_OCCUPANCY;
	/* The reachability of the target is the planner's contract (VICE-150):
	 * a planned site was already tested, so no path search runs here. */
	CVehicle* pVehicle;
	if (CModelInfo::IsBoatModel(carModel))
		pVehicle = new CBoat(carModel, bFarTraffic ? FAR_TRAFFIC_VEHICLE : RANDOM_VEHICLE);
	else if (CModelInfo::IsBikeModel(carModel))
		pVehicle = new CBike(carModel, bFarTraffic ? FAR_TRAFFIC_VEHICLE : RANDOM_VEHICLE);
	else
		pVehicle = new CAutomobile(carModel, bFarTraffic ? FAR_TRAFFIC_VEHICLE : RANDOM_VEHICLE);
	pVehicle->AutoPilot.m_nPrevRouteNode = 0;
	pVehicle->AutoPilot.m_nCurrentRouteNode = placement.node1;
	pVehicle->AutoPilot.m_nNextRouteNode = placement.node2;
	switch (carClass) {
	case COPS:
		pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		if (CWorld::Players[CWorld::PlayerInFocus].m_pPed->m_pWanted->GetWantedLevel() != 0){
			pVehicle->AutoPilot.m_nCruiseSpeed = CCarAI::FindPoliceCarSpeedForWantedLevel(pVehicle);
			pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed / 2;
			pVehicle->AutoPilot.m_nCarMission = CCarAI::FindPoliceCarMissionForWantedLevel();
			pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
		}else{
			pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(12, 16);
			pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed;
			pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_STOP_FOR_CARS;
			pVehicle->AutoPilot.m_nCarMission = MISSION_CRUISE;
		}
		if (carModel == MI_FBIRANCH){
			pVehicle->m_currentColour1 = 0;
			pVehicle->m_currentColour2 = 0;
		}
		pVehicle->bCreatedAsPoliceVehicle = true;
		break;
	case COPS_BOAT:
		pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(4, 16);
		pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed;
		pVehicle->AutoPilot.m_nCarMission = CCarAI::FindPoliceBoatMissionForWantedLevel();
		pVehicle->bCreatedAsPoliceVehicle = true;
		break;
	default:
		pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(9, 14);
		if (carClass == EXEC)
			pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(12, 18);
		else if (carClass == POOR)
			pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(7, 10);
		if (pVehicle->GetColModel()->boundingBox.max.y - pVehicle->GetColModel()->boundingBox.min.y > 10.0f || carClass == BIG) {
			pVehicle->AutoPilot.m_nCruiseSpeed *= 3;
			pVehicle->AutoPilot.m_nCruiseSpeed /= 4;
		}
		pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed;
		pVehicle->AutoPilot.m_nCarMission = MISSION_CRUISE;
		pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_STOP_FOR_CARS;
		break;
	}
	if (pVehicle && pVehicle->GetModelIndex() == MI_MRWHOOP)
		pVehicle->m_bSirenOrAlarm = true;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = linkId;
	pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane = lane;
	CBox* boundingBox = &CModelInfo::GetColModel(pVehicle->GetModelIndex())->boundingBox;
	float carLength = 1.0f + (boundingBox->max.y - boundingBox->min.y) / 2;
	float distanceBetweenNodes = (pCurNode->GetPosition() - pNextNode->GetPosition()).Magnitude2D();
	float positionBetweenNodes = placement.between;
	/* If car is so long that it doesn't fit between two car nodes, place it directly in the middle. */
	/* Otherwise put it at least in a way that full vehicle length fits between two nodes. */
	positionBetweenNodes = SpawnBetweenNodesClamp(positionBetweenNodes, carLength, distanceBetweenNodes);
	pVehicle->AutoPilot.m_nNextDirection = nextDirection;
	int16 nextConnection = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	int16 newLink;
	while (nextConnection == pVehicle->AutoPilot.m_nNextPathNodeInfo){
		newLink = CGeneral::GetRandomNumber() % pCurNode->numLinks;
		nextConnection = ThePaths.m_carPathConnections[newLink + pCurNode->firstLink];
	}
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = nextConnection;
	pVehicle->AutoPilot.m_nCurrentDirection = (ThePaths.ConnectedNode(newLink + pCurNode->firstLink) >= placement.node1) ? 1 : -1;
	CVector2D vecBetweenNodes = pNextNode->GetPosition() - pCurNode->GetPosition();
	float forwardX, forwardY;
	float distBetweenNodes = vecBetweenNodes.Magnitude();
	if (distanceBetweenNodes == 0.0f){
		forwardX = 1.0f;
		forwardY = 0.0f;
	}else{
		forwardX = vecBetweenNodes.x / distBetweenNodes;
		forwardY = vecBetweenNodes.y / distBetweenNodes;
	}
	/* I think the following might be some form of SetRotateZOnly. */
	/* Setting up direction between two car nodes. */
	pVehicle->GetForward() = CVector(forwardX, forwardY, 0.0f);
	pVehicle->GetRight() = CVector(forwardY, -forwardX, 0.0f);
	pVehicle->GetUp() = CVector(0.0f, 0.0f, 1.0f);

	float currentPathLinkForwardX = pVehicle->AutoPilot.m_nCurrentDirection * ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo].GetDirX();
	float currentPathLinkForwardY = pVehicle->AutoPilot.m_nCurrentDirection * ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo].GetDirY();
	float nextPathLinkForwardX = pVehicle->AutoPilot.m_nNextDirection * ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo].GetDirX();
	float nextPathLinkForwardY = pVehicle->AutoPilot.m_nNextDirection * ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo].GetDirY();

#ifdef FIX_BUGS
	CCarPathLink* pCurrentLink;
	CCarPathLink* pNextLink;
	CVector positionOnCurrentLinkIncludingLane;
	CVector positionOnNextLinkIncludingLane;
	float directionCurrentLinkX;
	float directionCurrentLinkY;
	float directionNextLinkX;
	float directionNextLinkY;
	if (positionBetweenNodes < 0.5f) {
		pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
		pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
		positionOnCurrentLinkIncludingLane = CVector(
			pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
			pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
			0.0f);
		positionOnNextLinkIncludingLane = CVector(
			pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
			pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
			0.0f);
		directionCurrentLinkX = pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
		directionCurrentLinkY = pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
		directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
		directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
		/* We want to make a path between two links that may not have the same forward directions a curve. */
		pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = CCurves::CalcSpeedScaleFactor(
			&positionOnCurrentLinkIncludingLane,
			&positionOnNextLinkIncludingLane,
			directionCurrentLinkX, directionCurrentLinkY,
			directionNextLinkX, directionNextLinkY
		) * (1000.0f / pVehicle->AutoPilot.m_fMaxTrafficSpeed);
		pVehicle->AutoPilot.m_nTimeEnteredCurve = CTimer::GetTimeInMilliseconds() -
			(uint32)((0.5f + positionBetweenNodes) * pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve);
	}
	else {
		PickNextNodeRandomly(pVehicle);
		pVehicle->AutoPilot.m_nTimeEnteredCurve = CTimer::GetTimeInMilliseconds() -
			(uint32)((positionBetweenNodes - 0.5f) * pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve);

		pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
		pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
		positionOnCurrentLinkIncludingLane = CVector(
			pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
			pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
			0.0f);
		positionOnNextLinkIncludingLane = CVector(
			pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
			pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
			0.0f);
		directionCurrentLinkX = pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
		directionCurrentLinkY = pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
		directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
		directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	}
#else

	CCarPathLink* pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink* pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector positionOnCurrentLinkIncludingLane(
		pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
		pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
		0.0f);
	CVector positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
		0.0f);
	float directionCurrentLinkX = pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionCurrentLinkY = pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	/* We want to make a path between two links that may not have the same forward directions a curve. */
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = CCurves::CalcSpeedScaleFactor(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		directionCurrentLinkX, directionCurrentLinkY,
		directionNextLinkX, directionNextLinkY
	) * (1000.0f / pVehicle->AutoPilot.m_fMaxTrafficSpeed);
	pVehicle->AutoPilot.m_nTimeEnteredCurve = CTimer::GetTimeInMilliseconds() -
		(0.5f + positionBetweenNodes) * pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
#endif

	CVector directionCurrentLink(directionCurrentLinkX, directionCurrentLinkY, 0.0f);
	CVector directionNextLink(directionNextLinkX, directionNextLinkY, 0.0f);
	CVector positionIncludingCurve;
	CVector directionIncludingCurve;
	CCurves::CalcCurvePoint(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		&directionCurrentLink,
		&directionNextLink,
		GetPositionAlongCurrentCurve(pVehicle),
		pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve,
		&positionIncludingCurve,
		&directionIncludingCurve
	);
	CVector vectorBetweenNodes = pCurNode->GetPosition() - pNextNode->GetPosition();
	CVector finalPosition = positionIncludingCurve + vectorBetweenNodes * 2.0f / vectorBetweenNodes.Magnitude();
	finalPosition.z = positionBetweenNodes * pNextNode->GetZ() +
		(1.0f - positionBetweenNodes) * pCurNode->GetZ();
	float groundZ = INFINITE_Z;
	CColPoint colPoint;
	CEntity* pEntity;
	if (bBoatGenerated) {
		if (!CWaterLevel::GetWaterLevel(finalPosition, &groundZ, true)) {
			CSpawnService::NoteWaterRejection();
			delete pVehicle;
			return SPAWN_REFUSAL_NO_SITE;
		}
	}
	else {
		if (CWorld::ProcessVerticalLine(finalPosition, 1000.0f, colPoint, pEntity, true, false, false, false, true, false, nil))
			groundZ = colPoint.point.z;
		if (CWorld::ProcessVerticalLine(finalPosition, -1000.0f, colPoint, pEntity, true, false, false, false, true, false, nil)) {
			if (ABS(colPoint.point.z - finalPosition.z) < ABS(groundZ - finalPosition.z))
				groundZ = colPoint.point.z;
		}
	}
	if (groundZ == INFINITE_Z) {
		/* Failed to find ground under the placement: the defect row. */
		CSpawnService::NoteGroundRejection();
		delete pVehicle;
		return SPAWN_REFUSAL_NO_SITE;
	}
	if (ABS(groundZ - finalPosition.z) > 7.0f) {
		/* The ground found is too far from the expected position: a car
		 * placed here would sit in the air. The defect row (VICE-150). */
		CSpawnService::NoteAirOffsetRejection();
		delete pVehicle;
		return SPAWN_REFUSAL_NO_SITE;
	}
	if (CModelInfo::IsBoatModel(carModel)) {
		finalPosition.z = groundZ;
		pVehicle->bExtendedRange = true;
	}
	else
		finalPosition.z = groundZ + pVehicle->GetHeightAboveRoad();
	pVehicle->SetPosition(finalPosition);
	pVehicle->SetMoveSpeed(directionIncludingCurve / GAME_SPEED_TO_CARAI_SPEED);
	CVector2D speedDifferenceWithTarget = (CVector2D)pVehicle->GetMoveSpeed() - vecPlayerSpeed;
	CVector2D distanceToTarget = positionIncludingCurve - vecTargetPos;
	switch (carClass) {
	case COPS:
		pVehicle->SetStatus((pVehicle->AutoPilot.m_nCarMission == MISSION_CRUISE) ? STATUS_SIMPLE : STATUS_PHYSICS);
		pVehicle->ChangeLawEnforcerState(1);
		break;
	case COPS_BOAT:
		pVehicle->ChangeLawEnforcerState(1);
		pVehicle->SetStatus(STATUS_PHYSICS);
		break;
	default:
		bBoatGenerated ? pVehicle->SetStatus(STATUS_PHYSICS) : pVehicle->SetStatus(STATUS_SIMPLE);
		break;
	}
	/* The destination mode (VICE-156): a responder's route is its incident,
	 * not a cruise. The car is already on the road at its placed position
	 * (the route and lane state above), so the old emergency route builder
	 * runs here: join the road system at the placed position and point the
	 * autopilot at the incident, or drive straight when the join found no
	 * path. The old emergency path set the same mission, speed and style
	 * after a raw collector placement; only the placement moved. */
	if (placement.responder != SPAWN_RESPONDER_NONE) {
		CVector destination(placement.destX, placement.destY, placement.destZ);

		pVehicle->AutoPilot.m_vecDestinationCoors = destination;
		pVehicle->AutoPilot.m_nCarMission = JoinCarWithRoadSystemGotoCoors(pVehicle, destination, false) ?
			MISSION_GOTOCOORDS_STRAIGHT : MISSION_GOTOCOORDS;
		pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed = RESPONDER_CRUISE_SPEED;
		pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
		/* The old emergency car stood still at the placement and left under
		 * physics; the curve's birth speed is the traffic's, not a
		 * responder's. */
		pVehicle->SetMoveSpeed(CVector(0.0f, 0.0f, 0.0f));
		pVehicle->SetStatus(STATUS_PHYSICS);
	}
	CVisibilityPlugins::SetClumpAlpha(pVehicle->GetClump(), 0);
	/* The birth cull. A fresh spawn must never be culled here, whatever the
	 * view state, the slider or the weather: the spawn ring plus the reserve of
	 * the table is the despawn threshold itself (see TrafficRings.h). The
	 * off-screen branch uses the same threshold as the on-screen one, because
	 * the lifetime cull (PossiblyRemoveVehicle) does: a car that is off screen
	 * now can come into view a frame later, and one ring serves both requests.
	 * The on-screen band keeps its own lower edge and the camera rule. The far
	 * layer's cars are placed past the despawn by design (VICE-128), so the
	 * near ring's birth cull is the near cars' own. */
	if (!bFarTraffic) {
		if (!pVehicle->GetIsOnScreen()){
			if ((vecTargetPos - pVehicle->GetPosition()).Magnitude2D() > rings.carDespawn * (pVehicle->bExtendedRange ? rings.extendedRangeFactor : 1.0f)) {
				/* Too far away cars that are not visible aren't needed. */
				delete pVehicle;
				return SPAWN_REFUSAL_VISIBILITY;
			}
		}else{
			if ((vecTargetPos - pVehicle->GetPosition()).Magnitude2D() > rings.carDespawn * (pVehicle->bExtendedRange ? rings.extendedRangeFactor : 1.0f) ||
				(vecTargetPos - pVehicle->GetPosition()).Magnitude2D() < rings.carOnScreenSpawnMin) {
				delete pVehicle;
				return SPAWN_REFUSAL_VISIBILITY;
			}
			if ((TheCamera.GetPosition() - pVehicle->GetPosition()).Magnitude2D() < rings.carCameraMin) {
				delete pVehicle;
				return SPAWN_REFUSAL_VISIBILITY;
			}
			if (pVehicle->GetModelIndex() == MI_MARQUIS) { // so marquis can only spawn if player doesn't see it?
				delete pVehicle;
				return SPAWN_REFUSAL_VISIBILITY;
			}
		}
	}
	/* The near bubble (VICE-127), behind the service's API (VICE-150): a car
	 * may only appear where the player cannot see it. The gate is asked with
	 * the final position, after the ground cast in the birth cull above, so
	 * the answer is about the spot the car really takes: outside the bubble,
	 * and hidden now (occluded, beyond the fade, or behind a blocked line of
	 * sight), or a frame the fade covers (the fill burst). A refusal is not
	 * an error: the plan's next site is tried, and the service counts the
	 * reason.
	 * The far layer (VICE-128) asks the same gate with `rayAllowed` false:
	 * its sites are behind the camera by the margin or beyond the fade, and a
	 * line-of-sight ray of two kilometres is not the far layer's work. */
	if (!CSpawnService::AllowsCreation(pVehicle->GetPosition(), vecTargetPos, NEAR_BUBBLE_ENTITY_RADIUS, !bFarTraffic, pArm)) {
		delete pVehicle;
		return SPAWN_REFUSAL_VISIBILITY;
	}
	/* The fresh-spawn grace: the culls of the frame may not take this car
	 * before it has lived NEAR_BUBBLE_GRACE_MS. */
	pVehicle->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + NEAR_BUBBLE_GRACE_MS;
	/* The car is the traffic's own: the bubble's fill counts it, and the cars
	 * the scripts and the set pieces create (RANDOM_VEHICLE too, so part of
	 * the car target) are the fill's other half (VICE-127). A far car is the
	 * traffic of its band (VICE-128) and becomes a RANDOM_VEHICLE when it is
	 * promoted, so the flag is right for both. */
	pVehicle->bIsTrafficCar = true;
	CVehicleModelInfo* pVehicleModel = pVehicle->GetModelInfo();
	float radiusToTest = pVehicleModel->GetColModel()->boundingSphere.radius;
	if (testForCollision){
		CWorld::FindObjectsKindaColliding(pVehicle->GetPosition(), radiusToTest + 20.0f, true, &colliding, 2, nil, false, true, false, false, false);
		if (colliding){
			delete pVehicle;
			return SPAWN_REFUSAL_OCCUPANCY;
		}
	}
	CWorld::FindObjectsKindaColliding(pVehicle->GetPosition(), radiusToTest, true, &colliding, 2, nil, false, true, false, false, false);
	if (colliding){
		delete pVehicle;
		return SPAWN_REFUSAL_OCCUPANCY;
	}
	/* The speed test refuses a spawn that drives away from the player: a near
	 * car would leave the ring without ever being seen. The far layer's cars
	 * drive away from the player by design (they are the traffic that
	 * recedes), so the test is the near cars' own (VICE-128). A responder
	 * drives to its incident, not away from the ring, so the test is not its
	 * own either: the old emergency path never asked it. */
	if (placement.responder == SPAWN_RESPONDER_NONE && !bFarTraffic &&
		speedDifferenceWithTarget.x * distanceToTarget.x +
		speedDifferenceWithTarget.y * distanceToTarget.y >= 0.0f){
		delete pVehicle;
		return SPAWN_REFUSAL_VISIBILITY;
	}
	/* The duty state of a responder (VICE-156): the flags the old emergency
	 * path set before the world add, and the siren. CVehicle's destructor and
	 * the emergency AI clear them (and their counters) on arrival or
	 * deletion. */
	if (placement.responder != SPAWN_RESPONDER_NONE) {
		pVehicle->m_bSirenOrAlarm = true;
		switch (placement.responder) {
		case SPAWN_RESPONDER_FIRETRUCK:
			pVehicle->bIsFireTruckOnDuty = true;
			++NumFiretrucksOnDuty;
			break;
		case SPAWN_RESPONDER_AMBULANCE:
			pVehicle->bIsAmbulanceOnDuty = true;
			++NumAmbulancesOnDuty;
			break;
		}
	}
	pVehicleModel->AvoidSameVehicleColour(&pVehicle->m_currentColour1, &pVehicle->m_currentColour2);
	CWorld::Add(pVehicle);
	if (bFarTraffic) {
		/* The far state (VICE-128), after the add: the car is rendered and
		 * moved by the far layer and by nothing else. The static flag goes on
		 * here, so the car leaves the moving entity list CWorld::Add put it on
		 * (CWorld::Process runs no control on it: no AI, no physics, no
		 * wheels); collision is off both ways (ProcessCollisionSectorList
		 * tests the flag, so the player cannot touch it and the near traffic
		 * cannot even brake for it); the audio entity of a car kilometres away
		 * is switched off after the add (CWorld::Add wakes it), so
		 * ProcessEntity skips it. The flags live in the layer
		 * (CFarTraffic::ApplyFarState), so a far spawn and a hand-over apply
		 * exactly the same state. */
		CFarTraffic::ApplyFarState(pVehicle);
	}
	/* The acceptance reads this: the car exists now, and the counter says
	 * where. The gate above already refused every catchable spot, so this is
	 * the guard of the paths that bypass it (VICE-127). */
	CSpawnService::CountCreation(pVehicle->GetPosition(), NEAR_BUBBLE_ENTITY_RADIUS);
	if (ppCreated)
		*ppCreated = pVehicle;
	/* The far car is done here: no occupants, no mad-driver roll, no accident
	 * at the spawn point (a ped would be created kilometres away), and no
	 * police bookkeeping. The layer takes it from here, and the promotion
	 * (CFarTraffic) gives it its driver when the player is near. */
	if (bFarTraffic)
		return SPAWN_REFUSAL_NONE;
	if (placement.responder == SPAWN_RESPONDER_FIRETRUCK)
		CCarAI::AddFiretruckOccupants(pVehicle);
	else if (placement.responder == SPAWN_RESPONDER_AMBULANCE)
		CCarAI::AddAmbulanceOccupants(pVehicle);
	else if (carClass == COPS || carClass == COPS_BOAT)
		CCarAI::AddPoliceCarOccupants(pVehicle);
	else {
		/* The driver contract (VICE-150): SetUpDriver needs RANDOM_VEHICLE;
		 * the near tier creates with it, so the call is legal here and the
		 * assert pins the contract. */
		assert(pVehicle->VehicleCreatedBy == RANDOM_VEHICLE);
		pVehicle->SetUpDriver();
		int32 passengers = 0;
		for (int i = 0; i < pVehicle->m_nNumMaxPassengers; i++)
			passengers += (CGeneral::GetRandomNumberInRange(0.0f, 1.0f) < PROBABILITY_OF_PASSENGER_IN_VEHICLE) ? 1 : 0;
		if (CModelInfo::IsCarModel(carModel) && (CModelInfo::GetModelInfo(carModel)->GetAnimFileIndex() == CAnimManager::GetAnimationBlockIndex("van") && passengers >= 1))
			passengers = 1;
		/* The counted passengers stop at their allowance (PopulationBudget.h);
		 * the police crews and the script passengers are not counted and live
		 * in the pool reserve. */
		if (passengers > 0 && CPopulation::ms_nTotalCarPassengerPeds >= MAX_PED_PASSENGERS)
			passengers = 0;
		/* One ped pool read for the whole car (the frame's read is in
		 * GenerateRandomCars): the passengers of one car take at most three
		 * slots, and the reserve covers them. */
		if (passengers > 0 && CPools::GetPedPool()->GetNoOfFreeSpaces() < PED_POOL_RESERVE)
			passengers = 0;
		for (int i = 0; i < passengers; i++) {
			CPed* pPassenger = pVehicle->SetupPassenger(i);
			if (pPassenger) {
				++CPopulation::ms_nTotalCarPassengerPeds;
				pPassenger->bCarPassenger = true;
			}
		}
	}
	int nMadDrivers;
	switch (pVehicle->GetVehicleAppearance()) {
	case VEHICLE_APPEARANCE_BIKE:
		nMadDrivers = 30;
		break;
	case VEHICLE_APPEARANCE_BOAT:
		nMadDrivers = 40;
		break;
	default:
		nMadDrivers = 6;
		break;
	}
	if (placement.responder == SPAWN_RESPONDER_NONE &&
		((CGeneral::GetRandomNumber() & 0x7F) < nMadDrivers || bMadDriversCheat)) {
		pVehicle->SetStatus(STATUS_PHYSICS);
		pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
		pVehicle->AutoPilot.m_nCruiseSpeed += 10;
	}
	if (carClass == COPS)
		LastTimeLawEnforcerCreated = CTimer::GetTimeInMilliseconds();
	if (pVehicle->GetModelIndex() == MI_CADDY) {
		pVehicle->SetStatus(STATUS_PHYSICS);
		pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
	}
	if (carClass == COPS && pVehicle->GetModelIndex() == MI_VICECHEE) {
		CVehicleModelInfo* pVehicleModel = (CVehicleModelInfo*)CModelInfo::GetModelInfo(MI_VICECHEE);
		switch (MiamiViceCycle) {
		case 0:
			pVehicleModel->SetVehicleColour(53, 77);
			break;
		case 1:
			pVehicleModel->SetVehicleColour(15, 77);
			break;
		case 2:
			pVehicleModel->SetVehicleColour(41, 77);
			break;
		case 3:
			pVehicleModel->SetVehicleColour(61, 77);
			break;
		default:
			break;
		}
	}
	if (placement.responder == SPAWN_RESPONDER_NONE &&
		CGeneral::GetRandomNumberInRange(0.0f, 1.0f) >= (1 - PROBABILITY_OF_DEAD_PED_ACCIDENT)) {
		if (CModelInfo::IsCarModel(pVehicle->GetModelIndex()) && !pVehicle->bIsLawEnforcer) {
			if (CPopulation::AddDeadPedInFrontOfCar(pVehicle->GetPosition() + pVehicle->GetForward() * DISTANCE_BETWEEN_CAR_AND_DEAD_PED, pVehicle)) {
				pVehicle->AutoPilot.m_nCruiseSpeed = 0;
				pVehicle->SetMoveSpeed(0.0f, 0.0f, 0.0f);
				for (int i = 0; i < pVehicle->m_nNumPassengers; i++) {
					if (pVehicle->pPassengers[i]) {
						pVehicle->pPassengers[i]->SetObjective(OBJECTIVE_LEAVE_CAR, pVehicle);
						pVehicle->pPassengers[i]->m_nLastPedState = PED_WANDER_PATH;
						pVehicle->pPassengers[i]->m_vehicleInAccident = pVehicle;
						pVehicle->pPassengers[i]->bDeadPedInFrontOfCar = true;
						pVehicle->RegisterReference((CEntity**)&pVehicle->pPassengers[i]->m_vehicleInAccident);
					}
				}
				if (pVehicle->pDriver) {
					pVehicle->pDriver->SetObjective(OBJECTIVE_LEAVE_CAR, pVehicle);
					pVehicle->pDriver->m_nLastPedState = PED_WANDER_PATH;
					pVehicle->pDriver->m_vehicleInAccident = pVehicle;
					pVehicle->pDriver->bDeadPedInFrontOfCar = true;
					pVehicle->RegisterReference((CEntity**)&pVehicle->pDriver->m_vehicleInAccident);
				}
			}
		}
	}
	return SPAWN_REFUSAL_NONE;
}

bool
CCarCtrl::BoatWithTallMast(int32 mi)
{
	return mi == MI_RIO || mi == MI_TROPIC || mi == MI_MARQUIS;
}

int32
CCarCtrl::ChooseBoatModel(int32 rating)
{
	++NumRequestsOfCarRating[rating];
	return ChooseCarModel(rating);
}

int32
CCarCtrl::ChooseBoatRating(CZoneInfo* pZoneInfo)
{
	int rnd = CGeneral::GetRandomNumberInRange(0, 1000);
	for (int i = 0; i < NUM_BOAT_CLASSES - 1; i++) {
		if (rnd < pZoneInfo->boatThreshold[i])
			return FIRST_BOAT_RATING + i;
	}
	return FIRST_BOAT_RATING + NUM_BOAT_CLASSES - 1;
}

int32
CCarCtrl::ChooseCarRating(CZoneInfo* pZoneInfo)
{
	int rnd = CGeneral::GetRandomNumberInRange(0, 1000);
	for (int i = 0; i < NUM_CAR_CLASSES - 1; i++) {
		if (rnd < pZoneInfo->carThreshold[i])
			return i;
	}
	return FIRST_CAR_RATING + NUM_CAR_CLASSES - 1;
}

int32
CCarCtrl::ChooseModel(CZoneInfo* pZone, int* pClass) {
	int32 model = -1;
	int32 i;
	for (i = 10; i > 0 && (model == -1 || !CStreaming::HasModelLoaded(model)); i--) {
		int rnd = CGeneral::GetRandomNumberInRange(0, 1000);

		if (rnd < pZone->copThreshold) {
			*pClass = COPS; 
			model = ChoosePoliceCarModel();
			continue;
		}

		int32 j;
		for (j = 0; j < NUM_GANG_CAR_CLASSES; j++) {
			if (rnd < pZone->gangThreshold[j]) {
				*pClass = j + FIRST_GANG_CAR_RATING;
				model = ChooseGangCarModel(j);
				break;
			}
		}

		if (j != NUM_GANG_CAR_CLASSES)
			continue;

		*pClass = ChooseCarRating(pZone);
		model = ChooseCarModel(*pClass);
	}
	if (i == 0)
		return -1;
	return model;
}

int32
CCarCtrl::ChooseCarModel(int32 vehclass)
{
	int32 model = -1;
	++NumRequestsOfCarRating[vehclass];
	if (NumOfLoadedCarsOfRating[vehclass] == 0)
		return -1;
	int32 rnd = CGeneral::GetRandomNumberInRange(0, CarFreqArrays[vehclass][NumOfLoadedCarsOfRating[vehclass] - 1]);
	int32 index = 0;
	while (rnd > CarFreqArrays[vehclass][index])
		index++;
	assert(LoadedCarsArray[vehclass][index]);
	return LoadedCarsArray[vehclass][index];
}

void
CCarCtrl::AddToLoadedVehicleArray(int32 mi, int32 rating, int32 freq)
{
	LoadedCarsArray[rating][NumOfLoadedCarsOfRating[rating]] = mi;
	assert(mi >= 130);
	CarFreqArrays[rating][NumOfLoadedCarsOfRating[rating]] = freq;
	if (NumOfLoadedCarsOfRating[rating])
		CarFreqArrays[rating][NumOfLoadedCarsOfRating[rating]] += CarFreqArrays[rating][NumOfLoadedCarsOfRating[rating] - 1];
	NumOfLoadedCarsOfRating[rating]++;
}

void
CCarCtrl::RemoveFromLoadedVehicleArray(int mi, int32 rating)
{
	int index = 0;
	while (LoadedCarsArray[rating][index] != -1) {
		if (LoadedCarsArray[rating][index] == mi)
			break;
		index++;
	}
	assert(LoadedCarsArray[rating][index] == mi);
	int32 freq = CarFreqArrays[rating][index];
	if (index > 0)
		freq -= CarFreqArrays[rating][index - 1];
	while (LoadedCarsArray[rating][index + 1] != -1) {
		LoadedCarsArray[rating][index] = LoadedCarsArray[rating][index + 1];
		CarFreqArrays[rating][index] = CarFreqArrays[rating][index + 1] - freq;
		index++;
	}
	--NumOfLoadedCarsOfRating[rating];
}

int32
CCarCtrl::ChooseCarModelToLoad(int rating)
{
	return CarArrays[rating][CGeneral::GetRandomNumberInRange(0, TotalNumOfCarsOfRating[rating])];
}

int32
CCarCtrl::ChoosePoliceCarModel(void)
{
	if (FindPlayerPed()->m_pWanted->AreMiamiViceRequired() &&
#ifdef FIX_BUGS
		(CTimer::GetTimeInMilliseconds() > LastTimeMiamiViceGenerated + 120000 || LastTimeMiamiViceGenerated == 0) &&
#else
		CTimer::GetTimeInMilliseconds() > LastTimeMiamiViceGenerated + 120000 &&
#endif
		CStreaming::HasModelLoaded(MI_VICECHEE)) {
		switch (MiamiViceCycle) {
		case 0:
			if (CStreaming::HasModelLoaded(MI_VICE1) && CStreaming::HasModelLoaded(MI_VICE2))
				return MI_VICECHEE;
			break;
		case 1:
			if (CStreaming::HasModelLoaded(MI_VICE3) && CStreaming::HasModelLoaded(MI_VICE4))
				return MI_VICECHEE;
			break;
		case 2:
			if (CStreaming::HasModelLoaded(MI_VICE5) && CStreaming::HasModelLoaded(MI_VICE6))
				return MI_VICECHEE;
			break;
		case 3:
			if (CStreaming::HasModelLoaded(MI_VICE7) && CStreaming::HasModelLoaded(MI_VICE8))
				return MI_VICECHEE;
			break;
		default:
			break;
		}
	}
	if (FindPlayerPed()->m_pWanted->AreSwatRequired() &&
		CStreaming::HasModelLoaded(MI_ENFORCER) &&
		CStreaming::HasModelLoaded(MI_POLICE))
		return ((CGeneral::GetRandomNumber() & 0xF) == 0) ? MI_ENFORCER : MI_POLICE;
	if (FindPlayerPed()->m_pWanted->AreFbiRequired() &&
		CStreaming::HasModelLoaded(MI_FBIRANCH) &&
		CStreaming::HasModelLoaded(MI_FBI))
		return MI_FBIRANCH;
	if (FindPlayerPed()->m_pWanted->AreArmyRequired() &&
		CStreaming::HasModelLoaded(MI_RHINO) &&
		CStreaming::HasModelLoaded(MI_BARRACKS) &&
		CStreaming::HasModelLoaded(MI_ARMY))
		return CGeneral::GetRandomTrueFalse() ? MI_BARRACKS : MI_RHINO;
	return MI_POLICE;
}

int32
CCarCtrl::ChooseGangCarModel(int32 gang)
{
	if (CGangs::HaveGangModelsLoaded(gang))
		return CGangs::GetGangVehicleModel(gang);
	return -1;
}

void
CCarCtrl::AddToCarArray(int32 id, int32 vehclass)
{
	assert(TotalNumOfCarsOfRating[vehclass] < MAX_CAR_MODELS_IN_ARRAY);
	CarArrays[vehclass][TotalNumOfCarsOfRating[vehclass]++] = id;
}

void
CCarCtrl::RemoveDistantCars()
{
	/* One read of the player centre covers the whole scan: nothing in the loop
	 * writes any input of FindPlayerCentreOfWorld. The camera was processed
	 * earlier in the frame, bCarsGeneratedAroundCamera only changes in
	 * CCarCtrl::Init and its script opcode, and neither the player's own car nor
	 * the remote vehicle can be removed here. */
	CVector vecPlayerPos = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	/* The fill of the near bubble (VICE-127): the traffic the spawner created
	 * (the random cars, the emergency services) the player's own 90 m disc
	 * holds, and the other half - the RANDOM_VEHICLEs the scripts, the set
	 * pieces, the road blocks, the car parks, the garages and the cheats put
	 * there. The car target counts both, so the two together are what the
	 * target describes. The scan is the one this function already makes. */
	int32 nearBubbleFill = 0;
	int32 nearBubbleOtherFill = 0;
	for (int i = CPools::GetVehiclePool()->GetSize()-1; i >= 0; i--) {
		CVehicle* pVehicle = CPools::GetVehiclePool()->GetSlot(i);
		if (!pVehicle)
			continue;
		if (pVehicle->VehicleCreatedBy == RANDOM_VEHICLE &&
			(pVehicle->GetPosition() - vecPlayerPos).Magnitude2D() <= NEAR_BUBBLE_RADIUS) {
			if (pVehicle->bIsTrafficCar)
				nearBubbleFill++;
			else
				nearBubbleOtherFill++;
		}
		// PossiblyRemoveVehicle may delete the vehicle, so handle the road
		// block peds first, while the pointer is still valid.
		if (pVehicle->bCreateRoadBlockPeds){
			if ((pVehicle->GetPosition() - vecPlayerPos).Magnitude2D() < DISTANCE_TO_SPAWN_ROADBLOCK_PEDS) {
				CRoadBlocks::GenerateRoadBlockCopsForCar(pVehicle, pVehicle->m_nRoadblockType);
				pVehicle->bCreateRoadBlockPeds = false;
			}
		}
		PossiblyRemoveVehicle(pVehicle, vecPlayerPos);
	}
	ms_nNearBubbleFill = nearBubbleFill;
	ms_nNearBubbleOtherFill = nearBubbleOtherFill;
}

void
CCarCtrl::RemoveCarsIfThePoolGetsFull(void)
{
	if ((CTimer::GetFrameCounter() & 7) != 3)
		return;
	if (CPools::GetVehiclePool()->GetNoOfFreeSpaces() >= 8)
		return;
	/* The valve of the pool is the last line behind the budgets: the creation
	 * paths stop at the pool reserve, so it runs only when something outside
	 * them filled the pool. The service's removal gate asks the rule first (a
	 * car outside the bubble and its reserve, past the grace, hidden); when
	 * nothing passes, the closest car goes anyway: a full pool would break
	 * the whole traffic, and the service's CountRemoval records the event.
	 * The selection below reads the cheap arms itself, so one valve frame
	 * asks the gate at most once. */
	CVector vecPlayerPos = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	int i = CPools::GetVehiclePool()->GetSize();
	float md = 10000000.f;
	CVehicle* pClosestVehicle = nil;
	float mdCandidate = 10000000.f;
	CVehicle* pClosestCandidate = nil;
	while (i--) {
		CVehicle* pVehicle = CPools::GetVehiclePool()->GetSlot(i);
		if (!pVehicle)
			continue;
		/* A far car is never the valve's victim (VICE-128): the layer's
		 * creation gate keeps FAR_TRAFFIC_POOL_RESERVE free slots and the
		 * cap keeps the car budget 96 slots clear of the pool, so the valve
		 * cannot be reached by the layer; taking a rendered car in the fog
		 * would be a deletion with no rule behind it. */
		if (pVehicle->VehicleCreatedBy == FAR_TRAFFIC_VEHICLE)
			continue;
		if (IsThisVehicleInteresting(pVehicle) || pVehicle->bIsLocked)
			continue;
		if (!pVehicle->CanBeDeleted() || CCranes::IsThisCarBeingTargettedByAnyCrane(pVehicle))
			continue;
		float distance = (TheCamera.GetPosition() - pVehicle->GetPosition()).Magnitude();
		if (distance < md) {
			md = distance;
			pClosestVehicle = pVehicle;
		}
		if (distance < mdCandidate &&
			(pVehicle->GetPosition() - vecPlayerPos).Magnitude2D() > NEAR_BUBBLE_REMOVE_DISTANCE &&
			CTimer::GetTimeInMilliseconds() >= pVehicle->m_nRemovalProtectedUntil) {
			mdCandidate = distance;
			pClosestCandidate = pVehicle;
		}
	}
	if (pClosestCandidate && !CSpawnService::AllowsRemoval(pClosestCandidate, vecPlayerPos, 0.0f))
		pClosestCandidate = nil;
	CVehicle* pRemove = pClosestCandidate ? pClosestCandidate : pClosestVehicle;
	if (pRemove) {
		if (pClosestCandidate)
			CSpawnService::CountRemoval(pRemove->GetPosition(), NEAR_BUBBLE_ENTITY_RADIUS, false);
		else
			/* The fallback: no car passes the rule and the pool is at its
			 * last slots. The removal is the pool's own line, counted
			 * apart from the culls the acceptance watches (VICE-127). */
			CSpawnService::CountValveRemoval();
		CWorld::Remove(pRemove);
		/* The service's own removal ledger (VICE-150): the note is taken
		 * before the delete, and the row is the car's own class. */
		CSpawnService::NoteVehicleRemoval(pRemove);
		delete pRemove;
	}
}

void
CCarCtrl::PossiblyRemoveVehicle(CVehicle* pVehicle, CVector vecPlayerPos)
{
	/* The far layer owns its own cars (VICE-128): a far car is outside every
	 * near threshold and rule, its removal is the band's own decision, and
	 * the near cull must not touch it. The guard is first, so a far car costs
	 * one compare in the frame's scan. */
	if (pVehicle->VehicleCreatedBy == FAR_TRAFFIC_VEHICLE)
		return;
#ifdef FIX_BUGS
	if (pVehicle->bIsLocked)
		return;
#endif
	/* One read each for this call: the camera and this car do not move inside
	 * it, and CGarages::Update and the scripts write the garages between frames,
	 * not inside this call. bFadeOut is written, not cached, below. */
	bool bOnScreenAtEntry = pVehicle->GetIsOnScreen();
	bool bInsideHideOutGarageAtEntry = CGarages::IsPointWithinHideOutGarage(pVehicle->GetPosition());
	if (!IsThisVehicleInteresting(pVehicle) && !pVehicle->bIsLocked &&
		pVehicle->CanBeDeleted() && !CCranes::IsThisCarBeingTargettedByAnyCrane(pVehicle)){
		if (pVehicle->bFadeOut && CVisibilityPlugins::GetClumpAlpha(pVehicle->GetClump()) == 0){
			/* The tail of a removal that started hidden: the car is fully
			 * faded, so the deletion is not an event the player can catch. */
			CSpawnService::CountRemoval(pVehicle->GetPosition(), NEAR_BUBBLE_ENTITY_RADIUS, true);
			CWorld::Remove(pVehicle);
			CSpawnService::NoteVehicleRemoval(pVehicle);
			delete pVehicle;
			return;
		}
		float distanceToPlayer = (pVehicle->GetPosition() - vecPlayerPos).Magnitude2D();
		/* The one despawn threshold of the table. config.h defines
		 * EXTENDED_OFFSCREEN_DESPAWN_RANGE in every shipped build, so a car out
		 * of sight is removed at the same distance as one in view: a car never
		 * vanishes right before it comes into view, and the birth cull of
		 * CCarCtrl::CreateCarFromPlacement reads the same row, so a fresh spawn
		 * is never removed on the frame it is created. The extended range of a
		 * boat or a set piece multiplies the whole threshold, so its own ring
		 * (1.5 times the car ring) keeps the same reserve. */
		const STrafficRings rings = CurrentTrafficRings();
		float threshold = rings.carDespawn;
		if (pVehicle->bExtendedRange)
			threshold *= rings.extendedRangeFactor;
		if (distanceToPlayer > threshold && !bInsideHideOutGarageAtEntry){
			/* The near bubble (VICE-127), behind the service's API
			 * (VICE-150): the threshold is the ring's own (already the
			 * spawn band plus the reserve), raised by the bubble and its
			 * reserve; the car must be hidden (off the frustum, occluded,
			 * or beyond the fade) and past its fresh-spawn grace. A car
			 * the player can see keeps driving: it fades out only where a
			 * turn cannot reveal the change. */
			if (!CSpawnService::AllowsRemoval(pVehicle, vecPlayerPos, threshold))
				return;
			if (bOnScreenAtEntry){
				pVehicle->bFadeOut = true;
			}else{
				/* The far layer's hand-over (VICE-128): a car of the traffic
				 * spawner that the cull is about to delete here may keep
				 * driving as a far car instead, farther than the near band's
				 * edge by the layer's own hysteresis. The car is hidden (the
				 * gate above said so) and the switch changes nothing the
				 * renderer draws, so the player sees a car drive away, not a
				 * car blink out. The layer refuses when it is off or full;
				 * the cull then deletes as it always did. */
				if (CFarTraffic::AdoptOnCull(pVehicle, vecPlayerPos))
					return;
				CSpawnService::CountRemoval(pVehicle->GetPosition(), NEAR_BUBBLE_ENTITY_RADIUS, false);
				CWorld::Remove(pVehicle);
				CSpawnService::NoteVehicleRemoval(pVehicle);
				delete pVehicle;
			}
			return;
		}
	}
	if ((pVehicle->GetStatus() == STATUS_SIMPLE || pVehicle->GetStatus() == STATUS_PHYSICS &&
		(pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS || pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS_IGNORE_LIGHTS)) &&
		CTimer::GetTimeInMilliseconds() - pVehicle->AutoPilot.m_nTimeToStartMission > 5000 &&
		!bOnScreenAtEntry &&
		(pVehicle->GetPosition() - vecPlayerPos).Magnitude2D() > 22.0f &&
		!IsThisVehicleInteresting(pVehicle) &&
		!pVehicle->bIsLocked &&
		pVehicle->CanBeDeleted() &&
		!CTrafficLights::ShouldCarStopForLight(pVehicle, true) &&
		!CTrafficLights::ShouldCarStopForBridge(pVehicle) &&
		!bInsideHideOutGarageAtEntry &&
		/* The near bubble, behind the service's API (VICE-150): the jam of
		 * a stopped car is cleared outside the bubble, hidden, and after
		 * the grace (VICE-127). */
		CSpawnService::AllowsRemoval(pVehicle, vecPlayerPos, 22.0f)){
		CSpawnService::CountRemoval(pVehicle->GetPosition(), NEAR_BUBBLE_ENTITY_RADIUS, false);
		CWorld::Remove(pVehicle);
		CSpawnService::NoteVehicleRemoval(pVehicle);
		delete pVehicle;
		return;
	}
	if (pVehicle->GetStatus() == STATUS_WRECKED) {
		if (pVehicle->m_nTimeOfDeath != 0) {
			if (CTimer::GetTimeInMilliseconds() > pVehicle->m_nTimeOfDeath + 60000 &&
				CTimer::GetTimeInMilliseconds() > pVehicle->m_nRemovalProtectedUntil &&
				!bOnScreenAtEntry) {
				if ((pVehicle->GetPosition() - vecPlayerPos).MagnitudeSqr() > SQR(6.5f)) {
					if (!bInsideHideOutGarageAtEntry &&
						/* The near bubble, behind the service's API: a wreck
						 * the player can see (or one inside the bubble)
						 * stays on the road (VICE-127). */
						CSpawnService::AllowsRemoval(pVehicle, vecPlayerPos, 6.5f)) {
						CSpawnService::CountRemoval(pVehicle->GetPosition(), NEAR_BUBBLE_ENTITY_RADIUS, false);
						CWorld::Remove(pVehicle);
						CSpawnService::NoteVehicleRemoval(pVehicle);
						delete pVehicle;
					}
				}
			}
		}
	}
}

int32
CCarCtrl::CountCarsOfType(int32 mi)
{
	int32 total = 0;
	for (int i = CPools::GetVehiclePool()->GetSize()-1; i >= 0; i--) {
		CVehicle* pVehicle = CPools::GetVehiclePool()->GetSlot(i);
		if (!pVehicle)
			continue;
		if (pVehicle->GetModelIndex() == mi)
			total++;
	}
	return total;
}

/*
 * The engine side of the shared car-on-rails core (CarOnRails.h, VICE-133).
 *
 * The core holds the rules - the lane offsets, the curve, the pick - and reads
 * the live car and the live path graph through this view. The far traffic
 * layer's simulation reads its own state and the FarTrafficWorld snapshot
 * through its own pair of views (FarTrafficWorker.cpp); the rules are one copy,
 * so the near traffic and the far cars cannot drift apart.
 */
struct CCarRailsEngineView
{
	CVehicle *pVehicle;

	/* the path graph, read-only */
	float NodeX(int32 node) { return ThePaths.m_pathNodes[node].GetX(); }
	float NodeY(int32 node) { return ThePaths.m_pathNodes[node].GetY(); }
	int32 NumLinks(int32 node) { return ThePaths.m_pathNodes[node].numLinks; }
	int32 NodeFirstLink(int32 node) { return ThePaths.m_pathNodes[node].firstLink; }
	bool NodeDeadEnd(int32 node) { return ThePaths.m_pathNodes[node].bDeadEnd; }
	bool NodeDisabled(int32 node) { return ThePaths.m_pathNodes[node].bDisabled; }
	bool NodeBetweenLevels(int32 node) { return ThePaths.m_pathNodes[node].bBetweenLevels; }
	int32 ConnectedNode(int32 connection) { return ThePaths.ConnectedNode(connection); }
	int32 LinkOfConnection(int32 connection) { return ThePaths.m_carPathConnections[connection]; }
	float LinkX(int32 link) { return ThePaths.m_carPathLinks[link].GetX(); }
	float LinkY(int32 link) { return ThePaths.m_carPathLinks[link].GetY(); }
	float LinkDirX(int32 link) { return ThePaths.m_carPathLinks[link].GetDirX(); }
	float LinkDirY(int32 link) { return ThePaths.m_carPathLinks[link].GetDirY(); }
	int32 LinkLeftLanes(int32 link) { return ThePaths.m_carPathLinks[link].numLeftLanes; }
	int32 LinkRightLanes(int32 link) { return ThePaths.m_carPathLinks[link].numRightLanes; }
	float LinkLaneOffset(int32 link) { return ThePaths.m_carPathLinks[link].OneWayLaneOffset(); }
	int32 LinkPathNode(int32 link) { return ThePaths.m_carPathLinks[link].pathNodeIndex; }

	/* the car's route state */
	int32 CurrentRouteNode() { return pVehicle->AutoPilot.m_nCurrentRouteNode; }
	int32 NextRouteNode() { return pVehicle->AutoPilot.m_nNextRouteNode; }
	int32 PrevRouteNode() { return pVehicle->AutoPilot.m_nPrevRouteNode; }
	void SetCurrentRouteNode(int32 v) { pVehicle->AutoPilot.m_nCurrentRouteNode = v; }
	void SetNextRouteNode(int32 v) { pVehicle->AutoPilot.m_nNextRouteNode = v; }
	void SetPrevRouteNode(int32 v) { pVehicle->AutoPilot.m_nPrevRouteNode = v; }
	int32 TimeEnteredCurve() { return pVehicle->AutoPilot.m_nTimeEnteredCurve; }
	int32 TimeOnCurve() { return pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve; }
	void SetTimeEnteredCurve(int32 v) { pVehicle->AutoPilot.m_nTimeEnteredCurve = v; }
	void SetTimeOnCurve(int32 v) { pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = v; }
	int32 CurrentPathNodeInfo() { return pVehicle->AutoPilot.m_nCurrentPathNodeInfo; }
	int32 NextPathNodeInfo() { return pVehicle->AutoPilot.m_nNextPathNodeInfo; }
	int32 PreviousPathNodeInfo() { return pVehicle->AutoPilot.m_nPreviousPathNodeInfo; }
	void SetCurrentPathNodeInfo(int32 v) { pVehicle->AutoPilot.m_nCurrentPathNodeInfo = v; }
	void SetNextPathNodeInfo(int32 v) { pVehicle->AutoPilot.m_nNextPathNodeInfo = v; }
	void SetPreviousPathNodeInfo(int32 v) { pVehicle->AutoPilot.m_nPreviousPathNodeInfo = v; }
	int32 CurrentDirection() { return pVehicle->AutoPilot.m_nCurrentDirection; }
	int32 NextDirection() { return pVehicle->AutoPilot.m_nNextDirection; }
	void SetCurrentDirection(int32 v) { pVehicle->AutoPilot.m_nCurrentDirection = v; }
	void SetNextDirection(int32 v) { pVehicle->AutoPilot.m_nNextDirection = v; }
	void SetPreviousDirection(int32 v) { pVehicle->AutoPilot.m_nPreviousDirection = v; }
	int32 CurrentLane() { return pVehicle->AutoPilot.m_nCurrentLane; }
	int32 NextLane() { return pVehicle->AutoPilot.m_nNextLane; }
	void SetCurrentLane(int32 v) { pVehicle->AutoPilot.m_nCurrentLane = v; }
	void SetNextLane(int32 v) { pVehicle->AutoPilot.m_nNextLane = v; }
	uint32 RandomSeed() { return pVehicle->m_randomSeed; }
	float MaxTrafficSpeed() { return pVehicle->AutoPilot.m_fMaxTrafficSpeed; }
	bool StayInCurrentLevel() { return pVehicle->AutoPilot.m_bStayInCurrentLevel; }
	bool StayInFastLane() { return pVehicle->AutoPilot.m_bStayInFastLane; }

	/* the frame side */
	float CurveProgress()
	{
		uint32 timeInCurve = CTimer::GetTimeInMilliseconds() - pVehicle->AutoPilot.m_nTimeEnteredCurve;
		return (float)timeInCurve / pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
	}
	uint32 Random() { return CGeneral::GetRandomNumber(); }
	bool RandomTrueFalse() { return CGeneral::GetRandomTrueFalse(); }
	/* The pick gave up and turns the car around: the engine's own two calls
	 * (the far worker publishes the decision to its applicator instead). */
	void UTurn()
	{
		pVehicle->SetStatus(STATUS_PHYSICS);
		CCarCtrl::SwitchVehicleToRealPhysics(pVehicle);
	}
	/* The curve time came out under 10 ms: the engine prints its old
	 * diagnostic (the far worker counts it). */
	void ShortCurve() { printf("fout\n"); }
};

void
CCarCtrl::ComputeCarOnRailPose(CVehicle* pVehicle, CVector *pPosition, CVector *pDirection)
{
	CCarRailsEngineView view;
	view.pVehicle = pVehicle;
	CarRailsPose(view, view, &pPosition->x, &pPosition->y, &pDirection->x, &pDirection->y);
	pPosition->z = 0.0f;
	pDirection->z = 0.0f;
}

void
CCarCtrl::UpdateCarOnRails(CVehicle* pVehicle)
{
	if (pVehicle->AutoPilot.m_nTempAction == TEMPACT_WAIT){
		pVehicle->SetMoveSpeed(0.0f, 0.0f, 0.0f);
		pVehicle->AutoPilot.ModifySpeed(0.0f);
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTempAction){
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
			pVehicle->AutoPilot.m_nAntiReverseTimer = CTimer::GetTimeInMilliseconds();
			pVehicle->AutoPilot.m_nTimeToStartMission = CTimer::GetTimeInMilliseconds();
		}
		return;
	}
	SlowCarOnRailsDownForTrafficAndLights(pVehicle);
	if (pVehicle->AutoPilot.m_nTimeEnteredCurve + pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve <= CTimer::GetTimeInMilliseconds())
		PickNextNodeAccordingStrategy(pVehicle);
	if (pVehicle->GetStatus() == STATUS_PHYSICS)
		return;
	CVector positionIncludingCurve;
	CVector directionIncludingCurve;
	ComputeCarOnRailPose(pVehicle, &positionIncludingCurve, &directionIncludingCurve);
	positionIncludingCurve.z = 15.0f;
	DragCarToPoint(pVehicle, &positionIncludingCurve);
	pVehicle->SetMoveSpeed(directionIncludingCurve / GAME_SPEED_TO_CARAI_SPEED);
}

float
CCarCtrl::FindMaximumSpeedForThisCarInTraffic(CVehicle* pVehicle)
{
	if (pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_AVOID_CARS ||
		pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_PLOUGH_THROUGH)
		return pVehicle->AutoPilot.GetCruiseSpeed();
	float left = pVehicle->GetPosition().x - DISTANCE_TO_SCAN_FOR_DANGER;
	float right = pVehicle->GetPosition().x + DISTANCE_TO_SCAN_FOR_DANGER;
	float top = pVehicle->GetPosition().y - DISTANCE_TO_SCAN_FOR_DANGER;
	float bottom = pVehicle->GetPosition().y + DISTANCE_TO_SCAN_FOR_DANGER;
	int xstart = Max(0, CWorld::GetSectorIndexX(left));
	int xend = Min(NUMSECTORS_X - 1, CWorld::GetSectorIndexX(right));
	int ystart = Max(0, CWorld::GetSectorIndexY(top));
	int yend = Min(NUMSECTORS_Y - 1, CWorld::GetSectorIndexY(bottom));
	assert(xstart <= xend);
	assert(ystart <= yend);

	float maxSpeed = pVehicle->AutoPilot.GetCruiseSpeed();

	CWorld::AdvanceCurrentScanCode();

	for (int y = ystart; y <= yend; y++){
		for (int x = xstart; x <= xend; x++){
			CSector* s = CWorld::GetSector(x, y);
			SlowCarDownForCarsSectorList(s->m_lists[ENTITYLIST_VEHICLES], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.GetCruiseSpeed());
			SlowCarDownForCarsSectorList(s->m_lists[ENTITYLIST_VEHICLES_OVERLAP], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.GetCruiseSpeed());
			SlowCarDownForPedsSectorList(s->m_lists[ENTITYLIST_PEDS], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.GetCruiseSpeed());
			SlowCarDownForPedsSectorList(s->m_lists[ENTITYLIST_PEDS_OVERLAP], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.GetCruiseSpeed());
		}
	}
	pVehicle->bWarnedPeds = true;
	if (pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS || pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS_IGNORE_LIGHTS)
		return maxSpeed;
	return (maxSpeed + pVehicle->AutoPilot.GetCruiseSpeed()) / 2;
}

void
CCarCtrl::ScanForPedDanger(CVehicle* pVehicle)
{
	bool storedSlowDownFlag = pVehicle->AutoPilot.m_bSlowedDownBecauseOfPeds;
	float left = pVehicle->GetPosition().x - DISTANCE_TO_SCAN_FOR_PED_DANGER;
	float right = pVehicle->GetPosition().x + DISTANCE_TO_SCAN_FOR_PED_DANGER;
	float top = pVehicle->GetPosition().y - DISTANCE_TO_SCAN_FOR_PED_DANGER;
	float bottom = pVehicle->GetPosition().y + DISTANCE_TO_SCAN_FOR_PED_DANGER;
	int xstart = Max(0, CWorld::GetSectorIndexX(left));
	int xend = Min(NUMSECTORS_X - 1, CWorld::GetSectorIndexX(right));
	int ystart = Max(0, CWorld::GetSectorIndexY(top));
	int yend = Min(NUMSECTORS_Y - 1, CWorld::GetSectorIndexY(bottom));
	assert(xstart <= xend);
	assert(ystart <= yend);

	float maxSpeed = pVehicle->AutoPilot.m_nCruiseSpeed;

	CWorld::AdvanceCurrentScanCode();

	for (int y = ystart; y <= yend; y++) {
		for (int x = xstart; x <= xend; x++) {
			CSector* s = CWorld::GetSector(x, y);
			SlowCarDownForPedsSectorList(s->m_lists[ENTITYLIST_PEDS], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.m_nCruiseSpeed);
			SlowCarDownForPedsSectorList(s->m_lists[ENTITYLIST_PEDS_OVERLAP], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.m_nCruiseSpeed);
		}
	}
	pVehicle->bWarnedPeds = true;
	pVehicle->AutoPilot.m_bSlowedDownBecauseOfPeds = storedSlowDownFlag;
}

void
CCarCtrl::SlowCarOnRailsDownForTrafficAndLights(CVehicle* pVehicle)
{
	float maxSpeed;
	if (CTrafficLights::ShouldCarStopForLight(pVehicle, false) || CTrafficLights::ShouldCarStopForBridge(pVehicle)){
		CCarAI::CarHasReasonToStop(pVehicle);
		maxSpeed = 0.0f;
	}else{
		maxSpeed = FindMaximumSpeedForThisCarInTraffic(pVehicle);
	}
	float curSpeed = pVehicle->AutoPilot.m_fMaxTrafficSpeed;
	if (maxSpeed >= curSpeed){
		if (maxSpeed > curSpeed)
			pVehicle->AutoPilot.ModifySpeed(Min(maxSpeed, curSpeed + 0.05f * CTimer::GetTimeStep()));
	}else if (curSpeed != 0.0f) {
		if (curSpeed < 0.1f)
			pVehicle->AutoPilot.ModifySpeed(0.0f);
		else
			pVehicle->AutoPilot.ModifySpeed(Max(maxSpeed, curSpeed - 0.7f * CTimer::GetTimeStep()));
	}
}

void CCarCtrl::SlowCarDownForPedsSectorList(CPtrList& lst, CVehicle* pVehicle, float x_inf, float y_inf, float x_sup, float y_sup, float* pSpeed, float curSpeed)
{
	float frontOffset = pVehicle->GetModelInfo()->GetColModel()->boundingBox.max.y;
	float frontSafe = frontOffset + SAFE_DISTANCE_TO_PED;
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next){
		CPed* pPed = (CPed*)pNode->item;
		if (pPed->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pPed->bUsesCollision)
			continue;
		pPed->m_scanCode = CWorld::GetCurrentScanCode();
		CVector vecPedPos = pPed->GetPosition();
		if (vecPedPos.x < x_inf || vecPedPos.x > x_sup)
			continue;
		if (vecPedPos.y < y_inf || vecPedPos.y > y_sup)
			continue;
		if (ABS(vecPedPos.z - pVehicle->GetPosition().z) >= 4.0f)
			continue;
		CVector vecToPed = vecPedPos - pVehicle->GetPosition();
		float dotDirection = DotProduct(pVehicle->GetForward(), vecToPed);
		float dotVelocity = DotProduct(pVehicle->GetForward(), pVehicle->GetMoveSpeed());
		if (dotDirection <= frontOffset) /* If already run him over, don't care */
			continue;
		float distanceUntilHit = dotDirection - frontOffset;
		float movementTowardsPedPerSecond = GAME_SPEED_TO_METERS_PER_SECOND * dotVelocity;
		if (4 * movementTowardsPedPerSecond <= distanceUntilHit)
			/* If car isn't projected to hit a ped in 4 seconds, don't care */
			continue;
		float sidewaysDistance = ABS(DotProduct(pVehicle->GetRight(), vecToPed));
		float sideLength = pVehicle->GetModelInfo()->GetColModel()->boundingBox.max.x;
		if (pVehicle->m_vehType == VEHICLE_TYPE_BIKE)
			sideLength *= 1.6f;
		if (sideLength + 0.5f < sidewaysDistance)
			/* If car is far enough taking side into account, don't care */
			continue;
		if (pPed->IsPed()){ /* ...how can it not be? */
			if (pPed->GetPedState() != PED_STEP_AWAY && pPed->GetPedState() != PED_DIVE_AWAY){
				if (distanceUntilHit < movementTowardsPedPerSecond){
					/* Very close. Time to evade. */
					if (pVehicle->GetModelIndex() == MI_RCBANDIT){
						if (dotVelocity * GAME_SPEED_TO_METERS_PER_SECOND / 2 > distanceUntilHit)
							pPed->SetEvasiveStep(pVehicle, 0);
					}else if (dotVelocity > 0.3f) {
						if (sideLength + 0.1f < sidewaysDistance)
							pPed->SetEvasiveStep(pVehicle, 0);
						else
							pPed->SetEvasiveDive(pVehicle, 0);
					}else if (dotVelocity > 0.1f) {
						if (sideLength - 0.5f < sidewaysDistance)
							pPed->SetEvasiveStep(pVehicle, 0);
						else
							pPed->SetEvasiveDive(pVehicle, 0);
					}
				}else{
					/* Relatively safe but annoying. */
					if (pVehicle->GetStatus() == STATUS_PLAYER &&
					  pPed->GetPedState() != PED_FLEE_ENTITY &&
					  pPed->CharCreatedBy == RANDOM_CHAR){
						float angleCarToPed = CGeneral::GetRadianAngleBetweenPoints(
							pVehicle->GetPosition().x, pVehicle->GetPosition().y,
							pPed->GetPosition().x, pPed->GetPosition().y
						);
						angleCarToPed = CGeneral::LimitRadianAngle(angleCarToPed);
						pPed->m_headingRate = CGeneral::LimitRadianAngle(pPed->m_headingRate);
						float visibilityAngle = ABS(angleCarToPed - pPed->m_headingRate);
						if (visibilityAngle > PI)
							visibilityAngle = TWOPI - visibilityAngle;
						if (visibilityAngle < HALFPI || pVehicle->m_nCarHornTimer){
							/* if ped sees the danger or if car horn is on */
							pPed->SetFlee(pVehicle, 2000);
							pPed->bUsePedNodeSeek = false;
							pPed->SetMoveState(PEDMOVE_RUN);
						}
					}else{
						CPlayerPed* pPlayerPed = (CPlayerPed*)pPed;
						if (pPlayerPed->IsPlayer() && dotDirection < frontSafe &&
						  pPlayerPed->IsPedInControl() &&
						  pPlayerPed->m_fMoveSpeed < 1.0f && !pPlayerPed->bIsLooking &&
						  CTimer::GetTimeInMilliseconds() > pPlayerPed->m_lookTimer) {
							pPlayerPed->AnnoyPlayerPed(false);
							pPlayerPed->SetLookFlag(pVehicle, true);
							pPlayerPed->SetLookTimer(1500);
							if (pPlayerPed->GetWeapon()->m_eWeaponType == WEAPONTYPE_UNARMED ||
								pPlayerPed->GetWeapon()->m_eWeaponType == WEAPONTYPE_BASEBALLBAT ||
								pPlayerPed->GetWeapon()->m_eWeaponType == WEAPONTYPE_COLT45 ||
								pPlayerPed->GetWeapon()->m_eWeaponType == WEAPONTYPE_UZI) {
								pPlayerPed->bShakeFist = true;
							}
						}
					}
				}
			}
		}
		/* Ped stuff done. Now vehicle stuff. */
		if (distanceUntilHit < 10.0f){
			if (pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS ||
			  pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_SLOW_DOWN_FOR_CARS){
				*pSpeed = Min(*pSpeed, ABS(distanceUntilHit - 1.0f) / 10.0f * curSpeed);
				pVehicle->AutoPilot.m_bSlowedDownBecauseOfPeds = true;
				if (distanceUntilHit < 2.0f){
					pVehicle->AutoPilot.m_nTempAction = TEMPACT_WAIT;
					pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 3000;
				}
			}
		}
	}
}

void CCarCtrl::SlowCarDownForCarsSectorList(CPtrList& lst, CVehicle* pVehicle, float x_inf, float y_inf, float x_sup, float y_sup, float* pSpeed, float curSpeed)
{
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next){
		CVehicle* pTestVehicle = (CVehicle*)pNode->item;
		if (pVehicle == pTestVehicle)
			continue;
		if (pTestVehicle->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pTestVehicle->bUsesCollision)
			continue;
		pTestVehicle->m_scanCode = CWorld::GetCurrentScanCode();
		CVector boundCenter = pTestVehicle->GetBoundCentre();
		if (boundCenter.x < x_inf || boundCenter.x > x_sup)
			continue;
		if (boundCenter.y < y_inf || boundCenter.y > y_sup)
			continue;
		if (Abs(boundCenter.z - pVehicle->GetPosition().z) < 5.0f)
			SlowCarDownForOtherCar(pTestVehicle, pVehicle, pSpeed, curSpeed);
	}
}

void CCarCtrl::SlowCarDownForOtherCar(CEntity* pOtherEntity, CVehicle* pVehicle, float* pSpeed, float curSpeed)
{
	CVector forwardA = pVehicle->GetForward();
	((CVector2D)forwardA).Normalise();
	if (DotProduct2D(pOtherEntity->GetPosition() - pVehicle->GetPosition(), forwardA) < 0.0f)
		return;
	CVector forwardB = pOtherEntity->GetForward();
	((CVector2D)forwardB).Normalise();
	forwardA.z = forwardB.z = 0.0f;
	CVehicle* pOtherVehicle = (CVehicle*)pOtherEntity;
	/* why is the argument CEntity if it's always CVehicle anyway and is casted? */
	float speedOtherX = GAME_SPEED_TO_CARAI_SPEED * pOtherVehicle->GetMoveSpeed().x;
	float speedOtherY = GAME_SPEED_TO_CARAI_SPEED * pOtherVehicle->GetMoveSpeed().y;
	float projectionX = speedOtherX - forwardA.x * curSpeed;
	float projectionY = speedOtherY - forwardA.y * curSpeed;
	float proximityA = TestCollisionBetween2MovingRects(pOtherVehicle, pVehicle, projectionX, projectionY, &forwardA, &forwardB, 0);
	float proximityB = TestCollisionBetween2MovingRects(pVehicle, pOtherVehicle, -projectionX, -projectionY, &forwardB, &forwardA, 1);
	float minProximity = Min(proximityA, proximityB);
	if (minProximity >= 0.0f && minProximity < 1.5f){
		minProximity = Max(0.0f, (minProximity - 0.2f) / 1.3f);
		pVehicle->AutoPilot.m_bSlowedDownBecauseOfCars = true;
		*pSpeed = Min(*pSpeed, minProximity * curSpeed);
	}
	if (minProximity >= 0.0f && minProximity < 0.5f && pOtherEntity->IsVehicle() &&
	  CTimer::GetTimeInMilliseconds() - pVehicle->AutoPilot.m_nTimeToStartMission > 15000 &&
	  CTimer::GetTimeInMilliseconds() - pOtherVehicle->AutoPilot.m_nTimeToStartMission > 15000){
		/* If cars are standing for 15 seconds, annoy one of them and make avoid cars. */
		if (pOtherEntity != FindPlayerVehicle() &&
		  DotProduct2D(pVehicle->GetForward(), pOtherVehicle->GetForward()) < -0.5f &&
		  pVehicle < pOtherVehicle){ /* that comparasion though... */
			*pSpeed = Max(curSpeed / 5, *pSpeed);
			if (pVehicle->GetStatus() == STATUS_SIMPLE){
				pVehicle->SetStatus(STATUS_PHYSICS);
				SwitchVehicleToRealPhysics(pVehicle);
			}
			pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
			pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 1000;
		}
	}
}

float CCarCtrl::TestCollisionBetween2MovingRects(CVehicle* pVehicleA, CVehicle* pVehicleB, float projectionX, float projectionY, CVector* pForwardA, CVector* pForwardB, uint8 id)
{
	CVector2D vecBToA = pVehicleA->GetPosition() - pVehicleB->GetPosition();
	float lenB = pVehicleB->GetModelInfo()->GetColModel()->boundingBox.max.y;
	float widthB = pVehicleB->GetModelInfo()->GetColModel()->boundingBox.max.x;
	float backLenB = -pVehicleB->GetModelInfo()->GetColModel()->boundingBox.min.y;
	float lenA = pVehicleA->GetModelInfo()->GetColModel()->boundingBox.max.y;
	float widthA = pVehicleA->GetModelInfo()->GetColModel()->boundingBox.max.x;
	float backLenA = -pVehicleA->GetModelInfo()->GetColModel()->boundingBox.min.y;
	float proximity = 1.0f;
	float fullWidthB = 2.0f * widthB;
	float fullLenB = lenB + backLenB;
	for (int i = 0; i < 4; i++){
		float testedOffsetX;
		float testedOffsetY;
		switch (i) {
		case 0: /* Front right corner */
			testedOffsetX = vecBToA.x + widthA * pForwardB->y + lenA * pForwardB->x;
			testedOffsetY = vecBToA.y + lenA * pForwardB->y - widthA * pForwardB->x;
			break;
		case 1: /* Front left corner */
			testedOffsetX = vecBToA.x + -widthA * pForwardB->x + lenA * pForwardB->x;
			testedOffsetY = vecBToA.y + lenA * pForwardB->y + widthA * pForwardB->x;
			break;
		case 2: /* Rear right corner */
			testedOffsetX = vecBToA.x + widthA * pForwardB->y - backLenA * pForwardB->x;
			testedOffsetY = vecBToA.y - backLenA * pForwardB->y - widthA * pForwardB->x;
			break;
		case 3: /* Rear left corner */
			testedOffsetX = vecBToA.x - widthA * pForwardB->y - backLenA * pForwardB->x;
			testedOffsetY = vecBToA.y - backLenA * pForwardB->y + widthA * pForwardB->x;
			break;
		default:
			break;
		}
		/* Testing width collision */
		float baseWidthProximity = 0.0f;
		float fullWidthProximity = 1.0f;
		float widthDistance = testedOffsetX * pForwardA->y - testedOffsetY * pForwardA->x;
		float widthProjection = projectionX * pForwardA->y - projectionY * pForwardA->x;
		if (widthDistance > widthB){
			if (widthProjection < 0.0f){
				float proximityWidth = -(widthDistance - widthB) / widthProjection;
				if (proximityWidth < 1.0f){
					baseWidthProximity = proximityWidth;
					fullWidthProximity = Min(1.0f, proximityWidth - fullWidthB / widthProjection);
				}else{
					baseWidthProximity = 1.0f;
				}
			}else{
				baseWidthProximity = 1.0f;
				fullWidthProximity = 1.0f;
			}
		}else if (widthDistance < -widthB){
			if (widthProjection > 0.0f) {
				float proximityWidth = -(widthDistance + widthB) / widthProjection;
				if (proximityWidth < 1.0f) {
					baseWidthProximity = proximityWidth;
					fullWidthProximity = Min(1.0f, proximityWidth + fullWidthB / widthProjection);
				}
				else {
					baseWidthProximity = 1.0f;
				}
			}
			else {
				baseWidthProximity = 1.0f;
				fullWidthProximity = 1.0f;
			}
		}else if (widthProjection > 0.0f){
			fullWidthProximity = (widthB - widthDistance) / widthProjection;
		}else if (widthProjection < 0.0f){
			fullWidthProximity = -(widthB + widthDistance) / widthProjection;
		}
		/* Testing length collision */
		float baseLengthProximity = 0.0f;
		float fullLengthProximity = 1.0f;
		float lenDistance = testedOffsetX * pForwardA->x + testedOffsetY * pForwardA->y;
		float lenProjection = projectionX * pForwardA->x + projectionY * pForwardA->y;
		if (lenDistance > lenB) {
			if (lenProjection < 0.0f) {
				float proximityLength = -(lenDistance - lenB) / lenProjection;
				if (proximityLength < 1.0f) {
					baseLengthProximity = proximityLength;
					fullLengthProximity = Min(1.0f, proximityLength - fullLenB / lenProjection);
				}
				else {
					baseLengthProximity = 1.0f;
				}
			}
			else {
				baseLengthProximity = 1.0f;
				fullLengthProximity = 1.0f;
			}
		}
		else if (lenDistance < -backLenB) {
			if (lenProjection > 0.0f) {
				float proximityLength = -(lenDistance + backLenB) / lenProjection;
				if (proximityLength < 1.0f) {
					baseLengthProximity = proximityLength;
					fullLengthProximity = Min(1.0f, proximityLength + fullLenB / lenProjection);
				}
				else {
					baseLengthProximity = 1.0f;
				}
			}
			else {
				baseLengthProximity = 1.0f;
				fullLengthProximity = 1.0f;
			}
		}
		else if (lenProjection > 0.0f) {
			fullLengthProximity = (lenB - lenDistance) / lenProjection;
		}
		else if (lenProjection < 0.0f) {
			fullLengthProximity = -(backLenB + lenDistance) / lenProjection;
		}
		float baseProximity = Max(baseWidthProximity, baseLengthProximity);
		if (baseProximity < fullWidthProximity && baseProximity < fullLengthProximity)
			proximity = Min(proximity, baseProximity);
	}
	return proximity;
}

float CCarCtrl::FindAngleToWeaveThroughTraffic(CVehicle* pVehicle, CPhysical* pTarget, float angleToTarget, float angleForward)
{
	float distanceToTest = Min(2.0f, pVehicle->GetMoveSpeed().Magnitude2D() / 0.4f + 1.0f) * 12.0f;
	float left = pVehicle->GetPosition().x - distanceToTest;
	float right = pVehicle->GetPosition().x + distanceToTest;
	float top = pVehicle->GetPosition().y - distanceToTest;
	float bottom = pVehicle->GetPosition().y + distanceToTest;
	int xstart = Max(0, CWorld::GetSectorIndexX(left));
	int xend = Min(NUMSECTORS_X - 1, CWorld::GetSectorIndexX(right));
	int ystart = Max(0, CWorld::GetSectorIndexY(top));
	int yend = Min(NUMSECTORS_Y - 1, CWorld::GetSectorIndexY(bottom));
	assert(xstart <= xend);
	assert(ystart <= yend);

	float angleToWeaveLeft = angleToTarget;
	float angleToWeaveRight = angleToTarget;

	CWorld::AdvanceCurrentScanCode();

	float angleToWeaveLeftLastIteration = -9999.9f;
	float angleToWeaveRightLastIteration = -9999.9f;

	while (angleToWeaveLeft != angleToWeaveLeftLastIteration ||
		   angleToWeaveRight != angleToWeaveRightLastIteration){
		angleToWeaveLeftLastIteration = angleToWeaveLeft;
		angleToWeaveRightLastIteration = angleToWeaveRight;
		for (int y = ystart; y <= yend; y++) {
			for (int x = xstart; x <= xend; x++) {
				CSector* s = CWorld::GetSector(x, y);
				WeaveThroughCarsSectorList(s->m_lists[ENTITYLIST_VEHICLES], pVehicle, pTarget,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughCarsSectorList(s->m_lists[ENTITYLIST_VEHICLES_OVERLAP], pVehicle, pTarget,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughPedsSectorList(s->m_lists[ENTITYLIST_PEDS], pVehicle, pTarget,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughPedsSectorList(s->m_lists[ENTITYLIST_PEDS_OVERLAP], pVehicle, pTarget,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughObjectsSectorList(s->m_lists[ENTITYLIST_OBJECTS], pVehicle,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughObjectsSectorList(s->m_lists[ENTITYLIST_OBJECTS_OVERLAP], pVehicle,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
			}
		}
	}
	float angleDiffFromActualToTarget = LimitRadianAngle(angleForward - angleToTarget);
	float angleToBisectActualToTarget = LimitRadianAngle(angleToTarget + angleDiffFromActualToTarget / 2);
	float angleDiffLeft = LimitRadianAngle(angleToWeaveLeft - angleToBisectActualToTarget);
	angleDiffLeft = ABS(angleDiffLeft);
	float angleDiffRight = LimitRadianAngle(angleToWeaveRight - angleToBisectActualToTarget);
	angleDiffRight = ABS(angleDiffRight);
	if (angleDiffLeft > HALFPI && angleDiffRight > HALFPI)
		return angleToBisectActualToTarget;
	if (ABS(angleDiffLeft - angleDiffRight) < 0.08f)
		return angleToWeaveRight;
	return angleDiffLeft < angleDiffRight ? angleToWeaveLeft : angleToWeaveRight;
}

void CCarCtrl::WeaveThroughCarsSectorList(CPtrList& lst, CVehicle* pVehicle, CPhysical* pTarget, float x_inf, float y_inf, float x_sup, float y_sup, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next) {
		CVehicle* pTestVehicle = (CVehicle*)pNode->item;
		if (pTestVehicle->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pTestVehicle->bUsesCollision)
			continue;
		if (pTestVehicle == pTarget)
			continue;
		pTestVehicle->m_scanCode = CWorld::GetCurrentScanCode();
		if (pTestVehicle->GetBoundCentre().x < x_inf || pTestVehicle->GetBoundCentre().x > x_sup)
			continue;
		if (pTestVehicle->GetBoundCentre().y < y_inf || pTestVehicle->GetBoundCentre().y > y_sup)
			continue;
		if (Abs(pTestVehicle->GetPosition().z - pVehicle->GetPosition().z) >= VEHICLE_HEIGHT_DIFF_TO_CONSIDER_WEAVING)
			continue;
		if (pTestVehicle != pVehicle && (!pVehicle->bPartOfConvoy || !pTestVehicle->bPartOfConvoy))
			WeaveForOtherCar(pTestVehicle, pVehicle, pAngleToWeaveLeft, pAngleToWeaveRight);
	}
}

void CCarCtrl::WeaveForOtherCar(CEntity* pOtherEntity, CVehicle* pVehicle, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	CVehicle* pOtherCar = (CVehicle*)pOtherEntity;
	if (pVehicle->AutoPilot.m_nCarMission == MISSION_RAMPLAYER_CLOSE && pOtherEntity == FindPlayerVehicle())
		return;
	if (pVehicle->AutoPilot.m_nCarMission == MISSION_RAMCAR_CLOSE && pOtherEntity == pVehicle->AutoPilot.m_pTargetCar)
		return;
	CVector2D vecDiff = pOtherCar->GetPosition() - pVehicle->GetPosition();
	float angleBetweenVehicles = CGeneral::GetATanOfXY(vecDiff.x, vecDiff.y);
	float distance = vecDiff.Magnitude();
	if (distance < 1.0f)
		return;
	if (DotProduct2D(pVehicle->GetMoveSpeed() - pOtherCar->GetMoveSpeed(), vecDiff) * 110.0f -
	  pOtherCar->GetColModel()->boundingSphere.radius -
	  pVehicle->GetColModel()->boundingSphere.radius < distance)
		return;
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
	float forwardAngle = CGeneral::GetATanOfXY(forward.x, forward.y);
	float angleDiff = angleBetweenVehicles - forwardAngle;
	float lenProjection = ABS(pOtherCar->GetColModel()->boundingBox.max.y * Sin(angleDiff));
	float widthProjection = ABS(pOtherCar->GetColModel()->boundingBox.max.x * Cos(angleDiff));
	float lengthToEvade = (2 * (lenProjection + widthProjection) + WIDTH_COEF_TO_WEAVE_SAFELY * 2 * pVehicle->GetColModel()->boundingBox.max.x) / distance;
	float diffToLeftAngle = LimitRadianAngle(angleBetweenVehicles - *pAngleToWeaveLeft);
	diffToLeftAngle = ABS(diffToLeftAngle);
	float angleToWeave = lengthToEvade / 2;
	if (diffToLeftAngle < angleToWeave){
		*pAngleToWeaveLeft = angleBetweenVehicles - angleToWeave;
		while (*pAngleToWeaveLeft < -PI)
			*pAngleToWeaveLeft += TWOPI;
	}
	float diffToRightAngle = LimitRadianAngle(angleBetweenVehicles - *pAngleToWeaveRight);
	diffToRightAngle = ABS(diffToRightAngle);
	if (diffToRightAngle < angleToWeave){
		*pAngleToWeaveRight = angleBetweenVehicles + angleToWeave;
		while (*pAngleToWeaveRight > PI)
			*pAngleToWeaveRight -= TWOPI;
	}
}

void CCarCtrl::WeaveThroughPedsSectorList(CPtrList& lst, CVehicle* pVehicle, CPhysical* pTarget, float x_inf, float y_inf, float x_sup, float y_sup, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next) {
		CPed* pPed = (CPed*)pNode->item;
		if (pPed->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pPed->bUsesCollision)
			continue;
		if (pPed == pTarget)
			continue;
		pPed->m_scanCode = CWorld::GetCurrentScanCode();
		if (pPed->GetPosition().x < x_inf || pPed->GetPosition().x > x_sup)
			continue;
		if (pPed->GetPosition().y < y_inf || pPed->GetPosition().y > y_sup)
			continue;
		if (Abs(pPed->GetPosition().z - pVehicle->GetPosition().z) >= PED_HEIGHT_DIFF_TO_CONSIDER_WEAVING)
			continue;
		if (pPed->m_pCurSurface != pVehicle && pPed->m_attachedTo != pVehicle)
			WeaveForPed(pPed, pVehicle, pAngleToWeaveLeft, pAngleToWeaveRight);
	}

}
void CCarCtrl::WeaveForPed(CEntity* pOtherEntity, CVehicle* pVehicle, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	if (pVehicle->AutoPilot.m_nCarMission == MISSION_RAMPLAYER_CLOSE && pOtherEntity == FindPlayerPed())
		return;
	CPed* pPed = (CPed*)pOtherEntity;
	CVector2D vecDiff = pPed->GetPosition() - pVehicle->GetPosition();
	float angleBetweenVehicleAndPed = CGeneral::GetATanOfXY(vecDiff.x, vecDiff.y);
	float distance = vecDiff.Magnitude();
	float lengthToEvade = (WIDTH_COEF_TO_WEAVE_SAFELY * 2 * pVehicle->GetColModel()->boundingBox.max.x + PED_WIDTH_TO_WEAVE) / distance;
	float diffToLeftAngle = LimitRadianAngle(angleBetweenVehicleAndPed - *pAngleToWeaveLeft);
	diffToLeftAngle = ABS(diffToLeftAngle);
	float angleToWeave = lengthToEvade / 2;
	if (diffToLeftAngle < angleToWeave) {
		*pAngleToWeaveLeft = angleBetweenVehicleAndPed - angleToWeave;
		while (*pAngleToWeaveLeft < -PI)
			*pAngleToWeaveLeft += TWOPI;
	}
	float diffToRightAngle = LimitRadianAngle(angleBetweenVehicleAndPed - *pAngleToWeaveRight);
	diffToRightAngle = ABS(diffToRightAngle);
	if (diffToRightAngle < angleToWeave) {
		*pAngleToWeaveRight = angleBetweenVehicleAndPed + angleToWeave;
		while (*pAngleToWeaveRight > PI)
			*pAngleToWeaveRight -= TWOPI;
	}
}

void CCarCtrl::WeaveThroughObjectsSectorList(CPtrList& lst, CVehicle* pVehicle, float x_inf, float y_inf, float x_sup, float y_sup, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next) {
		CObject* pObject = (CObject*)pNode->item;
		if (pObject->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pObject->bUsesCollision)
			continue;
		pObject->m_scanCode = CWorld::GetCurrentScanCode();
		if (pObject->GetPosition().x < x_inf || pObject->GetPosition().x > x_sup)
			continue;
		if (pObject->GetPosition().y < y_inf || pObject->GetPosition().y > y_sup)
			continue;
		if (Abs(pObject->GetPosition().z - pVehicle->GetPosition().z) >= OBJECT_HEIGHT_DIFF_TO_CONSIDER_WEAVING)
			continue;
		if (pObject->GetUp().z > 0.9f)
			WeaveForObject(pObject, pVehicle, pAngleToWeaveLeft, pAngleToWeaveRight);
	}
}

void CCarCtrl::WeaveForObject(CEntity* pOtherEntity, CVehicle* pVehicle, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	float rightCoef;
	float forwardCoef;
	if (pOtherEntity->GetModelIndex() == MI_TRAFFICLIGHTS){
		rightCoef = 2.957f;
		forwardCoef = 0.147f;
	}else if (pOtherEntity->GetModelIndex() == MI_SINGLESTREETLIGHTS1){
		rightCoef = 0.744f;
		forwardCoef = 0.0f;
	}else if (pOtherEntity->GetModelIndex() == MI_SINGLESTREETLIGHTS2){
		rightCoef = 0.043f;
		forwardCoef = 0.0f;
	}else if (pOtherEntity->GetModelIndex() == MI_SINGLESTREETLIGHTS3){
		rightCoef = 1.143f;
		forwardCoef = 0.145f;
	}else if (pOtherEntity->GetModelIndex() == MI_DOUBLESTREETLIGHTS){
		rightCoef = 0.0f;
		forwardCoef = -0.048f;
	}else if (IsTreeModel(pOtherEntity->GetModelIndex())){
		rightCoef = 0.0f;
		forwardCoef = 0.0f;
	}else if (pOtherEntity->GetModelIndex() == MI_STREETLAMP1 || pOtherEntity->GetModelIndex() == MI_STREETLAMP2){
		rightCoef = 0.0f;
		forwardCoef = 0.0f;
	}else
		return;
	CObject* pObject = (CObject*)pOtherEntity;
	CVector2D vecDiff = pObject->GetPosition() +
		rightCoef * pObject->GetRight() +
		forwardCoef * pObject->GetForward() -
		pVehicle->GetPosition();
	float angleBetweenVehicleAndObject = CGeneral::GetATanOfXY(vecDiff.x, vecDiff.y);
	float distance = vecDiff.Magnitude();
	float lengthToEvade = (WIDTH_COEF_TO_WEAVE_SAFELY * 2 * pVehicle->GetColModel()->boundingBox.max.x + OBJECT_WIDTH_TO_WEAVE) / distance;
	float diffToLeftAngle = LimitRadianAngle(angleBetweenVehicleAndObject - *pAngleToWeaveLeft);
	diffToLeftAngle = ABS(diffToLeftAngle);
	float angleToWeave = lengthToEvade / 2;
	if (diffToLeftAngle < angleToWeave) {
		*pAngleToWeaveLeft = angleBetweenVehicleAndObject - angleToWeave;
		while (*pAngleToWeaveLeft < -PI)
			*pAngleToWeaveLeft += TWOPI;
	}
	float diffToRightAngle = LimitRadianAngle(angleBetweenVehicleAndObject - *pAngleToWeaveRight);
	diffToRightAngle = ABS(diffToRightAngle);
	if (diffToRightAngle < angleToWeave) {
		*pAngleToWeaveRight = angleBetweenVehicleAndObject + angleToWeave;
		while (*pAngleToWeaveRight > PI)
			*pAngleToWeaveRight -= TWOPI;
	}
}

bool CCarCtrl::PickNextNodeAccordingStrategy(CVehicle* pVehicle)
{
	pVehicle->AutoPilot.m_nCruiseSpeedMultiplierType = ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode].speedLimit;
	switch (pVehicle->AutoPilot.m_nCarMission){
	case MISSION_RAMPLAYER_FARAWAY:
	case MISSION_BLOCKPLAYER_FARAWAY:
		PickNextNodeToChaseCar(pVehicle,
			FindPlayerCoors().x,
			FindPlayerCoors().y,
#ifdef FIX_PATHFIND_BUG
			FindPlayerCoors().z,
#endif
			FindPlayerVehicle());
		return false;
	case MISSION_GOTOCOORDS:
	case MISSION_GOTOCOORDS_ACCURATE:
		return PickNextNodeToFollowPath(pVehicle);
	case MISSION_RAMCAR_FARAWAY:
	case MISSION_BLOCKCAR_FARAWAY:
		PickNextNodeToChaseCar(pVehicle,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().x,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().y,
#ifdef FIX_PATHFIND_BUG
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().z,
#endif
			pVehicle->AutoPilot.m_pTargetCar);
		return false;
	default:
		PickNextNodeRandomly(pVehicle);
		if (ThePaths.GetNode(pVehicle->AutoPilot.m_nNextRouteNode)->bOnlySmallBoats && BoatWithTallMast(pVehicle->GetModelIndex()))
			pVehicle->AutoPilot.m_nCruiseSpeed = 0;
		return false;
	}
}

void CCarCtrl::PickNextNodeRandomly(CVehicle* pVehicle)
{
	/* The rules live in CarOnRails.h (VICE-133): the same pick runs on the far
	 * traffic layer's worker, over its own snapshot. The route seed reseeds
	 * the engine's shared stream here, on the side that owns it; the worker's
	 * sim carries its own per-car stream instead. */
	if (pVehicle->m_nRouteSeed)
		CGeneral::SetRandomSeed(pVehicle->m_nRouteSeed);
	CCarRailsEngineView view;
	view.pVehicle = pVehicle;
	CarRailsPickNextNode(view, view);
}

#ifdef FIX_PATHFIND_BUG
void CCarCtrl::PickNextNodeToChaseCar(CVehicle* pVehicle, float targetX, float targetY, float targetZ, CVehicle* pTarget)
#else
void CCarCtrl::PickNextNodeToChaseCar(CVehicle* pVehicle, float targetX, float targetY, CVehicle* pTarget)
#endif
{
	if (pVehicle->m_nRouteSeed)
		CGeneral::SetRandomSeed(pVehicle->m_nRouteSeed);
	int prevNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	int curNode = pVehicle->AutoPilot.m_nNextRouteNode;
	CPathNode* pPrevNode = &ThePaths.m_pathNodes[prevNode];
	CPathNode* pCurNode = &ThePaths.m_pathNodes[curNode];
	CPathNode* pTargetNode[2];
	int16 numNodes;
	float distanceToTargetNode;
	ThePaths.DoPathSearch(0, pCurNode->GetPosition(), curNode,
#ifdef FIX_PATHFIND_BUG
		CVector(targetX, targetY, targetZ),
#else
		CVector(targetX, targetY, 0.0f),
#endif
		pTargetNode, &numNodes, 2, pVehicle, &distanceToTargetNode, 999999.9f, -1);

	int newNextNode;
	int nextLink;
	if (numNodes != 1 && numNodes != 2 || pTargetNode[0] == pCurNode){
		if (numNodes != 2 || pTargetNode[1] == pCurNode) {
			float currentAngle = CGeneral::GetATanOfXY(targetX - pVehicle->GetPosition().x, targetY - pVehicle->GetPosition().y);
			nextLink = 0;
			float lowestAngleChange = 10.0f;
			int numLinks = pCurNode->numLinks;
			newNextNode = 0;
			for (int i = 0; i < numLinks; i++) {
				int conNode = ThePaths.ConnectedNode(i + pCurNode->firstLink);
				if (conNode == prevNode && i > 1)
					continue;
				CPathNode* pTestNode = &ThePaths.m_pathNodes[conNode];
				float angle = CGeneral::GetATanOfXY(pTestNode->GetX() - pCurNode->GetX(), pTestNode->GetY() - pCurNode->GetY());
				angle = LimitRadianAngle(angle - currentAngle);
				angle = ABS(angle);
				if (angle < lowestAngleChange) {
					lowestAngleChange = angle;
					newNextNode = conNode;
					nextLink = i;
				}
			}
		}
		else {
			nextLink = 0;
			newNextNode = pTargetNode[1] - ThePaths.m_pathNodes;
			for (int i = pCurNode->firstLink; ThePaths.ConnectedNode(i) != newNextNode; i++, nextLink++)
				;
		}
	}
	else {
		nextLink = 0;
		newNextNode = pTargetNode[0] - ThePaths.m_pathNodes;
		for (int i = pCurNode->firstLink; ThePaths.ConnectedNode(i) != newNextNode; i++, nextLink++)
			;
	}
	CPathNode* pNextPathNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode];
	CCarPathLink* pNextLink = &ThePaths.m_carPathLinks[ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink]];
	CCarPathLink* pCurLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	pVehicle->AutoPilot.m_nPrevRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_nNextRouteNode;
	pVehicle->AutoPilot.m_nNextRouteNode = newNextNode;
	pVehicle->AutoPilot.m_nTimeEnteredCurve += pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
	pVehicle->AutoPilot.m_nPreviousPathNodeInfo = pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	pVehicle->AutoPilot.m_nPreviousDirection = pVehicle->AutoPilot.m_nCurrentDirection;
	pVehicle->AutoPilot.m_nCurrentDirection = pVehicle->AutoPilot.m_nNextDirection;
	pVehicle->AutoPilot.m_nCurrentLane = pVehicle->AutoPilot.m_nNextLane;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink];
	int8 lanesOnNextNode;
	if (curNode >= pVehicle->AutoPilot.m_nNextRouteNode) {
		pVehicle->AutoPilot.m_nNextDirection = 1;
		lanesOnNextNode = pNextLink->numRightLanes;
	}
	else {
		pVehicle->AutoPilot.m_nNextDirection = -1;
		lanesOnNextNode = pNextLink->numLeftLanes;
	}
	float currentPathLinkForwardX = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirX();
	float currentPathLinkForwardY = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirY();
	float nextPathLinkForwardX = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirX();
	float nextPathLinkForwardY = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirY();
	if (lanesOnNextNode >= 0) {
		CVector2D dist = pNextPathNode->GetPosition() - pCurNode->GetPosition();
		if (dist.MagnitudeSqr() >= SQR(7.0f)){
			/* 25% chance vehicle will try to switch lane */
			/* No lane switching if following car from far away */
			/* ...although it's always one of those. */
			if ((CGeneral::GetRandomNumber() & 0x600) == 0 &&
			  pVehicle->AutoPilot.m_nCarMission != MISSION_RAMPLAYER_FARAWAY &&
			  pVehicle->AutoPilot.m_nCarMission != MISSION_BLOCKPLAYER_FARAWAY &&
			  pVehicle->AutoPilot.m_nCarMission != MISSION_RAMCAR_FARAWAY &&
			  pVehicle->AutoPilot.m_nCarMission != MISSION_BLOCKCAR_FARAWAY){
				if (CGeneral::GetRandomTrueFalse())
					pVehicle->AutoPilot.m_nNextLane += 1;
				else
					pVehicle->AutoPilot.m_nNextLane -= 1;
			}
		}
		pVehicle->AutoPilot.m_nNextLane = Min(lanesOnNextNode - 1, pVehicle->AutoPilot.m_nNextLane);
		pVehicle->AutoPilot.m_nNextLane = Max(0, pVehicle->AutoPilot.m_nNextLane);
	}
	else {
		pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane;
	}
	if (pVehicle->AutoPilot.m_bStayInFastLane)
		pVehicle->AutoPilot.m_nNextLane = 0;
	CVector positionOnCurrentLinkIncludingLane(
		pCurLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
		pCurLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
		0.0f);
	CVector positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
		0.0f);
	float directionCurrentLinkX = pCurLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionCurrentLinkY = pCurLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	/* We want to make a path between two links that may not have the same forward directions a curve. */
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = CCurves::CalcSpeedScaleFactor(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		directionCurrentLinkX, directionCurrentLinkY,
		directionNextLinkX, directionNextLinkY
	) * (1000.0f / pVehicle->AutoPilot.m_fMaxTrafficSpeed);
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = Max(10, pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve);
}

bool CCarCtrl::PickNextNodeToFollowPath(CVehicle* pVehicle)
{
	if (pVehicle->m_nRouteSeed)
		CGeneral::SetRandomSeed(pVehicle->m_nRouteSeed);
	int curNode = pVehicle->AutoPilot.m_nNextRouteNode;
	CPathNode* pCurNode = &ThePaths.m_pathNodes[curNode];
	if (pVehicle->AutoPilot.m_nPathFindNodesCount == 0){
		ThePaths.DoPathSearch(0, pVehicle->GetPosition(), curNode,
			pVehicle->AutoPilot.m_vecDestinationCoors, pVehicle->AutoPilot.m_aPathFindNodesInfo,
			&pVehicle->AutoPilot.m_nPathFindNodesCount, NUM_PATH_NODES_IN_AUTOPILOT,
			pVehicle, nil, 999999.9f, -1);
		if (pVehicle->AutoPilot.m_nPathFindNodesCount < 2)
			return true;
		pVehicle->AutoPilot.RemoveOnePathNode();
	}
	CPathNode* pNextPathNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode];
	CCarPathLink* pCurLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	pVehicle->AutoPilot.m_nPrevRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_nNextRouteNode;
	pVehicle->AutoPilot.m_nNextRouteNode = pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes;
	pVehicle->AutoPilot.RemoveOnePathNode();
	pVehicle->AutoPilot.m_nTimeEnteredCurve += pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
	pVehicle->AutoPilot.m_nPreviousPathNodeInfo = pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	pVehicle->AutoPilot.m_nPreviousDirection = pVehicle->AutoPilot.m_nCurrentDirection;
	pVehicle->AutoPilot.m_nCurrentDirection = pVehicle->AutoPilot.m_nNextDirection;
	pVehicle->AutoPilot.m_nCurrentLane = pVehicle->AutoPilot.m_nNextLane;
	int nextLink = 0;
	for (int i = pCurNode->firstLink; ThePaths.ConnectedNode(i) != pVehicle->AutoPilot.m_nNextRouteNode; i++, nextLink++)
		;
	CCarPathLink* pNextLink = &ThePaths.m_carPathLinks[ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink]];
	pVehicle->AutoPilot.m_nNextPathNodeInfo = ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink];
	int8 lanesOnNextNode;
	if (curNode >= pVehicle->AutoPilot.m_nNextRouteNode) {
		pVehicle->AutoPilot.m_nNextDirection = 1;
		lanesOnNextNode = pNextLink->numLeftLanes;
	}
	else {
		pVehicle->AutoPilot.m_nNextDirection = -1;
		lanesOnNextNode = pNextLink->numRightLanes;
	}
	float currentPathLinkForwardX = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirX();
	float currentPathLinkForwardY = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirY();
	float nextPathLinkForwardX = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirX();
	float nextPathLinkForwardY = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirY();
	if (lanesOnNextNode >= 0) {
		CVector2D dist = pNextPathNode->GetPosition() - pCurNode->GetPosition();
		if (dist.MagnitudeSqr() >= SQR(7.0f) && (CGeneral::GetRandomNumber() & 0x600) == 0) {
			if (CGeneral::GetRandomTrueFalse())
				pVehicle->AutoPilot.m_nNextLane += 1;
			else
				pVehicle->AutoPilot.m_nNextLane -= 1;
		}
		pVehicle->AutoPilot.m_nNextLane = Min(lanesOnNextNode - 1, pVehicle->AutoPilot.m_nNextLane);
		pVehicle->AutoPilot.m_nNextLane = Max(0, pVehicle->AutoPilot.m_nNextLane);
	}
	else {
		pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane;
	}
	if (pVehicle->AutoPilot.m_bStayInFastLane)
		pVehicle->AutoPilot.m_nNextLane = 0;
	CVector positionOnCurrentLinkIncludingLane(
		pCurLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
		pCurLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
		0.0f);
	CVector positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
		0.0f);
	float directionCurrentLinkX = pCurLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionCurrentLinkY = pCurLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	/* We want to make a path between two links that may not have the same forward directions a curve. */
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = CCurves::CalcSpeedScaleFactor(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		directionCurrentLinkX, directionCurrentLinkY,
		directionNextLinkX, directionNextLinkY
	) * (1000.0f / pVehicle->AutoPilot.m_fMaxTrafficSpeed);
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = Max(10, pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve);
	return false;
}

void CCarCtrl::Init(void)
{
	NumRandomCars = 0;
	NumLawEnforcerCars = 0;
	NumMissionCars = 0;
	NumParkedCars = 0;
	NumPermanentCars = 0;
	NumAmbulancesOnDuty = 0;
	NumFiretrucksOnDuty = 0;
	LastTimeFireTruckCreated = 0;
	LastTimeAmbulanceCreated = 0;
#ifdef FIX_BUGS
	LastTimeLawEnforcerCreated = 0;
	LastTimeMiamiViceGenerated = 0;
#endif
	bCarsGeneratedAroundCamera = false;
	CountDownToCarsAtStart = 2;
	CarDensityMultiplier = 1.0f;
	/* The burst totals and the end reasons are the evidence since this boot
	 * (VICE-146). */
	ms_nFillBurstTotalAttempts = 0;
	ms_nFillBurstTotalPlaced = 0;
	ms_nFillBurstEndedLine = 0;
	ms_nFillBurstEndedBudget = 0;
	ms_nFillBurstEndedStalls = 0;
	ms_nFillBurstEndedWindow = 0;
	/* The service's counters restart with the world (VICE-150). */
	CSpawnService::ReInit();
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++)
		apCarsToKeep[i] = nil;
	for (int i = 0; i < TOTAL_CUSTOM_CLASSES; i++){
		for (int j = 0; j < MAX_CAR_MODELS_IN_ARRAY; j++) {
			LoadedCarsArray[i][j] = -1;
		}
		NumOfLoadedCarsOfRating[i] = 0;
		NumRequestsOfCarRating[i] = 0;
		TotalNumOfCarsOfRating[i] = 0;
	}
}

void CCarCtrl::ReInit(void)
{
	NumRandomCars = 0;
	NumLawEnforcerCars = 0;
	NumMissionCars = 0;
	NumParkedCars = 0;
	NumPermanentCars = 0;
	NumAmbulancesOnDuty = 0;
	NumFiretrucksOnDuty = 0;
#ifdef FIX_BUGS
	LastTimeFireTruckCreated = 0;
	LastTimeAmbulanceCreated = 0;
	LastTimeLawEnforcerCreated = 0;
	LastTimeMiamiViceGenerated = 0;
#endif
	CountDownToCarsAtStart = 2;
	CarDensityMultiplier = 1.0f;
	/* The burst evidence restarts with the world (VICE-146). */
	ms_nFillBurstTotalAttempts = 0;
	ms_nFillBurstTotalPlaced = 0;
	ms_nFillBurstEndedLine = 0;
	ms_nFillBurstEndedBudget = 0;
	ms_nFillBurstEndedStalls = 0;
	ms_nFillBurstEndedWindow = 0;
	/* The service's counters restart with the world (VICE-150). */
	CSpawnService::ReInit();
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++)
		apCarsToKeep[i] = nil;
	for (int i = 0; i < TOTAL_CUSTOM_CLASSES; i++)
		NumRequestsOfCarRating[i] = 0;
}

void CCarCtrl::DragCarToPoint(CVehicle* pVehicle, CVector* pPoint)
{
	CVector2D posBehind = (CVector2D)pVehicle->GetPosition() - 3 * pVehicle->GetForward() / 2;
	CVector2D posTarget = *pPoint;
	CVector2D direction = posBehind - posTarget;
	CVector2D midPos = posTarget + direction * 3 / direction.Magnitude();
	float actualAheadZ;
	float actualBehindZ;
	CColPoint point;
	CEntity* pRoadObject;
	if (CCollision::IsStoredPolyStillValidVerticalLine(CVector(posTarget.x, posTarget.y, pVehicle->GetPosition().z - 3.0f),
	  pVehicle->GetPosition().z - 3.0f, point, &pVehicle->m_aCollPolys[0])){
		actualAheadZ = point.point.z;
	}else if (CWorld::ProcessVerticalLine(CVector(posTarget.x, posTarget.y, pVehicle->GetPosition().z + 1.5f),
	  pVehicle->GetPosition().z - 2.0f, point,
	  pRoadObject, true, false, false, false, false, false, &pVehicle->m_aCollPolys[0])){
		actualAheadZ = point.point.z;
		pVehicle->m_pCurGroundEntity = pRoadObject;
		if (ThisRoadObjectCouldMove(pRoadObject->GetModelIndex()))
			pVehicle->m_aCollPolys[0].valid = false;
	}else if (CWorld::ProcessVerticalLine(CVector(posTarget.x, posTarget.y, pVehicle->GetPosition().z + 3.0f),
	  pVehicle->GetPosition().z - 3.0f, point,
	  pRoadObject, true, false, false, false, false, false, &pVehicle->m_aCollPolys[0])) {
		actualAheadZ = point.point.z;
		pVehicle->m_pCurGroundEntity = pRoadObject;
		if (ThisRoadObjectCouldMove(pRoadObject->GetModelIndex()))
			pVehicle->m_aCollPolys[0].valid = false;
	}else{
		actualAheadZ = pVehicle->m_fMapObjectHeightAhead;
	}
	pVehicle->m_fMapObjectHeightAhead = actualAheadZ;
	if (CCollision::IsStoredPolyStillValidVerticalLine(CVector(midPos.x, midPos.y, pVehicle->GetPosition().z - 3.0f),
	  pVehicle->GetPosition().z - 3.0f, point, &pVehicle->m_aCollPolys[1])){
		actualBehindZ = point.point.z;
	}else if (CWorld::ProcessVerticalLine(CVector(midPos.x, midPos.y, pVehicle->GetPosition().z + 1.5f),
	  pVehicle->GetPosition().z - 2.0f, point,
	  pRoadObject, true, false, false, false, false, false, &pVehicle->m_aCollPolys[1])){
		actualBehindZ = point.point.z;
		pVehicle->m_pCurGroundEntity = pRoadObject;
		if (ThisRoadObjectCouldMove(pRoadObject->GetModelIndex()))
			pVehicle->m_aCollPolys[1].valid = false;
	}else if (CWorld::ProcessVerticalLine(CVector(midPos.x, midPos.y, pVehicle->GetPosition().z + 3.0f),
	  pVehicle->GetPosition().z - 3.0f, point,
	  pRoadObject, true, false, false, false, false, false, &pVehicle->m_aCollPolys[1])){
		actualBehindZ = point.point.z;
		pVehicle->m_pCurGroundEntity = pRoadObject;
		if (ThisRoadObjectCouldMove(pRoadObject->GetModelIndex()))
			pVehicle->m_aCollPolys[1].valid = false;
	}else{
		actualBehindZ = pVehicle->m_fMapObjectHeightBehind;
	}
	pVehicle->m_fMapObjectHeightBehind = actualBehindZ;
	float angleZ = Atan2((actualAheadZ - actualBehindZ) / 3, 1.0f);
	float cosZ = Cos(angleZ);
	float sinZ = Sin(angleZ);
	pVehicle->GetRight() = CVector(posTarget.y - midPos.y, -(posTarget.x - midPos.x), 0.0f) / 3;
	pVehicle->GetForward() = CVector(-cosZ * pVehicle->GetRight().y, cosZ * pVehicle->GetRight().x, sinZ);
	pVehicle->GetUp() = CrossProduct(pVehicle->GetRight(), pVehicle->GetForward());
	pVehicle->SetPosition((CVector(midPos.x, midPos.y, actualBehindZ) + CVector(posTarget.x, posTarget.y, actualAheadZ)) / 2);
	pVehicle->GetMatrix().GetPosition().z += pVehicle->GetHeightAboveRoad();
}

float CCarCtrl::FindSpeedMultiplier(float angleChange, float minAngle, float maxAngle, float coef)
{
	float angle = Abs(LimitRadianAngle(angleChange));
	float n = angle - minAngle;
	n = Max(0.0f, n);
	float d = maxAngle - minAngle;
	float mult = 1.0f - n / d * (1.0f - coef);
	if (n > d)
		return coef;
	return mult;
}

void CCarCtrl::SteerAICarWithPhysics(CVehicle* pVehicle)
{
	float swerve;
	float accel;
	float brake;
	bool handbrake;
	switch (pVehicle->AutoPilot.m_nTempAction){
	case TEMPACT_WAIT:
		swerve = 0.0f;
		accel = 0.0f;
		brake = 0.2f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction){
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
			pVehicle->AutoPilot.m_nAntiReverseTimer = CTimer::GetTimeInMilliseconds();
			pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds();
		}
		break;
	case TEMPACT_REVERSE:
		SteerAICarWithPhysics_OnlyMission(pVehicle, &swerve, &accel, &brake, &handbrake);
		handbrake = false;
		swerve = -swerve;
		if (DotProduct(pVehicle->GetMoveSpeed(), pVehicle->GetForward()) > 0.04f){
			accel = 0.0f;
			brake = 0.5f;
		}else{
			accel = -0.5f;
			brake = 0.0f;
		}
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_HANDBRAKETURNLEFT:
		swerve = 1.0f;
		accel = 0.0f;
		brake = 0.0f;
		handbrake = true;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_HANDBRAKETURNRIGHT:
		swerve = -1.0f;
		accel = 0.0f;
		brake = 0.0f;
		handbrake = true;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_HANDBRAKESTRAIGHT:
		swerve = 0.0f;
		accel = 0.0f;
		brake = 0.0f;
		handbrake = true;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_TURNLEFT:
		swerve = 1.0f;
		accel = 1.0f;
		brake = 0.0f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_TURNRIGHT:
		swerve = -1.0f;
		accel = 1.0f;
		brake = 0.0f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_GOFORWARD:
		swerve = 0.0f;
		accel = 0.5f;
		brake = 0.0f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_SWERVELEFT:
	case TEMPACT_SWERVERIGHT:
		swerve = (pVehicle->AutoPilot.m_nTempAction == TEMPACT_SWERVERIGHT) ? 0.15f : -0.15f;
		accel = 0.0f;
		brake = 0.001f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction - 1000)
			swerve = -swerve;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	default:
		SteerAICarWithPhysics_OnlyMission(pVehicle, &swerve, &accel, &brake, &handbrake);
		break;
	}
	pVehicle->m_fSteerAngle = swerve;
	pVehicle->bIsHandbrakeOn = handbrake;
	pVehicle->m_fGasPedal = accel;
	pVehicle->m_fBrakePedal = brake;
}

void CCarCtrl::SteerAICarWithPhysics_OnlyMission(CVehicle* pVehicle, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	switch (pVehicle->AutoPilot.m_nCarMission) {
	case MISSION_NONE:
		*pSwerve = 0.0f;
		*pAccel = 0.0f;
		*pBrake = 0.5f;
		*pHandbrake = true;
		return;
	case MISSION_CRUISE:
	case MISSION_RAMPLAYER_FARAWAY:
	case MISSION_BLOCKPLAYER_FARAWAY:
	case MISSION_GOTOCOORDS:
	case MISSION_GOTOCOORDS_ACCURATE:
	case MISSION_RAMCAR_FARAWAY:
	case MISSION_BLOCKCAR_FARAWAY:
		if (pVehicle->AutoPilot.m_bIgnorePathfinding) {
			*pSwerve = 0.0f;
			*pAccel = 1.0f;
			*pBrake = 0.0f;
			*pHandbrake = false;
		}else
			SteerAICarWithPhysicsFollowPath(pVehicle, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_RAMPLAYER_CLOSE:
	{
		CVector2D targetPos = FindPlayerCoors();
		if (FindPlayerVehicle()){
			if (pVehicle->m_randomSeed & 1 && DotProduct(FindPlayerVehicle()->GetForward(), pVehicle->GetForward()) > 0.5f){
				float targetWidth = FindPlayerVehicle()->GetColModel()->boundingBox.max.x;
				float ownWidth = pVehicle->GetColModel()->boundingBox.max.x;
				if (pVehicle->m_randomSeed & 2){
					targetPos += (targetWidth + ownWidth - 0.2f) * FindPlayerVehicle()->GetRight();
				}else{
					targetPos -= (targetWidth + ownWidth - 0.2f) * FindPlayerVehicle()->GetRight();
				}
				float targetSpeed = FindPlayerVehicle()->GetMoveSpeed().Magnitude();
				float distanceToTarget = ((CVector2D)pVehicle->GetPosition() - targetPos).Magnitude();
				if (12.0f * targetSpeed + 2.0f > distanceToTarget && pVehicle->AutoPilot.m_nTempAction == TEMPACT_NONE){
					pVehicle->AutoPilot.m_nTempAction = (pVehicle->m_randomSeed & 2) ? TEMPACT_TURNLEFT : TEMPACT_TURNRIGHT;
					pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 250;
				}
			}else{
				targetPos += FindPlayerVehicle()->GetRight() / 160 * ((pVehicle->m_randomSeed & 0xFF) - 128);
			}
		}
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, FindPlayerVehicle(), targetPos.x, targetPos.y, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	}
	case MISSION_BLOCKPLAYER_CLOSE:
		SteerAICarWithPhysicsTryingToBlockTarget(pVehicle, FindPlayerCoors().x, FindPlayerCoors().y,
			FindPlayerSpeed().x, FindPlayerSpeed().y, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_BLOCKPLAYER_HANDBRAKESTOP:
		SteerAICarWithPhysicsTryingToBlockTarget_Stop(pVehicle, FindPlayerCoors().x, FindPlayerCoors().y,
			FindPlayerSpeed().x, FindPlayerSpeed().y, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_WAITFORDELETION:
	case MISSION_HELI_LAND:
		return;
	case MISSION_GOTOCOORDS_STRAIGHT:
	case MISSION_GOTO_COORDS_STRAIGHT_ACCURATE:
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
			pVehicle->AutoPilot.m_vecDestinationCoors.x, pVehicle->AutoPilot.m_vecDestinationCoors.y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_EMERGENCYVEHICLE_STOP:
	case MISSION_STOP_FOREVER:
		*pSwerve = 0.0f;
		*pAccel = 0.0f;
		*pHandbrake = true;
		*pBrake = 0.5f;
		return;
	case MISSION_GOTOCOORDS_ASTHECROWSWIMS:
		SteerAIBoatWithPhysicsHeadingForTarget(pVehicle,
			pVehicle->AutoPilot.m_vecDestinationCoors.x, pVehicle->AutoPilot.m_vecDestinationCoors.y,
			pSwerve, pAccel, pBrake);
		*pHandbrake = false;
		return;
	case MISSION_RAMCAR_CLOSE:
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, pVehicle->AutoPilot.m_pTargetCar,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().x, pVehicle->AutoPilot.m_pTargetCar->GetPosition().y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_BLOCKCAR_CLOSE:
		SteerAICarWithPhysicsTryingToBlockTarget(pVehicle,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().x,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().y,
			pVehicle->AutoPilot.m_pTargetCar->GetMoveSpeed().x,
			pVehicle->AutoPilot.m_pTargetCar->GetMoveSpeed().y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_BLOCKCAR_HANDBRAKESTOP:
		SteerAICarWithPhysicsTryingToBlockTarget_Stop(pVehicle,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().x,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().y,
			pVehicle->AutoPilot.m_pTargetCar->GetMoveSpeed().x,
			pVehicle->AutoPilot.m_pTargetCar->GetMoveSpeed().y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_HELI_FLYTOCOORS:
		SteerAIHeliTowardsTargetCoors((CAutomobile*)pVehicle);
		return;
	case MISSION_ATTACKPLAYER:
		SteerAIBoatWithPhysicsAttackingPlayer(pVehicle, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_PLANE_FLYTOCOORS:
		SteerAIPlaneTowardsTargetCoors((CAutomobile*)pVehicle);
		return;
	case MISSION_SLOWLY_DRIVE_TOWARDS_PLAYER_1:
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
			pVehicle->AutoPilot.m_vecDestinationCoors.x, pVehicle->AutoPilot.m_vecDestinationCoors.y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_SLOWLY_DRIVE_TOWARDS_PLAYER_2:
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil, FindPlayerCoors().x, FindPlayerCoors().y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_BLOCKPLAYER_FORWARDANDBACK:
		SteerAICarBlockingPlayerForwardAndBack(pVehicle, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	default:
		assert(0);
		return;
	}
}

void CCarCtrl::SteerAICarBlockingPlayerForwardAndBack(CVehicle* pVehicle, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	*pSwerve = 0.0f;
	*pHandbrake = false;
	CVector player = FindPlayerSpeed() + 0.1f * FindPlayerEntity()->GetForward();
	player.z = 0.0f;
	CVector right(pVehicle->GetRight().x, pVehicle->GetRight().y, 0.0f);
	right.Normalise();
	CVector forward(pVehicle->GetForward().x, pVehicle->GetForward().y, 0.0f);
	forward.Normalise();
	float dpPlayerAndRight = DotProduct(player, right);
	if (dpPlayerAndRight == 0.0f)
		dpPlayerAndRight = 0.01f;
	float dpDiffAndRight = -DotProduct((FindPlayerCoors() - pVehicle->GetPosition()), right) / dpPlayerAndRight;
	if (dpDiffAndRight < 0.0f) {
		*pAccel = 0.0f;
		*pBrake = 0.0f;
		return;
	}
	float dpSpeedAndForward = DotProduct(pVehicle->GetMoveSpeed(), forward);
	float dpPlayerAndForward = DotProduct(player, forward);
	float dpDiffAndForward = DotProduct((FindPlayerCoors() - pVehicle->GetPosition()), forward);
	float multiplier = dpPlayerAndForward * dpDiffAndRight + dpDiffAndForward - dpSpeedAndForward * dpDiffAndRight;
	if (multiplier > 0) {
		*pAccel = Min(1.0f, 0.1f * multiplier);
		*pBrake = 0.0f;
	}
	else if (dpSpeedAndForward > 0) {
		*pAccel = 0.0f;
		*pBrake = Min(1.0f, -0.1f * multiplier);
		if (*pBrake > 0.95f)
			*pHandbrake = true;
	}
	else {
		*pAccel = Max(-1.0f, 0.1f * multiplier);
		*pBrake = 0.0f;
	}
}

void CCarCtrl::SteerAIBoatWithPhysicsHeadingForTarget(CVehicle* pVehicle, float targetX, float targetY, float* pSwerve, float* pAccel, float* pBrake)
{
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
	float angleToTarget = CGeneral::GetATanOfXY(targetX - pVehicle->GetPosition().x, targetY - pVehicle->GetPosition().y);
	float angleForward = CGeneral::GetATanOfXY(forward.x, forward.y);
	float steerAngle = LimitRadianAngle(angleToTarget - angleForward);
	steerAngle = Clamp(steerAngle, -DEFAULT_MAX_STEER_ANGLE, DEFAULT_MAX_STEER_ANGLE);
#ifdef FIX_BUGS
	float speedTarget = pVehicle->AutoPilot.GetCruiseSpeed();
#else
	float speedTarget = pVehicle->AutoPilot.m_nCruiseSpeed;
#endif
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float speedDiff = speedTarget - currentSpeed;
	if (speedDiff <= 0.0f) {
		speedDiff < -5.0f ? *pAccel = -0.2f : *pAccel = -0.1f;
		steerAngle *= -1;
	}
	else if (speedDiff / currentSpeed > 0.25f) {
		*pAccel = 1.0f;
	}
	else {
		*pAccel = 1.0f - (0.25f - speedDiff / currentSpeed) * 4.0f;
	}
	*pBrake = 0.0f;
	*pSwerve = steerAngle;
}

void CCarCtrl::SteerAIBoatWithPhysicsAttackingPlayer(CVehicle* pVehicle, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	float distanceToPlayer = (FindPlayerCoors() - pVehicle->GetPosition()).Magnitude();
	float projection = Min(distanceToPlayer / 20.0f, 2.0f);
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
	CVector2D vecToProjection = FindPlayerCoors() + FindPlayerSpeed() * projection * GAME_SPEED_TO_CARAI_SPEED;
	float angleToTarget = CGeneral::GetATanOfXY(vecToProjection.x - pVehicle->GetPosition().x, vecToProjection.y - pVehicle->GetPosition().y);
	float angleForward = CGeneral::GetATanOfXY(forward.x, forward.y);
	float steerAngle = LimitRadianAngle(angleToTarget - angleForward);
#ifdef FIX_BUGS
	float speedTarget = pVehicle->AutoPilot.GetCruiseSpeed();
#else
	float speedTarget = pVehicle->AutoPilot.m_nCruiseSpeed;
#endif
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float speedDiff = speedTarget - currentSpeed;
	if (speedDiff <= 0.0f) {
		speedDiff < -5.0f ? *pAccel = -0.2f : *pAccel = -0.1f;
	}
	else if (speedDiff / currentSpeed > 0.25f) {
		*pAccel = 1.0f;
	}
	else {
		*pAccel = 1.0f - (0.25f - speedDiff / currentSpeed) * 4.0f;
	}
	*pBrake = 0.0f;
	*pSwerve = steerAngle;
	*pHandbrake = false;
	if (pVehicle->GetModelIndex() == MI_PREDATOR && distanceToPlayer < 40.0f && steerAngle < 0.15f)
		pVehicle->FireFixedMachineGuns();
}

float CCarCtrl::FindMaxSteerAngle(CVehicle* pVehicle)
{
	return pVehicle->GetModelIndex() == MI_ENFORCER ? 0.7f : DEFAULT_MAX_STEER_ANGLE;
}

void CCarCtrl::SteerAIHeliTowardsTargetCoors(CAutomobile* pHeli)
{
	if (pHeli->m_aWheelSpeed[1] < 0.22f)
		pHeli->m_aWheelSpeed[1] += 0.001f;
	if (pHeli->m_aWheelSpeed[1] < 0.15f)
		return;
	CVector2D vecToTarget = pHeli->AutoPilot.m_vecDestinationCoors - pHeli->GetPosition();
	float distanceToTarget = vecToTarget.Magnitude();
#ifdef FIX_BUGS
	float speed = pHeli->AutoPilot.GetCruiseSpeed() * 0.01f;
#else
	float speed = pHeli->AutoPilot.m_nCruiseSpeed * 0.01f;
#endif
	if (distanceToTarget <= 100.0f)
	{
		if (distanceToTarget > 75.0f)
			speed *= 0.7f;
		else if (distanceToTarget > 10.0f)
			speed *= 0.4f;
		else
			speed *= 0.2f;
	}
	vecToTarget.Normalise();
	CVector2D vecAdvanceThisFrame(vecToTarget * speed);
	float resistance = Pow(0.997f, CTimer::GetTimeStep());
	pHeli->m_vecMoveSpeed.x *= resistance;
	pHeli->m_vecMoveSpeed.y *= resistance;
	CVector2D vecSpeedDirection = vecAdvanceThisFrame - pHeli->m_vecMoveSpeed;
	float vecSpeedChangeLength = vecSpeedDirection.Magnitude();
	vecSpeedDirection.Normalise();
	float changeMultiplier = 0.002f * CTimer::GetTimeStep();
	if (distanceToTarget < 5.0f)
		changeMultiplier /= 5.0f;
	if (vecSpeedChangeLength < changeMultiplier)
		pHeli->SetMoveSpeed(vecAdvanceThisFrame.x, vecAdvanceThisFrame.y, pHeli->GetMoveSpeed().z);
	else
		pHeli->AddToMoveSpeed(vecSpeedDirection * changeMultiplier);
	pHeli->GetMatrix().Translate(CTimer::GetTimeStep() * pHeli->GetMoveSpeed().x, CTimer::GetTimeStep() * pHeli->GetMoveSpeed().y, 0.0f);
	float ZTarget = pHeli->AutoPilot.m_vecDestinationCoors.z;
	if (CTimer::GetTimeInMilliseconds() & 0x800) // switch every ~2 seconds
		ZTarget += 2.0f;
	float ZSpeedTarget = (ZTarget - pHeli->GetPosition().z) * 0.01f;
	float ZSpeedChangeTarget = ZSpeedTarget - pHeli->GetMoveSpeed().z;
	float ZSpeedChangeMax = 0.001f * CTimer::GetTimeStep();
	if (!pHeli->bHeliDestroyed) {
		if (Abs(ZSpeedChangeTarget) < ZSpeedChangeMax)
			pHeli->SetMoveSpeed(pHeli->GetMoveSpeed().x, pHeli->GetMoveSpeed().y, ZSpeedTarget);
		else if (ZSpeedChangeTarget < 0.0f)
			pHeli->AddToMoveSpeed(0.0f, 0.0f, -ZSpeedChangeMax);
		else
			pHeli->AddToMoveSpeed(0.0f, 0.0f, 1.5f * ZSpeedChangeMax);
	}
	pHeli->GetMatrix().Translate(0.0f, 0.0f, CTimer::GetTimeStep() * pHeli->GetMoveSpeed().z);
	/* The script heli moves its matrix directly; the physics step does not
	 * carry it. The occupancy store follows both (VICE-149). */
	CSpawnOccupancy::EntityMoved(pHeli);
	pHeli->m_vecTurnSpeed.z *= Pow(0.99f, CTimer::GetTimeStep());
	float ZTurnSpeedTarget;
	if (distanceToTarget < 8.0f && pHeli->m_fHeliOrientation < 0.0f)
		ZTurnSpeedTarget = 0.0f;
	else {
		float fAngleTarget = CGeneral::GetATanOfXY(vecToTarget.x, vecToTarget.y) + PI;
		if (pHeli->m_fHeliOrientation >= 0.0f)
			fAngleTarget = pHeli->m_fHeliOrientation;
		fAngleTarget -= pHeli->m_fOrientation;
		while (fAngleTarget < -PI)
			fAngleTarget += TWOPI;
		while (fAngleTarget > PI)
			fAngleTarget -= TWOPI;
		if (Abs(fAngleTarget) <= 0.4f)
			ZTurnSpeedTarget = 0.0f;
		else if (fAngleTarget < 0.0f)
			ZTurnSpeedTarget = -0.03f;
		else
			ZTurnSpeedTarget = 0.03f;
	}
	float ZTurnSpeedChangeTarget = ZTurnSpeedTarget - pHeli->GetTurnSpeed().z;
	float ZTurnSpeedLimit = 0.0002f * CTimer::GetTimeStep();
	if (Abs(ZTurnSpeedChangeTarget) < ZTurnSpeedLimit)
		pHeli->m_vecTurnSpeed.z = ZTurnSpeedTarget;
	else if (ZTurnSpeedChangeTarget < 0.0f)
		pHeli->m_vecTurnSpeed.z -= ZTurnSpeedLimit;
	else
		pHeli->m_vecTurnSpeed.z += ZTurnSpeedLimit;
	pHeli->m_fOrientation += pHeli->GetTurnSpeed().z * CTimer::GetTimeStep();
	CVector up;
	if (pHeli->bHeliMinimumTilt)
		up = CVector(0.5f * pHeli->GetMoveSpeed().x, 0.5f * pHeli->GetMoveSpeed().y, 1.0f);
	else
		up = CVector(3.0f * pHeli->GetMoveSpeed().x, 3.0f * pHeli->GetMoveSpeed().y, 1.0f);
	up.Normalise();
	CVector forward(Cos(pHeli->m_fOrientation), Sin(pHeli->m_fOrientation), 0.0f);
	CVector right = CrossProduct(up, forward);
	forward = CrossProduct(up, right);
	pHeli->GetMatrix().GetRight() = right;
	pHeli->GetMatrix().GetForward() = forward;
	pHeli->GetMatrix().GetUp() = up;
}

void CCarCtrl::SteerAIPlaneTowardsTargetCoors(CAutomobile* pPlane)
{
	CVector2D vecToTarget = pPlane->AutoPilot.m_vecDestinationCoors - pPlane->GetPosition();
	float fForwardZ = (pPlane->AutoPilot.m_vecDestinationCoors.z - pPlane->GetPosition().z) / vecToTarget.Magnitude();
	fForwardZ = Clamp(fForwardZ, -0.3f, 0.3f);
	float angle = CGeneral::GetATanOfXY(vecToTarget.x, vecToTarget.y);
	while (angle > TWOPI)
		angle -= TWOPI;
	float difference = LimitRadianAngle(angle - pPlane->m_fOrientation);
	float steer = difference > 0.0f ? 0.04f : -0.04f;
	if (Abs(difference) < 0.2f)
		steer *= 5.0f * Abs(difference);
	pPlane->m_fPlaneSteer *= Pow(0.96f, CTimer::GetTimeStep());
	float steerChange = steer - pPlane->m_fPlaneSteer;
	float maxChange = 0.003f * CTimer::GetTimeStep();
	if (Abs(steerChange) < maxChange)
		pPlane->m_fPlaneSteer = steer;
	else if (steerChange < 0.0f)
		pPlane->m_fPlaneSteer -= maxChange;
	else
		pPlane->m_fPlaneSteer += maxChange;
	pPlane->m_fOrientation += pPlane->m_fPlaneSteer * CTimer::GetTimeStep();
	CVector up(0.0f, 0.0f, 1.0f);
	up.Normalise();
	CVector forward(Cos(pPlane->m_fOrientation), Sin(pPlane->m_fOrientation), fForwardZ);
	forward.Normalise();
	CVector right = CrossProduct(up, forward);
	right.z -= 5.0f * pPlane->m_fPlaneSteer;
	right.Normalise();
	up = CrossProduct(forward, right);
	up.Normalise();
	right = CrossProduct(forward, up);
	pPlane->GetMatrix().GetRight() = right;
	pPlane->GetMatrix().GetForward() = forward;
	pPlane->GetMatrix().GetUp() = up;
	float newSplit = 1.0f - Pow(0.95f, CTimer::GetTimeStep());
	float oldSplit = 1.0f - newSplit;
#ifdef FIX_BUGS
	pPlane->m_vecMoveSpeed = pPlane->m_vecMoveSpeed * oldSplit + pPlane->AutoPilot.GetCruiseSpeed() * 0.01f * forward * newSplit;
#else
	pPlane->m_vecMoveSpeed = pPlane->m_vecMoveSpeed * oldSplit + pPlane->AutoPilot.m_nCruiseSpeed * 0.01f * forward * newSplit;
#endif
	pPlane->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
}

void CCarCtrl::SteerAICarWithPhysicsFollowPath(CVehicle* pVehicle, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
	CCarPathLink* pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink* pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentPathLinkForward(pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection,
		pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection);
	float nextPathLinkForwardX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float nextPathLinkForwardY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	CVector2D positionOnCurrentLinkIncludingLane(
		pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.y,
		pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.x);
	CVector2D positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX);
	CVector2D distanceToNextNode = (CVector2D)pVehicle->GetPosition() - positionOnCurrentLinkIncludingLane;
	float scalarDistanceToNextNode = distanceToNextNode.Magnitude();
	CVector2D distanceBetweenNodes = positionOnNextLinkIncludingLane - positionOnCurrentLinkIncludingLane;
	float dp = DotProduct2D(distanceBetweenNodes, distanceToNextNode);
	if (scalarDistanceToNextNode < DISTANCE_TO_NEXT_NODE_TO_SELECT_NEW ||
	  dp > 0.0f && scalarDistanceToNextNode < DISTANCE_TO_FACING_NEXT_NODE_TO_SELECT_NEW ||
	  dp / (scalarDistanceToNextNode * distanceBetweenNodes.Magnitude()) > 0.7f ||
	  pVehicle->AutoPilot.m_nNextPathNodeInfo == pVehicle->AutoPilot.m_nCurrentPathNodeInfo){
		if (PickNextNodeAccordingStrategy(pVehicle)) {
			switch (pVehicle->AutoPilot.m_nCarMission){
			case MISSION_GOTOCOORDS:
				pVehicle->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS_STRAIGHT;
				SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil, pVehicle->AutoPilot.m_vecDestinationCoors.x,
					pVehicle->AutoPilot.m_vecDestinationCoors.y, pSwerve, pAccel, pBrake, pHandbrake);
				return;
			case MISSION_GOTOCOORDS_ACCURATE:
				pVehicle->AutoPilot.m_nCarMission = MISSION_GOTO_COORDS_STRAIGHT_ACCURATE;
				SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil, pVehicle->AutoPilot.m_vecDestinationCoors.x,
					pVehicle->AutoPilot.m_vecDestinationCoors.y, pSwerve, pAccel, pBrake, pHandbrake);
				return;
			default: break;
			}
		}
		pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
		scalarDistanceToNextNode = CVector2D(
			pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.y - pVehicle->GetPosition().x,
			pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.x - pVehicle->GetPosition().y).Magnitude();		
		pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
		currentPathLinkForward.x = pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
		currentPathLinkForward.y = pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
		nextPathLinkForwardX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
		nextPathLinkForwardY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	}
	positionOnCurrentLinkIncludingLane.x = pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.y;
	positionOnCurrentLinkIncludingLane.y = pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.x;
	CVector2D projectedPosition = positionOnCurrentLinkIncludingLane - currentPathLinkForward * scalarDistanceToNextNode * 0.4f;
	if (scalarDistanceToNextNode > DISTANCE_TO_NEXT_NODE_TO_CONSIDER_SLOWING_DOWN){
		projectedPosition.x = positionOnCurrentLinkIncludingLane.x;
		projectedPosition.y = positionOnCurrentLinkIncludingLane.y;
	}
	CVector2D distanceToProjectedPosition = projectedPosition - pVehicle->GetPosition();
	float angleCurrentLink = CGeneral::GetATanOfXY(distanceToProjectedPosition.x, distanceToProjectedPosition.y);
	float angleForward = CGeneral::GetATanOfXY(forward.x, forward.y);
	if (pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_AVOID_CARS)
		angleCurrentLink = FindAngleToWeaveThroughTraffic(pVehicle, nil, angleCurrentLink, angleForward);
	float steerAngle = LimitRadianAngle(angleCurrentLink - angleForward);
	float maxAngle = FindMaxSteerAngle(pVehicle);
	steerAngle = Min(maxAngle, Max(-maxAngle, steerAngle));
	if (pVehicle->GetMoveSpeed().Magnitude() > MIN_SPEED_TO_START_LIMITING_STEER)
		steerAngle = Min(MAX_ANGLE_TO_STEER_AT_HIGH_SPEED, Max(-MAX_ANGLE_TO_STEER_AT_HIGH_SPEED, steerAngle));
	float currentForwardSpeed = DotProduct(pVehicle->GetMoveSpeed(), pVehicle->GetForward()) * GAME_SPEED_TO_CARAI_SPEED;
	float speedStyleMultiplier;
	switch (pVehicle->AutoPilot.m_nDrivingStyle) {
	case DRIVINGSTYLE_STOP_FOR_CARS:
	case DRIVINGSTYLE_SLOW_DOWN_FOR_CARS:
	case DRIVINGSTYLE_STOP_FOR_CARS_IGNORE_LIGHTS:
		speedStyleMultiplier = FindMaximumSpeedForThisCarInTraffic(pVehicle);
#ifdef FIX_BUGS
		if (pVehicle->AutoPilot.GetCruiseSpeed() != 0)
			speedStyleMultiplier /= pVehicle->AutoPilot.GetCruiseSpeed();
#else
		speedStyleMultiplier /= pVehicle->AutoPilot.m_nCruiseSpeed;
#endif
		break;
	default:
		speedStyleMultiplier = 1.0f;
		break;
	}
	switch (pVehicle->AutoPilot.m_nDrivingStyle) {
	case DRIVINGSTYLE_STOP_FOR_CARS:
	case DRIVINGSTYLE_SLOW_DOWN_FOR_CARS:
		if (CTrafficLights::ShouldCarStopForLight(pVehicle, false)){
			CCarAI::CarHasReasonToStop(pVehicle);
			speedStyleMultiplier = 0.0f;
		}
		break;
	default:
		break;
	}
	if (CTrafficLights::ShouldCarStopForBridge(pVehicle)){
		CCarAI::CarHasReasonToStop(pVehicle);
		speedStyleMultiplier = 0.0f;
	}
	CVector2D trajectory(pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.y,
		pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.x);
	trajectory -= pVehicle->GetPosition();
	float speedAngleMultiplier = FindSpeedMultiplier(
		CGeneral::GetATanOfXY(trajectory.x, trajectory.y) - angleForward,
		MIN_ANGLE_FOR_SPEED_LIMITING, MAX_ANGLE_FOR_SPEED_LIMITING, MIN_LOWERING_SPEED_COEFFICIENT);
	float tmpWideMultiplier = FindSpeedMultiplier(
		CGeneral::GetATanOfXY(currentPathLinkForward.x, currentPathLinkForward.y) -
		CGeneral::GetATanOfXY(nextPathLinkForwardX, nextPathLinkForwardY),
		MIN_ANGLE_FOR_SPEED_LIMITING_BETWEEN_NODES, MAX_ANGLE_FOR_SPEED_LIMITING, MIN_LOWERING_SPEED_COEFFICIENT);
	float speedNodesMultiplier;
	if (scalarDistanceToNextNode > DISTANCE_TO_NEXT_NODE_TO_CONSIDER_SLOWING_DOWN || pVehicle->AutoPilot.m_nCruiseSpeed < 12)
		speedNodesMultiplier = 1.0f;
	else
		speedNodesMultiplier = 1.0f -
			(1.0f - scalarDistanceToNextNode / DISTANCE_TO_NEXT_NODE_TO_CONSIDER_SLOWING_DOWN) *
			(1.0f - tmpWideMultiplier);
	float speedMultiplier = Min(speedStyleMultiplier, Min(speedAngleMultiplier, speedNodesMultiplier));
	float speed = pVehicle->AutoPilot.m_nCruiseSpeed * speedMultiplier;
	float speedDifference = speed - currentForwardSpeed;
	if (speed < 0.05f && speedDifference < 0.03f){
		*pBrake = 1.0f;
		*pAccel = 0.0f;
	}else if (speedDifference <= 0.0f){
		*pBrake = Min(0.5f, -speedDifference * 0.05f);
		*pAccel = 0.0f;
	}else if (currentForwardSpeed < 2.0f){
		*pBrake = 0.0f;
		*pAccel = Min(1.0f, speedDifference * 0.25f);
	}else{
		*pBrake = 0.0f;
		*pAccel = Min(1.0f, speedDifference * 0.125f);
	}
	*pSwerve = steerAngle;
	*pHandbrake = false;
}

void CCarCtrl::SteerAICarWithPhysicsHeadingForTarget(CVehicle* pVehicle, CPhysical* pTarget, float targetX, float targetY, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	*pHandbrake = false;
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
	float angleToTarget = CGeneral::GetATanOfXY(targetX - pVehicle->GetPosition().x, targetY - pVehicle->GetPosition().y);
	float angleForward = CGeneral::GetATanOfXY(forward.x, forward.y);
	if (pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_AVOID_CARS)
		angleToTarget = FindAngleToWeaveThroughTraffic(pVehicle, pTarget, angleToTarget, angleForward);
	float steerAngle = LimitRadianAngle(angleToTarget - angleForward);
	if (pVehicle->GetMoveSpeed().Magnitude() > MIN_SPEED_TO_APPLY_HANDBRAKE)
		if (ABS(steerAngle) > MIN_ANGLE_TO_APPLY_HANDBRAKE)
			*pHandbrake = true;
	float maxAngle = FindMaxSteerAngle(pVehicle);
	steerAngle = Min(maxAngle, Max(-maxAngle, steerAngle));
	float speedMultiplier = FindSpeedMultiplier(CGeneral::GetATanOfXY(targetX - pVehicle->GetPosition().x, targetY - pVehicle->GetPosition().y) - angleForward,
		MIN_ANGLE_FOR_SPEED_LIMITING, MAX_ANGLE_FOR_SPEED_LIMITING, MIN_LOWERING_SPEED_COEFFICIENT);
	float speedTarget = pVehicle->AutoPilot.m_nCruiseSpeed * speedMultiplier;
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float speedDiff = speedTarget - currentSpeed;
	if (speedDiff <= 0.0f){
		*pAccel = 0.0f;
		*pBrake = Min(0.5f, -speedDiff / 20.0f);
	}else if (currentSpeed < 25.0f){
		*pAccel = Min(1.0f, speedDiff * 0.1f);
		*pBrake = 0.0f;
	}else{
		*pAccel = 1.0f;
		*pBrake = 0.0f;
	}
	*pSwerve = steerAngle;
}

void CCarCtrl::SteerAICarWithPhysicsTryingToBlockTarget(CVehicle* pVehicle, float targetX, float targetY, float targetSpeedX, float targetSpeedY, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	CVector2D targetPos(targetX, targetY);
	CVector2D offset(targetSpeedX, targetSpeedY);
	float trajectoryLen = offset.Magnitude();
	if (trajectoryLen > MAX_SPEED_TO_ACCOUNT_IN_INTERCEPTING)
		offset *= MAX_SPEED_TO_ACCOUNT_IN_INTERCEPTING / trajectoryLen;
	targetPos += offset * GAME_SPEED_TO_CARAI_SPEED;
	pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
	SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil, targetPos.x, targetPos.y, pSwerve, pAccel, pBrake, pHandbrake);
	if ((targetPos - pVehicle->GetPosition()).MagnitudeSqr() < SQR(DISTANCE_TO_SWITCH_FROM_BLOCK_TO_STOP))
		pVehicle->AutoPilot.m_nCarMission = (pVehicle->AutoPilot.m_nCarMission == MISSION_BLOCKCAR_CLOSE) ?
			MISSION_BLOCKCAR_HANDBRAKESTOP : MISSION_BLOCKPLAYER_HANDBRAKESTOP;
}

void CCarCtrl::SteerAICarWithPhysicsTryingToBlockTarget_Stop(CVehicle* pVehicle, float targetX, float targetY, float targetSpeedX, float targetSpeedY, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	*pSwerve = 0.0f;
	*pAccel = 0.0f;
	*pBrake = 1.0f;
	*pHandbrake = true;
	float distanceToTargetSqr = (CVector2D(targetX, targetY) - pVehicle->GetPosition()).MagnitudeSqr();
	if (distanceToTargetSqr > SQR(DISTANCE_TO_SWITCH_FROM_STOP_TO_BLOCK)){
		pVehicle->AutoPilot.m_nCarMission = (pVehicle->AutoPilot.m_nCarMission == MISSION_BLOCKCAR_HANDBRAKESTOP) ?
			MISSION_BLOCKCAR_CLOSE : MISSION_BLOCKPLAYER_CLOSE;
		return;
	}
	if (pVehicle->AutoPilot.m_nCarMission == MISSION_BLOCKCAR_HANDBRAKESTOP){
		if (((CVector2D)pVehicle->GetMoveSpeed()).MagnitudeSqr() < SQR(0.01f) &&
		  CVector2D(targetSpeedX, targetSpeedY).MagnitudeSqr() < SQR(0.02f) &&
		  pVehicle->bIsLawEnforcer){
			CCarAI::TellOccupantsToLeaveCar(pVehicle);
			pVehicle->AutoPilot.m_nCruiseSpeed = 0;
			pVehicle->AutoPilot.m_nCarMission = MISSION_NONE;
		}
	}else{
		if (FindPlayerVehicle() && FindPlayerVehicle()->GetMoveSpeed().Magnitude() < 0.05f)
#ifdef FIX_BUGS
			pVehicle->m_nTimeBlocked += CTimer::GetTimeStepInMilliseconds();
#else
			pVehicle->m_nTimeBlocked += 1000.0f / 60.0f * CTimer::GetTimeStep(); // very doubtful constant
#endif
		else
			pVehicle->m_nTimeBlocked = 0;
		if (FindPlayerVehicle() == nil || FindPlayerVehicle()->IsUpsideDown() ||
		  FindPlayerVehicle()->GetMoveSpeed().Magnitude() < 0.05f &&
		  pVehicle->m_nTimeBlocked > TIME_COPS_WAIT_TO_EXIT_AFTER_STOPPING){
			if (pVehicle->bIsLawEnforcer && distanceToTargetSqr < SQR(DISTANCE_TO_SWITCH_FROM_STOP_TO_BLOCK)){
				CCarAI::TellOccupantsToLeaveCar(pVehicle);
				pVehicle->AutoPilot.m_nCruiseSpeed = 0;
				pVehicle->AutoPilot.m_nCarMission = MISSION_NONE;
			}
		}
	}
}

void
CCarCtrl::RegisterVehicleOfInterest(CVehicle* pVehicle)
{
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (apCarsToKeep[i] == pVehicle) {
			aCarsToKeepTime[i] = CTimer::GetTimeInMilliseconds();
			return;
		}
	}
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (!apCarsToKeep[i]) {
			apCarsToKeep[i] = pVehicle;
			aCarsToKeepTime[i] = CTimer::GetTimeInMilliseconds();
			return;
		}
	}
	uint32 oldestCarWeKeepTime = UINT32_MAX;
	int oldestCarWeKeepIndex = 0;
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (apCarsToKeep[i] && aCarsToKeepTime[i] < oldestCarWeKeepTime) {
			oldestCarWeKeepTime = aCarsToKeepTime[i];
			oldestCarWeKeepIndex = i;
		}
	}
	apCarsToKeep[oldestCarWeKeepIndex] = pVehicle;
	aCarsToKeepTime[oldestCarWeKeepIndex] = CTimer::GetTimeInMilliseconds();
}

bool
CCarCtrl::IsThisVehicleInteresting(CVehicle* pVehicle)
{
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (apCarsToKeep[i] == pVehicle)
			return true;
	}
	return false;
}

void CCarCtrl::RemoveFromInterestingVehicleList(CVehicle* pVehicle)
{
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (apCarsToKeep[i] == pVehicle)
			apCarsToKeep[i] = nil;
	}
}

void CCarCtrl::ClearInterestingVehicleList()
{
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		apCarsToKeep[i] = nil;
	}
}

void CCarCtrl::SwitchVehicleToRealPhysics(CVehicle* pVehicle)
{
	pVehicle->AutoPilot.m_nCarMission = MISSION_CRUISE;
	pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
	pVehicle->AutoPilot.m_nAntiReverseTimer = CTimer::GetTimeInMilliseconds();
	pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds();
}

void CCarCtrl::JoinCarWithRoadSystem(CVehicle* pVehicle)
{
	pVehicle->AutoPilot.m_nPrevRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_nNextRouteNode = 0;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = pVehicle->AutoPilot.m_nPreviousPathNodeInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo = 0;
	int nodeId = ThePaths.FindNodeClosestToCoorsFavourDirection(pVehicle->GetPosition(), 0, pVehicle->GetForward().x, pVehicle->GetForward().y);
	CPathNode* pNode = &ThePaths.m_pathNodes[nodeId];
	int prevNodeId = -1;
	float minDistance = 999999.9f;
	for (int i = 0; i < pNode->numLinks; i++){
		int candidateId = ThePaths.ConnectedNode(i + pNode->firstLink);
		CPathNode* pCandidateNode = &ThePaths.m_pathNodes[candidateId];
		float distance = (pCandidateNode->GetPosition() - pNode->GetPosition()).Magnitude2D();
		if (distance < minDistance){
			minDistance = distance;
			prevNodeId = candidateId;
		}
	}
	if (prevNodeId < 0)
		return;
	CVector2D forward = pVehicle->GetForward();
	CPathNode* pPrevNode = &ThePaths.m_pathNodes[prevNodeId];
	if (forward.x == 0.0f && forward.y == 0.0f)
		forward.x = 1.0f;
	if (DotProduct2D(pNode->GetPosition() - pPrevNode->GetPosition(), forward) < 0.0f){
		int tmp;
		tmp = prevNodeId;
		prevNodeId = nodeId;
		nodeId = tmp;
	}
	pVehicle->AutoPilot.m_nPrevRouteNode = 0;
	pVehicle->AutoPilot.m_nCurrentRouteNode = prevNodeId;
	pVehicle->AutoPilot.m_nNextRouteNode = nodeId;
	pVehicle->AutoPilot.m_nPathFindNodesCount = 0;
	FindLinksToGoWithTheseNodes(pVehicle);
	pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane = 0;
}

bool CCarCtrl::JoinCarWithRoadSystemGotoCoors(CVehicle* pVehicle, CVector vecTarget, bool isProperNow)
{
	pVehicle->AutoPilot.m_vecDestinationCoors = vecTarget;
	ThePaths.DoPathSearch(0, pVehicle->GetPosition(), -1, vecTarget, pVehicle->AutoPilot.m_aPathFindNodesInfo,
		&pVehicle->AutoPilot.m_nPathFindNodesCount, NUM_PATH_NODES_IN_AUTOPILOT, pVehicle, nil, 999999.9f, -1);
	ThePaths.RemoveBadStartNode(pVehicle->GetPosition(),
		pVehicle->AutoPilot.m_aPathFindNodesInfo, &pVehicle->AutoPilot.m_nPathFindNodesCount);
	if (pVehicle->AutoPilot.m_nPathFindNodesCount < 2){
		pVehicle->AutoPilot.m_nPrevRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_nNextRouteNode = 0;
		pVehicle->AutoPilot.m_nPathFindNodesCount = 0;
		return true;
	}
	pVehicle->AutoPilot.m_nPrevRouteNode = 0;
	pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes;
	pVehicle->AutoPilot.RemoveOnePathNode();
	pVehicle->AutoPilot.m_nNextRouteNode = pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes;
	pVehicle->AutoPilot.RemoveOnePathNode();
	FindLinksToGoWithTheseNodes(pVehicle);
	pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane = 0;
	return false;
}

void CCarCtrl::FindLinksToGoWithTheseNodes(CVehicle* pVehicle)
{
	if (pVehicle->m_nRouteSeed)
		CGeneral::SetRandomSeed(pVehicle->m_nRouteSeed);
	int nextLink;
	CPathNode* pCurNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nCurrentRouteNode];
	for (nextLink = 0; nextLink < 12; nextLink++)
		if (ThePaths.ConnectedNode(nextLink + pCurNode->firstLink) == pVehicle->AutoPilot.m_nNextRouteNode)
			break;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink];
	pVehicle->AutoPilot.m_nNextDirection = (pVehicle->AutoPilot.m_nCurrentRouteNode >= pVehicle->AutoPilot.m_nNextRouteNode) ? 1 : -1;
	int curLink;
	int curConnection;
	if (pCurNode->numLinks == 1) {
		curLink = 0;
		curConnection = ThePaths.m_carPathConnections[pCurNode->firstLink];
	}else{
		int closestLink = -1;
		float md = 999999.9f;

		for (curLink = 0; curLink < pCurNode->numLinks; curLink++) {
			int node = ThePaths.ConnectedNode(curLink + pCurNode->firstLink);
			CPathNode* pNode = &ThePaths.m_pathNodes[node];
			if (node == pVehicle->AutoPilot.m_nNextRouteNode)
				continue;
			CVector vCurPos = pCurNode->GetPosition();
			CVector vNextPos = pNode->GetPosition();
			float dist = CCollision::DistToLine(&vCurPos, &vNextPos, &pVehicle->GetPosition());
			if (dist < md) {
				md = dist;
				closestLink = curLink;
			}
		}
		curConnection = ThePaths.m_carPathConnections[closestLink + pCurNode->firstLink];
	}
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = curConnection;
	pVehicle->AutoPilot.m_nCurrentDirection = (ThePaths.ConnectedNode(curLink + pCurNode->firstLink) >= pVehicle->AutoPilot.m_nCurrentRouteNode) ? 1 : -1;
}

/*
 * One responder call (VICE-156): the policy decides (the accident/fire
 * triggers, the cooldowns, the duty and model caps), the streaming asks for
 * the models, and the service places the car on the ring around the incident
 * with the destination mode. The cooldown is stamped only on a created car,
 * the old path's LastTime* update: a refusal or a streaming wait retries on
 * the next trigger. The crew model is released below the trigger, the old
 * rule that let the emergency peds go when no dispatch was wanted.
 */
static bool
GenerateResponder(ESpawnResponderRole role, uint32 model, uint32 crewModel, uint32 *pLastCreated,
	int32 activeIncidents, bool nearestFound, CVector nearestPos, float nearestDistance)
{
	CPlayerInfo *pPlayer = &CWorld::Players[CWorld::PlayerInFocus];
	const STrafficRings rings = CurrentTrafficRings();
	SResponderFacts facts;
	SResponderDecision decision;
	SSpawnCarRequest request;
	CVector playerCentre;
	CVehicle *pCreated = nil;

	if (pPlayer->m_pPed == nil || pPlayer->m_pPed->m_pWanted == nil)
		return false;
	playerCentre = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	facts.wantedLevel = pPlayer->m_pPed->m_pWanted->GetWantedLevel();
	facts.interior = CGame::IsInInterior();
	facts.carsInUse = CCarCtrl::NumCarsInUse();
	facts.carBudget = CCarCtrl::MaxNumberOfCarsInUse;
	facts.activeIncidents = activeIncidents;
	facts.nearestFound = nearestFound;
	facts.nearestDistance = nearestDistance;
	facts.nearestX = nearestPos.x;
	facts.nearestY = nearestPos.y;
	facts.nearestZ = nearestPos.z;
	facts.onDuty = role == SPAWN_RESPONDER_AMBULANCE ?
		CCarCtrl::NumAmbulancesOnDuty : CCarCtrl::NumFiretrucksOnDuty;
	facts.carsOfType = CCarCtrl::CountCarsOfType(model);
	facts.nowMs = CTimer::GetTimeInMilliseconds();
	facts.lastCreatedMs = *pLastCreated;

	ResponderEvaluate(role, facts, &decision);
	if (decision.gate == RESPONDER_GATE_BELOW_TRIGGER) {
		/* The old rule: while the incident count is below the trigger, the
		 * crew model is not pinned (the emergency peds may go). The old
		 * guard read the vehicle model's residency. */
		if (CStreaming::HasModelLoaded(model))
			CStreaming::SetModelIsDeletable(crewModel);
		return false;
	}
	if (!decision.dispatch)
		return false;

	CStreaming::RequestModel(model, STREAMFLAGS_DEPENDENCY);
	CStreaming::RequestModel(crewModel, STREAMFLAGS_DONT_REMOVE);
	if (!CStreaming::HasModelLoaded(model) || !CStreaming::HasModelLoaded(crewModel))
		return false;

	ResponderFillRequest(decision, model, CCarCtrl::NORMAL,
		playerCentre.x, playerCentre.y, playerCentre.z, rings.carOnScreenSpawnMax, &request);
	if (!CSpawnService::RequestCar(request, &pCreated))
		return false;
	*pLastCreated = facts.nowMs;
	return true;
}

void CCarCtrl::GenerateEmergencyServicesCar(void)
{
	/* The ambulance: the old trigger is two active accidents, the nearest
	 * within 30 m is the destination. The policy's own order keeps the
	 * wanted/interior/budget gates. */
	{
		int32 active = gAccidentManager.CountActiveAccidents();
		bool found = false;
		CVector nearestPos(0.0f, 0.0f, 0.0f);
		float distance = RESPONDER_NEAREST_RANGE;

		if (NumAmbulancesOnDuty == 0 && active >= RESPONDER_ACCIDENT_TRIGGER) {
			CAccident *pAccident = gAccidentManager.FindNearestAccident(FindPlayerCoors(), &distance);
			if (pAccident) {
				found = true;
				nearestPos = pAccident->m_pVictim->GetPosition();
			}
		}
		GenerateResponder(SPAWN_RESPONDER_AMBULANCE, MI_AMBULAN, MI_MEDIC,
			&LastTimeAmbulanceCreated, active, found, nearestPos, distance);
	}
	/* The fire truck: the old trigger is three active fires, the nearest
	 * within 30 m is the destination. */
	{
		int32 active = (int32)gFireManager.GetTotalActiveFires();
		bool found = false;
		CVector nearestPos(0.0f, 0.0f, 0.0f);
		float distance = RESPONDER_NEAREST_RANGE;

		if (NumFiretrucksOnDuty == 0 && active >= RESPONDER_FIRE_TRIGGER) {
			CFire *pFire = gFireManager.FindNearestFire(FindPlayerCoors(), &distance);
			if (pFire) {
				found = true;
				nearestPos = pFire->m_vecPos;
			}
		}
		GenerateResponder(SPAWN_RESPONDER_FIRETRUCK, MI_FIRETRUCK, MI_FIREMAN,
			&LastTimeFireTruckCreated, active, found, nearestPos, distance);
	}
}

void CCarCtrl::UpdateCarCount(CVehicle* pVehicle, bool remove)
{
	/* The far layer's cars are outside every budget (VICE-128): they hold pool
	 * slots, not car-target slots. */
	if (pVehicle->VehicleCreatedBy == FAR_TRAFFIC_VEHICLE)
		return;
	/* The law flag is counted independently of the class (VICE-166).
	 * ChangeLawEnforcerState toggles the flag on a car of every class, and a
	 * car restored from a save carries the flag of a car of any class
	 * (CPools::LoadVehiclePool, Pools.cpp): a class-bound count misses the
	 * addition of such a car and leaks its deletion. Only a car that carries
	 * the flag is counted. */
	if (remove){
		if (pVehicle->bIsLawEnforcer) {
			if (--NumLawEnforcerCars < 0)
				NumLawEnforcerCars = 0;
		}
		switch (pVehicle->VehicleCreatedBy){
		case RANDOM_VEHICLE:
			if (--NumRandomCars < 0)
				NumRandomCars = 0;
			return;
		case MISSION_VEHICLE:
			if (--NumMissionCars < 0)
				NumMissionCars = 0;
			return;
		case PARKED_VEHICLE:
			if (--NumParkedCars < 0)
				NumParkedCars = 0;
			return;
		case PERMANENT_VEHICLE:
			if (--NumPermanentCars < 0)
				NumPermanentCars = 0;
			return;
		}
	}
	else{
		if (pVehicle->bIsLawEnforcer)
			++NumLawEnforcerCars;
		switch (pVehicle->VehicleCreatedBy){
		case RANDOM_VEHICLE:
			++NumRandomCars;
			return;
		case MISSION_VEHICLE:
			++NumMissionCars;
			return;
		case PARKED_VEHICLE:
			++NumParkedCars;
			return;
		case PERMANENT_VEHICLE:
			++NumPermanentCars;
			return;
		}
	}
}

bool CCarCtrl::ThisRoadObjectCouldMove(int16 mi)
{
#ifdef GTA_BRIDGE
	return mi == MI_BRIDGELIFT || mi == MI_BRIDGEROADSEGMENT;
#else
	return false;
#endif
}

bool CCarCtrl::MapCouldMoveInThisArea(float x, float y)
{
#ifdef GTA_BRIDGE	// actually they forgot that in VC...
	// bridge moves up and down
	return x > -342.0f && x < -219.0f &&
		y > -677.0f && y < -580.0f;
#else
	return false;
#endif
}

float CCarCtrl::FindSpeedMultiplierWithSpeedFromNodes(int8 type)
{
	switch (type)
	{
	case 1: return 1.5f;
	case 2: return 2.0f;
	}
	return 1.0f;
}
