#ifndef __GTA_RESPONDERPOLICY_H__
#define __GTA_RESPONDERPOLICY_H__

/*
 * The responder and police policy (VICE-156).
 *
 * The spawn service owns the placement; this header owns the decision that a
 * placement should happen at all. The two requesters keep their old policy,
 * engine-free so the evidence program drives the same code the game runs:
 *
 *	the emergency services	an accident or a fire is the trigger, the
 *				nearest incident within 30 m is the destination,
 *				the 30 s (ambulance) and 35 s (fire truck)
 *				cooldowns, at most two cars of the model in the
 *				pool, and at most one of them on duty at a time
 *				(the duty counter is cleared on arrival, so a
 *				second call waits for the first to finish);
 *	the police		the wanted system's stars, its law-enforcer and
 *				cop caps and its model tiers (ChoosePoliceCarModel
 *				stays in CCarCtrl): the request cadence is every
 *				frame at four stars or more, 5 s at three and
 *				8 s at two, while the caps allow;
 *	the crew		the seats of one dispatch: the driver plus the
 *				passengers the model's own rule names. CCarAI's
 *				Add*Occupants fill the seats from this table, so
 *				the evidence program checks the same numbers the
 *				primitive creates.
 *
 * The decisions are deliberately separate from the service: a refusal here
 * costs no plan, no path search and no ledger request. A policy that lets a
 * request through is still subject to the service's gates (budget, readiness,
 * zone, occupancy, visibility, reachability), and the service counts what it
 * refuses.
 *
 * The destination mode: the emergency request's target and destination are one
 * point, the incident. The planner places on the ring around it and checks
 * that the site can reach it; the primitive joins the car to the road at its
 * placed position and sets the autopilot to MISSION_GOTOCOORDS (or
 * MISSION_GOTOCOORDS_STRAIGHT) for that point. The request's centre stays the
 * player's, so the visibility ladder and the birth cull still refuse a spot
 * the player can see. The old code built the route itself after a raw
 * path-collector placement; the route builder is the same primitive code, only
 * the placement moved to the service.
 */

#include <stdint.h>

#include "SpawnService.h"

/* The emergency triggers: the old GenerateEmergencyServicesCar dispatched an
 * ambulance only with two active accidents and a fire truck only with three
 * active fires (a lone accident or fire is handled by the emergency peds). */
#define RESPONDER_ACCIDENT_TRIGGER	2
#define RESPONDER_FIRE_TRIGGER		3

/* The cooldowns and the per-model cap, verbatim from the old path. */
#define RESPONDER_AMBULANCE_COOLDOWN_MS	30000
#define RESPONDER_FIRETRUCK_COOLDOWN_MS	35000
#define RESPONDER_MAX_CARS		2

/* The incident search range (FindNearestAccident/FindNearestFire's own
 * distance) and the emergency cruise speed of the old path. */
#define RESPONDER_NEAREST_RANGE		30.0f
#define RESPONDER_CRUISE_SPEED		25

/* The old emergency ring aimed 45 degrees with an open cone at the on-screen
 * band; the request keeps that shape. */
#define RESPONDER_RING_DIR		0.70710678f

/* The police request cadence at the lower stars. */
#define POLICE_COOLDOWN_LOW_MS		8000
#define POLICE_COOLDOWN_MID_MS		5000

/* Why a responder request is not made. DISPATCH is the only pass. */
enum EResponderGate
{
	RESPONDER_GATE_DISPATCH = 0,
	RESPONDER_GATE_WANTED,		/* above three stars the emergency services stay away */
	RESPONDER_GATE_INTERIOR,	/* no cars inside an interior */
	RESPONDER_GATE_BUDGET,		/* the car budget's top */
	RESPONDER_GATE_ON_DUTY,		/* one responder of the role is already on duty */
	RESPONDER_GATE_BELOW_TRIGGER,	/* not enough active incidents */
	RESPONDER_GATE_NO_INCIDENT,	/* none within the search range */
	RESPONDER_GATE_CAP,		/* two cars of the model already exist */
	RESPONDER_GATE_COOLDOWN,	/* the role's cooldown has not passed */
	RESPONDER_GATE_COUNT
};

/* The facts of one role's decision, resolved live by the engine. */
struct SResponderFacts
{
	int32_t wantedLevel;
	bool interior;
	int32_t carsInUse;
	int32_t carBudget;
	int32_t activeIncidents;
	bool nearestFound;
	float nearestDistance;
	float nearestX, nearestY, nearestZ;
	int32_t onDuty;			/* NumAmbulancesOnDuty or NumFiretrucksOnDuty */
	int32_t carsOfType;		/* CountCarsOfType(the role's model) */
	uint32_t nowMs;
	uint32_t lastCreatedMs;
};

struct SResponderDecision
{
	bool dispatch;
	EResponderGate gate;
	ESpawnResponderRole role;
	float destX, destY, destZ;
};

static inline int32_t
ResponderTrigger(ESpawnResponderRole role)
{
	if (role == SPAWN_RESPONDER_AMBULANCE)
		return RESPONDER_ACCIDENT_TRIGGER;
	if (role == SPAWN_RESPONDER_FIRETRUCK)
		return RESPONDER_FIRE_TRIGGER;
	return 0;
}

static inline uint32_t
ResponderCooldown(ESpawnResponderRole role)
{
	if (role == SPAWN_RESPONDER_AMBULANCE)
		return RESPONDER_AMBULANCE_COOLDOWN_MS;
	if (role == SPAWN_RESPONDER_FIRETRUCK)
		return RESPONDER_FIRETRUCK_COOLDOWN_MS;
	return 0;
}

static inline const char *
ResponderGateName(EResponderGate gate)
{
	switch (gate) {
	case RESPONDER_GATE_DISPATCH:		return "dispatch";
	case RESPONDER_GATE_WANTED:		return "wanted";
	case RESPONDER_GATE_INTERIOR:		return "interior";
	case RESPONDER_GATE_BUDGET:		return "budget";
	case RESPONDER_GATE_ON_DUTY:		return "on-duty";
	case RESPONDER_GATE_BELOW_TRIGGER:	return "below-trigger";
	case RESPONDER_GATE_NO_INCIDENT:	return "no-incident";
	case RESPONDER_GATE_CAP:		return "cap";
	case RESPONDER_GATE_COOLDOWN:		return "cooldown";
	default:				return "?";
	}
}

/*
 * The emergency decision, in the old path's own order: the wanted level, the
 * interior and the car budget stop both roles before any incident work; a
 * role already on duty is skipped (the old `Num*OnDuty == 0` guard); the
 * incident count, the nearest incident, the model cap and the cooldown follow.
 * A cooldown comparison uses the signed difference of the two clock reads, so
 * the timer's wrap cannot hold it forever.
 */
static inline void
ResponderEvaluate(ESpawnResponderRole role, const SResponderFacts &facts, SResponderDecision *pDecision)
{
	pDecision->dispatch = false;
	pDecision->gate = RESPONDER_GATE_DISPATCH;
	pDecision->role = role;
	pDecision->destX = 0.0f;
	pDecision->destY = 0.0f;
	pDecision->destZ = 0.0f;

	if (facts.wantedLevel > 3) {
		pDecision->gate = RESPONDER_GATE_WANTED;
		return;
	}
	if (facts.interior) {
		pDecision->gate = RESPONDER_GATE_INTERIOR;
		return;
	}
	if (facts.carBudget <= 0 || facts.carsInUse >= facts.carBudget) {
		pDecision->gate = RESPONDER_GATE_BUDGET;
		return;
	}
	if (facts.onDuty != 0) {
		pDecision->gate = RESPONDER_GATE_ON_DUTY;
		return;
	}
	if (facts.activeIncidents < ResponderTrigger(role)) {
		pDecision->gate = RESPONDER_GATE_BELOW_TRIGGER;
		return;
	}
	if (!facts.nearestFound || facts.nearestDistance > RESPONDER_NEAREST_RANGE) {
		pDecision->gate = RESPONDER_GATE_NO_INCIDENT;
		return;
	}
	if (facts.carsOfType >= RESPONDER_MAX_CARS) {
		pDecision->gate = RESPONDER_GATE_CAP;
		return;
	}
	if ((int32_t)(facts.nowMs - facts.lastCreatedMs) <= (int32_t)ResponderCooldown(role)) {
		pDecision->gate = RESPONDER_GATE_COOLDOWN;
		return;
	}
	pDecision->dispatch = true;
	pDecision->destX = facts.nearestX;
	pDecision->destY = facts.nearestY;
	pDecision->destZ = facts.nearestZ;
}

/*
 * The road-site request of one dispatched responder. The target is the
 * destination (the ring around the incident and the reachability target); the
 * old ring (45 degrees, open cone, on-screen band) and the collision test are
 * kept. `centreX/Y/Z` is the player centre the visibility ladder measures
 * from. The purpose is RESPONDER, the request is priority (the ambient fill
 * target is not a responder budget) and the water policy is land only: an
 * ambulance does not take a boat site.
 */
static inline void
ResponderFillRequest(const SResponderDecision &decision, int32_t carModel, int32_t carClass,
	float centreX, float centreY, float centreZ, float spawnDist, SSpawnCarRequest *pRequest)
{
	pRequest->purpose = SPAWN_PURPOSE_RESPONDER;
	pRequest->force = false;
	pRequest->priority = true;
	pRequest->allowBoats = false;
	pRequest->requireReach = true;
	pRequest->carClass = carClass;
	pRequest->carModel = carModel;
	pRequest->targetX = decision.destX;
	pRequest->targetY = decision.destY;
	pRequest->targetZ = decision.destZ;
	pRequest->centreX = centreX;
	pRequest->centreY = centreY;
	pRequest->centreZ = centreZ;
	pRequest->playerSpeedX = 0.0f;
	pRequest->playerSpeedY = 0.0f;
	pRequest->testForCollision = true;
	pRequest->spawnDist = spawnDist;
	pRequest->dirX = RESPONDER_RING_DIR;
	pRequest->dirY = RESPONDER_RING_DIR;
	pRequest->angleLimit = -1.0f;
	pRequest->forward = true;
	pRequest->allowDisabled = false;
	pRequest->responder = decision.role;
	pRequest->destinationX = decision.destX;
	pRequest->destinationY = decision.destY;
	pRequest->destinationZ = decision.destZ;
}

/* Why the police request is not made (VICE-156). */
enum EPoliceGate
{
	POLICE_GATE_ALLOWED = 0,
	POLICE_GATE_WANTED,		/* one star or less: the ambient cops' own level */
	POLICE_GATE_VEHICLE_CAP,	/* m_MaximumLawEnforcerVehicles */
	POLICE_GATE_COP_CAP,		/* m_MaxCops */
	POLICE_GATE_INTERIOR,
	POLICE_GATE_COOLDOWN,
	POLICE_GATE_COUNT
};

struct SPoliceFacts
{
	int32_t wantedLevel;
	int32_t lawEnforcers;		/* CCarCtrl::NumLawEnforcerCars */
	int32_t maxVehicles;		/* CWanted::m_MaximumLawEnforcerVehicles */
	int32_t currentCops;		/* CWanted::m_CurrentCops */
	int32_t maxCops;		/* CWanted::m_MaxCops */
	bool interior;
	uint32_t nowMs;
	uint32_t lastCreatedMs;		/* CCarCtrl::LastTimeLawEnforcerCreated */
};

static inline const char *
PoliceGateName(EPoliceGate gate)
{
	switch (gate) {
	case POLICE_GATE_ALLOWED:	return "allowed";
	case POLICE_GATE_WANTED:	return "wanted";
	case POLICE_GATE_VEHICLE_CAP:	return "vehicle-cap";
	case POLICE_GATE_COP_CAP:	return "cop-cap";
	case POLICE_GATE_INTERIOR:	return "interior";
	case POLICE_GATE_COOLDOWN:	return "cooldown";
	default:			return "?";
	}
}

/*
 * The police gate, the old wanted branch's condition verbatim: more than one
 * star, the law-enforcer cap, the cop cap, no interior, then the cadence -
 * four stars or more ask every frame, three stars wait five seconds, two stars
 * eight.
 */
static inline EPoliceGate
PoliceRequestGate(const SPoliceFacts &facts)
{
	int32_t elapsed;

	if (facts.wantedLevel <= 1)
		return POLICE_GATE_WANTED;
	if (facts.lawEnforcers >= facts.maxVehicles)
		return POLICE_GATE_VEHICLE_CAP;
	if (facts.currentCops >= facts.maxCops)
		return POLICE_GATE_COP_CAP;
	if (facts.interior)
		return POLICE_GATE_INTERIOR;
	if (facts.wantedLevel > 3)
		return POLICE_GATE_ALLOWED;
	elapsed = (int32_t)(facts.nowMs - facts.lastCreatedMs);
	if (facts.wantedLevel > 2)
		return elapsed > POLICE_COOLDOWN_MID_MS ? POLICE_GATE_ALLOWED : POLICE_GATE_COOLDOWN;
	return elapsed > POLICE_COOLDOWN_LOW_MS ? POLICE_GATE_ALLOWED : POLICE_GATE_COOLDOWN;
}

/*
 * The road-site request of one police dispatch: the wanted system chose the
 * model (ChoosePoliceCarModel) and the ring is the traffic ring helper's own;
 * the purpose is POLICE and the request is priority. Boats are allowed: a
 * water site takes the Predator (the service's COPS fallback), so the police
 * cover the water as the old ambient fallback did. The disabled flag follows
 * the wanted rule (a cop may use a script-disabled road).
 */
static inline void
PoliceFillRequest(int32_t carModel, int32_t carClass, float targetX, float targetY, float targetZ,
	float playerSpeedX, float playerSpeedY, float spawnDist, float dirX, float dirY,
	float angleLimit, bool forward, bool testForCollision, SSpawnCarRequest *pRequest)
{
	pRequest->purpose = SPAWN_PURPOSE_POLICE;
	pRequest->force = false;
	pRequest->priority = true;
	pRequest->allowBoats = true;
	pRequest->requireReach = true;
	pRequest->carClass = carClass;
	pRequest->carModel = carModel;
	pRequest->targetX = targetX;
	pRequest->targetY = targetY;
	pRequest->targetZ = targetZ;
	/* The police ring and the visibility centre are both the player. */
	pRequest->centreX = targetX;
	pRequest->centreY = targetY;
	pRequest->centreZ = targetZ;
	pRequest->playerSpeedX = playerSpeedX;
	pRequest->playerSpeedY = playerSpeedY;
	pRequest->testForCollision = testForCollision;
	pRequest->spawnDist = spawnDist;
	pRequest->dirX = dirX;
	pRequest->dirY = dirY;
	pRequest->angleLimit = angleLimit;
	pRequest->forward = forward;
	pRequest->allowDisabled = true;
	pRequest->responder = SPAWN_RESPONDER_NONE;
	pRequest->destinationX = 0.0f;
	pRequest->destinationY = 0.0f;
	pRequest->destinationZ = 0.0f;
}

/*
 * The crew of one dispatch. The passengers are a bit per seat index (bit 0 is
 * SetupPassenger(0)); the driver is always seat 0 of the ped arrangement.
 * CCarAI's AddPoliceCarOccupants selects the kind by the model and fills the
 * seats from here; the ambulance and fire truck helpers do the same, so the
 * evidence program's table is the one the game creates.
 */
enum ESpawnCrewKind
{
	SPAWN_CREW_NONE = 0,
	SPAWN_CREW_POLICE_VAN,		/* FBI Rancher, Enforcer: driver + 3 */
	SPAWN_CREW_POLICE_SEDAN,	/* Police, Rhino, Barracks: driver + 1 at two stars */
	SPAWN_CREW_POLICE_BOAT,		/* Predator: driver only */
	SPAWN_CREW_VICE,		/* VChee: two Miami Vice cops */
	SPAWN_CREW_AMBULANCE,		/* driver + seat 1 */
	SPAWN_CREW_FIRETRUCK,		/* driver + seat 0 */
	SPAWN_CREW_COUNT
};

struct SSpawnCrew
{
	uint8_t driver;			/* 1: SetUpDriver */
	uint8_t passengerSeats;		/* bit i: SetupPassenger(i) */
};

static inline SSpawnCrew
SpawnCrewOf(ESpawnCrewKind kind, int32_t wantedLevel)
{
	SSpawnCrew crew;

	crew.driver = 0;
	crew.passengerSeats = 0;
	switch (kind) {
	case SPAWN_CREW_POLICE_VAN:
		crew.driver = 1;
		crew.passengerSeats = (uint8_t)((1 << 0) | (1 << 1) | (1 << 2));
		break;
	case SPAWN_CREW_POLICE_SEDAN:
		crew.driver = 1;
		if (wantedLevel > 1)
			crew.passengerSeats = (uint8_t)(1 << 0);
		break;
	case SPAWN_CREW_POLICE_BOAT:
		crew.driver = 1;
		break;
	case SPAWN_CREW_VICE:
		crew.driver = 1;
		crew.passengerSeats = (uint8_t)(1 << 0);
		break;
	case SPAWN_CREW_AMBULANCE:
		crew.driver = 1;
		crew.passengerSeats = (uint8_t)(1 << 1);
		break;
	case SPAWN_CREW_FIRETRUCK:
		crew.driver = 1;
		crew.passengerSeats = (uint8_t)(1 << 0);
		break;
	default:
		break;
	}
	return crew;
}

/* The responder's crew kind, so one role has one seat table. */
static inline ESpawnCrewKind
ResponderCrewKind(ESpawnResponderRole role)
{
	if (role == SPAWN_RESPONDER_AMBULANCE)
		return SPAWN_CREW_AMBULANCE;
	if (role == SPAWN_RESPONDER_FIRETRUCK)
		return SPAWN_CREW_FIRETRUCK;
	return SPAWN_CREW_NONE;
}

#endif // __GTA_RESPONDERPOLICY_H__
