/*
 * The evidence program for VICE-160, the spawn planner's rank on a worker
 * thread.
 *
 * It compiles and drives the game's own worker module
 * (src/control/SpawnPlannerWorker.cpp - the real cycle, the real handshake,
 * the real snapshot and occupancy copies) and the planner's own engine-free
 * core (src/control/SpawnService.h, src/control/SpawnSitesIndex.cpp) over a
 * synthetic site graph. No engine header is included anywhere (the module does
 * not include one, and that is half of the proof).
 *
 *	A. The snapshot and the rank. The site copy is taken from the index
 *	   (SetSites), refreshed when the index's flags refresh, and a debug
 *	   cycle hashes every block it may read before and after the rank: the
 *	   site snapshot, the occupancy copy and the command ring must not move,
 *	   and neither must the other output block. The rank itself is the
 *	   planner's SpawnRankSites - the same function the inline path calls -
 *	   so the prefix is compared site by site over N scripted frames.
 *
 *	B. The plan equivalence. The same requests, stores, scripted deep facts
 *	   and attempts are driven twice: the inline planner (SpawnPlanSites +
 *	   SpawnRunPlan, on the live store) and the worker path (QueueRank, the
 *	   published plan, SpawnPlanFromRank + SpawnRunPlan, on a second store
 *	   with the identical history). The worker's plan of frame N-1 is applied
 *	   on frame N - the stated one-frame lag - and the comparison is against
 *	   the inline plan of frame N-1: the same ranked prefix, the same plan
 *	   sites, the same reservation set, the same creations and refusals, and
 *	   the same telemetry rows (cheap refusals, deep refusals, placements,
 *	   creations).
 *
 *	C. The contention. A plan ranked against a clean store is applied to a
 *	   store that moved after the publish: the top site reserved by another
 *	   request, the top site's cell filled, the top site's deep facts refused.
 *	   The tail's plan must equal the inline planner's plan for the same
 *	   store state (the reserve re-check, the next ranked candidate), with the
 *	   same OCCUPANCY row for the reservation (a reservation does not move the
 *	   rank's other keys). The cell fill moves the rank's own occupancy key for
 *	   the candidates in that cell, so its examined prefix may differ while
 *	   the plans still agree; both counts are printed. The prefix bound is
 *	   shown: when all eight ranked candidates are contended the worker's plan
 *	   is empty (the stated bound) while the inline planner reaches a ninth.
 *
 *	D. The write boundary and the window. The worker is idle after Join and
 *	   no main-thread entry point runs while it is in flight; a cycle
 *	   allocates nothing (a global operator new that counts while the cycle
 *	   runs); the module's own hash audit runs in the debug build this program
 *	   compiles; the site index and the stores hash the same around a cycle.
 *	   What this program cannot reach, stated plainly: the engine's frame
 *	   hooks (CGame::Process, CWorld::Process, the render) are the game's; the
 *	   harness proves the module's side of the window (Join/Published/Run and
 *	   AssertIdle) and the code path is inspected.
 *
 *	E. The cost. The inline rank is timed over the fixture's offers; the
 *	   worker's round trip is timed as the main thread pays it (the occupancy
 *	   publish, the command copy, the hand-over and the join), and the cycle
 *	   is timed inline for its own compute. The numbers are printed at the
 *	   end, with the candidate counts they were measured at.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 -pthread \
 *	    -o /tmp/spawn-worker utils/spawn-service/worker.cpp \
 *	    src/control/SpawnPlannerWorker.cpp src/control/SpawnSitesIndex.cpp
 *	/tmp/spawn-worker
 *
 *	# the cost without the debug build's write audit (the game's Release)
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 -pthread -DNDEBUG \
 *	    -o /tmp/spawn-worker-release utils/spawn-service/worker.cpp \
 *	    src/control/SpawnPlannerWorker.cpp src/control/SpawnSitesIndex.cpp
 *	/tmp/spawn-worker-release --cost
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "SpawnPlannerWorker.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The engine's own names for the fixed-width types (common.h), so the code
 * below reads like the code it mirrors; the module itself uses the _t names. */
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;

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

static double
NowSeconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ the fixture */

/*
 * The synthetic city: an 8 x 8 grid of road nodes at 40 m spacing, two lanes
 * each way, one one-way row (the westward travel has no lanes, so the index
 * drops that direction), one disabled node (2,2) the flags refresh toggles
 * back on at frame 250. Every edge is one link with two directed connections;
 * the index makes a site of each usable direction (217 sites).
 */

#define GRID 8
#define GRID_SPACING 40.0f
#define GRID_NODES (GRID * GRID)
#define MAX_EDGES (2 * GRID * (GRID - 1))

static SSpawnSiteNode g_nodes[GRID_NODES];
static SSpawnSiteLink g_links[MAX_EDGES];
static uint16_t g_connNode[NUM_PATHCONNECTIONS];
static int16_t g_connLink[NUM_PATHCONNECTIONS];
static int16_t g_connZone[NUM_PATHCONNECTIONS];

struct SEdge
{
	int32 a, b;
	int8 left, right;	/* the lanes of b->a and a->b */
};

static SEdge g_edges[MAX_EDGES];
static int32 g_numEdges;

static inline int32
RoadNode(int row, int col)
{
	return row * GRID + col;
}

static void
AddEdge(int32 a, int32 b, int8 left, int8 right)
{
	g_edges[g_numEdges].a = a;
	g_edges[g_numEdges].b = b;
	g_edges[g_numEdges].left = left;
	g_edges[g_numEdges].right = right;
	g_numEdges++;
}

static CSpawnSiteIndex g_index;

static void
BuildFixture(void)
{
	int32 row, col, i, cursor;

	memset(g_nodes, 0, sizeof(g_nodes));
	memset(g_links, 0, sizeof(g_links));
	memset(g_connNode, 0, sizeof(g_connNode));
	memset(g_connLink, 0, sizeof(g_connLink));
	memset(g_connZone, 0, sizeof(g_connZone));
	g_numEdges = 0;

	for (row = 0; row < GRID; row++) {
		for (col = 0; col < GRID; col++) {
			SSpawnSiteNode *pNode = &g_nodes[RoadNode(row, col)];
			pNode->x = col * GRID_SPACING;
			pNode->y = row * GRID_SPACING;
			pNode->z = 5.0f;
			pNode->spawnRate = (uint8)(1 + row % 3);
			if (row == 2 && col == 2)
				pNode->flags |= SPAWN_SITE_NODE_DISABLED;
		}
	}
	/* The horizontal edges: row 2 is east one-way (the b->a travel has no
	 * lanes and the index drops it). */
	for (row = 0; row < GRID; row++) {
		for (col = 0; col < GRID - 1; col++) {
			if (row == 2)
				AddEdge(RoadNode(row, col), RoadNode(row, col + 1), 0, 2);
			else
				AddEdge(RoadNode(row, col), RoadNode(row, col + 1), 2, 2);
		}
	}
	for (row = 0; row < GRID - 1; row++)
		for (col = 0; col < GRID; col++)
			AddEdge(RoadNode(row, col), RoadNode(row + 1, col), 2, 2);

	/* The link records: made from the lower node, its own direction west or
	 * south (the index's sign rule); a->b travels the right lanes, b->a the
	 * left. */
	for (i = 0; i < g_numEdges; i++) {
		bool horizontal = g_edges[i].a / GRID == g_edges[i].b / GRID;

		g_links[i].dirX = horizontal ? -100 : 0;
		g_links[i].dirY = horizontal ? 0 : -100;
		g_links[i].numLeftLanes = g_edges[i].left;
		g_links[i].numRightLanes = g_edges[i].right;
		g_links[i].pathNodeIndex = (int16)g_edges[i].a;
		g_links[i].width = 10;
	}

	/* The connections, per node, in edge order. */
	cursor = 0;
	for (i = 0; i < GRID_NODES; i++) {
		int32 e;
		uint8 n = 0;

		g_nodes[i].firstLink = (int16)cursor;
		for (e = 0; e < g_numEdges; e++) {
			const SEdge *pEdge = &g_edges[e];
			int32 other;

			if (pEdge->a == i)
				other = pEdge->b;
			else if (pEdge->b == i)
				other = pEdge->a;
			else
				continue;
			g_connNode[cursor] = (uint16)other;
			g_connLink[cursor] = (int16)e;
			g_connZone[cursor] = 0;
			cursor++;
			n++;
		}
		g_nodes[i].numLinks = n;
	}

	SSpawnSiteGraph graph;
	graph.numNodes = GRID_NODES;
	graph.numLinks = g_numEdges;
	graph.numConnections = cursor;
	graph.nodes = g_nodes;
	graph.links = g_links;
	graph.connectionNode = g_connNode;
	graph.connectionLink = g_connLink;
	graph.connectionZone = g_connZone;
	g_index.Build(graph);
}

/* ------------------------------------------------------------- the scripts */

/*
 * Per-site scripts, so every scenario puts exactly the facts it wants on a
 * candidate. They are site-only (not frame-dependent), so the inline plan of
 * frame N and the worker's apply on frame N+1 resolve the same facts:
 *
 *	g_armOf[site]		the arm the ladder's candidate must produce
 *	g_zoneRefused[site]	the deep zone fact
 *	g_reachRefused[site]	the deep reach fact
 *	g_tryRefusal[site]	the attempt's answer (NONE: created)
 */
static uint8 g_armOf[NUM_PATHCONNECTIONS];
static bool g_zoneRefused[NUM_PATHCONNECTIONS];
static bool g_reachRefused[NUM_PATHCONNECTIONS];
static ESpawnRefusal g_tryRefusal[NUM_PATHCONNECTIONS];

static void
ScriptsReset(void)
{
	int32 i;

	for (i = 0; i < NUM_PATHCONNECTIONS; i++) {
		static const uint8 arms[8] = {
			NEAR_BUBBLE_ARM_SOFT, NEAR_BUBBLE_ARM_SOFT,
			NEAR_BUBBLE_ARM_FADE_DISTANCE, NEAR_BUBBLE_ARM_RAY,
			NEAR_BUBBLE_ARM_FADE, NEAR_BUBBLE_ARM_BYPASS,
			NEAR_BUBBLE_ARM_ONSCREEN_BAND, NEAR_BUBBLE_ARM_NONE,
		};
		g_armOf[i] = arms[i % 8];
		g_zoneRefused[i] = (i * 7 + 3) % 31 == 0;
		g_reachRefused[i] = (i * 11 + 5) % 37 == 0;
		g_tryRefusal[i] = ((i * 13) % 41 == 0) ? SPAWN_REFUSAL_VISIBILITY : SPAWN_REFUSAL_NONE;
	}
}

/* The ladder's candidate of one site, crafted to produce the wanted arm. The
 * harness checks the crafted facts against NearBubbleArmFor, so a script
 * cannot lie about the arm the rank reads. */
static void
CandidateForArm(SNearBubbleCandidate *c, uint8 arm)
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

static void
HarnessDeep(void *user, const SSpawnOffer &offer, SSpawnSiteDeepFacts *facts)
{
	(void)user;
	facts->zoneOk = !g_zoneRefused[offer.site];
	facts->reachOk = !g_reachRefused[offer.site];
}

struct SAttempt
{
	SSpawnOccupancyStore *pStore;
	int32 createdSite;
	int32 nextHandle;
};

static ESpawnRefusal
HarnessTry(void *user, const SSpawnPlanSite &planSite, uint8_t *pArm)
{
	SAttempt *pAttempt = (SAttempt *)user;
	const SSpawnSite &site = g_index.GetSites()[planSite.site];

	(void)pArm;
	if (g_tryRefusal[planSite.site] != SPAWN_REFUSAL_NONE)
		return g_tryRefusal[planSite.site];
	pAttempt->createdSite = planSite.site;
	pAttempt->pStore->Create(SPAWN_OCCUPANCY_VEHICLE, pAttempt->nextHandle++, site.x, site.y);
	return SPAWN_REFUSAL_NONE;
}

/* One request of a scripted frame: the target walks the grid's interior, the
 * ring is wide enough for the rank to have a prefix to order. */
static SSpawnCarRequest
MakeRequest(int32 frame, int32 k)
{
	SSpawnCarRequest r;

	memset(&r, 0, sizeof(r));
	r.purpose = SPAWN_PURPOSE_AMBIENT;
	r.force = false;
	r.allowBoats = false;
	r.requireReach = true;
	r.carClass = 0;
	r.carModel = 0;
	r.targetX = 60.0f + (float)((frame * 13 + k * 29) % 160);
	r.targetY = 60.0f + (float)((frame * 7 + k * 17) % 160);
	r.targetZ = 5.0f;
	r.spawnDist = 120.0f;
	r.dirX = 1.0f;
	r.dirY = 0.0f;
	r.angleLimit = -1.0f;	/* the top-down case: every bearing passes */
	r.forward = true;
	r.allowDisabled = false;
	return r;
}

/* The offer walk and the ladder's candidates, the two inputs of the rank. */
static int32
BuildOffers(const SSpawnCarRequest &request, SSpawnOffer *offers, SNearBubbleCandidate *bubbles)
{
	SSpawnRingQuery query;
	int32 n, i;

	query.targetX = request.targetX;
	query.targetY = request.targetY;
	query.spawnDist = request.spawnDist;
	query.dirX = request.dirX;
	query.dirY = request.dirY;
	query.angleLimit = request.angleLimit;
	query.forward = request.forward;
	query.water = request.allowBoats ? SPAWN_SITES_ANY : SPAWN_SITES_LAND;
	query.allowDisabled = request.allowDisabled;

	n = SpawnOfferSites(g_index, query, offers, SPAWN_SERVICE_MAX_CANDIDATES);
	for (i = 0; i < n; i++) {
		CandidateForArm(&bubbles[i], g_armOf[offers[i].site]);
		if (NearBubbleArmFor(bubbles[i]) != g_armOf[offers[i].site])
			Fail("the crafted bubble produces the scripted arm");
	}
	return n;
}

/* The scripted presence noise: two cars enter, one leaves every twenty
 * frames, and one moves to a far cell every fifteen. The two stores of the
 * harness get the identical calls, so their history is the same. */
static void
MutateStore(SSpawnOccupancyStore &store, int32 frame)
{
	int32 k;

	for (k = 0; k < 2; k++) {
		int32 site = (frame * 17 + k * 53) % g_index.GetNumSites();
		const SSpawnSite &s = g_index.GetSites()[site];

		store.Create(SPAWN_OCCUPANCY_VEHICLE, (frame * 2 + k) % 1500, s.x, s.y);
	}
	if (frame >= 20) {
		int32 prev = frame - 20;
		store.Remove(SPAWN_OCCUPANCY_VEHICLE, (prev * 2) % 1500);
	}
	if (frame % 15 == 0) {
		int32 handle = 1000 + (frame % 400);

		store.Create(SPAWN_OCCUPANCY_VEHICLE, handle, 10.0f, 10.0f);
		store.Move(SPAWN_OCCUPANCY_VEHICLE, handle, 300.0f, 300.0f);
	}
}

/* ------------------------------------------------------------------ the plans */

/* The inline decision of one scripted request, and the debug record the
 * worker's plan of the next frame is compared against. */
struct SDebugPlan
{
	int32 rankCount;
	int32 rankSites[SPAWN_SERVICE_DEEP_CANDIDATES];
	int32 planCount;
	int32 planSites[SPAWN_PLAN_MAX_PLACEMENTS];
	int32 createdSite;
	ESpawnRefusal runRefusal;
};

static int32 g_inlineHandle = 2000;
static int32 g_workerHandle = 2000;

static void
InlineRequest(const SSpawnCarRequest &request, SSpawnOccupancyStore &store,
	SSpawnServiceTelemetry &tel, SDebugPlan *pDebug, uint32 now)
{
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	SSpawnRank rank;
	SSpawnPlan plan;
	SAttempt attempt;
	int32 n, i;

	n = BuildOffers(request, offers, bubbles);
	SpawnRankSites(g_index.GetSites(), offers, bubbles, n, store.View(), now, &rank);
	pDebug->rankCount = rank.count;
	for (i = 0; i < rank.count; i++)
		pDebug->rankSites[i] = offers[rank.entries[i].offer].site;

	SpawnPlanSites(g_index, offers, bubbles, n, &store, now, HarnessDeep, 0, &plan, &tel);
	pDebug->planCount = plan.count;
	for (i = 0; i < plan.count; i++)
		pDebug->planSites[i] = plan.sites[i].site;

	attempt.pStore = &store;
	attempt.createdSite = -1;
	attempt.nextHandle = g_inlineHandle;
	pDebug->runRefusal = SpawnRunPlan(plan, &store, HarnessTry, &attempt, &tel, request.purpose);
	pDebug->createdSite = attempt.createdSite;
	g_inlineHandle = attempt.nextHandle;
}

static void
ApplyWorkerPlan(const SSpawnRankedPlan &ranked, SSpawnOccupancyStore &store,
	SSpawnServiceTelemetry &tel, SDebugPlan *pDebug, uint32 now)
{
	SSpawnPlan plan;
	SAttempt attempt;
	int32 i;

	SpawnPlanFromRank(ranked, g_index.GetSites(), &store, now, HarnessDeep, 0, &plan, &tel);
	pDebug->planCount = plan.count;
	for (i = 0; i < plan.count; i++)
		pDebug->planSites[i] = plan.sites[i].site;

	attempt.pStore = &store;
	attempt.createdSite = -1;
	attempt.nextHandle = g_workerHandle;
	pDebug->runRefusal = SpawnRunPlan(plan, &store, HarnessTry, &attempt, &tel, ranked.request.purpose);
	pDebug->createdSite = attempt.createdSite;
	g_workerHandle = attempt.nextHandle;
}

/* The comparison of one worker plan (the rank of frame N-1) against the
 * inline record of the same request. */
static void
ComparePlan(const SSpawnRankedPlan &ranked, const SDebugPlan &inlineDebug,
	const SDebugPlan &workerDebug, const char *what)
{
	int32 i;

	CheckInt(ranked.count, inlineDebug.rankCount, what);
	for (i = 0; i < ranked.count && i < inlineDebug.rankCount; i++)
		if (ranked.sites[i].offer.site != inlineDebug.rankSites[i]) {
			g_failures++;
			printf("  FAIL: %s: the ranked prefix differs at %d (inline %d, worker %d)\n",
				what, i, inlineDebug.rankSites[i], ranked.sites[i].offer.site);
		}
	CheckInt(workerDebug.planCount, inlineDebug.planCount, what);
	for (i = 0; i < workerDebug.planCount && i < inlineDebug.planCount; i++)
		if (workerDebug.planSites[i] != inlineDebug.planSites[i]) {
			g_failures++;
			printf("  FAIL: %s: the plan differs at %d (inline %d, worker %d)\n",
				what, i, inlineDebug.planSites[i], workerDebug.planSites[i]);
		}
	CheckInt(workerDebug.createdSite, inlineDebug.createdSite, what);
	CheckInt(workerDebug.runRefusal, inlineDebug.runRefusal, what);
}

/* ------------------------------------------------------------------ section A */

/* The equivalence's frame: one road-site request a frame, so the store's only
 * movement between the rank and the apply is the frame's world (the scripted
 * presence noise) and never the same frame's own creations. The same-frame
 * creations are contention, and section B holds them (the tail's re-check).
 * The ambient spawner's three calls a frame are the contended case; the
 * one-frame-lag proof does not need them. */
#define FRAMES 400
#define REQUESTS_PER_FRAME 1

static void
SectionEquivalence(void)
{
	static SDebugPlan inlineDebug[FRAMES][REQUESTS_PER_FRAME];
	static SDebugPlan workerDebug[REQUESTS_PER_FRAME];
	static SSpawnCarRequest requests[REQUESTS_PER_FRAME];
	SSpawnOccupancyStore inlineStore;
	SSpawnOccupancyStore workerStore;
	SSpawnServiceTelemetry inlineTel;
	SSpawnServiceTelemetry workerTel;
	uint32 now = 1000;
	int32 frame, k, i;

	printf("A. the rank and the plan: the worker against the inline planner over %d frames\n", FRAMES);

	/* The worker's snapshot and thread, and the two stores with the same
	 * history. */
	CSpawnPlannerWorker::Stop();
	CSpawnPlannerWorker::Reset();
	CSpawnPlannerWorker::SetSites(g_index.GetSites(), g_index.GetNumSites());
	CSpawnPlannerWorker::SetThreaded(true);
	CSpawnPlannerWorker::Start();
	SpawnTelemetryReset(&inlineTel);
	SpawnTelemetryReset(&workerTel);

	for (frame = 0; frame < FRAMES; frame++) {
		/* The join first (the game's order): the plans of frame N-1 are
		 * applied to the worker store, then compared with the inline record. */
		if (frame > 0) {
			const SSpawnPlannerOutput *pOutput;

			CSpawnPlannerWorker::Join();
			Check(!CSpawnPlannerWorker::InFlight(), "the worker is idle after the join");
			CSpawnPlannerWorker::AssertIdle();
			pOutput = CSpawnPlannerWorker::Published();
			CheckInt(pOutput->cycle, frame, "the published cycle moves with the hand-over");
			CheckInt(pOutput->numPlans, REQUESTS_PER_FRAME, "the published batch holds the frame's plans");
			CheckInt(pOutput->numLost, 0, "no command is lost");
			for (k = 0; k < REQUESTS_PER_FRAME; k++) {
				char what[96];

				snprintf(what, sizeof(what), "the plan of frame %d request %d", frame - 1, k);
				ApplyWorkerPlan(pOutput->plans[k], workerStore, workerTel, &workerDebug[k], now);
				ComparePlan(pOutput->plans[k], inlineDebug[frame-1][k], workerDebug[k], what);
			}
		}

		/* The frame's world: the same presence noise on both stores. */
		Check(memcmp(inlineStore.m_aVehicleCount, workerStore.m_aVehicleCount,
			sizeof(inlineStore.m_aVehicleCount)) == 0, "the two stores' vehicle counts agree at the frame start");
		Check(inlineStore.m_nVehicleTotal == workerStore.m_nVehicleTotal, "the two stores' totals agree");
		MutateStore(inlineStore, frame);
		MutateStore(workerStore, frame);

		/* The site flags refresh (the script's road toggle): the index
		 * re-derives its sites' flags and the worker's snapshot follows. */
		if (frame == 250) {
			g_nodes[RoadNode(2, 2)].flags &= ~SPAWN_SITE_NODE_DISABLED;
			g_index.RefreshFlags(g_nodes);
			CSpawnPlannerWorker::RefreshSiteFlags(g_index.GetSites(), g_index.GetNumSites());
		}

		/* The inline planner, request by request, on the live store. */
		for (k = 0; k < REQUESTS_PER_FRAME; k++) {
			requests[k] = MakeRequest(frame, k);
			SpawnRequestBegin(&inlineTel, requests[k].purpose, SPAWN_KIND_ROAD_SITE);
			InlineRequest(requests[k], inlineStore, inlineTel, &inlineDebug[frame][k], now);
		}

		/* The worker's rank for this frame: the commands, the occupancy copy
		 * (the store as the applying frame starts from it), the hand-over. */
		for (k = 0; k < REQUESTS_PER_FRAME; k++) {
			SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
			SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
			int32 n;

			SpawnRequestBegin(&workerTel, requests[k].purpose, SPAWN_KIND_ROAD_SITE);
			n = BuildOffers(requests[k], offers, bubbles);
			Check(CSpawnPlannerWorker::QueueRank(requests[k], offers, bubbles, n),
				"the command fits the ring");
		}
		CSpawnPlannerWorker::PublishOccupancy(workerStore);
		Check(CSpawnPlannerWorker::Run(now), "the frame's cycle is handed over");
		now += 16;
	}

	/* The last published batch (the plans of FRAMES-1), applied and compared. */
	{
		const SSpawnPlannerOutput *pOutput;

		CSpawnPlannerWorker::Join();
		CSpawnPlannerWorker::AssertIdle();
		pOutput = CSpawnPlannerWorker::Published();
		CheckInt(pOutput->cycle, FRAMES, "the last published cycle is the last hand-over");
		for (k = 0; k < REQUESTS_PER_FRAME; k++) {
			char what[96];

			snprintf(what, sizeof(what), "the plan of frame %d request %d", FRAMES - 1, k);
			ApplyWorkerPlan(pOutput->plans[k], workerStore, workerTel, &workerDebug[k], now);
			ComparePlan(pOutput->plans[k], inlineDebug[FRAMES-1][k], workerDebug[k], what);
		}
	}

	/* The telemetry: the same requests, candidates, refusals, placements and
	 * creations on both paths. */
	for (i = 0; i < SPAWN_REFUSAL_COUNT; i++) {
		char what[96];

		snprintf(what, sizeof(what), "the telemetry's refusal row %d matches", i);
		CheckInt((int)workerTel.m_nRefusalsByReason[i], (int)inlineTel.m_nRefusalsByReason[i], what);
	}
	for (i = 0; i < SPAWN_PURPOSE_COUNT; i++) {
		char what[96];

		snprintf(what, sizeof(what), "the telemetry's created row %d matches", i);
		CheckInt((int)workerTel.m_nCreatedByPurpose[i], (int)inlineTel.m_nCreatedByPurpose[i], what);
	}
	CheckInt((int)workerTel.m_nPlanCandidates, (int)inlineTel.m_nPlanCandidates, "the candidate counts match");
	CheckInt((int)workerTel.m_nPlanPlacements, (int)inlineTel.m_nPlanPlacements, "the placement counts match");
	CheckInt((int)workerTel.m_nRequestsByKind[SPAWN_KIND_ROAD_SITE],
		(int)inlineTel.m_nRequestsByKind[SPAWN_KIND_ROAD_SITE], "the request counts match");
	printf("    %d requests, %d plans: the prefix, the plan, the creations and the telemetry match\n",
		FRAMES * REQUESTS_PER_FRAME, FRAMES * REQUESTS_PER_FRAME);

	/* The reservations are the plan's, and the run releases them. */
	{
		SSpawnOccupancyStore store;
		SSpawnServiceTelemetry tel;
		SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
		SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
		SSpawnPlan plan;
		SAttempt attempt;
		int32 n;

		n = BuildOffers(MakeRequest(7, 0), offers, bubbles);
		SpawnPlanSites(g_index, offers, bubbles, n, &store, 42, HarnessDeep, 0, &plan, &tel);
		for (i = 0; i < plan.count; i++)
			Check(store.SiteReserved(plan.sites[i].site, 42), "the plan holds its sites");
		attempt.pStore = &store;
		attempt.createdSite = -1;
		attempt.nextHandle = 3000;
		SpawnRunPlan(plan, &store, HarnessTry, &attempt, &tel, SPAWN_PURPOSE_AMBIENT);
		for (i = 0; i < plan.count; i++)
			Check(!store.SiteReserved(plan.sites[i].site, 42), "the run releases its sites");
	}
}

/* ------------------------------------------------------------------ section B */

/*
 * The contention cases. The worker's rank is taken against a clean store, the
 * way the publish is taken at the end of a frame; the tail then applies it to
 * a store that moved, and the inline planner runs on that same store. The two
 * plans must agree (the next ranked candidate), and the OCCUPANCY row must
 * count the conflict once on each path.
 */
static int32
CleanRank(const SSpawnCarRequest &request, SSpawnRankedPlan *pRanked, SSpawnOffer *offers,
	SNearBubbleCandidate *bubbles, uint32 now)
{
	SSpawnOccupancyStore clean;
	SSpawnRank rank;
	int32 n;

	n = BuildOffers(request, offers, bubbles);
	SpawnRankSites(g_index.GetSites(), offers, bubbles, n, clean.View(), now, &rank);
	memset(pRanked, 0, sizeof(*pRanked));
	pRanked->request = request;
	SpawnRankedPlanFromRank(rank, offers, pRanked);
	return n;
}

/* The contention comparison: the worker's tail and the inline planner on the
 * same store state. */
static void
ContentionCase(const char *what, const SSpawnCarRequest &request, uint32 now,
	SSpawnOccupancyStore &contended, bool bOccupancyParity)
{
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	SSpawnServiceTelemetry workerTel;
	SSpawnServiceTelemetry inlineTel;
	SSpawnRankedPlan ranked;
	SSpawnPlan workerPlan;
	SSpawnPlan inlinePlan;
	SSpawnOccupancyStore workerStore = contended;
	SSpawnOccupancyStore inlineStore = contended;
	SAttempt attempt;
	int32 n, i;

	n = CleanRank(request, &ranked, offers, bubbles, now);
	SpawnTelemetryReset(&workerTel);
	SpawnTelemetryReset(&inlineTel);

	SpawnPlanFromRank(ranked, g_index.GetSites(), &workerStore, now, HarnessDeep, 0, &workerPlan, &workerTel);
	attempt.pStore = &workerStore;
	attempt.createdSite = -1;
	attempt.nextHandle = 4000;
	SpawnRunPlan(workerPlan, &workerStore, HarnessTry, &attempt, &workerTel, request.purpose);

	SpawnPlanSites(g_index, offers, bubbles, n, &inlineStore, now, HarnessDeep, 0, &inlinePlan, &inlineTel);
	attempt.pStore = &inlineStore;
	attempt.createdSite = -1;
	attempt.nextHandle = 4000;
	SpawnRunPlan(inlinePlan, &inlineStore, HarnessTry, &attempt, &inlineTel, request.purpose);

	CheckInt(workerPlan.count, inlinePlan.count, what);
	for (i = 0; i < workerPlan.count && i < inlinePlan.count; i++)
		if (workerPlan.sites[i].site != inlinePlan.sites[i].site) {
			g_failures++;
			printf("  FAIL: %s: the plan differs at %d (inline %d, worker %d)\n",
				what, i, inlinePlan.sites[i].site, workerPlan.sites[i].site);
		}
	if (bOccupancyParity)
		CheckInt((int)workerTel.m_nRefusalsByReason[SPAWN_REFUSAL_OCCUPANCY],
			(int)inlineTel.m_nRefusalsByReason[SPAWN_REFUSAL_OCCUPANCY], "the occupancy row matches");
	CheckInt((int)workerTel.m_nCreatedByPurpose[SPAWN_PURPOSE_AMBIENT],
		(int)inlineTel.m_nCreatedByPurpose[SPAWN_PURPOSE_AMBIENT], "the created row matches");
	printf("    %s: %d placements, %d occupancy refusals (inline %d)%s\n", what,
		workerPlan.count, (int)workerTel.m_nRefusalsByReason[SPAWN_REFUSAL_OCCUPANCY],
		(int)inlineTel.m_nRefusalsByReason[SPAWN_REFUSAL_OCCUPANCY],
		bOccupancyParity ? ", the same on both" : "");
}

static void
SectionContention(void)
{
	SSpawnCarRequest request = MakeRequest(11, 1);
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	SSpawnRankedPlan ranked;
	SSpawnOccupancyStore clean;
	SSpawnOccupancyStore store;
	SSpawnPlan workerPlan;
	SSpawnPlan inlinePlan;
	SSpawnServiceTelemetry workerTel;
	SSpawnServiceTelemetry inlineTel;
	int32 n, i;
	uint32 now = 5000;

	printf("B. the contention: the tail against the inline planner for the same store state\n");

	/* The clean rank the worker publishes. */
	n = BuildOffers(request, offers, bubbles);
	{
		SSpawnRank rank;

		SpawnRankSites(g_index.GetSites(), offers, bubbles, n, clean.View(), now, &rank);
		memset(&ranked, 0, sizeof(ranked));
		ranked.request = request;
		SpawnRankedPlanFromRank(rank, offers, &ranked);
		CheckInt(ranked.count, SPAWN_SERVICE_DEEP_CANDIDATES, "the clean rank fills its prefix");
	}

	/* (a) the top site is reserved by another request after the publish. */
	store = clean;
	Check(store.ReserveSite(ranked.sites[0].offer.site, now), "the intruder reserves the top site");
	ContentionCase("the top site reserved", request, now, store, true);

	/* (b) the top site's cell is filled to the capacity after the publish.
	 * The cell fill moves the rank's occupancy key for every candidate in
	 * that cell, so the inline rank's order (the contended store) can differ
	 * from the worker's published order (the clean store) beyond the dropped
	 * site; the plans must still agree, the examined prefixes need not. */
	store = clean;
	{
		const SSpawnSite &top = g_index.GetSites()[ranked.sites[0].offer.site];

		store.Create(SPAWN_OCCUPANCY_VEHICLE, 500, top.x, top.y);
		store.Create(SPAWN_OCCUPANCY_VEHICLE, 501, top.x, top.y);
		store.Create(SPAWN_OCCUPANCY_VEHICLE, 502, top.x, top.y);
	}
	ContentionCase("the top site's cell filled", request, now, store, false);

	/* (c) the top site's deep zone fact refuses after the rank (a scripted
	 * zone move): both tails drop it by the same reason. */
	store = clean;
	g_zoneRefused[ranked.sites[0].offer.site] = true;
	{
		SSpawnServiceTelemetry workerTel2, inlineTel2;
		SSpawnPlan wp, ip;
		SSpawnOccupancyStore workerStore = store;
		SSpawnOccupancyStore inlineStore = store;

		SpawnTelemetryReset(&workerTel2);
		SpawnTelemetryReset(&inlineTel2);
		SpawnPlanFromRank(ranked, g_index.GetSites(), &workerStore, now, HarnessDeep, 0, &wp, &workerTel2);
		SpawnPlanSites(g_index, offers, bubbles, n, &inlineStore, now, HarnessDeep, 0, &ip, &inlineTel2);
		CheckInt(wp.count, ip.count, "the zone-refused top site: the plans agree");
		for (i = 0; i < wp.count && i < ip.count; i++)
			CheckInt(wp.sites[i].site, ip.sites[i].site, "the zone-refused top site: the next candidates agree");
		CheckInt((int)workerTel2.m_nRefusalsByReason[SPAWN_REFUSAL_ZONE],
			(int)inlineTel2.m_nRefusalsByReason[SPAWN_REFUSAL_ZONE], "the zone row matches");
	}
	g_zoneRefused[ranked.sites[0].offer.site] = false;

	/* (d) the prefix bound: every published candidate is contended, so the
	 * worker's tail has nothing left (the stated bound), while the inline
	 * planner sees the next candidates beyond the prefix. */
	store = clean;
	for (i = 0; i < ranked.count; i++)
		Check(store.ReserveSite(ranked.sites[i].offer.site, now), "the bound case reserves the prefix");
	SpawnTelemetryReset(&workerTel);
	SpawnTelemetryReset(&inlineTel);
	SpawnPlanFromRank(ranked, g_index.GetSites(), &store, now, HarnessDeep, 0, &workerPlan, &workerTel);
	SpawnPlanSites(g_index, offers, bubbles, n, &store, now, HarnessDeep, 0, &inlinePlan, &inlineTel);
	CheckInt(workerPlan.count, 0, "the prefix bound: the worker's plan is empty");
	Check(inlinePlan.count > 0, "the prefix bound: the inline planner reaches past the prefix");
	CheckInt((int)workerTel.m_nRefusalsByReason[SPAWN_REFUSAL_OCCUPANCY],
		SPAWN_SERVICE_DEEP_CANDIDATES, "the prefix bound: all eight conflicts are counted");
	if (inlinePlan.count > 0)
		printf("    the prefix bound: the worker's 8 candidates are contended, the inline plan's first site is %d\n",
			inlinePlan.sites[0].site);
}

/* ------------------------------------------------------------------ section C */

static uint64 g_allocCount;
static bool g_countAllocs;

void *
operator new(size_t size)
{
	void *p;

	if (g_countAllocs)
		g_allocCount++;
	p = malloc(size);
	if (p == 0)
		abort();
	return p;
}

void
operator delete(void *p) noexcept
{
	free(p);
}

void *
operator new[](size_t size)
{
	void *p;

	if (g_countAllocs)
		g_allocCount++;
	p = malloc(size);
	if (p == 0)
		abort();
	return p;
}

void
operator delete[](void *p) noexcept
{
	free(p);
}

static uint32
HashBytes(const void *p, size_t bytes)
{
	const uint8 *b = (const uint8 *)p;
	uint32 hash = 2166136261u;

	while (bytes-- > 0) {
		hash ^= *b++;
		hash *= 16777619u;
	}
	return hash;
}

static void
SectionBoundary(void)
{
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	SSpawnCarRequest request = MakeRequest(3, 2);
	SSpawnOccupancyStore store;
	uint32 indexHash, storeHash;
	int32 n;

	printf("C. the write boundary: the window, the inputs and the allocation\n");

	CSpawnPlannerWorker::Stop();
	CSpawnPlannerWorker::Reset();
	CSpawnPlannerWorker::SetSites(g_index.GetSites(), g_index.GetNumSites());
	CSpawnPlannerWorker::SetThreaded(true);
	CSpawnPlannerWorker::Start();
	n = BuildOffers(request, offers, bubbles);
	Check(CSpawnPlannerWorker::QueueRank(request, offers, bubbles, n), "the boundary command fits");
	CSpawnPlannerWorker::PublishOccupancy(store);

	indexHash = HashBytes(g_index.GetSites(), (size_t)g_index.GetNumSites() * sizeof(SSpawnSite));
	storeHash = HashBytes(&store, sizeof(store));

	CSpawnPlannerWorker::AssertIdle();
	Check(!CSpawnPlannerWorker::InFlight(), "the worker is idle before the hand-over");
	g_allocCount = 0;
	g_countAllocs = true;
	Check(CSpawnPlannerWorker::Run(1000), "the boundary cycle is handed over");
	CSpawnPlannerWorker::Join();
	g_countAllocs = false;
	Check(!CSpawnPlannerWorker::InFlight(), "the worker is idle after the join");
	CSpawnPlannerWorker::AssertIdle();
	CheckInt(g_allocCount, 0, "a cycle allocates nothing");
	CheckInt(CSpawnPlannerWorker::Published()->numPlans, 1, "the boundary cycle published its plan");
	Check(HashBytes(g_index.GetSites(), (size_t)g_index.GetNumSites() * sizeof(SSpawnSite)) == indexHash,
		"the site index did not move");
	Check(HashBytes(&store, sizeof(store)) == storeHash, "the occupancy store did not move");

	/* The flags refresh is the one write the main thread makes to the
	 * worker's snapshot, and it happens with the worker idle. */
	g_nodes[RoadNode(2, 2)].flags |= SPAWN_SITE_NODE_DISABLED;
	g_index.RefreshFlags(g_nodes);
	CSpawnPlannerWorker::RefreshSiteFlags(g_index.GetSites(), g_index.GetNumSites());
	CSpawnPlannerWorker::AssertIdle();
}

/* ------------------------------------------------------------------ section D */

static void
SectionCost(void)
{
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	SSpawnCarRequest request = MakeRequest(5, 0);
	SSpawnOccupancyStore store;
	SSpawnRank rank;
	int32 n, iter;
	double start, elapsed;
	uint64 publishNs = 0, queueNs = 0, roundNs = 0;

	printf("D. the cost: the inline rank against the worker's round trip\n");
#ifndef NDEBUG
	printf("    [debug build: the module's write audit hashes the inputs every cycle; the numbers are the audit's]\n");
#endif

	n = BuildOffers(request, offers, bubbles);
	for (iter = 0; iter < 1000; iter++)
		SpawnRankSites(g_index.GetSites(), offers, bubbles, n, store.View(), 0, &rank);
	start = NowSeconds();
	for (iter = 0; iter < 1000; iter++)
		SpawnRankSites(g_index.GetSites(), offers, bubbles, n, store.View(), 0, &rank);
	elapsed = NowSeconds() - start;
	printf("    the inline rank: %d candidates, %.3f us a call (1000 calls)\n", n, elapsed * 1e6 / 1000.0);

	/* The worker's round trip, threaded: the publish, the command copy and the
	 * hand-over/join, the three parts the main thread pays. */
	CSpawnPlannerWorker::Stop();
	CSpawnPlannerWorker::Reset();
	CSpawnPlannerWorker::SetSites(g_index.GetSites(), g_index.GetNumSites());
	CSpawnPlannerWorker::SetThreaded(true);
	CSpawnPlannerWorker::Start();
	for (iter = 0; iter < 200; iter++) {
		CSpawnPlannerWorker::PublishOccupancy(store);
		CSpawnPlannerWorker::QueueRank(request, offers, bubbles, n);
		CSpawnPlannerWorker::Run((uint32)iter);
		CSpawnPlannerWorker::Join();
	}
	{
		int32 i;

		for (i = 0; i < 1000; i++) {
			double a, b;

			a = NowSeconds();
			CSpawnPlannerWorker::PublishOccupancy(store);
			b = NowSeconds();
			publishNs += (uint64)((b - a) * 1e9);

			a = NowSeconds();
			CSpawnPlannerWorker::QueueRank(request, offers, bubbles, n);
			b = NowSeconds();
			queueNs += (uint64)((b - a) * 1e9);

			a = NowSeconds();
			CSpawnPlannerWorker::Run((uint32)i);
			CSpawnPlannerWorker::Join();
			b = NowSeconds();
			roundNs += (uint64)((b - a) * 1e9);
		}
	}
	printf("    the worker's main thread: publish %.3f us, queue %.3f us, run+join %.3f us (1000 cycles)\n",
		publishNs / 1000.0 / 1000.0, queueNs / 1000.0 / 1000.0, roundNs / 1000.0 / 1000.0);
	printf("    the worker's total main-thread cost: %.3f us a plan; the rank itself is the inline number above\n",
		(publishNs + queueNs + roundNs) / 1000.0 / 1000.0);

	/* The cycle's own compute, inline (the same rank, no thread). */
	CSpawnPlannerWorker::SetThreaded(false);
	start = NowSeconds();
	for (iter = 0; iter < 1000; iter++) {
		CSpawnPlannerWorker::QueueRank(request, offers, bubbles, n);
		CSpawnPlannerWorker::Run((uint32)iter);
	}
	elapsed = NowSeconds() - start;
	printf("    the same cycle inline (rank + handshake in one call): %.3f us a plan\n", elapsed * 1e6 / 1000.0);

	/* The occupancy publish's size: the copy the main thread pays. */
	printf("    the occupancy copy: %u B a buffer, the site snapshot: %u B (%d sites), the command: %u B\n",
		(uint)sizeof(SSpawnPlannerOccupancy), (uint)(g_index.GetNumSites() * sizeof(SSpawnSite)),
		g_index.GetNumSites(), (uint)sizeof(SSpawnRankCommand));
}

/* ------------------------------------------------------------------ main */

int
main(int argc, char **argv)
{
	bool bCostOnly = argc > 1 && strcmp(argv[1], "--cost") == 0;

	BuildFixture();
	ScriptsReset();
	Check(g_index.GetNumSites() > 100, "the fixture builds its sites");
	Check(g_index.GetStats().boatSites == 0, "the fixture has no water line");
	printf("spawn planner worker harness (VICE-160)\n");
	printf("  the fixture: %d nodes, %d links, %d connections, %d sites (%d dropped)\n",
		GRID_NODES, g_numEdges, g_index.GetStats().connections, g_index.GetNumSites(),
		g_index.GetStats().droppedNoLanes + g_index.GetStats().droppedMixed + g_index.GetStats().droppedUnresolved);

	/* The cost section alone, for a release build (-DNDEBUG): the module's
	 * write audit is a debug build's, and its hashes would dominate the
	 * numbers here. The debug run measures it too, labelled. */
	if (bCostOnly) {
		SectionCost();
		CSpawnPlannerWorker::Stop();
		printf("  %lld checks, %d failures\n", g_checks, g_failures);
		return g_failures ? 1 : 0;
	}

	SectionEquivalence();
	SectionContention();
	SectionBoundary();
	SectionCost();
	CSpawnPlannerWorker::Stop();

	printf("  %lld checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
