#include "common.h"

#include "SpawnService.h"

#include "CarCtrl.h"

#include "Automobile.h"
#include "Bike.h"
#include "Boat.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "Pools.h"
#include "SpawnReadiness.h"
#include "Streaming.h"
#include "Timer.h"
#include "World.h"
#include "Zones.h"

/*
 * The engine side of the spawn service (VICE-150). The planner, the two
 * creation kinds and the primitive's decisions live in SpawnService.h (the
 * engine-free core) and in CCarCtrl::CreateCarFromPlacement (CarCtrl.cpp);
 * this file is the frame's half:
 *
 *	the request gates	the budget facts of the frame (the car target,
 *				the car budget, the ped pool's free spaces) and
 *				the readiness of the primary model;
 *	the fact resolution	the ladder's candidate per offered site (the
 *				cheap fact), and the deep facts of the ranked
 *				prefix: the site's live zone compatibility and
 *				CPathFind::TestCoorsCloseness, the path search
 *				the primitive used to pay per creation;
 *	the attempt		the per-site model resolution (the ambient's
 *				water fallback, verbatim) and the primitive;
 *	the ladder API		the near bubble's gates and counters, so every
 *				spawn decision goes through this class (the
 *				counters stay CCarCtrl's own);
 *	the telemetry		the one static instance below, read live by
 *				utils/caps-audit/live-counters.py.
 *
 * The counters keep one owner: SSpawnServiceTelemetry has one instance,
 * CSpawnService::ms_Telemetry. The live reader reads its fields at the
 * offsets pinned by the static_asserts below; the header states the rows'
 * meanings.
 */

SSpawnServiceTelemetry CSpawnService::ms_Telemetry;

/* The live reader's offsets (utils/caps-audit/live-counters.py). The layout
 * is the header's field order; a field move breaks the build here before it
 * can misread the live rows. */
static_assert(offsetof(SSpawnServiceTelemetry, m_nRequestsByPurpose) == 0, "the live reader reads the request rows at offset 0");
static_assert(offsetof(SSpawnServiceTelemetry, m_nRequestsByKind) == 32, "the live reader reads the kind rows at offset 32");
static_assert(offsetof(SSpawnServiceTelemetry, m_nRefusalsByReason) == 40, "the live reader reads the refusal rows at offset 40");
static_assert(offsetof(SSpawnServiceTelemetry, m_nCreatedByPurpose) == 76, "the live reader reads the created rows at offset 76");
static_assert(offsetof(SSpawnServiceTelemetry, m_nRemovedByPurpose) == 108, "the live reader reads the removed rows at offset 108");
static_assert(offsetof(SSpawnServiceTelemetry, m_nCoveredFillCreations) == 140, "the live reader reads the covered-fill row at offset 140");
static_assert(offsetof(SSpawnServiceTelemetry, m_nReadinessWaits) == 144, "the live reader reads the wait rows at offset 144");
static_assert(offsetof(SSpawnServiceTelemetry, m_nPlanMicros) == 200, "the live reader reads the plan rows at offset 184 and the micros at 200");
static_assert(offsetof(SSpawnServiceTelemetry, m_nConservationCellSum) == 208, "the live reader reads the conservation rows at offset 208");
static_assert(offsetof(SSpawnServiceTelemetry, m_nGroundRejections) == 220, "the live reader reads the defect rows at offset 220");
static_assert(sizeof(SSpawnServiceTelemetry) == 232, "the telemetry's size is the live reader's contract");

/* The vehicle tag the core names (SPAWN_VEHICLE_TAG_RANDOM) is the engine's
 * own RANDOM_VEHICLE: the driver contract of SetUpDriver reads the tag. The
 * removal ledger's creation classes are pinned the same way. */
static_assert(SPAWN_VEHICLE_TAG_RANDOM == RANDOM_VEHICLE, "the random tag must be the engine's own");
static_assert(SPAWN_VEHICLE_TAG_MISSION == MISSION_VEHICLE, "the mission tag must be the engine's own");
static_assert(SPAWN_VEHICLE_TAG_PARKED == PARKED_VEHICLE, "the parked tag must be the engine's own");
static_assert(SPAWN_VEHICLE_TAG_PERMANENT == PERMANENT_VEHICLE, "the permanent tag must be the engine's own");

/*
 * The zone compatibility of one class: the live info zone of the position
 * must be able to carry the class. The threshold arrays are cumulative, so a
 * class has room when its threshold is above the one before it (the first
 * against zero). The cops and the gangs read their own rows; a class outside
 * the car ratings (a boat class asked directly) reads the boat rows.
 */
static bool
SpawnZoneAdmitsClass(const CVector &pos, int carClass)
{
	CZoneInfo zone;
	int32 i, lower;

	CTheZones::GetZoneInfoForTimeOfDay(&pos, &zone);

	if (carClass == CCarCtrl::COPS)
		return zone.copThreshold > 0;
	if (carClass >= CCarCtrl::FIRST_GANG_CAR_RATING && carClass <= CCarCtrl::GANG9) {
		int32 gang = carClass - CCarCtrl::FIRST_GANG_CAR_RATING;
		return zone.gangThreshold[gang] > 0;
	}
	if (carClass >= CCarCtrl::FIRST_BOAT_RATING && carClass <= CCarCtrl::WORKERBOAT) {
		int32 boat = carClass - CCarCtrl::FIRST_BOAT_RATING;
		return zone.boatThreshold[boat] > 0;
	}
	i = carClass - CCarCtrl::FIRST_CAR_RATING;
	if (i < 0 || i >= CCarCtrl::NUM_CAR_CLASSES)
		return true;	/* an unknown rating is not this gate's business */
	lower = i > 0 ? zone.carThreshold[i - 1] : 0;
	return zone.carThreshold[i] > lower;
}

struct SSpawnCarAttempt
{
	const SSpawnCarRequest *pRequest;
	CZoneInfo zone;
	CVector target;
	CVector centre;
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	int32 boatModel;
	bool boatModelChosen;
	CVehicle **ppCreated;
};

/*
 * The reachability cache. CPathFind::TestCoorsCloseness is the planner's one
 * expensive fact (about 15 us a search, ADR 0024). The same site against the
 * same target area answers the same way, and the frame makes up to six car
 * calls with the same target, so a small direct-mapped cache keeps the search
 * out of the per-creation loop: the key is the site id and the target's
 * presence cell (16 m), and an entry lives SPAWN_REACH_CACHE_FRAMES (15, a
 * quarter second at 60 fps) so a stale answer cannot outlive the target's own
 * movement inside the cell by long.
 *
 * The caveats, stated: the key is the target's 2D cell, so two targets inside
 * one 16 m cell share an answer (the search is against the target's actual
 * position and height, so the sharing is an approximation - the created car's
 * final collision query and the route join stay the true guards); and the
 * graph's flags (a script's road toggle, a save load) move the answer, so the
 * cache is cleared when CPathFind::m_nodeFlagsVersion moved - the
 * SpawnSites::RefreshFlags pattern, one comparison. A load or a restart
 * clears it too (ReInit).
 */
#define SPAWN_REACH_CACHE_ENTRIES 256
#define SPAWN_REACH_CACHE_FRAMES 15

struct SReachCacheEntry
{
	int32 site;
	int32 cell;
	uint32 frame;
	bool reach;
};

/* The constructor invalidates every entry before main, so a zero entry can
 * never read as a hit (site 0 in cell 0 would otherwise). */
struct SReachCache
{
	SReachCacheEntry entries[SPAWN_REACH_CACHE_ENTRIES];
	SReachCache(void)
	{
		for (int32 i = 0; i < SPAWN_REACH_CACHE_ENTRIES; i++)
			entries[i].site = -1;
	}
};

static SReachCache ms_ReachCache;
static uint32 ms_nReachCacheVersion;

static void
SpawnReachCacheClear(void)
{
	for (int32 i = 0; i < SPAWN_REACH_CACHE_ENTRIES; i++)
		ms_ReachCache.entries[i].site = -1;
}

/* The reachability of one site against one target cell, from the cache or the
 * search. A target off the presence grid keys on -1; the entry's frame is the
 * unsigned difference, so the counter's wrap cannot keep it alive. */
static bool
SpawnReachOf(int32 site, const CVector &target)
{
	int32 cell = -1;
	uint32 slot, frame = CTimer::GetFrameCounter();
	SReachCacheEntry *pEntry;

	/* The graph's flags move the answer (a script's road toggle, a save
	 * load): the index's own refresh pattern, one comparison a use. */
	if (ThePaths.m_nodeFlagsVersion != ms_nReachCacheVersion) {
		SpawnReachCacheClear();
		ms_nReachCacheVersion = ThePaths.m_nodeFlagsVersion;
	}

	SSpawnOccupancyStore::CellAt(target.x, target.y, cell);
	slot = ((uint32)site * 31u + (uint32)cell) & (SPAWN_REACH_CACHE_ENTRIES - 1);
	pEntry = &ms_ReachCache.entries[slot];
	if (pEntry->site == site && pEntry->cell == cell &&
	    (uint32)(frame - pEntry->frame) < SPAWN_REACH_CACHE_FRAMES)
		return pEntry->reach;

	pEntry->site = site;
	pEntry->cell = cell;
	pEntry->frame = frame;
	pEntry->reach = ThePaths.TestCoorsCloseness(target, false,
		CVector(CSpawnSites::Index().GetSites()[site].x,
			CSpawnSites::Index().GetSites()[site].y,
			CSpawnSites::Index().GetSites()[site].z));
	return pEntry->reach;
}

/* The deep facts of one ranked candidate: the site's live zone (never cached:
 * the script fills the zones after boot and a save load replaces the array)
 * and the reachability of the target. The boat sites keep the old
 * primitive's own exemption from the reach test. */
static void
SpawnResolveDeepFacts(void *user, int32 offerIndex, SSpawnSiteDeepFacts *facts)
{
	SSpawnCarAttempt *pAttempt = (SSpawnCarAttempt *)user;
	const SSpawnOffer &offer = pAttempt->offers[offerIndex];
	const SSpawnSite &site = CSpawnSites::Index().GetSites()[offer.site];
	CVector sitePos(site.x, site.y, site.z);

	facts->zoneOk = SpawnZoneAdmitsClass(sitePos, pAttempt->pRequest->carClass);
	facts->reachOk = true;
	/* The zone gate answers first (the core checks it first too), so a
	 * zone-refused candidate costs no path search. */
	if (facts->zoneOk && !offer.boat && pAttempt->pRequest->requireReach)
		facts->reachOk = SpawnReachOf(offer.site, pAttempt->target);
}

/* The attempt at one planned site: the model the site takes (the ambient's
 * water fallback, kept verbatim: the cop boat once, the boat model once per
 * call) and the primitive. The lane draw, the duration and the site facts are
 * the primitive's own; `pArm` receives the arm the creation gate resolved at
 * the final position (the covered-fill row's own answer, L3). */
static ESpawnRefusal
SpawnTrySite(void *user, const SSpawnPlanSite &planSite, uint8_t *pArm)
{
	SSpawnCarAttempt *pAttempt = (SSpawnCarAttempt *)user;
	const SSpawnCarRequest &request = *pAttempt->pRequest;
	const SSpawnSite &site = CSpawnSites::Index().GetSites()[planSite.site];
	int carClass = request.carClass;
	int carModel = request.carModel;
	ENearBubbleArm arm = NEAR_BUBBLE_ARM_NONE;
	ESpawnRefusal refusal;

	if (site.flags & SPAWN_SITE_BOAT) {
		if (!request.allowBoats)
			return SPAWN_REFUSAL_NO_SITE;
		if (carClass == CCarCtrl::COPS) {
			carClass = CCarCtrl::COPS_BOAT;
			carModel = MI_PREDATOR;
			if (!CStreaming::HasModelLoaded(MI_PREDATOR)) {
				CStreaming::RequestModel(MI_PREDATOR, STREAMFLAGS_DEPENDENCY);
				return SPAWN_REFUSAL_READINESS;
			}
		} else {
			if (!pAttempt->boatModelChosen) {
				int j;
				for (j = 10; j > 0 && (pAttempt->boatModel == -1 || !CStreaming::HasModelLoaded(pAttempt->boatModel)); j--)
					pAttempt->boatModel = CCarCtrl::ChooseBoatModel(CCarCtrl::ChooseBoatRating(&pAttempt->zone));
				if (j == 0)
					return SPAWN_REFUSAL_READINESS;
				pAttempt->boatModelChosen = true;
			}
			carModel = pAttempt->boatModel;
		}
	}

	SSpawnPlacement placement;

	/* The site's own mapping (B1): the primitive indexes m_carPathLinks with
	 * the site's LINK id, never with the directed connection slot. The
	 * destination (VICE-156) travels with the placement: a responder drives
	 * to its incident, every other request leaves the mode empty. */
	SpawnPlacementFromSite(site, planSite.site, &placement);
	SpawnPlacementSetDestination(&placement, request);
	refusal = CCarCtrl::CreateCarFromPlacement(placement, carClass, carModel,
		pAttempt->centre, CVector2D(request.playerSpeedX, request.playerSpeedY),
		request.testForCollision, SPAWN_TIER_NEAR, pAttempt->ppCreated, &arm);
	if (refusal == SPAWN_REFUSAL_NONE && pArm)
		*pArm = (uint8_t)arm;
	return refusal;
}

bool
CSpawnService::RequestCar(const SSpawnCarRequest &request, CVehicle **ppCreated)
{
	SSpawnCarAttempt attempt;
	SSpawnRingQuery query;
	SSpawnPlan plan;
	SSpawnBudgetFacts budget;
	int32 numOffers, i;
	uint32 startCycles, endCycles, cyclesPerMs;
	uint64 micros;

	SpawnRequestBegin(&ms_Telemetry, request.purpose, SPAWN_KIND_ROAD_SITE);

	/* The function gate: the car target, the car budget and the ped pool's
	 * reserve, the same counts the spawner's call checked one by one. */
	budget.randomCars = CCarCtrl::NumRandomCars;
	budget.randomTarget = CCarCtrl::GetRandomCarTarget();
	budget.carsInUse = CCarCtrl::NumCarsInUse();
	budget.carBudget = CCarCtrl::MaxNumberOfCarsInUse;
	budget.pedFreeSpaces = CPools::GetPedPool()->GetNoOfFreeSpaces();
	if (SpawnFunctionGate(budget, request.force, &ms_Telemetry, request.priority) != SPAWN_REFUSAL_NONE)
		return false;

	/* The request's own readiness (VICE-151): the model the requester chose
	 * must be resident with its txd and its animation block (IsModelReady:
	 * a "loaded" model can have an evicted anim block, so HasModelLoaded
	 * alone can lie). The wait clock of the purpose runs until a request
	 * passes. The prewarm's pins hold the class catalogue's own models,
	 * txds and anim blocks, so the ambient traffic's models (the class
	 * arrays and the police sets, whose anims are the pinned van block or
	 * none) never defer on the strict rule; a model outside the prewarm
	 * whose anim block was evicted has no healer yet - a follow-up
	 * decision, stated in SpawnReadiness.h. */
	if (request.carModel < 0 || !CSpawnReadiness::IsModelReady(request.carModel)) {
		SpawnReadinessDefer(&ms_Telemetry, request.purpose, CTimer::GetTimeInMilliseconds());
		return false;
	}
	SpawnReadinessReady(&ms_Telemetry, request.purpose, CTimer::GetTimeInMilliseconds());

	attempt.pRequest = &request;
	attempt.target = CVector(request.targetX, request.targetY, request.targetZ);
	attempt.centre = CVector(request.centreX, request.centreY, request.centreZ);
	attempt.boatModel = -1;
	attempt.boatModelChosen = false;
	attempt.ppCreated = ppCreated;
	CTheZones::GetZoneInfoForTimeOfDay(&attempt.target, &attempt.zone);

	query.targetX = request.targetX;
	query.targetY = request.targetY;
	query.spawnDist = request.spawnDist;
	query.dirX = request.dirX;
	query.dirY = request.dirY;
	query.angleLimit = request.angleLimit;
	query.forward = request.forward;
	query.water = request.allowBoats ? SPAWN_SITES_ANY : SPAWN_SITES_LAND;
	query.allowDisabled = request.allowDisabled;

	/* The plan's own price: the offer walk and the fact resolution (the
	 * ranked prefix's path searches included). The primitive's work is the
	 * run's, not the plan's. */
	startCycles = CTimer::GetCurrentTimeInCycles();
	numOffers = SpawnOfferSites(CSpawnSites::Index(), query, attempt.offers, SPAWN_SERVICE_MAX_CANDIDATES);
	for (i = 0; i < numOffers; i++) {
		const SSpawnSite &site = CSpawnSites::Index().GetSites()[attempt.offers[i].site];
		CCarCtrl::NearBubbleCandidateFor(CVector(site.x, site.y, site.z), attempt.centre,
			NEAR_BUBBLE_ENTITY_RADIUS, &attempt.bubbles[i]);
	}
	SpawnPlanSites(CSpawnSites::Index(), attempt.offers, attempt.bubbles, numOffers,
		&CSpawnOccupancy::Store(), CTimer::GetTimeInMilliseconds(),
		SpawnResolveDeepFacts, &attempt, &plan, &ms_Telemetry);
	endCycles = CTimer::GetCurrentTimeInCycles();
	cyclesPerMs = CTimer::GetCyclesPerMillisecond();
	micros = cyclesPerMs ? (uint64)(endCycles - startCycles) * 1000 / cyclesPerMs : 0;
	NotePlanCost(micros);

	return SpawnRunPlan(plan, &CSpawnOccupancy::Store(), SpawnTrySite, &attempt,
		&ms_Telemetry, request.purpose) == SPAWN_REFUSAL_NONE;
}

bool
CSpawnService::RequestExplicit(const SSpawnExplicitRequest &request, CVehicle **ppCreated)
{
	SSpawnExplicitDecision decision;
	SSpawnBudgetFacts budget;
	CVehicle *pVehicle;
	float dirX, dirY, len;

	SpawnRequestBegin(&ms_Telemetry, request.purpose, SPAWN_KIND_EXPLICIT);

	/* The force bypass covers the budgets and the reserves. The pool's last
	 * slots are physical, not a reserve: even a mission cannot put a vehicle
	 * in a full pool, and the request refuses with the pool's reason. */
	budget.randomCars = CCarCtrl::NumRandomCars;
	budget.randomTarget = CCarCtrl::GetRandomCarTarget();
	budget.carsInUse = CCarCtrl::NumCarsInUse();
	budget.carBudget = CCarCtrl::MaxNumberOfCarsInUse;
	budget.pedFreeSpaces = CPools::GetPedPool()->GetNoOfFreeSpaces();
	if (SpawnFunctionGate(budget, request.force, &ms_Telemetry) != SPAWN_REFUSAL_NONE)
		return false;
	if (CPools::GetVehiclePool()->GetNoOfFreeSpaces() <= 0) {
		SpawnNoteRefusal(&ms_Telemetry, SPAWN_REFUSAL_POOL);
		return false;
	}
	if (request.carModel < 0) {
		/* No model is a bad request, not a streaming wait: NO_SITE (there
		 * is no car to place); a model that is merely not resident is not
		 * refused here - the force bypass's own contract is that the
		 * streaming cannot fail a mission, so the creation asks for it. */
		SpawnNoteRefusal(&ms_Telemetry, SPAWN_REFUSAL_NO_SITE);
		return false;
	}

	SpawnDecideExplicit(request, &decision);

	if (CModelInfo::IsBoatModel(request.carModel))
		pVehicle = new CBoat(request.carModel, decision.createdBy);
	else if (CModelInfo::IsBikeModel(request.carModel))
		pVehicle = new CBike(request.carModel, decision.createdBy);
	else
		pVehicle = new CAutomobile(request.carModel, decision.createdBy);
	if (pVehicle == nil) {
		/* The constructor's own pool refusal (the free-space check above
		 * passed, so this is the pool's last slot going to another path in
		 * between): the pool's reason, counted. */
		SpawnNoteRefusal(&ms_Telemetry, SPAWN_REFUSAL_POOL);
		return false;
	}

	/* The exact position and heading the caller gave: no plan, no ring, no
	 * visibility (the script asks for the car; where it stands is the
	 * script's). The lifetime is floored by the fresh-spawn grace, so the
	 * culls cannot take the car on its way in. */
	pVehicle->SetPosition(CVector(decision.x, decision.y, decision.z));
	len = sqrtf(decision.dirX * decision.dirX + decision.dirY * decision.dirY);
	if (len > 0.0001f) {
		dirX = decision.dirX / len;
		dirY = decision.dirY / len;
	} else {
		dirX = 1.0f;
		dirY = 0.0f;
	}
	pVehicle->GetForward() = CVector(dirX, dirY, 0.0f);
	pVehicle->GetRight() = CVector(dirY, -dirX, 0.0f);
	pVehicle->GetUp() = CVector(0.0f, 0.0f, 1.0f);
	pVehicle->SetStatus(STATUS_ABANDONED);
	pVehicle->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + decision.protectMs;
	CWorld::Add(pVehicle);
	/* The live numbers of the pop-in ticket carry the scripted cars apart:
	 * the explicit kind is one of them. */
	CCarCtrl::NearBubbleCountOtherCreation(pVehicle->GetPosition(), NEAR_BUBBLE_ENTITY_RADIUS);
	SpawnNoteCreated(&ms_Telemetry, request.purpose);
	if (ppCreated)
		*ppCreated = pVehicle;
	return true;
}

/* --- The ladder's API ---------------------------------------------------- */

void
CSpawnService::BeginFrame(void)
{
	CCarCtrl::NearBubbleBeginFrame();
}

bool
CSpawnService::AllowsCreation(const CVector &pos, const CVector &playerCentre, float radius, bool rayAllowed, ENearBubbleArm *pArm)
{
	return CCarCtrl::NearBubbleAllowsCreation(pos, playerCentre, radius, rayAllowed, pArm);
}

bool
CSpawnService::AllowsRemoval(CVehicle *pVehicle, const CVector &playerCentre, float ringThreshold, bool rayAllowed)
{
	return CCarCtrl::NearBubbleAllowsRemoval(pVehicle, playerCentre, ringThreshold, rayAllowed);
}

void
CSpawnService::CountCreation(const CVector &pos, float radius)
{
	CCarCtrl::NearBubbleCountCreation(pos, radius);
}

void
CSpawnService::CountRemoval(const CVector &pos, float radius, bool wasInvisible)
{
	CCarCtrl::NearBubbleCountRemoval(pos, radius, wasInvisible);
}

void
CSpawnService::CountOtherCreation(const CVector &pos, float radius)
{
	CCarCtrl::NearBubbleCountOtherCreation(pos, radius);
}

void
CSpawnService::CountValveRemoval(void)
{
	CCarCtrl::NearBubbleCountValveRemoval();
}

/* --- The notes ----------------------------------------------------------- */

void
CSpawnService::NoteVehicleRemoval(CVehicle *pVehicle)
{
	if (pVehicle == nil)
		return;
	/* The live role flags answer first (VICE-156): an ambulance or a fire
	 * truck on duty leaves in the responder row, a law enforcer in the
	 * police row, and only then the creation class names the row. */
	SpawnNoteRemoval(&ms_Telemetry, SpawnPurposeOfRemovedVehicle(
		pVehicle->VehicleCreatedBy,
		pVehicle->bIsLawEnforcer != 0,
		pVehicle->bIsAmbulanceOnDuty != 0 || pVehicle->bIsFireTruckOnDuty != 0));
}

void
CSpawnService::NoteGroundRejection(void)
{
	SpawnNoteGroundRejection(&ms_Telemetry);
}

void
CSpawnService::NoteAirOffsetRejection(void)
{
	SpawnNoteAirOffsetRejection(&ms_Telemetry);
}

void
CSpawnService::NoteWaterRejection(void)
{
	SpawnNoteWaterRejection(&ms_Telemetry);
}

void
CSpawnService::NotePlanCost(uint64 micros)
{
	SpawnNotePlanCost(&ms_Telemetry, micros);
}

void
CSpawnService::NoteConservation(uint32 cellTargetSum, uint32 globalTarget)
{
	SpawnNoteConservation(&ms_Telemetry, cellTargetSum, globalTarget);
}

void
CSpawnService::ReInit(void)
{
	SpawnTelemetryReset(&ms_Telemetry);
	SpawnReachCacheClear();
}
