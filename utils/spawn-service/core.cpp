/*
 * The evidence program for the spawn service core (ticket VICE-150).
 *
 * It compiles the service's own engine-free header (src/control/SpawnService.h,
 * the planner and its decisions) and the site index's real build
 * (src/control/SpawnSitesIndex.cpp), and drives them over a small synthetic
 * site graph:
 *
 *	A. The offer. SpawnOfferSites' filters - the ring band (the old
 *	   collector's own outer distance and slack), the cone (the collector's
 *	   dot test), the water policy and the disabled flag - each checked
 *	   separately and against a reference scan over every site of the graph.
 *
 *	B. The rank. SpawnPlanSites' order, one key at a time: the hidden arms
 *	   first (the ray-free visibility rank), then the emptier cell (spread),
 *	   then the nearest to the request's ring, then the rarity weight, then
 *	   the site id. The scenario scripts the bubble facts per site
 *	   (SNearBubbleCandidate), so each key is decisive on its own.
 *
 *	C. The cheap gates. The occupancy store's capacity (3 per cell) and the
 *	   site reservations refuse by their own reason rows; the plan reserves
 *	   what it returns while the plan is outstanding, two plans cannot share
 *	   a site, and the run releases the reservations.
 *
 *	D. The deep facts. The zone and the reachability resolve for the ranked
 *	   prefix only (SPAWN_SERVICE_DEEP_CANDIDATES): a deep-refused candidate
 *	   is counted by its reason and the next in the rank fills the plan; the
 *	   callback's call count is the bound.
 *
 *	E. The run and the two kinds. SpawnRunPlan's attempts: a refusal counted
 *	   by reason, the next site created, the covered-fill row for a fade-arm
 *	   creation, the created row by purpose, the reservation release.
 *	   SpawnFunctionGate with and without the force bypass (BUDGET, POOL);
 *	   SpawnDecideExplicit's exact position, the floored lifetime and the
 *	   normalized tag; the readiness waits (the deferred requests and the
 *	   frames spent).
 *
 *	F. The telemetry and the helpers. The plan cost rows, the conservation
 *	   check and its breach row, the removal rows, the request rows by
 *	   purpose and kind, and the primitive's own decisions (the lane draw,
 *	   the travel direction, the between-nodes clamp).
 *
 * What this program cannot reach: the engine side (SpawnService.cpp) - the
 * facts from the camera, the zones and CPathFind::TestCoorsCloseness, the
 * primitive itself (CCarCtrl::CreateCarFromPlacement) and the explicit
 * creation's CVehicle - is the game's, checked by the build and by the live
 * soak; this harness drives the decisions they call. The explicit kind's
 * same-frame handle is the engine's ppCreated contract, and the counters the
 * engine's one owner fills are the same struct this program drives.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 \
 *	    -o /tmp/spawn-core utils/spawn-service/core.cpp \
 *	    src/control/SpawnSitesIndex.cpp
 *	/tmp/spawn-core
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

static double
NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ the fixture */

/*
 * The synthetic city: an 8 x 8 grid of road nodes at 40 m spacing, one
 * one-way row (the east direction only: the westward connection has no lanes
 * and the index drops it), one disabled node (2,2), a 4-node water line east
 * of the grid, and a per-row rarity weight. Every road edge is one link with
 * two directed connections (the index makes a site of each usable direction);
 * the water line makes directed water-water sites. The information zones:
 * the midpoints west of 160 m are zone 0, the rest zone 1.
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
	int8_t dirX, dirY;	/* the link's own direction: from the higher node id to the lower */
	int8_t left, right;	/* the lanes of the b->a and the a->b travels */
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

	/* The road nodes: the rarity is 1 + row % 3; (2,2) is a script's road
	 * toggle (disabled). */
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
	/* The water line. */
	for (i = 0; i < WATER_NODES; i++) {
		SSpawnSiteNode *pNode = &g_nodes[WaterNode(i)];
		pNode->x = 320.0f + i * GRID_SPACING;
		pNode->y = -80.0f;
		pNode->z = 0.0f;
		pNode->spawnRate = 2;
		pNode->flags |= SPAWN_SITE_NODE_WATER;
	}

	/* The road edges: the horizontal ones of row 2 are east one-way (the
	 * westward travel has no lanes). The link's own direction points west /
	 * south, the convention the index's sign rule reads. */
	for (row = 0; row < GRID_ROWS; row++) {
		for (col = 0; col < GRID_COLS - 1; col++) {
			int32_t a = RoadNode(row, col);
			int32_t b = RoadNode(row, col + 1);
			if (row == 2)
				AddEdge(a, b, -100, 0, 0, 2);	/* east only */
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

	/* The link records: the link was made from its lower node, so the a->b
	 * travel reads the right lanes and the b->a travel the left ones. */
	g_numLinks = g_numEdges;
	for (i = 0; i < g_numEdges; i++) {
		g_links[i].dirX = g_edges[i].dirX;
		g_links[i].dirY = g_edges[i].dirY;
		g_links[i].numLeftLanes = g_edges[i].left;
		g_links[i].numRightLanes = g_edges[i].right;
		g_links[i].pathNodeIndex = (int16_t)g_edges[i].a;
		g_links[i].width = g_edges[i].width;
	}

	/* The connections, per node: node i owns its own incident connections,
	 * in edge order. */
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

/*
 * Per-site scripts, so every scenario puts exactly the facts it wants on a
 * candidate:
 *
 *	g_armOf[site]		the arm the bubble facts must produce (the default
 *				is the soft arm, the hidden one; NONE refuses)
 *	g_zoneRefused[site]	the deep zone fact
 *	g_reachRefused[site]	the deep reach fact
 *	g_tryRefusal[site]	the attempt's answer (NONE: created)
 */
#define SPAWN_SCRIPT_DEFAULT 0xFF

static uint8_t g_armOf[NUM_PATHCONNECTIONS];
static uint8_t g_tryArm[NUM_PATHCONNECTIONS];	/* the attempt's own gate (L3) */
static bool g_zoneRefused[NUM_PATHCONNECTIONS];
static bool g_reachRefused[NUM_PATHCONNECTIONS];
static ESpawnRefusal g_tryRefusal[NUM_PATHCONNECTIONS];
static int32_t g_deepCalls;
static int32_t g_deepSiteList[SPAWN_SERVICE_DEEP_CANDIDATES + 4];
static int32_t g_tryCalls;
static int32_t g_createdSite;

static void
ScriptsReset(void)
{
	memset(g_armOf, SPAWN_SCRIPT_DEFAULT, sizeof(g_armOf));
	memset(g_tryArm, SPAWN_SCRIPT_DEFAULT, sizeof(g_tryArm));
	memset(g_zoneRefused, 0, sizeof(g_zoneRefused));
	memset(g_reachRefused, 0, sizeof(g_reachRefused));
	memset(g_tryRefusal, 0, sizeof(g_tryRefusal));
	g_deepCalls = 0;
	g_tryCalls = 0;
	g_createdSite = -1;
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
 * The bubble facts of one site, crafted to produce the wanted arm. The
 * program checks the crafted facts against NearBubbleArmFor, so a script
 * cannot lie about the arm the rank reads.
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
		/* Off the cone by the margin, beyond the soft distance. */
		break;
	case NEAR_BUBBLE_ARM_FADE_DISTANCE:
		/* Beyond the visible range. */
		c->visibleRange = 100.0f;
		c->distToCamera = 200.0f;
		break;
	case NEAR_BUBBLE_ARM_OCCLUDER:
		/* The occluded hint, close: a ray would confirm it. */
		c->occluded = true;
		c->distToCamera = NEAR_BUBBLE_SOFT_DISTANCE - 10.0f;
		break;
	case NEAR_BUBBLE_ARM_RAY:
		/* The close blind spot. */
		c->distToCamera = NEAR_BUBBLE_SOFT_DISTANCE - 10.0f;
		break;
	case NEAR_BUBBLE_ARM_FADE:
		/* Catchable, under the screen fade. */
		c->inFrustum = true;
		c->inFrustumMargin = true;
		c->fadeHides = true;
		c->distToCamera = RING_CAR_ONSCREEN_SPAWN_MIN - 30.0f;
		break;
	case NEAR_BUBBLE_ARM_BYPASS:
		c->bypassView = true;
		break;
	case NEAR_BUBBLE_ARM_ONSCREEN_BAND:
		/* In the frustum, at the band's minimum. */
		c->inFrustum = true;
		c->inFrustumMargin = true;
		c->distToCamera = RING_CAR_ONSCREEN_SPAWN_MIN;
		break;
	case NEAR_BUBBLE_ARM_NONE:
	default:
		/* In the frustum, inside the fade, nearer than the band: catchable. */
		c->inFrustum = true;
		c->inFrustumMargin = true;
		c->distToCamera = RING_CAR_ONSCREEN_SPAWN_MIN - 30.0f;
		break;
	}
}

static void
HarnessDeep(void *user, const SSpawnOffer &offer, SSpawnSiteDeepFacts *facts)
{
	int32_t site = offer.site;

	(void)user;
	if (g_deepCalls < (int32_t)(sizeof(g_deepSiteList) / sizeof(g_deepSiteList[0])))
		g_deepSiteList[g_deepCalls] = site;
	g_deepCalls++;
	facts->zoneOk = !g_zoneRefused[site];
	facts->reachOk = !g_reachRefused[site];
}

static ESpawnRefusal
HarnessTry(void *user, const SSpawnPlanSite &planSite, uint8_t *pArm)
{
	(void)user;
	g_tryCalls++;
	/* The scripted arm of the attempt's own gate (L3): the plan's midpoint
	 * arm is the fallback, and a scenario can overwrite it to show the
	 * covered-fill row reads the attempt's answer. */
	if (g_tryArm[planSite.site] != SPAWN_SCRIPT_DEFAULT)
		*pArm = g_tryArm[planSite.site];
	if (g_tryRefusal[planSite.site] == SPAWN_REFUSAL_NONE) {
		g_createdSite = planSite.site;
		return SPAWN_REFUSAL_NONE;
	}
	return g_tryRefusal[planSite.site];
}

/* ------------------------------------------------------------------ the helpers */

static SSpawnRingQuery
QueryAt(float x, float y, float spawnDist)
{
	SSpawnRingQuery q;

	memset(&q, 0, sizeof(q));
	q.targetX = x;
	q.targetY = y;
	q.spawnDist = spawnDist;
	q.dirX = 1.0f;
	q.dirY = 0.0f;
	q.angleLimit = -1.0f;	/* the top-down case: every bearing passes */
	q.forward = true;
	q.water = SPAWN_SITES_ANY;
	return q;
}

static int32_t
FindSite(int16_t node1, int16_t node2)
{
	int32_t i;

	for (i = 0; i < g_index.GetNumSites(); i++) {
		const SSpawnSite *pSite = &g_index.GetSites()[i];
		if (pSite->node1 == node1 && pSite->node2 == node2)
			return i;
	}
	return -1;
}

static float
RingErrorOf(int32_t site, const SSpawnRingQuery &q)
{
	const SSpawnSite *pSite = &g_index.GetSites()[site];
	float dx = pSite->x - q.targetX;
	float dy = pSite->y - q.targetY;
	float dist = sqrtf(dx * dx + dy * dy);
	float ringDist = (pSite->flags & SPAWN_SITE_BOAT) ? q.spawnDist * RING_BOAT_SPAWN_FACTOR : q.spawnDist;

	return dist > ringDist ? dist - ringDist : ringDist - dist;
}

/* One plan over the offer, with the scripts' arms and deep facts applied. */
static int32_t
PlanOnce(const SSpawnRingQuery &q, SSpawnPlan *plan, SSpawnServiceTelemetry *tel, uint32_t now)
{
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	int32_t n, i;

	n = SpawnOfferSites(g_index, q, offers, SPAWN_SERVICE_MAX_CANDIDATES);
	for (i = 0; i < n; i++) {
		BubbleForArm(&bubbles[i], ArmOf(offers[i].site));
		CheckInt(NearBubbleArmFor(bubbles[i]), ArmOf(offers[i].site), "the crafted bubble produces the scripted arm");
	}
	g_deepCalls = 0;
	SpawnPlanSites(g_index, offers, bubbles, n, &g_store, now, HarnessDeep, offers, plan, tel);
	return n;
}

/*
 * The reference rank (M1): every valid offer sorted by the core's own public
 * comparator with a plain selection sort - a different algorithm from the
 * core's bounded insert - and the top `max` sites. The harness compares the
 * deep callback's own prefix against it, so a wrong bounded insert (an
 * unconditional overwrite of the buffer's worst entry) cannot pass.
 */
static int32_t
ReferenceTopSites(const SSpawnRingQuery &q, int32_t *out, int32_t max)
{
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	int32_t order[SPAWN_SERVICE_MAX_CANDIDATES];
	int32_t n, i, j, kept = 0;

	n = SpawnOfferSites(g_index, q, offers, SPAWN_SERVICE_MAX_CANDIDATES);
	for (i = 0; i < n; i++) {
		const SSpawnSite *pSite = &g_index.GetSites()[offers[i].site];
		int32_t occupancy = g_store.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, pSite->x, pSite->y);

		if (ArmOf(offers[i].site) == NEAR_BUBBLE_ARM_NONE)
			continue;
		if (occupancy >= SPAWN_SERVICE_CELL_CAPACITY)
			continue;
		if (g_store.SiteReserved(offers[i].site, 0))
			continue;
		order[kept++] = i;
	}
	for (i = 0; i < kept; i++) {
		int32_t best = i;
		for (j = i + 1; j < kept; j++) {
			int32_t a = order[j], b = order[best];
			const SSpawnSite *pA = &g_index.GetSites()[offers[a].site];
			const SSpawnSite *pB = &g_index.GetSites()[offers[b].site];

			if (SpawnPlanSiteSortsBefore(
				SpawnVisibilityRank(ArmOf(offers[a].site)),
				g_store.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, pA->x, pA->y),
				offers[a].ringError, offers[a].spawnRate, offers[a].site,
				SpawnVisibilityRank(ArmOf(offers[b].site)),
				g_store.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, pB->x, pB->y),
				offers[b].ringError, offers[b].spawnRate, offers[b].site))
				best = j;
		}
		if (best != i) {
			int32_t tmp = order[i];
			order[i] = order[best];
			order[best] = tmp;
		}
	}
	if (kept > max)
		kept = max;
	for (i = 0; i < kept; i++)
		out[i] = offers[order[i]].site;
	return kept;
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

/* ------------------------------------------------------------------ section A */

/*
 * The site-to-placement mapping (B1): the primitive indexes m_carPathLinks
 * with the site's LINK id; the directed connection slot (the site id) is a
 * different number. The fixture must hold the difference for most sites, and
 * the placement built through SpawnPlacementFromSite must carry the link.
 */
static void
Section0Mapping(void)
{
	const SSpawnSite *sites = g_index.GetSites();
	int32_t i, differing = 0;

	printf("0. the site mapping: the connection slot is not the link id\n");
	for (i = 0; i < g_index.GetNumSites(); i++) {
		const SSpawnSite &site = sites[i];
		SSpawnPlacement placement;

		Check(site.link >= 0 && site.link < g_numLinks, "0: the site's link is a link id");
		CheckInt(g_connectionLink[site.connection], site.link, "0: the site's link is its connection's link");
		if (site.connection != site.link)
			differing++;
		SpawnPlacementFromSite(site, i, &placement);
		CheckInt(placement.link, site.link, "0: the placement carries the site's link");
		CheckInt(placement.site, i, "0: the placement carries the site id");
		CheckInt(placement.node1, site.node1, "0: the placement carries the entry node");
		CheckInt(placement.node2, site.node2, "0: the placement carries the target node");
		Check(placement.x == site.x && placement.y == site.y && placement.z == site.z,
			"0: the placement carries the midpoint");
		Check(placement.between == 0.5f, "0: the midpoint is the pair's half");
	}
	Check(differing * 2 > g_index.GetNumSites(), "0: most sites' slots differ from their links");
	printf("    %d of %d sites have a slot different from their link\n", differing, g_index.GetNumSites());
}

static void
Section1Offer(void)
{
	SSpawnRingQuery q = QueryAt(140.0f, 140.0f, 120.0f);
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	const SSpawnSite *sites = g_index.GetSites();
	int32_t n, i;

	printf("A. the offer: the band, the cone, the water policy, the disabled flag\n");

	n = SpawnOfferSites(g_index, q, offers, SPAWN_SERVICE_MAX_CANDIDATES);
	Check(n > 0, "the offer answers the ring");
	{
		float maxDist = SpawnRingMaxDist(q.spawnDist);
		float slack = SpawnRingDistSlack(q.spawnDist);
		int32_t reference = 0;
		int32_t wrong = 0;

		for (i = 0; i < n; i++) {
			const SSpawnSite *pSite = &sites[offers[i].site];
			float dx = pSite->x - q.targetX;
			float dy = pSite->y - q.targetY;
			float dist = sqrtf(dx * dx + dy * dy);
			bool boat = (pSite->flags & SPAWN_SITE_BOAT) != 0;
			float ringDist = boat ? q.spawnDist * RING_BOAT_SPAWN_FACTOR : q.spawnDist;
			float ringError = dist > ringDist ? dist - ringDist : ringDist - dist;
			float dot = dist > 0.0001f ? (dx / dist) * q.dirX + (dy / dist) * q.dirY : 0.0f;

			if (dist > maxDist || ringError > slack)
				wrong++;
			if (q.forward ? !(dot > q.angleLimit) : !(dot <= q.angleLimit))
				wrong++;
			if (pSite->flags & SPAWN_SITE_DISABLED)
				wrong++;
			if (offers[i].boat != (boat ? 1 : 0))
				wrong++;
			if (offers[i].dist != dist || offers[i].ringError != ringError)
				wrong++;	/* the offer's own numbers are the site's */
			if (offers[i].spawnRate != pSite->spawnRate)
				wrong++;
		}
		CheckInt(wrong, 0, "every offered site and its numbers pass the band, the cone and the policy");

		/* The reference scan: every site the predicates keep, counted
		 * independently (the index's query is a bounding box of cells, so
		 * the band and the policy are the offer's own). */
		for (i = 0; i < g_index.GetNumSites(); i++) {
			const SSpawnSite *pSite = &sites[i];
			float dx = pSite->x - q.targetX;
			float dy = pSite->y - q.targetY;
			float dist = sqrtf(dx * dx + dy * dy);
			bool boat = (pSite->flags & SPAWN_SITE_BOAT) != 0;
			float ringDist = boat ? q.spawnDist * RING_BOAT_SPAWN_FACTOR : q.spawnDist;
			float ringError = dist > ringDist ? dist - ringDist : ringDist - dist;
			float dot;

			if (pSite->flags & SPAWN_SITE_DISABLED)
				continue;
			if (dist > maxDist || ringError > slack)
				continue;
			dot = dist > 0.0001f ? (dx / dist) * q.dirX + (dy / dist) * q.dirY : 0.0f;
			if (q.forward ? !(dot > q.angleLimit) : !(dot <= q.angleLimit))
				continue;
			reference++;
		}
		CheckInt(n, reference, "the offer's count is the reference scan's");
		Check(reference > 8, "the ring holds enough candidates for the rank");
	}

	/* The water policies. The water line's ring is the boat ring (1.5x): the
	 * query aims 180 m off, the ring the boat sites belong to. */
	{
		int32_t boatAny = 0, boatLand = 0, landWater = 0;
		SSpawnRingQuery water = QueryAt(380.0f, 100.0f, 120.0f);

		q.water = SPAWN_SITES_ANY;
		n = SpawnOfferSites(g_index, q, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		for (i = 0; i < n; i++)
			boatAny += offers[i].boat;
		CheckInt(boatAny, 0, "the any policy's land ring keeps no boat sites");

		q.water = SPAWN_SITES_LAND;
		n = SpawnOfferSites(g_index, q, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		for (i = 0; i < n; i++)
			boatLand += offers[i].boat;
		CheckInt(boatLand, 0, "the land policy keeps no boat sites");

		water.water = SPAWN_SITES_WATER;
		n = SpawnOfferSites(g_index, water, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		Check(n > 0, "the water policy answers by the water line");
		for (i = 0; i < n; i++)
			landWater += offers[i].boat ? 0 : 1;
		CheckInt(landWater, 0, "the water policy keeps no land sites");
	}

	/* The cone: the two sides partition the band. The stock full scan
	 * (angleLimit -1) drops the exactly-opposite bearing, so the sides are
	 * compared against the non-cone filters' own reference. */
	{
		SSpawnRingQuery fwd = QueryAt(140.0f, 140.0f, 120.0f);
		SSpawnRingQuery bwd = fwd;
		int32_t forwardCount, backwardCount, unionCount = 0;
		float maxDist = SpawnRingMaxDist(fwd.spawnDist);
		float slack = SpawnRingDistSlack(fwd.spawnDist);

		fwd.angleLimit = 0.2f;
		fwd.forward = true;
		bwd.angleLimit = 0.2f;
		bwd.forward = false;
		forwardCount = SpawnOfferSites(g_index, fwd, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		for (i = 0; i < forwardCount; i++) {
			const SSpawnSite *pSite = &sites[offers[i].site];
			float dx = pSite->x - fwd.targetX;
			float dy = pSite->y - fwd.targetY;
			float dist = sqrtf(dx * dx + dy * dy);
			float dot = dist > 0.0001f ? (dx / dist) * fwd.dirX + (dy / dist) * fwd.dirY : 0.0f;
			Check(dot > fwd.angleLimit, "A: the forward cone keeps only its side");
		}
		backwardCount = SpawnOfferSites(g_index, bwd, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		for (i = 0; i < backwardCount; i++) {
			const SSpawnSite *pSite = &sites[offers[i].site];
			float dx = pSite->x - bwd.targetX;
			float dy = pSite->y - bwd.targetY;
			float dist = sqrtf(dx * dx + dy * dy);
			float dot = dist > 0.0001f ? (dx / dist) * bwd.dirX + (dy / dist) * bwd.dirY : 0.0f;
			Check(dot <= bwd.angleLimit, "A: the backward cone keeps only its side");
		}
		for (i = 0; i < g_index.GetNumSites(); i++) {
			const SSpawnSite *pSite = &sites[i];
			float dx = pSite->x - fwd.targetX;
			float dy = pSite->y - fwd.targetY;
			float dist = sqrtf(dx * dx + dy * dy);
			bool boat = (pSite->flags & SPAWN_SITE_BOAT) != 0;
			float ringDist = boat ? fwd.spawnDist * RING_BOAT_SPAWN_FACTOR : fwd.spawnDist;
			float ringError = dist > ringDist ? dist - ringDist : ringDist - dist;

			if (pSite->flags & SPAWN_SITE_DISABLED)
				continue;
			if (dist > maxDist || ringError > slack)
				continue;
			unionCount++;
		}
		Check(forwardCount > 0 && backwardCount > 0, "A: both cone sides answer candidates");
		CheckInt(forwardCount + backwardCount, unionCount, "A: the two cone sides partition the band");
	}

	/* The disabled flag: the sites of node (2,2) are off unless allowed. The
	 * query aims at the band of that node's own sites (120 m away). */
	{
		int32_t kept = 0, allowed = 0;
		SSpawnRingQuery strict = QueryAt(80.0f, 200.0f, 120.0f);
		SSpawnRingQuery loose = strict;

		loose.allowDisabled = true;
		n = SpawnOfferSites(g_index, strict, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		for (i = 0; i < n; i++)
			if (sites[offers[i].site].flags & SPAWN_SITE_DISABLED)
				kept++;
		CheckInt(kept, 0, "the offer drops every disabled site");
		n = SpawnOfferSites(g_index, loose, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		for (i = 0; i < n; i++)
			if (sites[offers[i].site].flags & SPAWN_SITE_DISABLED)
				allowed++;
		Check(allowed > 0, "the cops' policy keeps the disabled sites");
	}

	/* The boat ring: a boat site targets the water ring (1.5x), not the
	 * road one. The water policy's own sites are all inside the boat band. */
	{
		SSpawnRingQuery water = QueryAt(380.0f, 100.0f, 120.0f);
		float slack = SpawnRingDistSlack(water.spawnDist);

		water.water = SPAWN_SITES_WATER;
		n = SpawnOfferSites(g_index, water, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		for (i = 0; i < n; i++)
			Check(offers[i].ringError <= slack && offers[i].boat == 1, "a boat site's ring is the water ring");
	}
}

/* ------------------------------------------------------------------ section B */

static void
Section2Rank(void)
{
	SSpawnRingQuery q = QueryAt(140.0f, 140.0f, 120.0f);
	SSpawnPlan plan;
	SSpawnServiceTelemetry tel;

	printf("B. the rank: hidden first, spread, nearest, rarity, the site id\n");

	/* The fixture's sites under the query: the near-band one (the east
	 * travel of the edge (3,0)-(3,1)) and a farther one (the edge
	 * (2,3)-(2,4) east). */
	{
		int32_t nearSite = FindSite(RoadNode(3, 0), RoadNode(3, 1));
		int32_t farSite = FindSite(RoadNode(2, 3), RoadNode(2, 4));

		Check(nearSite >= 0 && farSite >= 0, "the fixture holds the rank's sites");
		Check(RingErrorOf(nearSite, q) < RingErrorOf(farSite, q), "the rank's near site is the nearer to the ring");

		/* B1: hidden first - the near site is in the on-screen band, the far
		 * one is the soft arm; the hidden one wins. */
		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		g_armOf[nearSite] = NEAR_BUBBLE_ARM_ONSCREEN_BAND;
		g_armOf[farSite] = NEAR_BUBBLE_ARM_SOFT;
		PlanOnce(q, &plan, &tel, 0);
		CheckInt(plan.count, 2, "B1: the plan holds the two valid sites");
		CheckInt(plan.sites[0].site, farSite, "B1: the hidden arm ranks first");
		CheckInt(plan.sites[1].site, nearSite, "B1: the in-view band ranks second");
		CheckInt(plan.sites[0].arm, NEAR_BUBBLE_ARM_SOFT, "B1: the rank keeps the arm");

		/* The visibility rank's own order over every arm of the ladder. */
		Check(SpawnVisibilityRank(NEAR_BUBBLE_ARM_SOFT) == 0 &&
			SpawnVisibilityRank(NEAR_BUBBLE_ARM_FADE_DISTANCE) == 0 &&
			SpawnVisibilityRank(NEAR_BUBBLE_ARM_OCCLUDER) == 1 &&
			SpawnVisibilityRank(NEAR_BUBBLE_ARM_RAY) == 1 &&
			SpawnVisibilityRank(NEAR_BUBBLE_ARM_FADE) == 2 &&
			SpawnVisibilityRank(NEAR_BUBBLE_ARM_BYPASS) == 2 &&
			SpawnVisibilityRank(NEAR_BUBBLE_ARM_ONSCREEN_BAND) == 2 &&
			SpawnVisibilityRank(NEAR_BUBBLE_ARM_NONE) == 3,
			"B1: the visibility rank orders the ladder's arms");

		/* B2: spread - the nearer site's cell holds two cars, the farther
		 * site's cell is empty: the emptier one ranks first. */
		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		g_armOf[nearSite] = NEAR_BUBBLE_ARM_SOFT;
		g_armOf[farSite] = NEAR_BUBBLE_ARM_SOFT;
		g_store.Create(SPAWN_OCCUPANCY_VEHICLE, 0, g_index.GetSites()[nearSite].x, g_index.GetSites()[nearSite].y);
		g_store.Create(SPAWN_OCCUPANCY_VEHICLE, 1, g_index.GetSites()[nearSite].x, g_index.GetSites()[nearSite].y);
		PlanOnce(q, &plan, &tel, 0);
		CheckInt(g_store.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, g_index.GetSites()[nearSite].x, g_index.GetSites()[nearSite].y),
			2, "B2: the near site's cell holds two cars");
		CheckInt(plan.count, 2, "B2: both sites stay valid below the capacity");
		CheckInt(plan.sites[0].site, farSite, "B2: the spread ranks the emptier cell first");
		g_store.Remove(SPAWN_OCCUPANCY_VEHICLE, 0);
		g_store.Remove(SPAWN_OCCUPANCY_VEHICLE, 1);

		/* B3: nearest - both cells empty, both hidden: the nearer to the
		 * ring ranks first. */
		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		g_armOf[nearSite] = NEAR_BUBBLE_ARM_SOFT;
		g_armOf[farSite] = NEAR_BUBBLE_ARM_SOFT;
		PlanOnce(q, &plan, &tel, 0);
		CheckInt(plan.sites[0].site, nearSite, "B3: the nearest to the ring ranks first");

		/* B4: the site id breaks a perfect tie. The two symmetric sites of
		 * the edge (3,0)-(3,1) (east and west) have the same distance,
		 * occupancy, arm and rarity; the lower site id wins. */
		{
			int32_t eastSite = FindSite(RoadNode(3, 0), RoadNode(3, 1));
			int32_t westSite = FindSite(RoadNode(3, 1), RoadNode(3, 0));

			Check(eastSite >= 0 && westSite >= 0, "B4: the fixture holds the symmetric pair");
			Check(RingErrorOf(eastSite, q) == RingErrorOf(westSite, q), "B4: the pair's ring errors are equal");
			g_store.Reset();
			SpawnTelemetryReset(&tel);
			ScriptsReset();
			ArmAll(NEAR_BUBBLE_ARM_NONE);
			g_armOf[eastSite] = NEAR_BUBBLE_ARM_SOFT;
			g_armOf[westSite] = NEAR_BUBBLE_ARM_SOFT;
			PlanOnce(q, &plan, &tel, 0);
			CheckInt(plan.count, 2, "B4: both symmetric sites are valid");
			CheckInt(plan.sites[0].site, eastSite < westSite ? eastSite : westSite,
				"B4: the lower site id breaks the tie");
		}

		/* B4b: the comparator's remaining key - the rarity - and the id,
		 * as unit checks (the fixture cannot tie the ring error of two
		 * sites of different rates). */
		Check(SpawnPlanSiteSortsBefore(0, 0, 1.0f, 9, 5, 0, 0, 1.0f, 3, 5),
			"B4b: the higher rarity weight sorts first");
		Check(SpawnPlanSiteSortsBefore(0, 0, 1.0f, 3, 5, 0, 0, 1.0f, 3, 6),
			"B4b: the lower site id breaks the last tie");
		Check(!SpawnPlanSiteSortsBefore(1, 0, 0.0f, 9, 1, 0, 9, 99.0f, 1, 2),
			"B4b: the hidden rank dominates every other key");

		/* B5: the plan's bound - four placements, all reserved. */
		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_SOFT);
		PlanOnce(q, &plan, &tel, 100);
		CheckInt(plan.count, SPAWN_PLAN_MAX_PLACEMENTS, "B5: the plan returns its four placements");
		for (int32_t i = 0; i < plan.count; i++)
			Check(g_store.SiteReserved(plan.sites[i].site, 100), "B5: every returned placement is reserved");
	}
}

/* ------------------------------------------------------------------ section C */

static void
Section3Gates(void)
{
	SSpawnRingQuery q = QueryAt(140.0f, 140.0f, 120.0f);
	SSpawnPlan plan;
	SSpawnServiceTelemetry tel;
	int32_t site = FindSite(RoadNode(3, 0), RoadNode(3, 1));

	printf("C. the cheap gates: the capacity, the reservations, the visibility\n");

	/* C1: the cell at its capacity refuses. */
	{
		uint32_t before, after;
		int32_t i;

		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		g_armOf[site] = NEAR_BUBBLE_ARM_SOFT;
		for (i = 0; i < SPAWN_SERVICE_CELL_CAPACITY; i++)
			g_store.Create(SPAWN_OCCUPANCY_VEHICLE, i, g_index.GetSites()[site].x, g_index.GetSites()[site].y);
		before = Refusal(tel, SPAWN_REFUSAL_OCCUPANCY);
		PlanOnce(q, &plan, &tel, 0);
		after = Refusal(tel, SPAWN_REFUSAL_OCCUPANCY);
		CheckInt(plan.count, 0, "C1: the full cell empties the plan");
		CheckInt(Delta(after, before), 2,
			"C1: both directions of the edge sit in the cell and are counted");
		g_store.Reset();
	}

	/* C2: a reserved site refuses. */
	{
		uint32_t before, after;

		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		g_armOf[site] = NEAR_BUBBLE_ARM_SOFT;
		Check(g_store.ReserveSite(site, 0), "C2: the site reserves");
		before = Refusal(tel, SPAWN_REFUSAL_OCCUPANCY);
		PlanOnce(q, &plan, &tel, 0);
		after = Refusal(tel, SPAWN_REFUSAL_OCCUPANCY);
		CheckInt(plan.count, 0, "C2: a reserved site is not planned");
		CheckInt(Delta(after, before), 1, "C2: the reservation refusal is counted, as occupancy");
		g_store.ReleaseSite(site);
	}

	/* C3: the catchable arm refuses, as visibility. */
	{
		uint32_t before, after;

		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		g_armOf[site] = NEAR_BUBBLE_ARM_NONE;
		before = Refusal(tel, SPAWN_REFUSAL_VISIBILITY);
		PlanOnce(q, &plan, &tel, 0);
		after = Refusal(tel, SPAWN_REFUSAL_VISIBILITY);
		CheckInt(plan.count, 0, "C3: a catchable candidate empties the plan");
		Check(Delta(after, before) >= 1, "C3: the visibility refusal is counted");
	}

	/* C4: two plans outstanding at the same time cannot share a site; the
	 * run releases its reservations (a created site's own at once), and the
	 * released sites plan again. */
	{
		SSpawnPlan plan2;
		SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
		int32_t n, i, j;
		bool overlap = false;

		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_SOFT);
		PlanOnce(q, &plan, &tel, 200);
		CheckInt(plan.count, SPAWN_PLAN_MAX_PLACEMENTS, "C4: the first plan is full");
		PlanOnce(q, &plan2, &tel, 200);
		CheckInt(plan2.count, SPAWN_PLAN_MAX_PLACEMENTS, "C4: the second plan is full too");
		for (i = 0; i < plan.count; i++)
			for (j = 0; j < plan2.count; j++)
				if (plan.sites[i].site == plan2.sites[j].site)
					overlap = true;
		Check(!overlap, "C4: the two plans of the frame are disjoint");
		for (i = 0; i < plan2.count; i++)
			Check(g_store.SiteReserved(plan2.sites[i].site, 200), "C4: the second plan's sites are reserved");

		/* The run of the second plan: every attempt refuses, so every
		 * reservation is released. */
		g_tryCalls = 0;
		for (i = 0; i < plan2.count; i++)
			g_tryRefusal[plan2.sites[i].site] = SPAWN_REFUSAL_OCCUPANCY;
		CheckInt(SpawnRunPlan(plan2, &g_store, HarnessTry, 0, &tel, SPAWN_PURPOSE_AMBIENT), SPAWN_REFUSAL_OCCUPANCY,
			"C4: a fully refused run answers the last attempt's reason");
		CheckInt(g_tryCalls, plan2.count, "C4: the run tries every planned site once");
		for (i = 0; i < plan2.count; i++) {
			Check(!g_store.SiteReserved(plan2.sites[i].site, 200), "C4: the run released every reservation");
			g_tryRefusal[plan2.sites[i].site] = SPAWN_REFUSAL_NONE;
		}
		/* The first plan's sites are still held by the first plan; release
		 * them and the third plan answers the same four. */
		for (i = 0; i < plan.count; i++)
			g_store.ReleaseSite(plan.sites[i].site);
		n = SpawnOfferSites(g_index, q, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		Check(n > 0, "C4: the offer still answers");
		PlanOnce(q, &plan2, &tel, 200);
		for (i = 0; i < plan.count; i++)
			CheckInt(plan2.sites[i].site, plan.sites[i].site, "C4: the deterministic plan returns after its release");
		g_store.Reset();
	}
}

/* ------------------------------------------------------------------ section D */

static void
Section4DeepFacts(void)
{
	SSpawnRingQuery q = QueryAt(140.0f, 140.0f, 120.0f);
	SSpawnPlan plan, plan2;
	SSpawnServiceTelemetry tel;
	int32_t i;

	printf("D. the deep facts: the zone, the reach, the prefix bound\n");

	/* D1: the zone. Every valid candidate's live zone refuses the class: the
	 * deep gate drops the whole prefix by the zone reason. */
	{
		uint32_t zoneBefore, reachBefore;

		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_SOFT);
		for (i = 0; i < g_index.GetNumSites(); i++)
			g_zoneRefused[i] = true;
		zoneBefore = Refusal(tel, SPAWN_REFUSAL_ZONE);
		reachBefore = Refusal(tel, SPAWN_REFUSAL_REACHABILITY);
		PlanOnce(q, &plan, &tel, 0);
		CheckInt(plan.count, 0, "D1: the zone refusal empties the plan");
		CheckInt((int)Delta(Refusal(tel, SPAWN_REFUSAL_ZONE), zoneBefore), SPAWN_SERVICE_DEEP_CANDIDATES,
			"D1: every deep zone refusal is counted");
		CheckInt((int)Delta(Refusal(tel, SPAWN_REFUSAL_REACHABILITY), reachBefore), 0,
			"D1: the zone gate answers before the reach gate");
		CheckInt(plan.refusal, SPAWN_REFUSAL_ZONE, "D1: the plan answers the zone reason");
	}

	/* D2: reachability. The same shape with the reach gate refusing. */
	{
		uint32_t reachBefore, zoneBefore;

		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_SOFT);
		for (i = 0; i < g_index.GetNumSites(); i++)
			g_reachRefused[i] = true;
		reachBefore = Refusal(tel, SPAWN_REFUSAL_REACHABILITY);
		zoneBefore = Refusal(tel, SPAWN_REFUSAL_ZONE);
		PlanOnce(q, &plan, &tel, 0);
		CheckInt(plan.count, 0, "D2: the unreachable candidates empty the plan");
		CheckInt((int)Delta(Refusal(tel, SPAWN_REFUSAL_REACHABILITY), reachBefore), SPAWN_SERVICE_DEEP_CANDIDATES,
			"D2: every deep reach refusal is counted");
		CheckInt((int)Delta(Refusal(tel, SPAWN_REFUSAL_ZONE), zoneBefore), 0,
			"D2: the zone accepted every candidate");
		CheckInt(plan.refusal, SPAWN_REFUSAL_REACHABILITY, "D2: the plan answers the reachability reason");
		CheckInt(g_deepCalls, SPAWN_SERVICE_DEEP_CANDIDATES, "D2: the deep prefix is the bound, not the offer");
	}

	/* D3: the backfill and the bound. The rank's first three candidates are
	 * deep-refused; the next four fill the plan, so the deep facts resolve
	 * for seven candidates exactly. */
	{
		int32_t refused[3];

		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_SOFT);
		PlanOnce(q, &plan, &tel, 0);
		CheckInt(plan.count, SPAWN_PLAN_MAX_PLACEMENTS, "D3: the first plan is full");
		for (i = 0; i < 3; i++)
			refused[i] = plan.sites[i].site;

		g_store.Reset();
		for (i = 0; i < 3; i++)
			g_reachRefused[refused[i]] = true;
		PlanOnce(q, &plan2, &tel, 0);
		CheckInt(plan2.count, SPAWN_PLAN_MAX_PLACEMENTS, "D3: the plan backfills the refused prefix");
		CheckInt(plan2.sites[0].site, plan.sites[3].site, "D3: the rank's fourth candidate leads the second plan");
		CheckInt(g_deepCalls, 3 + SPAWN_PLAN_MAX_PLACEMENTS, "D3: the deep facts stop at the filled plan");
	}

	/* D4: the bounded top-K holds the true best candidates (M1). The deep
	 * facts refuse everything, so the deep callback's own site list is the
	 * rank's prefix; the reference sort of every valid offer (a different
	 * algorithm, the same public comparator) must match it element for
	 * element. A bounded insert that overwrites its worst entry with every
	 * later candidate cannot pass this. */
	{
		int32_t reference[SPAWN_SERVICE_DEEP_CANDIDATES];
		int32_t refCount, i;
		bool same = true;

		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_SOFT);
		for (i = 0; i < g_index.GetNumSites(); i++)
			g_reachRefused[i] = true;
		refCount = ReferenceTopSites(q, reference, SPAWN_SERVICE_DEEP_CANDIDATES);
		CheckInt(refCount, SPAWN_SERVICE_DEEP_CANDIDATES, "D4: the reference holds the bound");
		PlanOnce(q, &plan, &tel, 0);
		CheckInt(plan.count, 0, "D4: every candidate's reach refuses");
		CheckInt(g_deepCalls, SPAWN_SERVICE_DEEP_CANDIDATES, "D4: the deep prefix is the bound, not the offer");
		CheckInt((int)Refusal(tel, SPAWN_REFUSAL_REACHABILITY), SPAWN_SERVICE_DEEP_CANDIDATES,
			"D4: every deep refusal is counted");
		for (i = 0; i < g_deepCalls && i < SPAWN_SERVICE_DEEP_CANDIDATES; i++)
			if (g_deepSiteList[i] != reference[i])
				same = false;
		Check(same, "D4: the deep prefix is the true top of the rank (M1)");
	}
}

/* ------------------------------------------------------------------ section E */

static void
Section5Run(void)
{
	SSpawnRingQuery q = QueryAt(140.0f, 140.0f, 120.0f);
	SSpawnPlan plan;
	SSpawnServiceTelemetry tel;
	int32_t nearSite = FindSite(RoadNode(3, 0), RoadNode(3, 1));
	int32_t farSite = FindSite(RoadNode(2, 3), RoadNode(2, 4));

	printf("E. the run and the two kinds\n");

	/* E1: the run's refusal and creation rows. */
	{
		uint32_t occBefore, creatBefore;
		int32_t n;

		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		g_armOf[nearSite] = NEAR_BUBBLE_ARM_ONSCREEN_BAND;
		g_armOf[farSite] = NEAR_BUBBLE_ARM_SOFT;
		n = PlanOnce(q, &plan, &tel, 0);
		Check(n > 0 && plan.count == 2, "E1: the plan holds the two sites");
		g_tryRefusal[plan.sites[0].site] = SPAWN_REFUSAL_OCCUPANCY;
		occBefore = Refusal(tel, SPAWN_REFUSAL_OCCUPANCY);
		creatBefore = tel.m_nCreatedByPurpose[SPAWN_PURPOSE_AMBIENT];
		CheckInt(SpawnRunPlan(plan, &g_store, HarnessTry, 0, &tel, SPAWN_PURPOSE_AMBIENT), SPAWN_REFUSAL_NONE,
			"E1: the second attempt creates");
		CheckInt(g_createdSite, plan.sites[1].site, "E1: the created site is the run's second");
		CheckInt((int)Delta(Refusal(tel, SPAWN_REFUSAL_OCCUPANCY), occBefore), 1, "E1: the attempt's refusal is counted");
		CheckInt((int)Delta(tel.m_nCreatedByPurpose[SPAWN_PURPOSE_AMBIENT], creatBefore), 1, "E1: the creation is counted by purpose");
		Check(!g_store.SiteReserved(plan.sites[0].site, 0) && !g_store.SiteReserved(plan.sites[1].site, 0),
			"E1: the run released both reservations");
	}

	/* E2: the covered fill reads the attempt's arm (L3). The plan's midpoint
	 * arm is the soft one, but the attempt's gate resolves the fade: the
	 * covered-fill row must count the creation. The fallback case (the plan's
	 * fade arm with no attempt override) counts too. */
	{
		SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
		SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
		int32_t n, i;
		uint32_t coveredBefore;

		g_store.Reset();
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		g_armOf[nearSite] = NEAR_BUBBLE_ARM_SOFT;
		g_tryArm[nearSite] = NEAR_BUBBLE_ARM_FADE;
		n = SpawnOfferSites(g_index, q, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		for (i = 0; i < n; i++)
			BubbleForArm(&bubbles[i], ArmOf(offers[i].site));
		SpawnPlanSites(g_index, offers, bubbles, n, &g_store, 0, HarnessDeep, offers, &plan, &tel);
		CheckInt(plan.count, 1, "E2: the soft site plans");
		CheckInt(plan.sites[0].arm, NEAR_BUBBLE_ARM_SOFT, "E2: the plan kept the midpoint arm");
		coveredBefore = tel.m_nCoveredFillCreations;
		CheckInt(SpawnRunPlan(plan, &g_store, HarnessTry, 0, &tel, SPAWN_PURPOSE_AMBIENT), SPAWN_REFUSAL_NONE,
			"E2: the site creates");
		CheckInt((int)Delta(tel.m_nCoveredFillCreations, coveredBefore), 1,
			"E2: the covered-fill row reads the attempt's arm, not the midpoint's");

		/* The fallback: the plan's own fade arm, no attempt override. */
		g_store.Reset();
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_NONE);
		g_armOf[nearSite] = NEAR_BUBBLE_ARM_FADE;
		n = SpawnOfferSites(g_index, q, offers, SPAWN_SERVICE_MAX_CANDIDATES);
		for (i = 0; i < n; i++)
			BubbleForArm(&bubbles[i], ArmOf(offers[i].site));
		SpawnPlanSites(g_index, offers, bubbles, n, &g_store, 0, HarnessDeep, offers, &plan, &tel);
		CheckInt(plan.sites[0].arm, NEAR_BUBBLE_ARM_FADE, "E2: the plan kept the fade arm");
		coveredBefore = tel.m_nCoveredFillCreations;
		CheckInt(SpawnRunPlan(plan, &g_store, HarnessTry, 0, &tel, SPAWN_PURPOSE_AMBIENT), SPAWN_REFUSAL_NONE,
			"E2: the fade site creates");
		CheckInt((int)Delta(tel.m_nCoveredFillCreations, coveredBefore), 1,
			"E2: the plan's fade arm is the fallback");
	}

	/* E3: the function gate - the budget, the pool and the force bypass. */
	{
		SSpawnBudgetFacts facts;
		SSpawnServiceTelemetry gateTel;
		uint32_t budgetBefore, poolBefore;
		const char *what;

		facts.randomCars = 10;
		facts.randomTarget = 100.0f;
		facts.carsInUse = 100;
		facts.carBudget = 200;
		facts.pedFreeSpaces = 1000;

		SpawnTelemetryReset(&gateTel);
		what = "E3: a budget below its lines passes";
		CheckInt(SpawnFunctionGate(facts, false, &gateTel), SPAWN_REFUSAL_NONE, what);
		facts.randomCars = 100;
		budgetBefore = Refusal(gateTel, SPAWN_REFUSAL_BUDGET);
		CheckInt(SpawnFunctionGate(facts, false, &gateTel), SPAWN_REFUSAL_BUDGET, "E3: the full random target refuses");
		CheckInt((int)Delta(Refusal(gateTel, SPAWN_REFUSAL_BUDGET), budgetBefore), 1, "E3: the budget refusal is counted");
		facts.randomCars = 10;
		facts.carsInUse = 200;
		CheckInt(SpawnFunctionGate(facts, false, &gateTel), SPAWN_REFUSAL_BUDGET, "E3: the full car budget refuses");
		facts.carsInUse = 100;
		facts.pedFreeSpaces = PED_POOL_RESERVE - 1;
		poolBefore = Refusal(gateTel, SPAWN_REFUSAL_POOL);
		CheckInt(SpawnFunctionGate(facts, false, &gateTel), SPAWN_REFUSAL_POOL, "E3: the pool reserve refuses");
		CheckInt((int)Delta(Refusal(gateTel, SPAWN_REFUSAL_POOL), poolBefore), 1, "E3: the pool refusal is counted");
		budgetBefore = Refusal(gateTel, SPAWN_REFUSAL_BUDGET);
		poolBefore = Refusal(gateTel, SPAWN_REFUSAL_POOL);
		CheckInt(SpawnFunctionGate(facts, true, &gateTel), SPAWN_REFUSAL_NONE, "E3: the force bypass steps over everything");
		CheckInt((int)Delta(Refusal(gateTel, SPAWN_REFUSAL_BUDGET), budgetBefore), 0, "E3: a forced pass counts no budget row");
		CheckInt((int)Delta(Refusal(gateTel, SPAWN_REFUSAL_POOL), poolBefore), 0, "E3: a forced pass counts no pool row");
	}

	/* E4: the explicit kind - the exact position, the floored lifetime, the
	 * normalized tag (the same-frame handle is the engine's ppCreated). */
	{
		SSpawnExplicitRequest request;
		SSpawnExplicitDecision decision;

		memset(&request, 0, sizeof(request));
		request.purpose = SPAWN_PURPOSE_MISSION;
		request.carModel = 190;
		request.x = 12.5f;
		request.y = -3.25f;
		request.z = 8.0f;
		request.dirX = 2.0f;
		request.dirY = 0.0f;
		request.lifetimeMs = 0;
		SpawnDecideExplicit(request, &decision);
		Check(decision.x == 12.5f && decision.y == -3.25f && decision.z == 8.0f, "E4: the explicit position is the request's, unchanged");
		Check(decision.dirX == 2.0f && decision.dirY == 0.0f, "E4: the explicit heading is the request's");
		CheckInt(decision.protectMs, NEAR_BUBBLE_GRACE_MS, "E4: a zero lifetime floors at the fresh-spawn grace");
		CheckInt(decision.createdBy, SPAWN_VEHICLE_TAG_RANDOM, "E4: a zero tag becomes the random tag");
		request.lifetimeMs = 9000;
		request.createdBy = 2;
		SpawnDecideExplicit(request, &decision);
		CheckInt(decision.protectMs, 9000, "E4: a longer lifetime stands");
		CheckInt(decision.createdBy, 2, "E4: the caller's tag stands");
	}

	/* E5: the readiness waits - the deferrals and the frames spent. */
	{
		SSpawnServiceTelemetry waitTel;
		uint32_t waitsBefore;

		SpawnTelemetryReset(&waitTel);
		waitsBefore = waitTel.m_nReadinessWaits;
		SpawnReadinessDefer(&waitTel, SPAWN_PURPOSE_AMBIENT, 1000);
		SpawnReadinessDefer(&waitTel, SPAWN_PURPOSE_AMBIENT, 1016);
		CheckInt((int)Delta(waitTel.m_nReadinessWaits, waitsBefore), 2, "E5: both deferrals count");
		CheckInt((int)Refusal(waitTel, SPAWN_REFUSAL_READINESS), 2, "E5: the readiness refusals count");
		SpawnReadinessReady(&waitTel, SPAWN_PURPOSE_AMBIENT, 1100);
		CheckInt((int)waitTel.m_nReadinessWaitMs, 100, "E5: the frames spent waiting are the clock's delta");
		SpawnReadinessReady(&waitTel, SPAWN_PURPOSE_AMBIENT, 1200);
		CheckInt((int)waitTel.m_nReadinessWaitMs, 100, "E5: a second ready adds nothing");
	}
}

/* ------------------------------------------------------------------ section F */

static void
Section6Telemetry(void)
{
	SSpawnRingQuery q = QueryAt(140.0f, 140.0f, 120.0f);
	SSpawnPlan plan;
	SSpawnServiceTelemetry tel;
	int32_t n;

	printf("F. the telemetry and the primitive's decisions\n");

	/* F1: the request rows by purpose and kind. */
	{
		SpawnTelemetryReset(&tel);
		SpawnRequestBegin(&tel, SPAWN_PURPOSE_AMBIENT, SPAWN_KIND_ROAD_SITE);
		SpawnRequestBegin(&tel, SPAWN_PURPOSE_AMBIENT, SPAWN_KIND_ROAD_SITE);
		SpawnRequestBegin(&tel, SPAWN_PURPOSE_AMBIENT, SPAWN_KIND_EXPLICIT);
		SpawnRequestBegin(&tel, SPAWN_PURPOSE_MISSION, SPAWN_KIND_EXPLICIT);
		CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_AMBIENT], 3, "F1: the ambient requests count");
		CheckInt((int)tel.m_nRequestsByPurpose[SPAWN_PURPOSE_MISSION], 1, "F1: the mission requests count");
		CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_ROAD_SITE], 2, "F1: the road-site requests count");
		CheckInt((int)tel.m_nRequestsByKind[SPAWN_KIND_EXPLICIT], 2, "F1: the explicit requests count");
	}

	/* F2: the plan cost rows. */
	{
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		ArmAll(NEAR_BUBBLE_ARM_SOFT);
		n = PlanOnce(q, &plan, &tel, 0);
		CheckInt((int)tel.m_nPlanCandidates, n, "F2: the candidates examined are the offer's");
		CheckInt((int)tel.m_nPlanPlacements, plan.count, "F2: the placements returned are the plan's");
		SpawnNotePlanCost(&tel, 123);
		CheckInt((int)tel.m_nPlanCalls, 1, "F2: the plan call counts");
		Check((int)tel.m_nPlanMicros == 123, "F2: the plan microseconds accumulate");
	}

	/* F3: the conservation check and its breach row. */
	{
		SpawnTelemetryReset(&tel);
		SpawnNoteConservation(&tel, 40, 40);
		CheckInt((int)tel.m_nConservationCellSum, 40, "F3: the cell target sum is carried");
		CheckInt((int)tel.m_nConservationGlobal, 40, "F3: the global target is carried");
		CheckInt((int)tel.m_nConservationBreaches, 0, "F3: an equal pair breaches nothing");
		SpawnNoteConservation(&tel, 39, 40);
		CheckInt((int)tel.m_nConservationBreaches, 1, "F3: a mismatch counts a breach");
	}

	/* F4: the removal and the defect rows. */
	{
		SpawnTelemetryReset(&tel);
		SpawnNoteRemoval(&tel, SPAWN_PURPOSE_AMBIENT);
		SpawnNoteRemoval(&tel, SPAWN_PURPOSE_AMBIENT);
		SpawnNoteRemoval(&tel, SPAWN_PURPOSE_PARKED);
		CheckInt((int)tel.m_nRemovedByPurpose[SPAWN_PURPOSE_AMBIENT], 2, "F4: the ambient removals count");
		CheckInt((int)tel.m_nRemovedByPurpose[SPAWN_PURPOSE_PARKED], 1, "F4: the parked removals count");
		SpawnNoteGroundRejection(&tel);
		SpawnNoteAirOffsetRejection(&tel);
		SpawnNoteWaterRejection(&tel);
		SpawnNoteWaterRejection(&tel);
		CheckInt((int)tel.m_nGroundRejections, 1, "F4: the ground rejection row counts");
		CheckInt((int)tel.m_nAirOffsetRejections, 1, "F4: the air offset row counts");
		CheckInt((int)tel.m_nWaterRejections, 2, "F4: the water rejection row counts");
	}

	/* F5: the primitive's own decisions - the lane draw, the travel
	 * direction, the between-nodes clamp. */
	{
		CheckInt(SpawnLaneFromDraw(3, 0), 0, "F5: the lane draw's first lane");
		CheckInt(SpawnLaneFromDraw(3, 2), 2, "F5: the lane draw's last lane");
		CheckInt(SpawnLaneFromDraw(3, 3), 0, "F5: the lane draw wraps");
		CheckInt(SpawnLaneFromDraw(3, 99), 0, "F5: the lane draw wraps far");
		CheckInt(SpawnLaneFromDraw(0, 5), 0, "F5: a zero-lane site draws lane 0");
		CheckInt(SpawnTravelDirection(1, 2), -1, "F5: the lower entry node travels forward to the higher");
		CheckInt(SpawnTravelDirection(2, 1), 1, "F5: the higher entry node travels backward");
		CheckInt(SpawnTravelDirection(2, 2), 1, "F5: an equal pair reads forward");
		Check(SpawnBetweenNodesClamp(0.5f, 5.0f, 20.0f) == 0.5f, "F5: a fitting car keeps the fraction");
		Check(SpawnBetweenNodesClamp(0.01f, 5.0f, 20.0f) == 0.25f, "F5: the clamp keeps the car's length off the entry node");
		Check(SpawnBetweenNodesClamp(0.99f, 5.0f, 20.0f) == 0.75f, "F5: the clamp keeps the car's length off the target node");
		Check(SpawnBetweenNodesClamp(0.5f, 5.0f, 8.0f) == 0.5f, "F5: a car that does not fit is centred");
		Check(SpawnBetweenNodesClamp(0.5f, 5.0f, 0.0f) == 0.5f, "F5: a zero-length pair is centred");
	}

	/* F6: the empty offer - the plan answers no-site. */
	{
		SpawnTelemetryReset(&tel);
		ScriptsReset();
		SpawnPlanSites(g_index, 0, 0, 0, &g_store, 0, 0, 0, &plan, &tel);
		CheckInt(plan.count, 0, "F6: an empty offer plans nothing");
		CheckInt(plan.refusal, SPAWN_REFUSAL_NO_SITE, "F6: the empty offer answers no-site");
		CheckInt((int)tel.m_nRefusalsByReason[SPAWN_REFUSAL_NO_SITE], 1, "F6: the no-site refusal is counted");
	}
}

/* ------------------------------------------------------------------ the cost */

static void
PrintRateHistogram(void)
{
	int32_t histogram[16];
	int32_t i;

	memset(histogram, 0, sizeof(histogram));
	for (i = 0; i < g_index.GetNumSites(); i++) {
		uint8_t rate = g_index.GetSites()[i].spawnRate;
		histogram[rate < 16 ? rate : 15]++;
	}
	printf("  the rarity weights (the rank's last key, the old dice's own 0..15):");
	for (i = 0; i < 16; i++)
		if (histogram[i])
			printf(" %d:%d", i, histogram[i]);
	printf("\n");
}

static void
PrintCost(void)
{
	SSpawnRingQuery q = QueryAt(140.0f, 140.0f, 120.0f);
	SSpawnPlan plan;
	SSpawnServiceTelemetry tel;
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	double start, elapsed;
	int32_t n, i, iter;

	SpawnTelemetryReset(&tel);
	ScriptsReset();
	ArmAll(NEAR_BUBBLE_ARM_SOFT);
	n = SpawnOfferSites(g_index, q, offers, SPAWN_SERVICE_MAX_CANDIDATES);
	for (i = 0; i < n; i++)
		BubbleForArm(&bubbles[i], ArmOf(offers[i].site));

	start = NowSeconds();
	for (iter = 0; iter < 1000; iter++) {
		g_store.Reset();
		SpawnPlanSites(g_index, offers, bubbles, n, &g_store, (uint32_t)iter, HarnessDeep, offers, &plan, &tel);
	}
	elapsed = NowSeconds() - start;
	printf("  the planner's cost: %d candidates, %d placements, %.3f us a plan call (1000 calls)\n",
		n, plan.count, elapsed * 1e6 / 1000.0);
}

int
main(void)
{
	SSpawnServiceTelemetry tel;

	BuildFixture();
	g_index.Build(FixtureGraph());
	Check(g_index.GetNumSites() > 100, "the fixture builds its sites");
	Check(g_index.GetStats().boatSites > 0, "the fixture holds boat sites");
	printf("spawn-service core harness (VICE-150)\n");
	printf("  the fixture: %d nodes, %d links, %d connections, %d sites (%d boat, %d dropped)\n",
		g_numNodes, g_numLinks, g_numConnections,
		g_index.GetNumSites(), g_index.GetStats().boatSites,
		g_index.GetStats().droppedNoLanes + g_index.GetStats().droppedMixed + g_index.GetStats().droppedUnresolved);

	SpawnTelemetryReset(&tel);
	ScriptsReset();
	Section0Mapping();
	Section1Offer();
	Section2Rank();
	Section3Gates();
	Section4DeepFacts();
	Section5Run();
	Section6Telemetry();
	PrintRateHistogram();
	PrintCost();

	printf("  %lld checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
