/*
 * The evidence program for the responders and the police as requesters
 * (ticket VICE-156).
 *
 * It compiles the service's engine-free core (src/control/SpawnService.h) and
 * its policy (src/control/ResponderPolicy.h), plus the site index's real build
 * (src/control/SpawnSitesIndex.cpp), and drives them over the synthetic city
 * of the core harness:
 *
 *	A. The policy. The emergency gates (the two-accident and three-fire
 *	   triggers, the 30/35 second cooldowns, the one-on-duty guard, the
 *	   two-cars-per-model cap, the wanted/interior/budget stops) and the
 *	   police gates (the stars, the law-enforcer and cop caps, the
 *	   five/eight second cadence), one gate at a time.
 *
 *	B. The scenarios. A crime (wanted level four) makes a police request,
 *	   an accident makes an ambulance and a fire makes a fire truck. Each
 *	   request goes through SpawnFunctionGate, SpawnOfferSites,
 *	   SpawnPlanSites and SpawnRunPlan over the scripted world; the
 *	   creation is checked against the ladder (never a catchable arm), the
 *	   purpose rows and the destination fields the primitive reads.
 *
 *	C. The caps over a timeline. The same policy is driven over the old
 *	   dispatch cadence (the emergency trigger every four seconds, the
 *	   police every frame): the created rows stop at two per emergency
 *	   model and at the wanted caps for the police, and the refused calls
 *	   cost no request.
 *
 *	D. The placement gates. A catchable candidate set creates nothing
 *	   (VISIBILITY); a full presence cell refuses (OCCUPANCY); a zone or a
 *	   reach that refuses leaves the plan empty; the priority gate steps
 *	   over the ambient car target but not the car budget or the pool.
 *
 *	E. The occupants and the ledger. The crew table (the ambulance's seat
 *	   1, the fire truck's seat 0, the police models' seats, the
 *	   Predator's driver only) and the removal purposes (a responder or a
 *	   law enforcer leaves in its own row; a mission car keeps its own).
 *
 * What this program cannot reach: the engine side (SpawnService.cpp and the
 * primitive, CCarCtrl::CreateCarFromPlacement) - the live zone, the path
 * search, the model resolution (the Predator fallback for a COPS water site),
 * the vehicle creation and the streaming - is the game's, checked by the build
 * and by the live soak. The boat scenario checks the water site the request
 * selects; the primitive's COPS fallback is the engine's own code.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 \
 *	    -o /tmp/spawn-responders utils/spawn-service/responders.cpp \
 *	    src/control/SpawnSitesIndex.cpp
 *	/tmp/spawn-responders
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "ResponderPolicy.h"
#include "SpawnService.h"

#include <math.h>
#include <stddef.h>
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

/* ------------------------------------------------------------------ the fixture */

/*
 * The synthetic city of utils/spawn-service/core.cpp: an 8 x 8 grid of road
 * nodes at 40 m spacing, one one-way row, one disabled node, a four-node
 * water line east of the grid, and a per-row rarity weight. Every road edge is
 * one link with two directed connections; the index makes a site of each
 * usable direction. The police boat scenario aims at the water line.
 */

#define GRID_COLS 8
#define GRID_ROWS 8
#define GRID_SPACING 40.0f
#define LAND_NODES (GRID_COLS * GRID_ROWS)
#define WATER_FIRST LAND_NODES
#define WATER_NODES 4
#define FIXTURE_NODES (LAND_NODES + WATER_NODES)
#define FIXTURE_EDGES (GRID_COLS * (GRID_ROWS - 1) + (GRID_COLS - 1) * GRID_ROWS + (WATER_NODES - 1))

static SSpawnSiteNode g_nodes[FIXTURE_NODES];
static SSpawnSiteLink g_links[FIXTURE_EDGES];
static uint16_t g_connectionNode[NUM_PATHCONNECTIONS];
static int16_t g_connectionLink[NUM_PATHCONNECTIONS];
static int16_t g_connectionZone[NUM_PATHCONNECTIONS];
static int32_t g_numNodes;
static int32_t g_numLinks;
static int32_t g_numConnections;

struct SFixtureEdge
{
	int32_t a, b;
	int8_t dirX, dirY;
	int8_t left, right;
	int8_t width;
};

static SFixtureEdge g_edges[FIXTURE_EDGES];
static int32_t g_numEdges;

static inline int32_t
RoadNode(int row, int col)
{
	return row * GRID_COLS + col;
}

static inline int32_t
WaterNode(int i)
{
	return WATER_FIRST + i;
}

static int32_t
ZoneAt(float x)
{
	return x < 160.0f ? 0 : 1;
}

static void
AddEdge(int32_t a, int32_t b, int8_t dirX, int8_t dirY, int8_t left, int8_t right)
{
	SFixtureEdge *pEdge = &g_edges[g_numEdges++];

	pEdge->a = a;
	pEdge->b = b;
	pEdge->dirX = dirX;
	pEdge->dirY = dirY;
	pEdge->left = left;
	pEdge->right = right;
	pEdge->width = 10;
}

static void
BuildFixture(void)
{
	int32_t row, col, i;

	memset(g_nodes, 0, sizeof(g_nodes));
	memset(g_links, 0, sizeof(g_links));
	memset(g_connectionNode, 0, sizeof(g_connectionNode));
	memset(g_connectionLink, 0, sizeof(g_connectionLink));
	memset(g_connectionZone, 0, sizeof(g_connectionZone));
	g_numEdges = 0;

	for (row = 0; row < GRID_ROWS; row++) {
		for (col = 0; col < GRID_COLS; col++) {
			SSpawnSiteNode *pNode = &g_nodes[RoadNode(row, col)];
			pNode->x = col * GRID_SPACING;
			pNode->y = row * GRID_SPACING;
			pNode->z = 5.0f;
			pNode->spawnRate = (uint8_t)(1 + row % 3);
			if (row == 2 && col == 2)
				pNode->flags |= SPAWN_SITE_NODE_DISABLED;
		}
	}
	for (i = 0; i < WATER_NODES; i++) {
		SSpawnSiteNode *pNode = &g_nodes[WaterNode(i)];
		pNode->x = 320.0f + i * GRID_SPACING;
		pNode->y = -80.0f;
		pNode->z = 0.0f;
		pNode->spawnRate = 2;
		pNode->flags |= SPAWN_SITE_NODE_WATER;
	}
	for (row = 0; row < GRID_ROWS; row++) {
		for (col = 0; col < GRID_COLS - 1; col++) {
			int32_t a = RoadNode(row, col);
			int32_t b = RoadNode(row, col + 1);
			if (row == 2)
				AddEdge(a, b, -100, 0, 0, 2);
			else
				AddEdge(a, b, -100, 0, 2, 2);
		}
	}
	for (row = 0; row < GRID_ROWS - 1; row++) {
		for (col = 0; col < GRID_COLS; col++)
			AddEdge(RoadNode(row, col), RoadNode(row + 1, col), 0, -100, 2, 2);
	}
	for (i = 0; i < WATER_NODES - 1; i++)
		AddEdge(WaterNode(i), WaterNode(i + 1), -100, 0, 1, 1);

	g_numLinks = g_numEdges;
	for (i = 0; i < g_numEdges; i++) {
		g_links[i].dirX = g_edges[i].dirX;
		g_links[i].dirY = g_edges[i].dirY;
		g_links[i].numLeftLanes = g_edges[i].left;
		g_links[i].numRightLanes = g_edges[i].right;
		g_links[i].pathNodeIndex = (int16_t)g_edges[i].a;
		g_links[i].width = g_edges[i].width;
	}

	g_numNodes = FIXTURE_NODES;
	{
		int32_t cursor = 0;

		for (i = 0; i < g_numNodes; i++) {
			int32_t e;
			uint8_t n = 0;

			g_nodes[i].firstLink = (int16_t)cursor;
			for (e = 0; e < g_numEdges; e++) {
				const SFixtureEdge *pEdge = &g_edges[e];
				int32_t other;
				int32_t conn;

				if (pEdge->a == i)
					other = pEdge->b;
				else if (pEdge->b == i)
					other = pEdge->a;
				else
					continue;
				conn = cursor++;
				g_connectionNode[conn] = (uint16_t)other;
				g_connectionLink[conn] = (int16_t)e;
				g_connectionZone[conn] = (int16_t)ZoneAt((g_nodes[i].x + g_nodes[other].x) * 0.5f);
				n++;
			}
			g_nodes[i].numLinks = n;
		}
		g_numConnections = cursor;
	}
	Check(g_numEdges <= FIXTURE_EDGES, "the fixture's edge array holds the edges");
	Check(g_numConnections <= NUM_PATHCONNECTIONS, "the fixture's connections fit the index");
}

static SSpawnSiteGraph
FixtureGraph(void)
{
	SSpawnSiteGraph graph;

	graph.numNodes = g_numNodes;
	graph.numLinks = g_numLinks;
	graph.numConnections = g_numConnections;
	graph.nodes = g_nodes;
	graph.links = g_links;
	graph.connectionNode = g_connectionNode;
	graph.connectionLink = g_connectionLink;
	graph.connectionZone = g_connectionZone;
	return graph;
}

static CSpawnSiteIndex g_index;
static SSpawnOccupancyStore g_store;

/* ------------------------------------------------------------- the scripts */

#define SPAWN_SCRIPT_DEFAULT 0xFF

static uint8_t g_armOf[NUM_PATHCONNECTIONS];
static bool g_zoneRefused[NUM_PATHCONNECTIONS];
static bool g_reachRefused[NUM_PATHCONNECTIONS];

static void
ScriptsReset(void)
{
	memset(g_armOf, SPAWN_SCRIPT_DEFAULT, sizeof(g_armOf));
	memset(g_zoneRefused, 0, sizeof(g_zoneRefused));
	memset(g_reachRefused, 0, sizeof(g_reachRefused));
}

static void
ArmAll(uint8_t arm)
{
	memset(g_armOf, arm, sizeof(g_armOf));
}

static uint8_t
ArmOf(int32_t site)
{
	return g_armOf[site] == SPAWN_SCRIPT_DEFAULT ? (uint8_t)NEAR_BUBBLE_ARM_SOFT : g_armOf[site];
}

/*
 * The bubble facts of one site, crafted to produce the wanted arm (the core
 * harness's own crafting, checked against NearBubbleArmFor).
 */
static void
BubbleForArm(SNearBubbleCandidate *c, uint8_t arm)
{
	memset(c, 0, sizeof(*c));
	c->distToPlayer = NEAR_BUBBLE_RADIUS + 40.0f;
	c->visibleRange = 0.0f;
	c->onScreenSpawnMin = RING_CAR_ONSCREEN_SPAWN_MIN;
	c->distToCamera = NEAR_BUBBLE_SOFT_DISTANCE + 10.0f;

	switch (arm) {
	case NEAR_BUBBLE_ARM_SOFT:
		break;
	case NEAR_BUBBLE_ARM_FADE_DISTANCE:
		c->visibleRange = 100.0f;
		c->distToCamera = 200.0f;
		break;
	case NEAR_BUBBLE_ARM_OCCLUDER:
		c->occluded = true;
		c->distToCamera = NEAR_BUBBLE_SOFT_DISTANCE - 10.0f;
		break;
	case NEAR_BUBBLE_ARM_RAY:
		c->distToCamera = NEAR_BUBBLE_SOFT_DISTANCE - 10.0f;
		break;
	case NEAR_BUBBLE_ARM_FADE:
		c->inFrustum = true;
		c->inFrustumMargin = true;
		c->fadeHides = true;
		c->distToCamera = RING_CAR_ONSCREEN_SPAWN_MIN - 30.0f;
		break;
	case NEAR_BUBBLE_ARM_BYPASS:
		c->bypassView = true;
		break;
	case NEAR_BUBBLE_ARM_ONSCREEN_BAND:
		c->inFrustum = true;
		c->inFrustumMargin = true;
		c->distToCamera = RING_CAR_ONSCREEN_SPAWN_MIN;
		break;
	case NEAR_BUBBLE_ARM_NONE:
	default:
		c->inFrustum = true;
		c->inFrustumMargin = true;
		c->distToCamera = RING_CAR_ONSCREEN_SPAWN_MIN - 30.0f;
		break;
	}
}

static int32_t g_deepCalls;

static void
HarnessDeep(void *user, int32_t offerIndex, SSpawnSiteDeepFacts *facts)
{
	const SSpawnOffer *offers = (const SSpawnOffer *)user;
	int32_t site = offers[offerIndex].site;

	(void)g_deepCalls++;
	facts->zoneOk = !g_zoneRefused[site];
	facts->reachOk = !g_reachRefused[site];
}

/* ------------------------------------------------------------------ the helpers */

static SSpawnRingQuery
QueryOf(const SSpawnCarRequest &request)
{
	SSpawnRingQuery q;

	memset(&q, 0, sizeof(q));
	q.targetX = request.targetX;
	q.targetY = request.targetY;
	q.spawnDist = request.spawnDist;
	q.dirX = request.dirX;
	q.dirY = request.dirY;
	q.angleLimit = request.angleLimit;
	q.forward = request.forward;
	q.water = request.allowBoats ? SPAWN_SITES_ANY : SPAWN_SITES_LAND;
	q.allowDisabled = request.allowDisabled;
	return q;
}

static SSpawnBudgetFacts
HealthyBudget(void)
{
	SSpawnBudgetFacts budget;

	budget.randomCars = 0;
	budget.randomTarget = 100.0f;
	budget.carsInUse = 10;
	budget.carBudget = 100;
	budget.pedFreeSpaces = PED_POOL_RESERVE + 100;
	return budget;
}

static uint32_t
Refusal(const SSpawnServiceTelemetry &t, ESpawnRefusal r)
{
	return t.m_nRefusalsByReason[r];
}

static uint32_t
Delta(uint32_t after, uint32_t before)
{
	return after - before;
}

/* One planned dispatch: the request's own gate, the offer, the plan and the
 * run, with the scripts' arms and deep facts applied. */
struct SScenarioResult
{
	bool created;
	bool hasDestination;
	bool destinationChecked;
	bool destinationOk;
	bool boatSite;
	int32_t site;
	uint8_t arm;
	ESpawnRefusal refusal;
	uint32_t refusalsAtOffer;
};

struct SScenarioScript
{
	const SSpawnCarRequest *pRequest;
	SScenarioResult *pResult;
};

static ESpawnRefusal
ScenarioTry(void *user, const SSpawnPlanSite &planSite, uint8_t *pArm)
{
	SScenarioScript *pScript = (SScenarioScript *)user;
	const SSpawnCarRequest *pRequest = pScript->pRequest;
	SScenarioResult *pResult = pScript->pResult;
	const SSpawnSite &site = g_index.GetSites()[planSite.site];
	SSpawnPlacement placement;

	SpawnPlacementFromSite(site, planSite.site, &placement);
	SpawnPlacementSetDestination(&placement, *pRequest);
	pResult->hasDestination = pRequest->responder != SPAWN_RESPONDER_NONE;
	if (pRequest->responder != SPAWN_RESPONDER_NONE) {
		pResult->destinationChecked = true;
		pResult->destinationOk =
			placement.responder == pRequest->responder &&
			placement.destX == pRequest->destinationX &&
			placement.destY == pRequest->destinationY &&
			placement.destZ == pRequest->destinationZ;
	} else {
		pResult->destinationChecked = true;
		pResult->destinationOk = placement.responder == SPAWN_RESPONDER_NONE;
	}
	pResult->site = planSite.site;
	pResult->arm = planSite.arm;
	pResult->boatSite = (site.flags & SPAWN_SITE_BOAT) != 0;
	*pArm = planSite.arm;
	return SPAWN_REFUSAL_NONE;
}

static SScenarioResult
RunRequest(const SSpawnCarRequest &request, SSpawnServiceTelemetry *pTel, uint32_t nowMs)
{
	SScenarioResult result;
	SScenarioScript script;
	SSpawnRingQuery query;
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	SSpawnPlan plan;
	SSpawnBudgetFacts budget;
	int32_t numOffers, i;

	memset(&result, 0, sizeof(result));
	result.site = -1;
	result.refusal = SPAWN_REFUSAL_NONE;

	SpawnRequestBegin(pTel, request.purpose, SPAWN_KIND_ROAD_SITE);
	budget = HealthyBudget();
	CheckInt(SpawnFunctionGate(budget, request.force, pTel, request.priority), SPAWN_REFUSAL_NONE,
		"the scenario's request passes the function gate");

	query = QueryOf(request);
	numOffers = SpawnOfferSites(g_index, query, offers, SPAWN_SERVICE_MAX_CANDIDATES);
	for (i = 0; i < numOffers; i++)
		BubbleForArm(&bubbles[i], ArmOf(offers[i].site));

	result.refusalsAtOffer = pTel->m_nRefusalsByReason[SPAWN_REFUSAL_VISIBILITY];
	SpawnPlanSites(g_index, offers, bubbles, numOffers, &g_store, nowMs,
		HarnessDeep, offers, &plan, pTel);

	script.pRequest = &request;
	script.pResult = &result;
	result.refusal = SpawnRunPlan(plan, &g_store, ScenarioTry, &script, pTel, request.purpose);
	result.created = result.refusal == SPAWN_REFUSAL_NONE && result.site >= 0;
	return result;
}

/* The fixture's own check: the crafted bubble produces the arm the script
 * names, so a scenario cannot lie about what the ladder sees. */
static void
CheckBubbleCrafting(void)
{
	SNearBubbleCandidate candidate;
	int arm;

	for (arm = 0; arm <= NEAR_BUBBLE_ARM_ONSCREEN_BAND; arm++) {
		BubbleForArm(&candidate, (uint8_t)arm);
		CheckInt(NearBubbleArmFor(candidate), arm, "the crafted bubble produces the scripted arm");
	}
}

/* ------------------------------------------------------------------ section A */

static SResponderFacts
AccidentFacts(void)
{
	SResponderFacts facts;

	memset(&facts, 0, sizeof(facts));
	facts.wantedLevel = 2;
	facts.carsInUse = 10;
	facts.carBudget = 100;
	facts.activeIncidents = RESPONDER_ACCIDENT_TRIGGER;
	facts.nearestFound = true;
	facts.nearestDistance = 12.0f;
	facts.nearestX = 140.0f;
	facts.nearestY = 140.0f;
	facts.nearestZ = 5.0f;
	facts.nowMs = 100000;
	facts.lastCreatedMs = 0;
	return facts;
}

static SResponderFacts
FireFacts(void)
{
	SResponderFacts facts = AccidentFacts();

	facts.activeIncidents = RESPONDER_FIRE_TRIGGER;
	facts.nearestDistance = 20.0f;
	return facts;
}

static void
SectionPolicy(void)
{
	SResponderFacts facts;
	SResponderDecision decision;
	SPoliceFacts police;

	printf("A. the policy: the triggers, the cooldowns, the caps and the stars\n");

	/* The emergency gates, one at a time. */
	facts = AccidentFacts();
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	Check(decision.dispatch, "A: two accidents dispatch an ambulance");
	CheckInt((int)decision.gate, RESPONDER_GATE_DISPATCH, "A: the dispatch gate");
	Check(decision.destX == 140.0f && decision.destY == 140.0f && decision.destZ == 5.0f,
		"A: the destination is the nearest accident");

	facts = AccidentFacts();
	facts.activeIncidents = 1;
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_BELOW_TRIGGER, "A: one accident is below the trigger");

	facts = FireFacts();
	ResponderEvaluate(SPAWN_RESPONDER_FIRETRUCK, facts, &decision);
	Check(decision.dispatch, "A: three fires dispatch a fire truck");
	facts.activeIncidents = 2;
	ResponderEvaluate(SPAWN_RESPONDER_FIRETRUCK, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_BELOW_TRIGGER, "A: two fires are below the trigger");

	facts = AccidentFacts();
	facts.wantedLevel = 4;
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_WANTED, "A: four stars keep the emergency services away");
	facts = AccidentFacts();
	facts.interior = true;
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_INTERIOR, "A: an interior refuses the responder");
	facts = AccidentFacts();
	facts.carsInUse = facts.carBudget;
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_BUDGET, "A: the car budget refuses the responder");

	facts = AccidentFacts();
	facts.onDuty = 1;
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_ON_DUTY, "A: one on duty is the duty cap");
	facts = AccidentFacts();
	facts.nearestFound = false;
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_NO_INCIDENT, "A: no incident in range refuses");
	facts = AccidentFacts();
	facts.carsOfType = RESPONDER_MAX_CARS;
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_CAP, "A: two cars of the model are the cap");
	facts = AccidentFacts();
	facts.nowMs = facts.lastCreatedMs + RESPONDER_AMBULANCE_COOLDOWN_MS;
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_COOLDOWN, "A: the 30 s cooldown holds at the boundary");
	facts.nowMs++;
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	Check(decision.dispatch, "A: the ambulance asks after 30 s");
	facts = FireFacts();
	facts.lastCreatedMs = 100000;
	facts.nowMs = 100000 + RESPONDER_FIRETRUCK_COOLDOWN_MS;
	ResponderEvaluate(SPAWN_RESPONDER_FIRETRUCK, facts, &decision);
	CheckInt((int)decision.gate, RESPONDER_GATE_COOLDOWN, "A: the 35 s cooldown holds at the boundary");
	facts.nowMs++;
	ResponderEvaluate(SPAWN_RESPONDER_FIRETRUCK, facts, &decision);
	Check(decision.dispatch, "A: the fire truck asks after 35 s");

	/* The police gates, the old branch's condition. */
	memset(&police, 0, sizeof(police));
	police.wantedLevel = 4;
	police.maxVehicles = 3;
	police.maxCops = 10;
	police.nowMs = 100000;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_ALLOWED, "A: four stars ask at once");
	police.wantedLevel = 1;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_WANTED, "A: one star has no police request");
	police.wantedLevel = 4;
	police.lawEnforcers = 3;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_VEHICLE_CAP, "A: the law-enforcer cap holds");
	police.lawEnforcers = 0;
	police.currentCops = 10;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_COP_CAP, "A: the cop cap holds");
	police.currentCops = 0;
	police.interior = true;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_INTERIOR, "A: an interior refuses the police request");
	police.interior = false;
	police.wantedLevel = 2;
	police.lastCreatedMs = 100000;
	police.nowMs = 107999;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_COOLDOWN, "A: two stars wait eight seconds");
	police.nowMs = 108001;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_ALLOWED, "A: two stars ask after eight seconds");
	police.wantedLevel = 3;
	police.nowMs = 105001;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_ALLOWED, "A: three stars ask after five seconds");
	police.nowMs = 105000;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_COOLDOWN, "A: three stars wait five seconds");
	police.wantedLevel = 4;
	police.nowMs = 100001;
	CheckInt((int)PoliceRequestGate(police), POLICE_GATE_ALLOWED, "A: four stars ignore the cadence");
}

/* ------------------------------------------------------------------ section B */

static void
PrintCreated(const char *name, const SScenarioResult &result, const SSpawnServiceTelemetry &tel, ESpawnPurpose purpose)
{
	printf("    %s: site %d, arm %s, destination %s, created row %u, request row %u\n",
		name, result.site,
		result.arm == NEAR_BUBBLE_ARM_SOFT ? "soft" :
		result.arm == NEAR_BUBBLE_ARM_FADE_DISTANCE ? "fade-distance" :
		result.arm == NEAR_BUBBLE_ARM_OCCLUDER ? "occluder" :
		result.arm == NEAR_BUBBLE_ARM_RAY ? "ray" :
		result.arm == NEAR_BUBBLE_ARM_FADE ? "fade" :
		result.arm == NEAR_BUBBLE_ARM_BYPASS ? "bypass" : "band",
		result.hasDestination ? (result.destinationOk ? "carried" : "MISSING") : "not a responder",
		tel.m_nCreatedByPurpose[purpose], tel.m_nRequestsByPurpose[purpose]);
}

/* A crime: the wanted system lets a police request through, the service
 * places it legally, and the ledger counts the POLICE purpose. */
static void
SectionCrime(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnCarRequest request;
	SScenarioResult result;
	SPoliceFacts facts;

	printf("B. a crime makes the police arrive legally\n");
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	SpawnTelemetryReset(&tel);
	g_store.Reset();

	memset(&facts, 0, sizeof(facts));
	facts.wantedLevel = 4;
	facts.maxVehicles = 3;
	facts.maxCops = 10;
	facts.nowMs = 100000;
	CheckInt((int)PoliceRequestGate(facts), POLICE_GATE_ALLOWED, "B: the crime opens the police gate");

	PoliceFillRequest(596, 11, 140.0f, 140.0f, 5.0f, 0.0f, 0.0f,
		120.0f, 1.0f, 0.0f, -1.0f, true, true, &request);
	CheckInt((int)request.purpose, SPAWN_PURPOSE_POLICE, "B: the request's purpose is POLICE");
	Check(request.priority, "B: the police request is priority");
	Check(request.allowBoats, "B: the police request allows the Predator's water sites");
	CheckInt((int)request.responder, SPAWN_RESPONDER_NONE, "B: a police car is not a responder");

	result = RunRequest(request, &tel, 100000);
	Check(result.created, "B: the police request creates its car");
	Check(result.destinationChecked && result.destinationOk, "B: the police placement has no destination");
	Check(result.arm != NEAR_BUBBLE_ARM_NONE, "B: the police car is not created on a catchable site");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_POLICE], 1, "B: the created row is POLICE");
	CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_POLICE], 1, "B: the request row is POLICE");
	CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_AMBIENT], 0, "B: the police request is not ambient");
	CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_ROAD_SITE], 1, "B: the request kind is the road-site plan");
	PrintCreated("crime", result, tel, SPAWN_PURPOSE_POLICE);
}

/* An accident: the policy dispatches an ambulance with the accident as its
 * destination, and the primitive's destination mode is checked through the
 * placement copy. */
static void
SectionAccident(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnCarRequest request;
	SScenarioResult result;
	SResponderFacts facts;
	SResponderDecision decision;

	printf("C. an accident makes an ambulance\n");
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	SpawnTelemetryReset(&tel);
	g_store.Reset();

	facts = AccidentFacts();
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	Check(decision.dispatch, "C: two accidents dispatch an ambulance");
	ResponderFillRequest(decision, 300, 0, 140.0f, 140.0f, 5.0f, 120.0f, &request);
	CheckInt((int)request.purpose, SPAWN_PURPOSE_RESPONDER, "C: the request's purpose is RESPONDER");
	Check(request.priority, "C: the responder request is priority");
	Check(!request.allowBoats, "C: an ambulance stays on land");
	Check(request.requireReach, "C: the responder asks for reachability to the incident");
	CheckInt((int)request.responder, SPAWN_RESPONDER_AMBULANCE, "C: the request's role is the ambulance");
	Check(request.targetX == 140.0f && request.targetY == 140.0f && request.targetZ == 5.0f,
		"C: the ring is centred on the accident");
	Check(request.destinationX == 140.0f && request.destinationY == 140.0f && request.destinationZ == 5.0f,
		"C: the destination is the accident");

	result = RunRequest(request, &tel, 100000);
	Check(result.created, "C: the ambulance request creates its car");
	Check(result.destinationChecked && result.destinationOk, "C: the placement carries the destination");
	Check(result.arm != NEAR_BUBBLE_ARM_NONE, "C: the ambulance is not created on a catchable site");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_RESPONDER], 1, "C: the created row is RESPONDER");
	CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_RESPONDER], 1, "C: the request row is RESPONDER");
	PrintCreated("accident", result, tel, SPAWN_PURPOSE_RESPONDER);
}

/* A fire: the same path with the fire truck's own trigger, cooldown and
 * destination. */
static void
SectionFire(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnCarRequest request;
	SScenarioResult result;
	SResponderFacts facts;
	SResponderDecision decision;

	printf("D. a fire makes a fire truck\n");
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	SpawnTelemetryReset(&tel);
	g_store.Reset();

	facts = FireFacts();
	ResponderEvaluate(SPAWN_RESPONDER_FIRETRUCK, facts, &decision);
	Check(decision.dispatch, "D: three fires dispatch a fire truck");
	ResponderFillRequest(decision, 301, 0, 140.0f, 140.0f, 5.0f, 120.0f, &request);
	CheckInt((int)request.purpose, SPAWN_PURPOSE_RESPONDER, "D: the request's purpose is RESPONDER");
	CheckInt((int)request.responder, SPAWN_RESPONDER_FIRETRUCK, "D: the request's role is the fire truck");
	Check(request.destinationX == 140.0f && request.destinationY == 140.0f,
		"D: the destination is the fire");

	result = RunRequest(request, &tel, 100000);
	Check(result.created, "D: the fire-truck request creates its car");
	Check(result.destinationChecked && result.destinationOk, "D: the placement carries the destination");
	Check(result.arm != NEAR_BUBBLE_ARM_NONE, "D: the fire truck is not created on a catchable site");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_RESPONDER], 1, "D: the created row is RESPONDER");
	PrintCreated("fire", result, tel, SPAWN_PURPOSE_RESPONDER);
}

/* ------------------------------------------------------------------ section C */

/* The ambulance dispatch cadence over the old four-second trigger, with the
 * policy driving the real service request on every dispatch. */
static void
SectionEmergencyCaps(void)
{
	static const uint32_t times[] = { 100000, 100100, 129999, 130001, 170000, 200000 };
	SSpawnServiceTelemetry tel;
	SResponderFacts facts;
	SResponderDecision decision;
	int i, created = 0, requests = 0;
	int onDuty = 0;
	int carsOfType = 0;
	uint32_t lastCreated = 0;

	printf("E. the emergency caps over the trigger timeline\n");
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	SpawnTelemetryReset(&tel);
	g_store.Reset();

	for (i = 0; i < (int)(sizeof(times) / sizeof(times[0])); i++) {
		facts = AccidentFacts();
		facts.nowMs = times[i];
		facts.lastCreatedMs = lastCreated;
		facts.onDuty = onDuty;
		facts.carsOfType = carsOfType;
		ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
		if (decision.dispatch) {
			SSpawnCarRequest request;
			SScenarioResult result;

			ResponderFillRequest(decision, 300, 0, 140.0f, 140.0f, 5.0f, 120.0f, &request);
			result = RunRequest(request, &tel, times[i]);
			Check(result.created, "E: the dispatched ambulance creates");
			if (result.created) {
				created++;
				carsOfType++;
				lastCreated = times[i];
			}
			requests++;
		}
		/* The responder arrives before the next trigger: the duty guard
		 * is the one-on-duty rule, not a permanent lock. */
		onDuty = 0;
	}
	CheckInt(created, RESPONDER_MAX_CARS, "E: the ambulance stops at two cars");
	CheckInt(requests, RESPONDER_MAX_CARS, "E: only two dispatches reach the service");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_RESPONDER], 2, "E: the responder row stops at two");
	CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_RESPONDER], 2, "E: two responder requests");
	printf("    ambulance timeline: 6 triggers, %d requests, %d created (cap %d)\n",
		requests, created, RESPONDER_MAX_CARS);

	/* The fire truck's own cooldown: 35 s. */
	{
		static const uint32_t fireTimes[] = { 100000, 100100, 134999, 135001, 180000, 200000 };
		int fireCreated = 0;
		int fireOnDuty = 0;
		int fireCars = 0;
		uint32_t fireLast = 0;

		SpawnTelemetryReset(&tel);
		for (i = 0; i < (int)(sizeof(fireTimes) / sizeof(fireTimes[0])); i++) {
			facts = FireFacts();
			facts.nowMs = fireTimes[i];
			facts.lastCreatedMs = fireLast;
			facts.onDuty = fireOnDuty;
			facts.carsOfType = fireCars;
			ResponderEvaluate(SPAWN_RESPONDER_FIRETRUCK, facts, &decision);
			if (decision.dispatch) {
				SSpawnCarRequest request;
				SScenarioResult result;

				ResponderFillRequest(decision, 301, 0, 140.0f, 140.0f, 5.0f, 120.0f, &request);
				result = RunRequest(request, &tel, fireTimes[i]);
				Check(result.created, "E: the dispatched fire truck creates");
				if (result.created) {
					fireCreated++;
					fireCars++;
					fireLast = fireTimes[i];
				}
			}
			fireOnDuty = 0;
		}
		CheckInt(fireCreated, RESPONDER_MAX_CARS, "E: the fire truck stops at two cars");
		CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_RESPONDER], 2, "E: the fire row stops at two");
		printf("    fire timeline: 6 triggers, %d created (cap %d, cooldown %d ms)\n",
			fireCreated, RESPONDER_MAX_CARS, RESPONDER_FIRETRUCK_COOLDOWN_MS);
	}
}

/* The police caps over the wanted timeline: four stars fill to the
 * law-enforcer cap, then the gate refuses. */
static void
SectionPoliceCaps(void)
{
	SSpawnServiceTelemetry tel;
	SPoliceFacts facts;
	SSpawnCarRequest request;
	SScenarioResult result;
	int lawEnforcers = 0;
	int created = 0;
	int i;
	uint32_t lastCreated = 0;

	printf("F. the police caps over the wanted timeline\n");
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	SpawnTelemetryReset(&tel);
	g_store.Reset();

	memset(&facts, 0, sizeof(facts));
	facts.wantedLevel = 4;
	facts.maxVehicles = 3;
	facts.maxCops = 10;
	for (i = 0; i < 5; i++) {
		facts.lawEnforcers = lawEnforcers;
		facts.nowMs = 100000 + i;
		facts.lastCreatedMs = lastCreated;
		if (PoliceRequestGate(facts) != POLICE_GATE_ALLOWED)
			break;
		PoliceFillRequest(596, 11, 140.0f, 140.0f, 5.0f, 0.0f, 0.0f,
			120.0f, 1.0f, 0.0f, -1.0f, true, true, &request);
		result = RunRequest(request, &tel, facts.nowMs);
		Check(result.created, "F: the dispatched police car creates");
		if (result.created) {
			created++;
			lawEnforcers++;
			lastCreated = facts.nowMs;
		}
	}
	CheckInt(created, 3, "F: the police stop at the law-enforcer cap");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_POLICE], 3, "F: the police row stops at the cap");
	CheckInt((int)PoliceRequestGate(facts), POLICE_GATE_VEHICLE_CAP, "F: the fifth call is refused by the cap");
	printf("    police timeline: %d gate calls, %d created (cap %d), gate %s\n",
		i + 1, created, facts.maxVehicles, PoliceGateName(PoliceRequestGate(facts)));

	/* One police boat: the water request carries the Predator's site. */
	{
		SSpawnCarRequest boatRequest;
		int32_t boatSite = -1;
		int32_t i2;

		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		for (i2 = 0; i2 < g_index.GetNumSites(); i2++) {
			if (g_index.GetSites()[i2].flags & SPAWN_SITE_BOAT) {
				boatSite = i2;
				break;
			}
		}
		Check(boatSite >= 0, "F: the fixture holds a boat site");
		if (boatSite >= 0)
			g_armOf[boatSite] = NEAR_BUBBLE_ARM_SOFT;
		SpawnTelemetryReset(&tel);
		g_store.Reset();
		PoliceFillRequest(596, 11, 380.0f, -80.0f, 0.0f, 0.0f, 0.0f,
			60.0f, -1.0f, 0.0f, -1.0f, true, true, &boatRequest);
		result = RunRequest(boatRequest, &tel, 120000);
		Check(result.created, "F: the police boat request creates on the water site");
		Check(result.boatSite, "F: the created police site is a boat site (the Predator fallback)");
		printf("    police boat: water site %d, created %d, purpose POLICE row %u\n",
			result.site, result.created ? 1 : 0, tel.m_nCreatedByPurpose[SPAWN_PURPOSE_POLICE]);
	}
}

/* ------------------------------------------------------------------ section D */

static SResponderFacts
CatchableFacts(void)
{
	return AccidentFacts();
}

/* The placement gates: the ladder's refusal, the occupancy store, the deep
 * facts, and the priority gate's own line. */
static void
SectionPlacementGates(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnCarRequest request;
	SScenarioResult result;
	SResponderFacts facts;
	SResponderDecision decision;
	SSpawnBudgetFacts budget;
	SSpawnRingQuery query;
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	int32_t numOffers, i;

	printf("G. the placement gates and the no-catchable rule\n");
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_NONE);
	SpawnTelemetryReset(&tel);
	g_store.Reset();

	facts = CatchableFacts();
	ResponderEvaluate(SPAWN_RESPONDER_AMBULANCE, facts, &decision);
	ResponderFillRequest(decision, 300, 0, 140.0f, 140.0f, 5.0f, 120.0f, &request);
	result = RunRequest(request, &tel, 100000);
	Check(!result.created, "G: an all-catchable candidate set creates nothing");
	CheckInt((int)result.refusal, SPAWN_REFUSAL_VISIBILITY, "G: the refusal is VISIBILITY");
	Check(Refusal(tel, SPAWN_REFUSAL_VISIBILITY) > 0, "G: the catchable candidates are counted");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_RESPONDER], 0, "G: the catchable run creates no responder");
	printf("    catchable set: %u candidates refused by visibility, 0 created\n",
		Refusal(tel, SPAWN_REFUSAL_VISIBILITY));

	/* The nearest site is catchable, the rest are hidden: the plan skips the
	 * catchable one and creates on a legal site. */
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	SpawnTelemetryReset(&tel);
	g_store.Reset();
	query = QueryOf(request);
	numOffers = SpawnOfferSites(g_index, query, offers, SPAWN_SERVICE_MAX_CANDIDATES);
	Check(numOffers > 1, "G: the mixed set holds more than one candidate");
	{
		int32_t nearest = 0;
		float best = 1e9f;

		for (i = 0; i < numOffers; i++) {
			if (offers[i].ringError < best) {
				best = offers[i].ringError;
				nearest = offers[i].site;
			}
		}
		g_armOf[nearest] = NEAR_BUBBLE_ARM_NONE;
		result = RunRequest(request, &tel, 100000);
		Check(result.created, "G: the mixed set creates on a hidden site");
		Check(result.arm != NEAR_BUBBLE_ARM_NONE, "G: the mixed set never creates on the catchable site");
		Check(result.site != nearest, "G: the catchable nearest site is skipped");
		printf("    mixed set: nearest site %d refused, created site %d (arm %d)\n",
			nearest, result.site, result.arm);
	}

	/* The occupancy: every offered site's cell is at the planner's capacity. */
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	SpawnTelemetryReset(&tel);
	g_store.Reset();
	query = QueryOf(request);
	numOffers = SpawnOfferSites(g_index, query, offers, SPAWN_SERVICE_MAX_CANDIDATES);
	{
		int32_t handle = 0;
		for (i = 0; i < numOffers; i++) {
			const SSpawnSite &site = g_index.GetSites()[offers[i].site];
			int32_t k;
			for (k = 0; k < SPAWN_SERVICE_CELL_CAPACITY; k++) {
				g_store.Register(SPAWN_OCCUPANCY_VEHICLE, handle);
				g_store.Create(SPAWN_OCCUPANCY_VEHICLE, handle, site.x, site.y);
				handle++;
			}
		}
		result = RunRequest(request, &tel, 100000);
		Check(!result.created, "G: a full presence cell creates nothing");
		CheckInt((int)result.refusal, SPAWN_REFUSAL_OCCUPANCY, "G: the refusal is OCCUPANCY");
		Check(Refusal(tel, SPAWN_REFUSAL_OCCUPANCY) > 0, "G: the occupied candidates are counted");
		printf("    occupied set: %u candidates refused by occupancy, 0 created\n",
			Refusal(tel, SPAWN_REFUSAL_OCCUPANCY));
	}

	/* The deep facts: a zone and a reachability refusal over the whole set
	 * leave the plan empty, counted by their own reasons. */
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	SpawnTelemetryReset(&tel);
	g_store.Reset();
	for (i = 0; i < g_index.GetNumSites(); i++)
		g_zoneRefused[i] = true;
	result = RunRequest(request, &tel, 100000);
	Check(!result.created, "G: a zone-refused set creates nothing");
	CheckInt((int)result.refusal, SPAWN_REFUSAL_ZONE, "G: the refusal is ZONE");
	Check(Refusal(tel, SPAWN_REFUSAL_ZONE) > 0, "G: the zone refusals are counted");

	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	SpawnTelemetryReset(&tel);
	g_store.Reset();
	for (i = 0; i < g_index.GetNumSites(); i++)
		g_reachRefused[i] = true;
	result = RunRequest(request, &tel, 100000);
	Check(!result.created, "G: a reach-refused set creates nothing");
	CheckInt((int)result.refusal, SPAWN_REFUSAL_REACHABILITY, "G: the refusal is REACHABILITY");
	Check(Refusal(tel, SPAWN_REFUSAL_REACHABILITY) > 0, "G: the reach refusals are counted");
	printf("    deep facts: zone and reach refusals counted, 0 created\n");

	/* The priority gate: an event request steps over the ambient car target,
	 * never over the car budget or the pool. */
	budget = HealthyBudget();
	budget.randomCars = (int32_t)budget.randomTarget;
	CheckInt((int)SpawnFunctionGate(budget, false, &tel, false), SPAWN_REFUSAL_BUDGET,
		"G: an ambient request stops at the car target");
	CheckInt((int)SpawnFunctionGate(budget, false, &tel, true), SPAWN_REFUSAL_NONE,
		"G: a priority request steps over the car target");
	budget.carsInUse = budget.carBudget;
	CheckInt((int)SpawnFunctionGate(budget, false, &tel, true), SPAWN_REFUSAL_BUDGET,
		"G: the priority request still stops at the car budget");
	budget = HealthyBudget();
	budget.pedFreeSpaces = 0;
	CheckInt((int)SpawnFunctionGate(budget, false, &tel, true), SPAWN_REFUSAL_POOL,
		"G: the priority request still stops at the ped pool");
	printf("    priority gate: target bypassed, budget and pool kept\n");
}

/* ------------------------------------------------------------------ section E */

static void
CheckCrew(ESpawnCrewKind kind, int wantedLevel, int driver, int seats, const char *what)
{
	SSpawnCrew crew = SpawnCrewOf(kind, wantedLevel);

	CheckInt(crew.driver, driver, what);
	CheckInt(crew.passengerSeats, seats, what);
}

static void
SectionCrewAndLedger(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnCrew crew;
	uint32_t before;

	printf("H. the occupants and the ledger\n");

	CheckCrew(SPAWN_CREW_AMBULANCE, 0, 1, 1 << 1, "H: the ambulance is driver + seat 1");
	CheckCrew(SPAWN_CREW_FIRETRUCK, 0, 1, 1 << 0, "H: the fire truck is driver + seat 0");
	CheckCrew(SPAWN_CREW_POLICE_VAN, 0, 1, (1 << 0) | (1 << 1) | (1 << 2), "H: the police van is driver + three");
	CheckCrew(SPAWN_CREW_POLICE_SEDAN, 0, 1, 0, "H: the police sedan is the driver alone below two stars");
	CheckCrew(SPAWN_CREW_POLICE_SEDAN, 2, 1, 1 << 0, "H: the police sedan adds seat 0 at two stars");
	CheckCrew(SPAWN_CREW_POLICE_BOAT, 0, 1, 0, "H: the Predator is the driver alone");
	CheckCrew(SPAWN_CREW_VICE, 0, 1, 1 << 0, "H: the Vice car is two cops");
	CheckInt((int)ResponderCrewKind(SPAWN_RESPONDER_AMBULANCE), SPAWN_CREW_AMBULANCE,
		"H: the ambulance role maps to its crew");
	CheckInt((int)ResponderCrewKind(SPAWN_RESPONDER_FIRETRUCK), SPAWN_CREW_FIRETRUCK,
		"H: the fire role maps to its crew");
	crew = SpawnCrewOf(SPAWN_CREW_FIRETRUCK, 0);
	Check(crew.passengerSeats != (1 << 1), "H: the fire truck does not take the ambulance's seat");

	/* The removal rows: the live role flags first, then the creation class. */
	CheckInt((int)SpawnPurposeOfRemovedVehicle(SPAWN_VEHICLE_TAG_RANDOM, false, true), SPAWN_PURPOSE_RESPONDER,
		"H: a duty responder leaves in the RESPONDER row");
	CheckInt((int)SpawnPurposeOfRemovedVehicle(SPAWN_VEHICLE_TAG_RANDOM, true, false), SPAWN_PURPOSE_POLICE,
		"H: a law enforcer leaves in the POLICE row");
	CheckInt((int)SpawnPurposeOfRemovedVehicle(SPAWN_VEHICLE_TAG_RANDOM, false, false), SPAWN_PURPOSE_AMBIENT,
		"H: a plain random car leaves in the AMBIENT row");
	CheckInt((int)SpawnPurposeOfRemovedVehicle(SPAWN_VEHICLE_TAG_MISSION, true, true), SPAWN_PURPOSE_MISSION,
		"H: a mission car keeps the MISSION row");
	CheckInt((int)SpawnPurposeOfRemovedVehicle(SPAWN_VEHICLE_TAG_PARKED, false, false), SPAWN_PURPOSE_PARKED,
		"H: a parked car leaves in the PARKED row");
	CheckInt((int)SpawnPurposeOfRemovedVehicle(SPAWN_VEHICLE_TAG_PERMANENT, false, false), SPAWN_PURPOSE_SPECIAL,
		"H: a permanent car leaves in the SPECIAL row");

	SpawnTelemetryReset(&tel);
	before = tel.m_nRemovedByPurpose[SPAWN_PURPOSE_POLICE];
	SpawnNoteRemoval(&tel, SpawnPurposeOfRemovedVehicle(SPAWN_VEHICLE_TAG_RANDOM, true, false));
	CheckInt((int)Delta(tel.m_nRemovedByPurpose[SPAWN_PURPOSE_POLICE], before), 1,
		"H: the removal row counts the police car");

	printf("    crew: ambulance driver+1, fire truck driver+0, van driver+3, sedan driver+1 at 2 stars, Predator driver\n");
	printf("    removal rows: responder, police, ambient, mission, parked, special all reachable\n");
}

/* ------------------------------------------------------------------ the cost */

int
main(void)
{
	BuildFixture();
	g_index.Build(FixtureGraph());
	Check(g_index.GetNumSites() > 100, "the fixture builds its sites");
	Check(g_index.GetStats().boatSites > 0, "the fixture holds boat sites");

	printf("spawn responders harness (VICE-156)\n");
	printf("  the fixture: %d nodes, %d links, %d connections, %d sites (%d boat)\n",
		g_numNodes, g_numLinks, g_numConnections,
		g_index.GetNumSites(), g_index.GetStats().boatSites);

	CheckBubbleCrafting();
	SectionPolicy();
	SectionCrime();
	SectionAccident();
	SectionFire();
	SectionEmergencyCaps();
	SectionPoliceCaps();
	SectionPlacementGates();
	SectionCrewAndLedger();

	printf("  %lld checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
