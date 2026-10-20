#ifndef __GTA_SPAWNSERVICE_H__
#define __GTA_SPAWNSERVICE_H__

/*
 * The spawn service (VICE-150, the design's sections 4 and 5): the one writer
 * of spawn intent. A producer requests; the service decides where the actor
 * may legally appear and creates it there.
 *
 * The request envelope carries two creation kinds:
 *
 *	road-site placement	the planner: candidates from the site index
 *				(VICE-148), filtered by purpose, the live
 *				zone, the occupancy store (VICE-149) and the
 *				visibility ladder (VICE-127, behind this API),
 *				ranked (hidden first, spread, nearest),
 *				reserved, then created by the primitive. The
 *				reachability of the target is part of the plan
 *				(a site must reach it), so the expensive path
 *				search left the per-creation loop.
 *	explicit		the exact position, model and lifetime given
 *				(scripts, missions): synchronous, no
 *				visibility and no ring; the force bypass
 *				covers the budgets and the reserves too, so a
 *				mission never fails on a scheduling decision.
 *				The caller gets the actor in the same frame.
 *				The ped arm (VICE-157) creates the standing
 *				and in-car script peds; every script opcode
 *				forces, because a mission cannot fail on a
 *				scheduling decision.
 *
 * Both kinds are counted, and every refusal is counted by its reason. The
 * ambient traffic is the first requester (CCarCtrl::GenerateOneRandomCar ->
 * CSpawnService::RequestCar): the same rings and gates, drawn from the same
 * tables, with the old dice gone. The reversal of the merge is the branch's
 * revert: the ambient path makes one call into the service, and the old
 * decision sites live in the parent commit (the per-frame dice are the thing
 * this ticket replaces, so no runtime switch keeps them alive here).
 *
 * The planner (engine-free, this header)
 * --------------------------------------
 *
 * SpawnOfferSites collects the sites within the request's ring band from the
 * index's spatial hash: the distance band is the old collector's own outer
 * filter (max(spawnDist + 70, spawnDist x 1.7), CPathFind::
 * CollectCarCreationCoors), the cone is the same dot test against the site's
 * own travel direction, and the water policy and the disabled flag are the
 * purpose's own filters. The main thread then resolves three live facts per
 * offered site and hands them to SpawnPlanSites:
 *
 *	the bubble	the ladder's own candidate (NearBubble.h): the arm
 *			is derived from it, the ray stays unspent (the
 *			primitive asks the full gate with the ray when it
 *			is about to create);
 *	the zone	the site's info zone, resolved live at request
 *			time (never cached): the requested class must be
 *			one the zone can carry;
 *	the reach	CPathFind::TestCoorsCloseness(target, site): the
 *			expensive search the primitive used to pay per
 *			creation (the engine caches the answer per site
 *			and target cell for a few frames, so the frame's
 *			calls share it).
 *
 * SpawnPlanSites drops the candidates the gates refuse (by reason), ranks what
 * is left - hidden arms first, then the emptier cell (spread), then the
 * nearest to the request's ring, then the rarity weight, then the site id -
 * and reserves the returned placements in the occupancy store while the plan
 * is outstanding, so two requests cannot plan the same connection at the same
 * time. The deep facts
 * (the live zone and the reachability's path search) resolve for the rank's
 * first SPAWN_SERVICE_DEEP_CANDIDATES only: the path search is the frame's
 * expensive call, and the prefix is twice the plan's four, so a few deep
 * refusals do not empty the plan. SpawnRunPlan tries the planned sites in
 * order through the caller's attempt (the primitive) and counts every
 * post-plan refusal by reason.
 *
 * The primitive (CarCtrl.cpp, CreateCarFromPlacement)
 * ---------------------------------------------------
 *
 * The keep list of the design: route and lane state; ground and water height;
 * physics and status; occupants with the caps and reserves; the final ground
 * and collision validation; the counters. The strips: the rarity dice (the
 * planner's rarity weight replaces it) and the camera decisions (the top-down
 * flag; the ladder and the planner own visibility). The tier (ESpawnTier)
 * passes in: the far layer's cars skip the near ring's birth cull and leave
 * in the far state. The driver contract holds for every kind: SetUpDriver
 * needs RANDOM_VEHICLE, so a kind that wants occupants creates with the
 * random tag (the near road-site kind does; the far promotion sets the tag
 * before its own SetUpDriver call).
 *
 * The equivalence, stated (the deliberate deltas)
 * -----------------------------------------------
 *
 * The gates are the old ones, one by one, with the deltas this ticket owns
 * and states rather than hides:
 *
 *	the candidate set	the plan's candidates are the sites whose
 *				midpoint is in the ring band, not the node
 *				pairs that bracket the ring (the index
 *				carries no per-node positions); the rank's
 *				"nearest to the ring" keeps the placement on
 *				the ring.
 *	the ped pool		the reserve is checked once per request as well
 *				as once per frame and once per car (the
 *				service's gate); the old path checked the
 *				frame and the car only.
 *	the top-down view	the ladder's bypass arm allows a creation the
 *				old birth cull's top-down disjunct refused;
 *				the ladder owns the view now.
 *	the rarity		the old dice thinned the placements
 *				probabilistically; the planner's weight is
 *				the rank's last key, so the earlier keys
 *				decide and the rate only breaks their ties.
 *	the plan's conservatism	a candidate whose midpoint arm is catchable is
 *				dropped (VISIBILITY) and never reaches the
 *				primitive, so a final-position occluded hint
 *				cannot rescue it; the old path attempted it.
 *	the RNG stream		the removed rarity draw and the moved budget
 *				gate consume the shared dice differently
 *				(the fill burst's own shift, ADR 0021).
 *
 * The telemetry (the ticket's list)
 * ---------------------------------
 *
 * Requests by purpose and by kind; refusals by reason (no site, occupancy,
 * visibility, zone, readiness, budget, pool, reachability); placements
 * created and removed by purpose, with the covered-fill creations in their
 * own row; the readiness waits (requests deferred for models and the
 * milliseconds spent waiting); the plan cost (candidates examined, placements
 * returned, microseconds per plan call); the conservation check (the cell
 * target sum against the global target); and the primitive's defect rows (the
 * ground, the air offset and the water rejections). The counters are fields
 * of SSpawnServiceTelemetry under one owner per counter: the engine's one
 * static instance, CSpawnService::ms_Telemetry (read live by
 * utils/caps-audit/live-counters.py at the offsets pinned in SpawnService.cpp).
 *
 * The module is engine-free: it reads the plain site records, the occupancy
 * store and the ladder, and every float here is a plain float. The engine
 * side (SpawnService.cpp) fills the facts from the camera, the zones, the
 * path finder and the store; the evidence program (utils/spawn-service,
 * core.cpp) drives the same functions over a synthetic graph.
 */

#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include "config.h"
#include "PopulationBudget.h"	/* CAR_SPAWN_ATTEMPTS, PED_POOL_RESERVE */
#include "SpawnSites.h"		/* CSpawnSiteIndex, SSpawnSite */
#include "SpawnOccupancy.h"	/* SSpawnOccupancyStore */
#include "NearBubble.h"		/* the ladder: SNearBubbleCandidate, the arms */
#include "PathLanes.h"		/* LANE_WIDTH (the primitive's lane arithmetic) */
#include "TrafficRings.h"	/* the ring table (the query's own factor) */

class CVehicle;
class CVector;
class CPed;

/* --- The vocabulary of the request envelope ----------------------------- */

/* The producer. Ambient is the traffic spawner (the first requester of
 * VICE-150); the rest name their tickets' migrations; debug is the probes. */
enum ESpawnPurpose
{
	SPAWN_PURPOSE_AMBIENT = 0,
	SPAWN_PURPOSE_PARKED,
	SPAWN_PURPOSE_RESPONDER,
	SPAWN_PURPOSE_POLICE,
	SPAWN_PURPOSE_MISSION,
	SPAWN_PURPOSE_SCENARIO,
	SPAWN_PURPOSE_SPECIAL,
	SPAWN_PURPOSE_DEBUG,
	SPAWN_PURPOSE_COUNT
};

/* The two creation kinds of the design's section 4. */
enum ESpawnKind
{
	SPAWN_KIND_ROAD_SITE = 0,	/* the planner: sites, occupancy, visibility */
	SPAWN_KIND_EXPLICIT,		/* the exact position, model and lifetime */
	SPAWN_KIND_COUNT
};

/* The tier of a creation. The far layer's cars skip the near ring's birth
 * cull and leave in the far state; the primitive must match the tier. */
enum ESpawnTier
{
	SPAWN_TIER_NEAR = 0,
	SPAWN_TIER_FAR,
	SPAWN_TIER_COUNT
};

/* The refusal reasons of the ticket, and nothing else: the primitives below
 * map every refusal onto one of them. NONE is the created state. */
enum ESpawnRefusal
{
	SPAWN_REFUSAL_NONE = 0,
	SPAWN_REFUSAL_NO_SITE,		/* nothing to offer, or the site cannot take the car */
	SPAWN_REFUSAL_OCCUPANCY,	/* the cell is at its capacity, the site is reserved, or something occupies the spot */
	SPAWN_REFUSAL_VISIBILITY,	/* the ladder, the birth cull, the camera rule or the speed-away test */
	SPAWN_REFUSAL_ZONE,		/* the site's live zone does not carry the requested class */
	SPAWN_REFUSAL_READINESS,	/* a model is not resident (the request's gate and the site's model) */
	SPAWN_REFUSAL_BUDGET,		/* the car target or the car budget */
	SPAWN_REFUSAL_POOL,		/* the pool's reserve */
	SPAWN_REFUSAL_REACHABILITY,	/* the site cannot reach the request's target */
	SPAWN_REFUSAL_COUNT
};

/* The water policy of a request: the ambient takes both (its water sites fall
 * back to a boat model), the far layer is land-only, a boat producer asks for
 * water. */
enum ESpawnSiteWater
{
	SPAWN_SITES_ANY = 0,
	SPAWN_SITES_LAND,
	SPAWN_SITES_WATER
};

/* The vehicle tag the driver contract needs (Vehicle.h: RANDOM_VEHICLE).
 * SpawnService.cpp pins it to the engine's own value. */
#define SPAWN_VEHICLE_TAG_RANDOM 1

/* The mission tag the script car opcode names (Vehicle.h: MISSION_VEHICLE).
 * SpawnService.cpp pins it to the engine's own value too. */
#define SPAWN_VEHICLE_TAG_MISSION 2

/* The script's headings are degrees; the core is engine-free, so the
 * conversion is its own (common.h's DEGTORAD is the engine's). */
#define SPAWN_SCRIPT_PI 3.14159265358979323846f

/* --- The telemetry ------------------------------------------------------ */

/*
 * The service's counters, one owner per counter: the engine's single static
 * instance (CSpawnService::ms_Telemetry). The fields the live reader reads sit
 * at pinned offsets (static_asserts in SpawnService.cpp); the arrays are in
 * the purpose/kind/reason enumerations' own order.
 *
 * The refusal rows count one event per increment: a request's own gate, a
 * candidate the plan dropped, or an attempt the primitive refused. The
 * request rows count every RequestCar/RequestExplicit call; the created and
 * removed rows count the placements, by purpose; the covered-fill row counts
 * the creations the screen fade covered (the fill burst's own work, its own
 * row of the ticket). The plan rows are the cost: one call row per plan, the
 * candidates examined, the placements returned and the total microseconds
 * (the caller times the call; the core only accumulates). The conservation
 * rows are set by the check (the director calls it later); a breach is a
 * defect row. The defect rows are the primitive's own: the ground casts that
 * found nothing, the offsets over the air guard, and the water casts that
 * failed.
 */
struct SSpawnServiceTelemetry
{
	uint32_t m_nRequestsByPurpose[SPAWN_PURPOSE_COUNT];
	uint32_t m_nRequestsByKind[SPAWN_KIND_COUNT];
	uint32_t m_nRefusalsByReason[SPAWN_REFUSAL_COUNT];
	uint32_t m_nCreatedByPurpose[SPAWN_PURPOSE_COUNT];
	uint32_t m_nRemovedByPurpose[SPAWN_PURPOSE_COUNT];
	uint32_t m_nCoveredFillCreations;
	uint32_t m_nReadinessWaits;
	uint32_t m_nReadinessWaitMs;
	uint32_t m_aReadinessSinceMs[SPAWN_PURPOSE_COUNT];	/* 0: not waiting */
	uint32_t m_nPlanCalls;
	uint32_t m_nPlanCandidates;
	uint32_t m_nPlanPlacements;
	uint64_t m_nPlanMicros;
	uint32_t m_nConservationCellSum;
	uint32_t m_nConservationGlobal;
	uint32_t m_nConservationBreaches;
	uint32_t m_nGroundRejections;
	uint32_t m_nAirOffsetRejections;
	uint32_t m_nWaterRejections;
};

static inline void
SpawnTelemetryReset(SSpawnServiceTelemetry *tel)
{
	uint8_t *p = (uint8_t *)tel;
	size_t i;

	for (i = 0; i < sizeof(*tel); i++)
		p[i] = 0;
}

/* One request, counted by its purpose and its kind. */
static inline void
SpawnRequestBegin(SSpawnServiceTelemetry *tel, ESpawnPurpose purpose, ESpawnKind kind)
{
	if (purpose >= 0 && purpose < SPAWN_PURPOSE_COUNT)
		tel->m_nRequestsByPurpose[purpose]++;
	if (kind >= 0 && kind < SPAWN_KIND_COUNT)
		tel->m_nRequestsByKind[kind]++;
}

/* One refusal event, by reason. */
static inline void
SpawnNoteRefusal(SSpawnServiceTelemetry *tel, ESpawnRefusal reason)
{
	if (reason > SPAWN_REFUSAL_NONE && reason < SPAWN_REFUSAL_COUNT)
		tel->m_nRefusalsByReason[reason]++;
}

/* One created (or removed) placement, by purpose. The run counts its own
 * creations through this; the engine's explicit kind and its cull call it
 * directly. */
static inline void
SpawnNoteCreated(SSpawnServiceTelemetry *tel, ESpawnPurpose purpose)
{
	if (purpose >= 0 && purpose < SPAWN_PURPOSE_COUNT)
		tel->m_nCreatedByPurpose[purpose]++;
}

static inline void
SpawnNoteRemoval(SSpawnServiceTelemetry *tel, ESpawnPurpose purpose)
{
	if (purpose >= 0 && purpose < SPAWN_PURPOSE_COUNT)
		tel->m_nRemovedByPurpose[purpose]++;
}

/* The plan cost of one call: the caller times it, the core accumulates. */
static inline void
SpawnNotePlanCost(SSpawnServiceTelemetry *tel, uint64_t micros)
{
	tel->m_nPlanCalls++;
	tel->m_nPlanMicros += micros;
}

/* The conservation check (the director's later call): the cell target sum
 * against the global target; a mismatch is a defect row. */
static inline void
SpawnNoteConservation(SSpawnServiceTelemetry *tel, uint32_t cellTargetSum, uint32_t globalTarget)
{
	tel->m_nConservationCellSum = cellTargetSum;
	tel->m_nConservationGlobal = globalTarget;
	if (cellTargetSum != globalTarget)
		tel->m_nConservationBreaches++;
}

/* The primitive's defect rows. */
static inline void
SpawnNoteGroundRejection(SSpawnServiceTelemetry *tel)
{
	tel->m_nGroundRejections++;
}

static inline void
SpawnNoteAirOffsetRejection(SSpawnServiceTelemetry *tel)
{
	tel->m_nAirOffsetRejections++;
}

static inline void
SpawnNoteWaterRejection(SSpawnServiceTelemetry *tel)
{
	tel->m_nWaterRejections++;
}

/* --- The ring query and the offer --------------------------------------- */

/* The planner's own band: the old collector's outer filter, exactly
 * (max(spawnDist + 70, spawnDist x 1.7), CPathFind::CollectCarCreationCoors).
 * The query radius is it, and the band's slack is the same margin around the
 * ring; the bubble (90 m) is what keeps the very close sites out of the
 * plan. */
#define SPAWN_RING_DIST_ADD		70.0f
#define SPAWN_RING_DIST_FACTOR		1.7f

static inline float
SpawnRingMaxDist(float spawnDist)
{
	float add = spawnDist + SPAWN_RING_DIST_ADD;
	float factor = spawnDist * SPAWN_RING_DIST_FACTOR;

	return factor > add ? factor : add;
}

static inline float
SpawnRingDistSlack(float spawnDist)
{
	float add = SPAWN_RING_DIST_ADD;
	float factor = spawnDist * (SPAWN_RING_DIST_FACTOR - 1.0f);

	return factor > add ? factor : add;
}

/* The planner's own bounds: the placements one plan returns and reserves (the
 * old placement loop's own four attempts), and the offer bound per request
 * (the shipped map holds a few dozen sites in a band; the bound is the dense
 * worst case). */
#define SPAWN_PLAN_MAX_PLACEMENTS	CAR_SPAWN_ATTEMPTS
#define SPAWN_SERVICE_MAX_CANDIDATES	512
/* The rank's prefix whose deep facts (the zone and the path search) resolve;
 * 2 x the plan's four, so a few deep refusals do not empty the plan. */
#define SPAWN_SERVICE_DEEP_CANDIDATES	(2 * SPAWN_PLAN_MAX_PLACEMENTS)

/* The occupancy capacity of one presence cell: the planner's coarse filter
 * (the 16 m grid's lanes at the ring held 2..3 cars in the stock traffic; the
 * final collision query at creation stays the true guard). */
#define SPAWN_SERVICE_CELL_CAPACITY	3

/* One request's site query: the target, the ring it aims at, the cone it
 * looks through and the site policy of its purpose. */
struct SSpawnRingQuery
{
	float targetX, targetY;
	float spawnDist;	/* the ring the request aims at */
	float dirX, dirY;	/* the cone's axis (the request's travel direction) */
	float angleLimit;	/* the collector's own limit */
	bool forward;		/* true: dot > limit; false: dot <= limit */
	uint8_t water;		/* ESpawnSiteWater */
	bool allowDisabled;	/* the cops' wanted rule (the old ignoreDisabled) */
};

/* One offered site: the index id, its distance to the target, its error
 * against the ring it belongs to (the water ring for a boat site) and the
 * rarity weight the rank reads. */
struct SSpawnOffer
{
	int32_t site;
	float dist;
	float ringError;
	uint8_t boat;
	uint8_t spawnRate;
};

/*
 * The candidates of one request, from the index's spatial hash: the ring
 * band, the cone, the water policy and the disabled flag, in that order. The
 * answer is in the hash's walk order (a truncated answer is the index's
 * contract, and the count cannot exceed SPAWN_SERVICE_MAX_CANDIDATES); the
 * rank imposes the plan's own order. Zero is a legal answer (nothing in the
 * band): the request refuses with NO_SITE.
 */
static inline int32_t
SpawnOfferSites(const CSpawnSiteIndex &index, const SSpawnRingQuery &query,
	SSpawnOffer *out, int32_t maxOut)
{
	int32_t ids[SPAWN_SERVICE_MAX_CANDIDATES];
	int32_t idCap = SPAWN_SERVICE_MAX_CANDIDATES;
	int32_t numIds, numOffers = 0;
	int32_t i;

	if (index.GetNumSites() == 0 || maxOut <= 0)
		return 0;
	if (idCap > index.GetNumSites())
		idCap = index.GetNumSites();
	numIds = index.CollectSitesNear(query.targetX, query.targetY,
		SpawnRingMaxDist(query.spawnDist), ids, idCap);

	for (i = 0; i < numIds && numOffers < maxOut; i++) {
		const SSpawnSite *pSite = &index.GetSites()[ids[i]];
		bool boat = (pSite->flags & SPAWN_SITE_BOAT) != 0;
		float dx, dy, dist, dot, ringDist, slack;

		/* The purpose's own site policy. */
		if ((pSite->flags & SPAWN_SITE_DISABLED) && !query.allowDisabled)
			continue;
		if (query.water == SPAWN_SITES_LAND && boat)
			continue;
		if (query.water == SPAWN_SITES_WATER && !boat)
			continue;

		dx = pSite->x - query.targetX;
		dy = pSite->y - query.targetY;
		dist = sqrtf(dx * dx + dy * dy);
		/* The index's query walks cells; the band is exact. */
		if (dist > SpawnRingMaxDist(query.spawnDist))
			continue;
		/* The cone: the collector's own test, on the site's midpoint (the
		 * old crossing point lies between the two nodes, and the cone is a
		 * coarse gate). A site exactly on the target takes no side. */
		if (dist > 0.0001f) {
			dot = (dx / dist) * query.dirX + (dy / dist) * query.dirY;
			if (query.forward ? !(dot > query.angleLimit) : !(dot <= query.angleLimit))
				continue;
		}
		/* The ring the site belongs to: the water ring for a boat site. */
		ringDist = boat ? query.spawnDist * RING_BOAT_SPAWN_FACTOR : query.spawnDist;
		slack = SpawnRingDistSlack(query.spawnDist);
		if ((dist > ringDist ? dist - ringDist : ringDist - dist) > slack)
			continue;
		out[numOffers].site = ids[i];
		out[numOffers].dist = dist;
		out[numOffers].ringError = dist > ringDist ? dist - ringDist : ringDist - dist;
		out[numOffers].boat = boat ? 1 : 0;
		out[numOffers].spawnRate = pSite->spawnRate;
		numOffers++;
	}
	return numOffers;
}

/* --- The live facts of one offered site --------------------------------- */

/*
 * The cheap facts of the rank (per offer): the ladder's own candidate for the
 * site's midpoint (NearBubble.h). The rank derives the arm from it and never
 * spends a ray; the primitive asks the full gate, with the ray, when it is
 * about to create.
 */
/* (the array of SNearBubbleCandidate is the rank's own input) */

/*
 * The deep facts, resolved for the ranked prefix only: the live zone
 * compatibility (the site's info zone resolved at request time; the requested
 * class must be one the zone carries) and the reachability of the request's
 * target (the planner's contract: the path search left the per-creation
 * loop, and its price is bounded by the prefix).
 */
struct SSpawnSiteDeepFacts
{
	bool zoneOk;
	bool reachOk;
};

/* The engine's (or the evidence program's) resolution of one candidate's deep
 * facts, by the offer index. */
typedef void (*SpawnDeepFactsFn)(void *user, int32_t offerIndex, SSpawnSiteDeepFacts *facts);

/* --- The plan and its rank ---------------------------------------------- */

/* One planned placement: the offered site it came from, its id and the
 * visibility arm the rank used. */
struct SSpawnPlanSite
{
	int32_t offer;		/* the index into the caller's offers/facts arrays */
	int32_t site;
	float ringError;
	uint8_t arm;
};

/* One plan: up to SPAWN_PLAN_MAX_PLACEMENTS ranked sites, those the run
 * tries in order, and the refusal the filter recorded first (the plan's own
 * answer when the run creates nothing). */
struct SSpawnPlan
{
	int32_t count;
	ESpawnRefusal refusal;
	SSpawnPlanSite sites[SPAWN_PLAN_MAX_PLACEMENTS];
};

/* The visibility rank of one arm, the rank's first key: the hidden arms
 * first, the arms that need a confirmation ray next (their answer is not
 * known yet), the covered and on-screen arms last. The ladder allows no arm
 * below the bubble, so a valid candidate is one of these. */
static inline int32_t
SpawnVisibilityRank(uint8_t arm)
{
	switch (arm) {
	case NEAR_BUBBLE_ARM_SOFT:
	case NEAR_BUBBLE_ARM_FADE_DISTANCE:
		return 0;
	case NEAR_BUBBLE_ARM_OCCLUDER:
	case NEAR_BUBBLE_ARM_RAY:
		return 1;
	case NEAR_BUBBLE_ARM_FADE:
	case NEAR_BUBBLE_ARM_BYPASS:
	case NEAR_BUBBLE_ARM_ONSCREEN_BAND:
		return 2;
	default:
		return 3;	/* refused (never ranked) */
	}
}

/* The rank's order: the hidden arms first, then the emptier cell (spread),
 * then the nearest to the request's ring, then the higher rarity weight, then
 * the site id (deterministic). The rarity is the weight the old dice became
 * (the ticket's decision, stated): it is the rank's last key, so the order's
 * own keys decide first and a main street wins over a side street only where
 * everything else is equal. It is deliberately not a pass/fail filter - a
 * filter would thin the plan's candidates by the site's own rate, which is
 * the dice again - and the shipped rates the key works with are printed by
 * the dump path (utils/spawn-service). True when `a` sorts before `b`. */
static inline bool
SpawnPlanSiteSortsBefore(int32_t rankA, int32_t occupancyA, float ringErrorA, uint8_t rateA, int32_t siteA,
	int32_t rankB, int32_t occupancyB, float ringErrorB, uint8_t rateB, int32_t siteB)
{
	if (rankA != rankB)
		return rankA < rankB;
	if (occupancyA != occupancyB)
		return occupancyA < occupancyB;
	if (ringErrorA != ringErrorB)
		return ringErrorA < ringErrorB;
	if (rateA != rateB)
		return rateA > rateB;
	return siteA < siteB;
}

/*
 * The planner's decision: filter the offered sites by the cheap facts, rank
 * the valid ones, resolve the deep facts of the ranked prefix, and reserve
 * the returned placements in the store.
 *
 * The cheap filter drops a candidate at its first failing gate, counted by
 * reason: a reserved site or a cell at its capacity (OCCUPANCY) and an arm
 * the ladder refuses (VISIBILITY). The deep gates then run in the ticket's
 * order for the ranked prefix: a zone the requested class is not carried in
 * (ZONE) and an unreachable site (REACHABILITY).
 *
 * The returned placements are reserved for SPAWN_OCCUPANCY_RESERVE_MS while
 * the plan is outstanding, so two requests cannot plan the same connection at
 * the same time; the run releases them (a created site's own at once - the
 * created car's presence in the store is the guard from then on - or the
 * expiry does). The plan's own `refusal` is the first drop reason (NONE when
 * something survived); a request whose offer list is empty counts NO_SITE and
 * returns an empty plan.
 */
static inline int32_t
SpawnPlanSites(const CSpawnSiteIndex &index, const SSpawnOffer *offers,
	const SNearBubbleCandidate *bubbles, int32_t numOffers,
	SSpawnOccupancyStore *store, uint32_t nowMs,
	SpawnDeepFactsFn deepFacts, void *deepUser,
	SSpawnPlan *plan, SSpawnServiceTelemetry *tel)
{
	int32_t i, nb, examined;
	/* The cheap rank's top-K: the deep-facts prefix (the plan's four plus
	 * the backfill's four), so four deep refusals cannot empty the plan. */
	int32_t bestOffer[SPAWN_SERVICE_DEEP_CANDIDATES];
	int32_t bestOcc[SPAWN_SERVICE_DEEP_CANDIDATES];
	uint8_t bestArm[SPAWN_SERVICE_DEEP_CANDIDATES];
	float bestErr[SPAWN_SERVICE_DEEP_CANDIDATES];
	uint8_t bestRate[SPAWN_SERVICE_DEEP_CANDIDATES];

	plan->count = 0;
	plan->refusal = SPAWN_REFUSAL_NONE;
	if (tel)
		tel->m_nPlanCandidates += numOffers;

	if (numOffers <= 0) {
		plan->refusal = SPAWN_REFUSAL_NO_SITE;
		if (tel)
			SpawnNoteRefusal(tel, SPAWN_REFUSAL_NO_SITE);
		return 0;
	}

	/* Stage one: the cheap gates and the rank. */
	nb = 0;
	for (i = 0; i < numOffers; i++) {
		const SSpawnSite *pSite = &index.GetSites()[offers[i].site];
		ESpawnRefusal refusal = SPAWN_REFUSAL_NONE;
		uint8_t arm = (uint8_t)NearBubbleArmFor(bubbles[i]);
		int32_t occupancy = 0;
		int32_t rank;

		if (store && store->SiteReserved(offers[i].site, nowMs))
			refusal = SPAWN_REFUSAL_OCCUPANCY;
		else {
			occupancy = store ? store->CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, pSite->x, pSite->y) : 0;
			if (occupancy >= SPAWN_SERVICE_CELL_CAPACITY)
				refusal = SPAWN_REFUSAL_OCCUPANCY;
			else if (arm == NEAR_BUBBLE_ARM_NONE)
				refusal = SPAWN_REFUSAL_VISIBILITY;
		}
		if (refusal != SPAWN_REFUSAL_NONE) {
			if (plan->refusal == SPAWN_REFUSAL_NONE)
				plan->refusal = refusal;
			if (tel)
				SpawnNoteRefusal(tel, refusal);
			continue;
		}
		rank = SpawnVisibilityRank(arm);

		/* The bounded top-K insert (K = SPAWN_SERVICE_DEEP_CANDIDATES): a
		 * full buffer takes the candidate only when it sorts before the
		 * buffer's worst entry; then the candidate walks up while it sorts
		 * before the entry above it. */
		{
			bool insert = nb < SPAWN_SERVICE_DEEP_CANDIDATES;

			if (!insert)
				insert = SpawnPlanSiteSortsBefore(rank, occupancy, offers[i].ringError, offers[i].spawnRate, offers[i].site,
					SpawnVisibilityRank(bestArm[SPAWN_SERVICE_DEEP_CANDIDATES-1]),
					bestOcc[SPAWN_SERVICE_DEEP_CANDIDATES-1],
					bestErr[SPAWN_SERVICE_DEEP_CANDIDATES-1],
					bestRate[SPAWN_SERVICE_DEEP_CANDIDATES-1],
					offers[bestOffer[SPAWN_SERVICE_DEEP_CANDIDATES-1]].site);
			if (insert) {
				int32_t at = nb < SPAWN_SERVICE_DEEP_CANDIDATES ? nb : SPAWN_SERVICE_DEEP_CANDIDATES - 1;
				while (at > 0 &&
					SpawnPlanSiteSortsBefore(rank, occupancy, offers[i].ringError, offers[i].spawnRate, offers[i].site,
						SpawnVisibilityRank(bestArm[at-1]), bestOcc[at-1], bestErr[at-1], bestRate[at-1], offers[bestOffer[at-1]].site)) {
					bestOffer[at] = bestOffer[at-1];
					bestOcc[at] = bestOcc[at-1];
					bestArm[at] = bestArm[at-1];
					bestErr[at] = bestErr[at-1];
					bestRate[at] = bestRate[at-1];
					at--;
				}
				bestOffer[at] = i;
				bestOcc[at] = occupancy;
				bestArm[at] = arm;
				bestErr[at] = offers[i].ringError;
				bestRate[at] = offers[i].spawnRate;
				if (nb < SPAWN_SERVICE_DEEP_CANDIDATES)
					nb++;
			}
		}
	}

	/* Stage two: the deep facts of the ranked prefix, and the reservations.
	 * The prefix bound keeps the path search out of the per-creation loop
	 * and out of the whole offer list. */
	for (i = 0, examined = 0; i < nb &&
		examined < SPAWN_SERVICE_DEEP_CANDIDATES && plan->count < SPAWN_PLAN_MAX_PLACEMENTS; i++, examined++) {
		int32_t offerIndex = bestOffer[i];
		SSpawnSiteDeepFacts deep;
		ESpawnRefusal refusal = SPAWN_REFUSAL_NONE;

		deep.zoneOk = true;
		deep.reachOk = true;
		if (deepFacts)
			deepFacts(deepUser, offerIndex, &deep);
		if (!deep.zoneOk)
			refusal = SPAWN_REFUSAL_ZONE;
		else if (!deep.reachOk)
			refusal = SPAWN_REFUSAL_REACHABILITY;
		else if (store && !store->ReserveSite(offers[offerIndex].site, nowMs))
			refusal = SPAWN_REFUSAL_OCCUPANCY;
		if (refusal != SPAWN_REFUSAL_NONE) {
			if (plan->refusal == SPAWN_REFUSAL_NONE)
				plan->refusal = refusal;
			if (tel)
				SpawnNoteRefusal(tel, refusal);
			continue;
		}
		plan->sites[plan->count].offer = offerIndex;
		plan->sites[plan->count].site = offers[offerIndex].site;
		plan->sites[plan->count].ringError = offers[offerIndex].ringError;
		plan->sites[plan->count].arm = bestArm[i];
		plan->count++;
	}
	if (plan->count == 0 && plan->refusal == SPAWN_REFUSAL_NONE)
		plan->refusal = SPAWN_REFUSAL_OCCUPANCY;
	if (tel)
		tel->m_nPlanPlacements += plan->count;
	return plan->count;
}

/* --- The run: the plan through the primitive ----------------------------- */

/*
 * The caller's attempt at one planned site: the engine resolves the model and
 * calls the primitive; the evidence program scripts the answer. The return is
 * the refusal (NONE: created). `pArm` starts as the arm the rank read and the
 * attempt may overwrite it with the arm its own gate resolved at the final
 * position - the creation's covered-fill row reads the attempt's arm, not the
 * midpoint's.
 */
typedef ESpawnRefusal (*SpawnTryFn)(void *user, const SSpawnPlanSite &site, uint8_t *pArm);

/*
 * Try the plan's sites in order. Every attempt that refuses is counted by its
 * reason; the site's reservation is released either way (the created site's
 * siblings too, so an abandoned plan frees the street at once). A creation
 * counts by purpose, and a creation the screen fade covered moves the
 * covered-fill row (the arm the attempt's gate resolved; the plan's midpoint
 * arm is the fallback). The return is NONE on a creation, else the last
 * attempt's refusal (or the plan's own first drop reason when there was
 * nothing to try).
 */
static inline ESpawnRefusal
SpawnRunPlan(const SSpawnPlan &plan, SSpawnOccupancyStore *store, SpawnTryFn attempt, void *user,
	SSpawnServiceTelemetry *tel, ESpawnPurpose purpose)
{
	int32_t i, j;
	ESpawnRefusal last = plan.refusal;

	if (plan.count == 0)
		return last;
	for (i = 0; i < plan.count; i++) {
		uint8_t arm = plan.sites[i].arm;
		ESpawnRefusal refusal = attempt(user, plan.sites[i], &arm);
		if (store)
			store->ReleaseSite(plan.sites[i].site);
		if (refusal == SPAWN_REFUSAL_NONE) {
			if (tel) {
				SpawnNoteCreated(tel, purpose);
				if (arm == NEAR_BUBBLE_ARM_FADE)
					tel->m_nCoveredFillCreations++;
			}
			for (j = i + 1; j < plan.count; j++)
				if (store)
					store->ReleaseSite(plan.sites[j].site);
			return SPAWN_REFUSAL_NONE;
		}
		last = refusal;
		if (tel)
			SpawnNoteRefusal(tel, refusal);
	}
	return last;
}

/* --- The request gates --------------------------------------------------- */

/* The facts of the function gate: the car budget (NumRandomCars against the
 * target and NumCarsInUse against the top) and the ped pool's free spaces
 * (the crew of one car must fit the reserve). */
struct SSpawnBudgetFacts
{
	int32_t randomCars;		/* CCarCtrl::NumRandomCars */
	float randomTarget;		/* CCarCtrl::GetRandomCarTarget() */
	int32_t carsInUse;		/* CCarCtrl::NumCarsInUse() */
	int32_t carBudget;		/* CCarCtrl::MaxNumberOfCarsInUse */
	int32_t pedFreeSpaces;		/* CPools::GetPedPool()->GetNoOfFreeSpaces() */
};

/*
 * The gate before the plan (and before an explicit creation): the force
 * bypass steps over everything, because a mission cannot fail on a scheduling
 * decision; otherwise the car target, the car budget and the pool reserve
 * refuse in that order. The gate counts its refusal itself; the request's own
 * readiness gate is separate (the model is not a budget).
 */
static inline ESpawnRefusal
SpawnFunctionGate(const SSpawnBudgetFacts &facts, bool force, SSpawnServiceTelemetry *tel)
{
	ESpawnRefusal refusal = SPAWN_REFUSAL_NONE;

	if (force)
		return SPAWN_REFUSAL_NONE;
	if (facts.randomTarget <= 0.0f || facts.carBudget <= 0)
		refusal = SPAWN_REFUSAL_BUDGET;
	else if ((float)facts.randomCars >= facts.randomTarget)
		refusal = SPAWN_REFUSAL_BUDGET;
	else if (facts.carsInUse >= facts.carBudget)
		refusal = SPAWN_REFUSAL_BUDGET;
	else if (facts.pedFreeSpaces < PED_POOL_RESERVE)
		refusal = SPAWN_REFUSAL_POOL;
	if (refusal != SPAWN_REFUSAL_NONE)
		SpawnNoteRefusal(tel, refusal);
	return refusal;
}

/*
 * The readiness wait of one purpose: the requester asked for a model the
 * streamer does not hold yet. The first deferral starts the clock; the first
 * request that gets past the gate adds the milliseconds the wait spent (the
 * game clock; the trickle asks once a frame). A model that never arrives
 * leaves the clock running and the wait row counts the deferrals.
 */
static inline void
SpawnReadinessDefer(SSpawnServiceTelemetry *tel, ESpawnPurpose purpose, uint32_t nowMs)
{
	if (purpose < 0 || purpose >= SPAWN_PURPOSE_COUNT)
		return;
	tel->m_nReadinessWaits++;
	if (tel->m_aReadinessSinceMs[purpose] == 0)
		tel->m_aReadinessSinceMs[purpose] = nowMs ? nowMs : 1;
	SpawnNoteRefusal(tel, SPAWN_REFUSAL_READINESS);
}

static inline void
SpawnReadinessReady(SSpawnServiceTelemetry *tel, ESpawnPurpose purpose, uint32_t nowMs)
{
	if (purpose < 0 || purpose >= SPAWN_PURPOSE_COUNT)
		return;
	if (tel->m_aReadinessSinceMs[purpose] != 0) {
		tel->m_nReadinessWaitMs += nowMs - tel->m_aReadinessSinceMs[purpose];
		tel->m_aReadinessSinceMs[purpose] = 0;
	}
}

/* --- The explicit kind --------------------------------------------------- */

/* The explicit request: the exact position, the model, the heading and the
 * lifetime the caller gives (a script, a mission, a set piece). The tag is
 * the vehicle's VehicleCreatedBy; 0 asks for the random tag (RANDOM_VEHICLE),
 * the one the driver contract needs: a caller that does not name a tag lets
 * the service pick the traffic's own, so SetUpDriver works on the car it just
 * made (a script that wants a mission car names MISSION_VEHICLE itself). */
struct SSpawnExplicitRequest
{
	ESpawnPurpose purpose;
	bool force;
	int32_t carModel;
	float x, y, z;
	float dirX, dirY;
	uint32_t lifetimeMs;	/* 0: the fresh-spawn grace */
	uint8_t createdBy;	/* 0: SPAWN_VEHICLE_TAG_RANDOM */
	bool clearSpace;	/* the script's ClearSpaceForMissionEntity before the add */
};

/* The engine's placement of an explicit car: the position is the request's,
 * unchanged (the plan and the ring never touch it); the lifetime is floored
 * by the fresh-spawn grace, so a scripted car is not culled on its way in;
 * the tag is normalized. */
struct SSpawnExplicitDecision
{
	float x, y, z;
	float dirX, dirY;
	uint32_t protectMs;
	uint8_t createdBy;
};

static inline void
SpawnDecideExplicit(const SSpawnExplicitRequest &request, SSpawnExplicitDecision *decision)
{
	decision->x = request.x;
	decision->y = request.y;
	decision->z = request.z;
	decision->dirX = request.dirX;
	decision->dirY = request.dirY;
	decision->protectMs = request.lifetimeMs < NEAR_BUBBLE_GRACE_MS ? NEAR_BUBBLE_GRACE_MS : request.lifetimeMs;
	decision->createdBy = request.createdBy ? request.createdBy : SPAWN_VEHICLE_TAG_RANDOM;
}

/* --- The explicit pedestrian arm (VICE-157) ------------------------------ */

/*
 * The script and mission opcodes are explicit requests (VICE-157): the model
 * and the placement are the caller's, the call is synchronous and the handle
 * comes back in the same frame. Every one of them forces - a mission cannot
 * fail on a scheduling decision - while the ledger counts them by the MISSION
 * or SPECIAL purpose (a mission script's actor is the mission's; the idle
 * scripts' is a special actor). The service owns the creation and the world
 * insert; the opcode keeps its own wiring after the handle returns (the seat,
 * the autopilot, the mission list and the mission counters), so the clean-up
 * paths and the counters are untouched.
 *
 * The ped arm exists because the script opcodes create peds as well as cars:
 * a standing ped (COMMAND_CREATE_CHAR, COMMAND_CREATE_RANDOM_CHAR) and an
 * in-car ped (the inside-car and passenger opcodes). The random in-car
 * opcodes do not name a model by design - the population chooses it - so
 * `pedModel` -1 asks the service for CPopulation::AddPedInCar, the
 * population's own path, instead of duplicating its choice here.
 */

/* Where an explicit ped is created: standing at the request's position, or in
 * the request's vehicle as its driver or a passenger. */
enum ESpawnPedPlace
{
	SPAWN_PED_PLACE_STANDING = 0,
	SPAWN_PED_PLACE_DRIVER,
	SPAWN_PED_PLACE_PASSENGER
};

/* The explicit ped request: the exact position or vehicle, the type and the
 * model the script gave. The standing arm's `clearSpace` is the script's
 * ClearSpaceForMissionEntity, which must run before the world insert; the
 * service owns that order. The mission flags and the world insert are the
 * service's; the opcode keeps the seat wiring and its counters. */
struct SSpawnExplicitPedRequest
{
	ESpawnPurpose purpose;
	bool force;
	uint8_t place;		/* ESpawnPedPlace */
	int32_t pedType;	/* the constructor's selector (ePedType/eCopType) */
	int32_t pedModel;	/* the model or cop type; -1: the population's in-car choice */
	CVehicle *vehicle;	/* the in-car arms */
	float x, y, z;		/* the standing arm */
	bool clearSpace;
};

/* The facts the explicit admission needs from the frame: the budget counts
 * and the actor pool's own free space (the vehicle pool for a car, the ped
 * pool for a ped). */
struct SSpawnExplicitFacts
{
	SSpawnBudgetFacts budget;
	bool poolHasSpace;
};

/*
 * The car arm's admission, the one path CSpawnService::RequestExplicit takes:
 * the request is counted by its purpose and kind, the function gate runs (the
 * force arm steps over the budgets and the reserve), the pool's last slot
 * refuses (physical, even for force), and a request without a model is a bad
 * request. The placement decision is resolved only for an admitted request,
 * so a refused request never reaches the creation.
 */
static inline bool
SpawnExplicitCarAdmit(const SSpawnExplicitRequest &request, const SSpawnExplicitFacts &facts,
	SSpawnExplicitDecision *decision, SSpawnServiceTelemetry *tel)
{
	SpawnRequestBegin(tel, request.purpose, SPAWN_KIND_EXPLICIT);
	if (SpawnFunctionGate(facts.budget, request.force, tel) != SPAWN_REFUSAL_NONE)
		return false;
	if (!facts.poolHasSpace) {
		SpawnNoteRefusal(tel, SPAWN_REFUSAL_POOL);
		return false;
	}
	if (request.carModel < 0) {
		SpawnNoteRefusal(tel, SPAWN_REFUSAL_NO_SITE);
		return false;
	}
	SpawnDecideExplicit(request, decision);
	return true;
}

/* The ped arm's admission: the same rows and the same force bypass. A
 * standing request without a model is a bad request; an in-car request
 * without its vehicle is one too. */
static inline bool
SpawnExplicitPedAdmit(const SSpawnExplicitPedRequest &request, const SSpawnExplicitFacts &facts,
	SSpawnServiceTelemetry *tel)
{
	bool inCar;

	SpawnRequestBegin(tel, request.purpose, SPAWN_KIND_EXPLICIT);
	if (SpawnFunctionGate(facts.budget, request.force, tel) != SPAWN_REFUSAL_NONE)
		return false;
	if (!facts.poolHasSpace) {
		SpawnNoteRefusal(tel, SPAWN_REFUSAL_POOL);
		return false;
	}
	if (request.place > SPAWN_PED_PLACE_PASSENGER) {
		SpawnNoteRefusal(tel, SPAWN_REFUSAL_NO_SITE);
		return false;
	}
	inCar = request.place != SPAWN_PED_PLACE_STANDING;
	if (inCar ? request.vehicle == 0 : request.pedModel < 0) {
		SpawnNoteRefusal(tel, SPAWN_REFUSAL_NO_SITE);
		return false;
	}
	return true;
}

/* --- The script opcodes' argument sets ----------------------------------- */

/*
 * The script opcodes' argument sets, mapped to the explicit requests (the one
 * construction the engine and the offline harness share):
 *
 *	COMMAND_CREATE_CAR		the model and the position; no heading
 *					(the opcode's own orientation)
 *	CREATE_RANDOM_CAR_FOR_CAR_PARK	the model, the position and the heading;
 *					the traffic's tag (a car park car is not
 *					a mission car)
 *	COMMAND_CREATE_CHAR		the type, the model and the position
 *	CREATE_RANDOM_CHAR		the population's chosen civilian and the
 *					position
 *	CREATE_CHAR_INSIDE_CAR		the type, the model and the vehicle (the
 *					driver's seat)
 *	CREATE_CHAR_AS_PASSENGER	the type, the model and the vehicle
 *	CREATE_RANDOM_CHAR_AS_DRIVER	the population's model, the vehicle
 *	CREATE_RANDOM_CHAR_AS_PASSENGER	the population's model, the vehicle
 *
 * The car position below is the final one: the opcode resolves the ground and
 * the model's centre-to-base offset before the request, so the service sets
 * the request's z unchanged (the explicit kind's contract). The heading is
 * the request's direction; the identity is the default. The ped position is
 * the script's own; the service resolves its ground, exactly as the old
 * opcodes did.
 */

struct SSpawnScriptCarArgs
{
	int32_t carModel;
	float x, y, z;
	float headingDegrees;	/* the car park opcode's heading */
	bool hasHeading;
	bool parked;		/* the car park's traffic tag */
	bool missionScript;
};

static inline void
SpawnScriptCarRequest(const SSpawnScriptCarArgs &args, SSpawnExplicitRequest *request)
{
	request->purpose = args.missionScript ? SPAWN_PURPOSE_MISSION : SPAWN_PURPOSE_SPECIAL;
	/* Every script creator forces: a mission cannot fail on a scheduling
	 * decision. The car park's old NumRandomCars cap was the pool's own
	 * number (MAX_RANDOM_CARS), never the traffic target, so the force
	 * keeps its behaviour and the pool's last slot stays the only refusal. */
	request->force = true;
	request->carModel = args.carModel;
	request->x = args.x;
	request->y = args.y;
	request->z = args.z;
	if (args.hasHeading) {
		float radians = args.headingDegrees * (SPAWN_SCRIPT_PI / 180.0f);
		request->dirX = cosf(radians);
		request->dirY = sinf(radians);
	} else {
		request->dirX = 1.0f;
		request->dirY = 0.0f;
	}
	request->lifetimeMs = 0;
	request->createdBy = args.parked ? SPAWN_VEHICLE_TAG_RANDOM : SPAWN_VEHICLE_TAG_MISSION;
	request->clearSpace = true;	/* both car opcodes clear the spot first */
}

struct SSpawnScriptPedArgs
{
	int32_t pedType;
	int32_t pedModel;	/* -1: the population's in-car choice */
	uint8_t place;		/* ESpawnPedPlace */
	float x, y, z;
	bool missionScript;
};

static inline void
SpawnScriptPedRequest(const SSpawnScriptPedArgs &args, SSpawnExplicitPedRequest *request)
{
	request->purpose = args.missionScript ? SPAWN_PURPOSE_MISSION : SPAWN_PURPOSE_SPECIAL;
	request->force = true;
	request->place = args.place;
	request->pedType = args.pedType;
	request->pedModel = args.pedModel;
	request->vehicle = 0;	/* the in-car opcode sets its own */
	request->x = args.x;
	request->y = args.y;
	request->z = args.z;
	/* The standing peds clear the spot before the world insert; the in-car
	 * peds take the car's own place and never clear it. */
	request->clearSpace = args.place == SPAWN_PED_PLACE_STANDING;
}

/* --- The primitive's own decisions --------------------------------------- */

/* The lane the primitive takes on the travelled side: the lane dice of the
 * old primitive, kept (the lane state is the primitive's). */
static inline uint8_t
SpawnLaneFromDraw(uint8_t lanes, uint32_t draw)
{
	return lanes > 0 ? (uint8_t)(draw % lanes) : 0;
}

/* The travel direction of a placement's node pair: the primitive's own sign
 * rule (the link's direction points from the higher node index to the
 * lower). */
static inline int16_t
SpawnTravelDirection(int16_t node1, int16_t node2)
{
	return node1 >= node2 ? 1 : -1;
}

/* The between-nodes fraction with the car-length clamp: a car that does not
 * fit between two nodes is centred (0.5); otherwise the fraction stays at
 * least the car's own length from either end (the old primitive's rule). */
static inline float
SpawnBetweenNodesClamp(float between, float carLength, float distanceBetweenNodes)
{
	if (distanceBetweenNodes <= 0.0f || distanceBetweenNodes / 2 < carLength)
		return 0.5f;
	if (between < carLength / distanceBetweenNodes)
		return carLength / distanceBetweenNodes;
	if (between > 1.0f - carLength / distanceBetweenNodes)
		return 1.0f - carLength / distanceBetweenNodes;
	return between;
}

/* One planned creation handed to the primitive: the site's own geometry plus
 * the fraction between the nodes. `site` is -1 for a raw placement (the far
 * layer's collector, until its own migration): the primitive then resolves
 * the link by its own node-pair lookup. `link` is the site's **car path link
 * id** (`SSpawnSite::link`, the value `m_carPathLinks` is indexed with), not
 * the directed connection slot the site id and the reservations use: the two
 * differ on almost every site of the shipped graph. */
struct SSpawnPlacement
{
	int32_t site;
	int16_t node1, node2;
	int16_t link;
	float x, y, z;		/* the site midpoint (or the collector's point) */
	float between;
};

/* The placement of one planned site, the one construction of the mapping: the
 * site's midpoint, its node pair and its link id, the fraction 0.5 (the
 * midpoint is the pair's own). The engine and the evidence program both build
 * placements through this, so the slot-versus-link mistake cannot come back. */
static inline void
SpawnPlacementFromSite(const SSpawnSite &site, int32_t siteId, SSpawnPlacement *pOut)
{
	pOut->site = siteId;
	pOut->node1 = site.node1;
	pOut->node2 = site.node2;
	pOut->link = site.link;
	pOut->x = site.x;
	pOut->y = site.y;
	pOut->z = site.z;
	pOut->between = 0.5f;
}

/* --- The request envelopes ----------------------------------------------- */

/*
 * The road-site request envelope: what a producer chooses (the purpose, the
 * ring, the class and model policy) - the service chooses the site. The
 * target is the player centre for the traffic and the destination for the
 * responders; it is both the cone's origin and the reachability's target.
 */
struct SSpawnCarRequest
{
	ESpawnPurpose purpose;
	bool force;
	bool allowBoats;	/* a water site falls back to a boat model */
	bool requireReach;	/* the planner asks TestCoorsCloseness per site */
	int32_t carClass;
	int32_t carModel;
	float targetX, targetY, targetZ;
	float playerSpeedX, playerSpeedY;
	bool testForCollision;	/* the requester's own camera/speed policy */

	/* The ring query (the requester's own branch, verbatim). */
	float spawnDist;
	float dirX, dirY;
	float angleLimit;
	bool forward;
	bool allowDisabled;
};

/* --- The engine side ----------------------------------------------------- */

/*
 * The service (SpawnService.cpp): the one static telemetry instance, the
 * engine's RequestCar/RequestExplicit, and the ladder's API (AllowsCreation,
 * AllowsRemoval and the counter notes) so no spawn decision outside this
 * class calls the near bubble's gates directly.
 */
class CSpawnService
{
public:
	/* One road-site request: the gates, the plan (offer, facts, rank,
	 * reserve) and the run through CCarCtrl::CreateCarFromPlacement. The
	 * created car is handed back (ppCreated when the caller asks). */
	static bool RequestCar(const SSpawnCarRequest &request, CVehicle **ppCreated);

	/* One explicit creation: synchronous, exact, no visibility and no ring;
	 * the force bypass covers the budgets and the reserves. True and the
	 * vehicle in the same frame when it was created. */
	static bool RequestExplicit(const SSpawnExplicitRequest &request, CVehicle **ppCreated);

	/* The explicit ped arm (VICE-157): the standing and in-car script peds,
	 * synchronous and exact, with the same force bypass. True and the ped in
	 * the same frame when it was created. */
	static bool RequestExplicitPed(const SSpawnExplicitPedRequest &request, CPed **ppCreated);

	/* The ladder's API (NearBubble.cpp's gates, behavior preserved): the
	 * creation and removal gates and the acceptance counters. Every caller
	 * goes through here; the counters stay CCarCtrl's own (one owner).
	 * `pArm` (optional) receives the arm the creation gate resolved at the
	 * final position, for the covered-fill row. */
	static void BeginFrame(void);
	static bool AllowsCreation(const CVector &pos, const CVector &playerCentre, float radius, bool rayAllowed, ENearBubbleArm *pArm = 0);
	static bool AllowsRemoval(CVehicle *pVehicle, const CVector &playerCentre, float ringThreshold, bool rayAllowed = true);
	static void CountCreation(const CVector &pos, float radius);
	static void CountRemoval(const CVector &pos, float radius, bool wasInvisible);
	static void CountOtherCreation(const CVector &pos, float radius);
	static void CountValveRemoval(void);

	/* The removal note: the near cull's deletions, by the removed car's own
	 * class (RANDOM_VEHICLE -> ambient, MISSION_VEHICLE -> mission,
	 * PARKED_VEHICLE -> parked, PERMANENT_VEHICLE -> special; a class the
	 * map does not name counts as the ambient traffic's, the cull's own
	 * subject). */
	static void NoteVehicleRemoval(CVehicle *pVehicle);

	/* The primitive's defect notes (the ground casts and the water casts). */
	static void NoteGroundRejection(void);
	static void NoteAirOffsetRejection(void);
	static void NoteWaterRejection(void);

	/* The plan cost of one call (the engine times it; the core accumulates)
	 * and the conservation check. */
	static void NotePlanCost(uint64_t micros);
	static void NoteConservation(uint32_t cellTargetSum, uint32_t globalTarget);

	/* The boot, the restart and the load: the counters go. */
	static void ReInit(void);

private:
	/* The engine's one counter home (the live reader's symbol). */
	static SSpawnServiceTelemetry ms_Telemetry;
};

#endif // __GTA_SPAWNSERVICE_H__
