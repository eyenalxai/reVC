/*
 * The evidence program for the population director (ticket VICE-152).
 *
 * It compiles the director's own engine-free core (src/control/SpawnDirector.h)
 * and the site index's real build (src/control/SpawnSitesIndex.cpp), builds a
 * synthetic city of the shipped shape, and drives two scripted routes over it:
 * the old ring placement (the before) and the director's maintenance (the
 * after). The per-cell minimum over a drive is the before/after number the
 * design asks for; the acceptance is that no cell stays below its floor longer
 * than the fill bound, that the cell targets sum to the budget, and that no sum
 * exceeds the caps.
 *
 *	A. The city and the site facts. The synthetic city is a street grid
 *	   aligned with the director's 128 m cells (so every cell holds
 *	   streets), a water bay near the route, and four zone quadrants with
 *	   different densities. The site index is the real build over the
 *	   synthetic graph, and the director's presence facts come from it
 *	   (SpawnDirectorCollectSiteFacts): the streets, the water lanes and the
 *	   lanes of every cell.
 *
 *	B. The target math. The car target is the engine's own formula
 *	   (RandomCarTarget: 30 x the Car Density x the ini multiplier x scale^2,
 *	   topped by MAX_RANDOM_CARS): 60 at the stock sliders, 408 at the
 *	   owner's. The apportionment's sums are checked against it at every
 *	   refresh, cell by cell against the ceilings, and the caps are checked
 *	   (MAX_RANDOM_CARS, MAX_PEDS_IN_USE).
 *
 *	C. The after drive. The trace starts from a filled street: a warm-up at
 *	   the route's start (the boot fill's job in the live game, the covered
 *	   fill of VICE-158) runs the same maintenance for 30 s, then the
 *	   director's metric and the counters restart and the player drives the
 *	   route at 20 m/s. Every frame the director refreshes when the player's
 *	   cell moved (the live zone densities and the view), ticks (the live
 *	   ladder arm of every active cell, the counts from the occupancy store),
 *	   and the ambient calls ask it for the cell to serve: the largest deficit
 *	   fraction first, the nearest then, the cells the ladder refuses never.
 *	   Each served cell runs the *real* service core (SpawnOfferSites,
 *	   SpawnPlanSites, SpawnRunPlan) over the real site index, with the
 *	   harness's own primitive (the store's create, the primitive's speed
 *	   test). The traffic drives (15 m/s along the site's direction), the
 *	   cull removes at the despawn behind the hidden gate, and the director's
 *	   over-target release is exercised. The metric is read from the
 *	   director's own per-cell minima and clocks; a cell that enters the
 *	   active set empty shows a minimum of 0 (the director creates only
 *	   inside the set), so the acceptance is the duration - no cell below its
 *	   floor past the fill bound.
 *
 *	D. The before drive. The same route and the same traffic model, but the
 *	   requests keep the old call's shape: the target is the player, the ring
 *	   the front (500 m) or the side (240 m) band and the cone +-45 degrees.
 *	   The same per-cell minimum distribution is reported; the dry cells of
 *	   the ring dice are the contrast the ticket is about.
 *
 *	E. The water cells. The bay's cells carry their own share of the car
 *	   target (the water lanes), the requests aimed at them ask the service
 *	   for a water-capable site, and the boats created there are counted.
 *
 *	F. The modes. An interior refresh: the car targets are all zero (no road
 *	   planning inside interiors) and the ped target is the flat interior
 *	   budget. An off-map refresh: no active set, no targets.
 *
 *	G. The ledger. The far layer's own class (SPAWN_PURPOSE_FAR) is driven
 *	   through the service's note helpers, and the conservation note is
 *	   checked against the service's own telemetry rows.
 *
 *	H. The cost. The tick and the refresh are timed over the drive; the rows
 *	   are the frame's budget.
 *
 * What this program cannot reach: the engine side (SpawnDirector.cpp) - the
 * camera, the zones, the rings and the real cull - is the game's, checked by
 * the build and by the live soak. The watchdog (the slow-frame pedestrian gate
 * of Game.cpp) is the frame's own and is untouched by the ticket; the harness
 * only states it. The drive's traffic model is a model: the cars drive
 * straight along the sites' directions (no turns), the peds walk the ring
 * bands (the population's own trickle until its migration), and the culls use
 * the same gates the engine's do (the hidden arms and the fresh-spawn grace).
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 \
 *	    -o /tmp/spawn-director utils/spawn-service/director.cpp \
 *	    src/control/SpawnSitesIndex.cpp
 *	/tmp/spawn-director
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "SpawnDirector.h"

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

/* ------------------------------------------------------------------ *
 * A. the synthetic city
 * ------------------------------------------------------------------ */

/*
 * The shipped shape, at the scale the director's grid needs. The streets run
 * along the director's cell boundaries (128 m apart), so every cell of the
 * city holds streets: a horizontal line and a vertical line cross at every
 * node and each directed connection carries two lanes a side. The bay sits
 * between two street rows near the route, its own channel network, so its
 * cells carry water lanes and the water track has something to spread.
 *
 * The node counts stay far below the graph's capacities (NUM_PATHNODES 9650,
 * NUM_CARPATHLINKS 3500, NUM_PATHCONNECTIONS 20400): 576 road nodes, 15 water
 * nodes, 1126 links, 2252 directed connections - the site index's build is the
 * real one, over a graph of the shipped shape.
 */
#define CITY_ORIGIN_X	(-2272.0f)	/* the first street line: -2400 + 128 */
#define CITY_ORIGIN_Y	(-1872.0f)	/* -2000 + 128 */
#define CITY_STEP	128.0f
#define CITY_COLS	24
#define CITY_ROWS	24
#define NUM_ROAD_NODES	(CITY_COLS * CITY_ROWS)
#define WATER_ORIGIN_X	(-256.0f)
#define WATER_ORIGIN_Y	(-1168.0f)
#define WATER_COLS	5
#define WATER_ROWS	3
#define NUM_WATER_NODES	(WATER_COLS * WATER_ROWS)
#define NUM_CITY_NODES	(NUM_ROAD_NODES + NUM_WATER_NODES)
#define MAX_EDGES	2400

static SSpawnSiteNode g_nodes[NUM_PATHNODES];
static SSpawnSiteLink g_links[NUM_CARPATHLINKS];
static uint16_t g_connectionNode[NUM_PATHCONNECTIONS];
static int16_t g_connectionLink[NUM_PATHCONNECTIONS];
static int16_t g_connectionZone[NUM_PATHCONNECTIONS];

struct SEdge
{
	int a, b;
};

static SEdge g_edges[MAX_EDGES];
static int g_numEdges;

static void
AddEdge(int a, int b)
{
	if (g_numEdges >= MAX_EDGES) {
		printf("the edge list is full\n");
		exit(1);
	}
	g_edges[g_numEdges].a = a;
	g_edges[g_numEdges].b = b;
	g_numEdges++;
}

/* The zone quadrants around the origin: the four densities the live facts
 * return (the zones' own carDensity / pedDensity). The bay is zone 4. */
static int
ZoneAt(float x, float y)
{
	if (x < -350.0f && x > -450.0f)
		return 0;
	if (y > -1250.0f && y < -830.0f && x > -350.0f && x < 350.0f)
		return 4;	/* the bay's own */
	if (x < 0.0f)
		return y < 0.0f ? 0 : 2;
	return y < 0.0f ? 1 : 3;
}

static float
ZoneCarDensity(int zone)
{
	static const float d[5] = { 1.0f, 1.3f, 0.7f, 1.1f, 0.9f };

	return d[zone];
}

static float
ZonePedDensity(int zone)
{
	static const float d[5] = { 1.0f, 1.2f, 0.8f, 1.0f, 0.6f };

	return d[zone];
}

static inline int
RoadNode(int row, int col)
{
	return row * CITY_COLS + col;
}

static inline int
WaterNode(int row, int col)
{
	return NUM_ROAD_NODES + row * WATER_COLS + col;
}

/* The bay's rectangle: no street runs through it (the shipped map's shore
 * roads turn around the water, and a street's traffic must not fill a water
 * cell's count). */
static bool
InBay(float x, float y)
{
	return x > -350.0f && x < 350.0f && y > -1250.0f && y < -830.0f;
}

static void
BuildCity(void)
{
	int i, r, c, edge;

	memset(g_nodes, 0, sizeof(g_nodes));
	memset(g_links, 0, sizeof(g_links));
	memset(g_connectionNode, 0, sizeof(g_connectionNode));
	memset(g_connectionLink, 0, sizeof(g_connectionLink));
	memset(g_connectionZone, 0, sizeof(g_connectionZone));
	g_numEdges = 0;

	for (i = 0; i < NUM_ROAD_NODES; i++) {
		r = i / CITY_COLS;
		c = i % CITY_COLS;
		g_nodes[i].x = CITY_ORIGIN_X + c * CITY_STEP;
		g_nodes[i].y = CITY_ORIGIN_Y + r * CITY_STEP;
		g_nodes[i].z = 10.0f;
		g_nodes[i].spawnRate = 5 + (i * 7) % 11;
	}
	for (i = 0; i < NUM_WATER_NODES; i++) {
		r = i / WATER_COLS;
		c = i % WATER_COLS;
		g_nodes[NUM_ROAD_NODES + i].x = WATER_ORIGIN_X + c * CITY_STEP;
		g_nodes[NUM_ROAD_NODES + i].y = WATER_ORIGIN_Y + r * CITY_STEP;
		g_nodes[NUM_ROAD_NODES + i].z = 0.0f;
		g_nodes[NUM_ROAD_NODES + i].spawnRate = 6;
		g_nodes[NUM_ROAD_NODES + i].flags |= SPAWN_SITE_NODE_WATER;
	}

	/* The street grid: the rows and the columns, with the bay carved out. */
	for (r = 0; r < CITY_ROWS; r++)
		for (c = 0; c + 1 < CITY_COLS; c++) {
			int a = RoadNode(r, c), b = RoadNode(r, c + 1);
			if (InBay((g_nodes[a].x + g_nodes[b].x) * 0.5f, (g_nodes[a].y + g_nodes[b].y) * 0.5f))
				continue;
			AddEdge(a, b);
		}
	for (c = 0; c < CITY_COLS; c++)
		for (r = 0; r + 1 < CITY_ROWS; r++) {
			int a = RoadNode(r, c), b = RoadNode(r + 1, c);
			if (InBay((g_nodes[a].x + g_nodes[b].x) * 0.5f, (g_nodes[a].y + g_nodes[b].y) * 0.5f))
				continue;
			AddEdge(a, b);
		}

	/* The bay's channels. */
	for (r = 0; r < WATER_ROWS; r++)
		for (c = 0; c + 1 < WATER_COLS; c++)
			AddEdge(WaterNode(r, c), WaterNode(r, c + 1));
	for (c = 0; c < WATER_COLS; c++)
		for (r = 0; r + 1 < WATER_ROWS; r++)
			AddEdge(WaterNode(r, c), WaterNode(r + 1, c));

	/* The graph's arrays: the links (one per edge) and the connections (two
	 * per edge, one per direction). The lane counts are the shipped shape's
	 * two a side; the direction is the edge's own, in hundredths. */
	{
		int numNodes = NUM_CITY_NODES;
		int firstLink[NUM_CITY_NODES];
		int linkOfEdge[MAX_EDGES];
		int conn = 0;

		for (i = 0; i < numNodes; i++)
			firstLink[i] = 0;
		for (edge = 0; edge < g_numEdges; edge++) {
			const SEdge &e = g_edges[edge];
			float dx = g_nodes[e.b].x - g_nodes[e.a].x;
			float dy = g_nodes[e.b].y - g_nodes[e.a].y;
			float len = sqrtf(dx * dx + dy * dy);

			linkOfEdge[edge] = edge;
			g_links[edge].dirX = (int8_t)(100.0f * dx / len);
			g_links[edge].dirY = (int8_t)(100.0f * dy / len);
			g_links[edge].numLeftLanes = 2;
			g_links[edge].numRightLanes = 2;
			g_links[edge].pathNodeIndex = (int16_t)e.a;
			g_links[edge].width = 8;
		}
		for (edge = 0; edge < g_numEdges; edge++) {
			const SEdge &e = g_edges[edge];
			firstLink[e.a]++;
			firstLink[e.b]++;
		}
		{
			int sum = 0;

			for (i = 0; i < numNodes; i++) {
				int count = firstLink[i];

				firstLink[i] = sum;
				sum += count;
			}
			g_nodes[0].firstLink = (int16_t)firstLink[0];
			for (i = 0; i < numNodes; i++)
				g_nodes[i].firstLink = (int16_t)firstLink[i];
		}
		for (i = 0; i < numNodes; i++)
			g_nodes[i].numLinks = 0;
		/* The connections: laid out per node, in edge order. */
		{
			int cursor[NUM_CITY_NODES];

			for (i = 0; i < numNodes; i++)
				cursor[i] = firstLink[i];
			for (edge = 0; edge < g_numEdges; edge++) {
				const SEdge &e = g_edges[edge];
				float mx = (g_nodes[e.a].x + g_nodes[e.b].x) * 0.5f;
				float my = (g_nodes[e.a].y + g_nodes[e.b].y) * 0.5f;

				g_connectionNode[cursor[e.a]] = (uint16_t)e.b;
				g_connectionLink[cursor[e.a]] = (int16_t)linkOfEdge[edge];
				g_connectionZone[cursor[e.a]] = (int16_t)ZoneAt(mx, my);
				cursor[e.a]++;
				g_nodes[e.a].numLinks++;

				g_connectionNode[cursor[e.b]] = (uint16_t)e.a;
				g_connectionLink[cursor[e.b]] = (int16_t)linkOfEdge[edge];
				g_connectionZone[cursor[e.b]] = (int16_t)ZoneAt(mx, my);
				cursor[e.b]++;
				g_nodes[e.b].numLinks++;
			}
			conn = firstLink[numNodes - 1] + g_nodes[numNodes - 1].numLinks;
		}
		printf("city: %d road nodes, %d water nodes, %d links, %d directed connections\n",
			NUM_ROAD_NODES, NUM_WATER_NODES, g_numEdges, conn);
		Check(g_numEdges <= NUM_CARPATHLINKS, "the links fit the graph's capacity");
		Check(conn <= NUM_PATHCONNECTIONS, "the connections fit the graph's capacity");
	}
}

static SSpawnSiteGraph
CityGraph(void)
{
	SSpawnSiteGraph graph;

	graph.numNodes = NUM_CITY_NODES;
	graph.numLinks = g_numEdges;
	graph.numConnections = NUM_PATHCONNECTIONS;
	graph.nodes = g_nodes;
	graph.links = g_links;
	graph.connectionNode = g_connectionNode;
	graph.connectionLink = g_connectionLink;
	graph.connectionZone = g_connectionZone;
	return graph;
}

/* ------------------------------------------------------------------ *
 * the drive's world
 * ------------------------------------------------------------------ */

/* The rings of the drive: the cap (the owner's settings, scale 2). */
#define DRIVE_SCALE		2.0f
#define CAR_DESPAWN		560.0f	/* rings.carDespawn at the cap */
#define PED_RADIUS		262.0f	/* rings.pedRemove at the cap */
#define PED_CREATE_MIN		200.0f	/* rings.pedCreateMin at the cap */
#define PED_CREATE_MAX		260.0f	/* rings.pedCreateMax at the cap */
#define VISIBLE_RANGE		1200.0f
#define ONSCREEN_SPAWN_MIN	440.0f	/* rings.carOnScreenSpawnMin at the cap */
#define FPS			30
#define FRAME_MS		33	/* the integer clock the tick reads */
#define DRIVE_FRAMES		3600	/* 120 s, 2400 m at 20 m/s */
#define WARMUP_FRAMES		900	/* 30 s: the street fills at the route's start */
#define PLAYER_SPEED		20.0f
#define TRAFFIC_SPEED		15.0f
#define RATE_PER_FRAME		6	/* RANDOM_CAR_ATTEMPTS_PER_FRAME x scale */
#define PED_RATE_PER_FRAME	6	/* PED_ATTEMPTS_PER_FRAME x scale */
#define MAX_CARS		600
#define MAX_PEDS		1600
#define ROAD_DENSITY		1.0f	/* the player's m_fRoadDensity */
#define CAR_MULTIPLIER		3.4f	/* CIniFile::CarNumberMultiplier at the owner's sliders */
#define PED_MULTIPLIER		3.4f	/* CIniFile::PedNumberMultiplier */
#define PED_AREA_DENSITY_SCALE	3.0f	/* Population.cpp's own */

static SSpawnOccupancyStore g_store;
static CSpawnSiteIndex g_index;
static SSpawnDirector g_director;
static SSpawnServiceTelemetry g_serviceTel;
static SSpawnDirectorTelemetry g_directorTel;

struct SCarState
{
	bool live;
	float x, y;
	float dirX, dirY;
	int32_t handle;
	int32_t driverHandle;
	uint32_t bornMs;
};

struct SPedState
{
	bool live;
	float x, y;
	float dirX, dirY;
	int32_t handle;
	uint32_t bornMs;
};

static SCarState g_cars[MAX_CARS];
static SPedState g_peds[MAX_PEDS];
static int32_t g_numCars;
static int32_t g_numPeds;
static int32_t g_nextCarHandle;
static int32_t g_nextPedHandle;

static float g_playerX, g_playerY;
static uint32_t g_nowMs;
static int g_directorRequests;
static int g_directorEmpty;
static int g_directorReleases;
static int g_planPlacements;
static int g_planZero;
static int g_attempts;
static int g_speedRefusals;
static int g_budgetSkips;
static int g_waterRequests;
static int g_waterServed;
static int g_createdByKind[2];	/* 0: land, 1: water */
static int g_refusals[SPAWN_REFUSAL_COUNT];

/* The seeded dice: the model's own stream, deterministic. */
static uint32_t g_rngState = 0x1234567u;

static uint32_t
Rng(void)
{
	g_rngState = g_rngState * 1103515245u + 12345u;
	return (g_rngState >> 16) & 0x7fff;
}

static float
RngFloat(float lo, float hi)
{
	return lo + (hi - lo) * ((float)Rng() / 32767.0f);
}

/* The scripted ladder at a position: the camera looks +x, the bubble and the
 * bands are the cap's. The candidate is the real one (SNearBubbleCandidate),
 * so the arms are the rule's own. */
static uint8_t
ArmAt(float x, float y)
{
	SNearBubbleCandidate c;
	float dx = x - g_playerX;
	float dy = y - g_playerY;
	float dist = sqrtf(dx * dx + dy * dy);
	float bearing = dist > 0.001f ? dx / dist : 1.0f;

	memset(&c, 0, sizeof(c));
	c.distToPlayer = dist;
	c.distToCamera = dist;
	c.visibleRange = VISIBLE_RANGE;
	c.onScreenSpawnMin = ONSCREEN_SPAWN_MIN;
	c.inFrustum = bearing > 0.5f;			/* about +-60 degrees of +x */
	c.inFrustumMargin = bearing > 0.31f;		/* about +-72 degrees */
	c.occluded = false;
	c.fadeHides = false;
	c.bypassView = false;
	return (uint8_t)NearBubbleArmFor(c);
}

/* The hidden gate of the culls: the arms a removal may run under (the near
 * bubble's own rule, simplified to the arms the scripted ladder can produce). */
static bool
HiddenAt(float x, float y)
{
	uint8_t arm = ArmAt(x, y);

	return arm == NEAR_BUBBLE_ARM_SOFT || arm == NEAR_BUBBLE_ARM_FADE_DISTANCE ||
		arm == NEAR_BUBBLE_ARM_FADE || arm == NEAR_BUBBLE_ARM_OCCLUDER ||
		arm == NEAR_BUBBLE_ARM_RAY;
}

/* The live facts of one active cell (the refresh's callback): the info zone's
 * densities at the centre and the view. */
static void
LiveFacts(void *user, int32_t cell, float centreX, float centreY, SSpawnDirectorCellFacts *facts)
{
	int zone = ZoneAt(centreX, centreY);
	uint8_t arm = ArmAt(centreX, centreY);

	(void)user;
	facts->zoneCarDensity = ZoneCarDensity(zone);
	facts->zonePedDensity = ZonePedDensity(zone);
	facts->inView = arm == NEAR_BUBBLE_ARM_NONE || arm == NEAR_BUBBLE_ARM_ONSCREEN_BAND;
}

/* The engine's targets, mirrored: the car target is the one formula
 * (RandomCarTarget, PopulationBudget.h) and the ped target is the area budget
 * plus the crews (Population.cpp's AreaPedCountTarget). */
static int32_t
DriveCarTarget(void)
{
	return (int32_t)ceilf(RandomCarTarget(1.0f, CAR_MULTIPLIER, DRIVE_SCALE));
}

static int32_t
DrivePedTarget(void)
{
	int zone = ZoneAt(g_playerX, g_playerY);
	float budget = (ZonePedDensity(zone) + ZoneCarDensity(zone)) * ROAD_DENSITY *
		PED_MULTIPLIER * PED_AREA_DENSITY_SCALE * DRIVE_SCALE * DRIVE_SCALE;

	if (budget > (float)MAX_PEDS_IN_USE)
		budget = (float)MAX_PEDS_IN_USE;
	return (int32_t)ceilf(budget) + g_numCars;
}

/* The traffic's records and the store. */
static int32_t
AddCarRecord(float x, float y, float dirX, float dirY)
{
	int32_t i, slot = -1;

	for (i = 0; i < MAX_CARS; i++)
		if (!g_cars[i].live) {
			slot = i;
			break;
		}
	if (slot < 0)
		return -1;
	SCarState &car = g_cars[slot];

	car.live = true;
	car.x = x;
	car.y = y;
	car.dirX = dirX;
	car.dirY = dirY;
	car.bornMs = g_nowMs;
	car.handle = g_nextCarHandle++ % NUMVEHICLES;
	car.driverHandle = g_nextPedHandle++ % NUMPEDS;
	g_numCars++;
	g_store.Register(SPAWN_OCCUPANCY_VEHICLE, car.handle);
	g_store.Create(SPAWN_OCCUPANCY_VEHICLE, car.handle, x, y);
	/* The driver: a pedestrian of the same pool, counted at the car (the
	 * population's own arithmetic counts the crews). */
	g_store.Register(SPAWN_OCCUPANCY_PED, car.driverHandle);
	g_store.Create(SPAWN_OCCUPANCY_PED, car.driverHandle, x, y);
	return slot;
}

static void
RemoveCarRecord(int32_t slot)
{
	SCarState &car = g_cars[slot];

	g_store.Remove(SPAWN_OCCUPANCY_VEHICLE, car.handle);
	g_store.Remove(SPAWN_OCCUPANCY_PED, car.driverHandle);
	car.live = false;
	g_numCars--;
}

static int32_t
AddPedRecord(float x, float y)
{
	int32_t i, slot = -1;

	for (i = 0; i < MAX_PEDS; i++)
		if (!g_peds[i].live) {
			slot = i;
			break;
		}
	if (slot < 0)
		return -1;
	SPedState &ped = g_peds[slot];
	float angle = RngFloat(0.0f, 6.2831853f);

	ped.live = true;
	ped.x = x;
	ped.y = y;
	ped.dirX = cosf(angle);
	ped.dirY = sinf(angle);
	ped.bornMs = g_nowMs;
	ped.handle = g_nextPedHandle++ % NUMPEDS;
	g_numPeds++;
	g_store.Register(SPAWN_OCCUPANCY_PED, ped.handle);
	g_store.Create(SPAWN_OCCUPANCY_PED, ped.handle, x, y);
	return slot;
}

static void
RemovePedRecord(int32_t slot)
{
	g_store.Remove(SPAWN_OCCUPANCY_PED, g_peds[slot].handle);
	g_peds[slot].live = false;
	g_numPeds--;
}

/* ------------------------------------------------------------------ *
 * the service's own core, driven by the harness
 * ------------------------------------------------------------------ */

struct SAttempt
{
	float targetX, targetY;
	float playerSpeedX, playerSpeedY;
	bool water;		/* the request aimed at a water cell */
};

/* The harness's primitive: the store's create plus the primitive's own speed
 * test (a car that drives away from the target is refused, CarCtrl.cpp). */
static ESpawnRefusal
HarnessTrySite(void *user, const SSpawnPlanSite &planSite, uint8_t *pArm)
{
	SAttempt *pAttempt = (SAttempt *)user;
	const SSpawnSite &site = g_index.GetSites()[planSite.site];
	int16_t sign = SpawnTravelDirection(site.node1, site.node2);
	float fx = sign * ((float)site.dirX / 100.0f);
	float fy = sign * ((float)site.dirY / 100.0f);
	float speedX = fx * TRAFFIC_SPEED;
	float speedY = fy * TRAFFIC_SPEED;
	float dx = site.x - pAttempt->targetX;
	float dy = site.y - pAttempt->targetY;

	(void)pArm;
	g_attempts++;
	/* The primitive's speed test: (the car's speed - the player's speed) .
	 * (the car - the target) >= 0 refuses. */
	if ((speedX - pAttempt->playerSpeedX) * dx + (speedY - pAttempt->playerSpeedY) * dy >= 0.0f) {
		g_speedRefusals++;
		return SPAWN_REFUSAL_VISIBILITY;
	}
	if (AddCarRecord(site.x, site.y, fx, fy) < 0)
		return SPAWN_REFUSAL_POOL;
	if (site.flags & SPAWN_SITE_BOAT) {
		g_createdByKind[1]++;
		g_waterServed++;
	} else
		g_createdByKind[0]++;
	return SPAWN_REFUSAL_NONE;
}

/* One request through the real service core: the offer, the plan and the run.
 * The facts are the harness's own (the ladder's candidates at the sites). */
static void
RunServiceRequest(const SSpawnRingQuery &query)
{
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
	SSpawnPlan plan;
	SAttempt attempt;
	int32_t i, numOffers;

	attempt.targetX = query.targetX;
	attempt.targetY = query.targetY;
	attempt.playerSpeedX = PLAYER_SPEED;
	attempt.playerSpeedY = 0.0f;
	attempt.water = query.water == SPAWN_SITES_WATER;

	numOffers = SpawnOfferSites(g_index, query, offers, SPAWN_SERVICE_MAX_CANDIDATES);
	for (i = 0; i < numOffers; i++) {
		const SSpawnSite &site = g_index.GetSites()[offers[i].site];
		uint8_t arm = ArmAt(site.x, site.y);
		SNearBubbleCandidate c;

		memset(&c, 0, sizeof(c));
		c.distToPlayer = 1000.0f;
		c.distToCamera = 1000.0f;
		c.visibleRange = VISIBLE_RANGE;
		c.onScreenSpawnMin = ONSCREEN_SPAWN_MIN;
		c.inFrustum = false;
		c.inFrustumMargin = false;
		c.occluded = false;
		c.fadeHides = false;
		c.bypassView = false;
		/* The candidate the rank reads is the arm the scripted ladder
		 * resolved; the harness writes it back into the candidate's shape by
		 * its own arm table. */
		switch (arm) {
		case NEAR_BUBBLE_ARM_NONE:	c.inFrustumMargin = true; break;
		case NEAR_BUBBLE_ARM_SOFT:	c.distToCamera = 200.0f; break;
		case NEAR_BUBBLE_ARM_RAY:	c.distToCamera = 50.0f; break;
		case NEAR_BUBBLE_ARM_FADE_DISTANCE:	c.distToCamera = VISIBLE_RANGE + 1.0f; break;
		case NEAR_BUBBLE_ARM_ONSCREEN_BAND:	c.inFrustum = true; c.distToCamera = ONSCREEN_SPAWN_MIN + 1.0f; break;
		case NEAR_BUBBLE_ARM_OCCLUDER:	c.occluded = true; break;
		default:	c.inFrustumMargin = true; break;
		}
		bubbles[i] = c;
	}
	SpawnPlanSites(g_index, offers, bubbles, numOffers, &g_store, g_nowMs,
		NULL, NULL, &plan, &g_serviceTel);
	g_planPlacements += plan.count;
	if (plan.count == 0) {
		g_planZero++;
		g_refusals[plan.refusal]++;
	}
	SpawnRunPlan(plan, &g_store, HarnessTrySite, &attempt, &g_serviceTel, SPAWN_PURPOSE_AMBIENT);
}

/* The director's request: the cell it handed out, the band around the cell
 * (half a cell), no cone. */
static void
ServeDirectorRequest(void)
{
	float targetX, targetY;
	uint8_t water;
	SSpawnRingQuery query;

	/* The core hands out the served cell's request target (the cell centre
	 * one lead behind along the travel) and its water tag; the drive's travel
	 * is +x (the player's velocity) and the cone is the half-plane the
	 * primitive's speed test can accept. */
	if (!SpawnDirectorNextRequest(&g_director, g_playerX, g_playerY, 1.0f, 0.0f, &targetX, &targetY, &water)) {
		g_directorEmpty++;
		return;
	}
	g_directorRequests++;
	if (water)
		g_waterRequests++;
	query.targetX = targetX;
	query.targetY = targetY;
	query.spawnDist = SPAWN_DIRECTOR_REQUEST_RING;
	query.dirX = 1.0f;
	query.dirY = 0.0f;
	query.angleLimit = 0.0f;
	query.forward = true;
	query.water = water ? SPAWN_SITES_ANY : SPAWN_SITES_LAND;
	query.allowDisabled = false;
	RunServiceRequest(query);
}

/* The old call's own shape (the before): the player as the target, the front
 * (500 m) or the side (240 m) ring and the cone +-45 degrees. */
static void
ServeRingRequest(void)
{
	SSpawnRingQuery query;

	query.targetX = g_playerX;
	query.targetY = g_playerY;
	query.spawnDist = (g_nowMs / FRAME_MS) & 1 ? 500.0f : 240.0f;
	query.dirX = 1.0f;
	query.dirY = 0.0f;
	query.angleLimit = 0.707f;
	query.forward = true;
	query.water = SPAWN_SITES_ANY;
	query.allowDisabled = false;
	RunServiceRequest(query);
}

/* ------------------------------------------------------------------ *
 * the drive
 * ------------------------------------------------------------------ */

static int g_useDirector;
static double g_tickSeconds;
static double g_refreshSeconds;
static int g_refreshes;
static int g_trackCarCells;
static int g_trackPedCells;

/* The arms array of the frame, indexed by cell id. */
static uint8_t g_arms[SPAWN_DIRECTOR_CELLS];

static void
RefreshDirector(void)
{
	int32_t i;

	for (i = 0; i < SPAWN_DIRECTOR_CELLS; i++)
		g_arms[i] = (uint8_t)NEAR_BUBBLE_ARM_SOFT;
	SpawnDirectorRefresh(&g_director, g_nowMs, g_playerX, g_playerY, ROAD_DENSITY,
		CAR_DESPAWN, PED_RADIUS, DriveCarTarget(), DrivePedTarget(),
		SPAWN_DIRECTOR_MODE_MAP, LiveFacts, NULL);
	g_refreshes++;
	/* The conservation check of every refresh: the sums are the budget. */
	CheckInt(g_director.targetSum[SPAWN_DIRECTOR_TRACK_CAR], g_director.target[SPAWN_DIRECTOR_TRACK_CAR],
		"the car target sum is the budget");
	CheckInt(g_director.targetSum[SPAWN_DIRECTOR_TRACK_PED], g_director.target[SPAWN_DIRECTOR_TRACK_PED],
		"the ped target sum is the budget");
	Check(g_director.target[SPAWN_DIRECTOR_TRACK_CAR] <= MAX_RANDOM_CARS,
		"the car target never exceeds MAX_RANDOM_CARS");
	{
		int32_t pedBudget = DrivePedTarget() - g_numCars;

		Check(pedBudget <= MAX_PEDS_IN_USE, "the ped budget never exceeds MAX_PEDS_IN_USE");
	}
	for (i = 0; i < g_director.numActive[SPAWN_DIRECTOR_TRACK_CAR]; i++) {
		int32_t cell = g_director.aActive[SPAWN_DIRECTOR_TRACK_CAR][i];
		const SSpawnDirectorTrackState &track = g_director.aState[cell].track[SPAWN_DIRECTOR_TRACK_CAR];

		if (track.target > track.ceiling)
			Check(false, "a car target never exceeds its cell's lane ceiling");
	}
}

static void
TickDirector(void)
{
	int32_t i;
	double t0 = NowSeconds();

	for (i = 0; i < g_director.numActive[SPAWN_DIRECTOR_TRACK_CAR]; i++) {
		int32_t cell = g_director.aActive[SPAWN_DIRECTOR_TRACK_CAR][i];

		g_arms[cell] = ArmAt(SpawnDirectorCellCentreX(cell), SpawnDirectorCellCentreY(cell));
	}
	SpawnDirectorBeginFrame(&g_director, g_store, g_nowMs, g_arms);
	g_tickSeconds += NowSeconds() - t0;
}

/* The cull of one frame: the despawn behind the hidden gate, the grace, and
 * the director's over-target release. */
static void
CullCars(void)
{
	int32_t i;

	for (i = 0; i < MAX_CARS; i++) {
		SCarState &car = g_cars[i];
		float dx, dy, dist;

		if (!car.live)
			continue;
		dx = car.x - g_playerX;
		dy = car.y - g_playerY;
		dist = sqrtf(dx * dx + dy * dy);
		if (g_nowMs - car.bornMs < NEAR_BUBBLE_GRACE_MS)
			continue;
		if (!HiddenAt(car.x, car.y))
			continue;
		if (dist > CAR_DESPAWN) {
			RemoveCarRecord(i);
			continue;
		}
		{
			int32_t cell = SpawnDirectorCellOf(car.x, car.y);

			if (SpawnDirectorReleaseAllowed(&g_director, cell, car.x, car.y, g_playerX, g_playerY, g_nowMs)) {
				SpawnDirectorNoteReleaseAt(&g_director, cell, g_nowMs);
				SpawnDirectorNoteRelease(&g_directorTel);
				g_directorReleases++;
				RemoveCarRecord(i);
			}
		}
	}
}

/* The traffic's own frame: the movement (the site's direction, no turns) and
 * the driver with it. */
static void
MoveTraffic(void)
{
	int32_t i;
	float step = TRAFFIC_SPEED / FPS;

	for (i = 0; i < MAX_CARS; i++) {
		SCarState &car = g_cars[i];

		if (!car.live)
			continue;
		car.x += car.dirX * step;
		car.y += car.dirY * step;
		g_store.Move(SPAWN_OCCUPANCY_VEHICLE, car.handle, car.x, car.y);
		g_store.Move(SPAWN_OCCUPANCY_PED, car.driverHandle, car.x, car.y);
	}
	for (i = 0; i < MAX_PEDS; i++) {
		SPedState &ped = g_peds[i];
		float dx, dy, dist;

		if (!ped.live)
			continue;
		ped.x += ped.dirX * (1.5f / FPS);
		ped.y += ped.dirY * (1.5f / FPS);
		g_store.Move(SPAWN_OCCUPANCY_PED, ped.handle, ped.x, ped.y);
		dx = ped.x - g_playerX;
		dy = ped.y - g_playerY;
		dist = sqrtf(dx * dx + dy * dy);
		if (g_nowMs - ped.bornMs > NEAR_BUBBLE_GRACE_MS && dist > PED_RADIUS)
			RemovePedRecord(i);
	}
}

/* The population's own trickle (the ped track's model): the ring band around
 * the player, a group of peds a call, six calls a frame. The director owns the
 * ped targets; the placement stays the population's until its migration. */
static void
PedTrickle(void)
{
	int i;

	for (i = 0; i < PED_RATE_PER_FRAME; i++) {
		float angle = RngFloat(0.0f, 6.2831853f);
		float dist = RngFloat(PED_CREATE_MIN, PED_CREATE_MAX);
		int group = 1 + (int)(Rng() % 3);

		for (int g = 0; g < group; g++)
			AddPedRecord(g_playerX + cosf(angle) * dist + RngFloat(-8.0f, 8.0f),
				g_playerY + sinf(angle) * dist + RngFloat(-8.0f, 8.0f));
	}
}

/* One frame of a drive: the tick, the ambient calls, the crowd, the traffic
 * and the cull. `moving` false is the warm-up (the player stands at the
 * route's start while the street fills). */
static void
DriveFrame(bool useDirector, bool moving, int32_t *pLastPlayerCell, double *pRefreshSeconds)
{
	int32_t i;
	int32_t playerCell = SpawnDirectorCellOf(g_playerX, g_playerY);
	double t0 = NowSeconds(), refreshTime = 0.0;

	g_nowMs += FRAME_MS;
	/* The refresh: the player's cell moved, the cadence passed, or the first
	 * frame. */
	if (playerCell != *pLastPlayerCell || (g_nowMs % SPAWN_DIRECTOR_REFRESH_MS) < FRAME_MS) {
		*pLastPlayerCell = playerCell;
		RefreshDirector();
		refreshTime = NowSeconds() - t0;
	}
	*pRefreshSeconds += refreshTime;
	TickDirector();
	/* The frame's ambient calls: the director's cells or the old ring. The
	 * service's function gate (the car target, the car budget) holds both,
	 * exactly as CCarCtrl::GenerateRandomCars' own stop line does. */
	if (g_numCars >= DriveCarTarget()) {
		g_budgetSkips += RATE_PER_FRAME;
	} else if (useDirector) {
		for (i = 0; i < RATE_PER_FRAME; i++)
			ServeDirectorRequest();
	} else {
		for (i = 0; i < RATE_PER_FRAME; i++)
			ServeRingRequest();
	}
	PedTrickle();
	MoveTraffic();
	CullCars();
	/* The player drives the route (+x at 20 m/s). */
	if (moving)
		g_playerX += PLAYER_SPEED / FPS;
}

/* One drive: the before or the after. The metrics are the director's own
 * per-cell minima, read from the state after the run. The trace starts from a
 * filled street (the warm-up at the route's start: the boot fill's job in the
 * live game, the covered fill of VICE-158), so the cold start's fill is not
 * counted against the maintenance; the director's state and the counters
 * restart with the trace. */
static void
RunDrive(bool useDirector)
{
	int frame;
	int32_t lastPlayerCell = -1;

	g_useDirector = useDirector;
	g_playerX = -2100.0f;
	g_playerY = -1400.0f;
	g_numCars = 0;
	g_numPeds = 0;
	g_nextCarHandle = 0;
	g_nextPedHandle = 0;
	g_nowMs = 1000;
	g_directorRequests = 0;
	g_directorEmpty = 0;
	g_directorReleases = 0;
	g_planPlacements = 0;
	g_planZero = 0;
	g_attempts = 0;
	g_speedRefusals = 0;
	g_budgetSkips = 0;
	g_waterRequests = 0;
	g_waterServed = 0;
	g_createdByKind[0] = 0;
	g_createdByKind[1] = 0;
	memset(g_refusals, 0, sizeof(g_refusals));
	memset(g_cars, 0, sizeof(g_cars));
	memset(g_peds, 0, sizeof(g_peds));
	g_store.Reset();
	SpawnDirectorReset(&g_director);
	SpawnDirectorCollectSiteFacts(g_index, g_director.aFacts);
	SpawnDirectorTelemetryReset(&g_directorTel);
	SpawnTelemetryReset(&g_serviceTel);
	g_tickSeconds = 0.0;
	g_refreshSeconds = 0.0;
	g_refreshes = 0;

	/* The warm-up: the street fills at the route's start before the trace. */
	for (frame = 0; frame < WARMUP_FRAMES; frame++)
		DriveFrame(useDirector, false, &lastPlayerCell, &g_refreshSeconds);

	/* The trace's own state: the metric (the minima and the clocks) and the
	 * counters restart; the world (the store, the cars) stays. */
	SpawnDirectorReset(&g_director);
	SpawnDirectorCollectSiteFacts(g_index, g_director.aFacts);
	SpawnDirectorTelemetryReset(&g_directorTel);
	SpawnTelemetryReset(&g_serviceTel);
	g_directorRequests = 0;
	g_directorEmpty = 0;
	g_directorReleases = 0;
	g_planPlacements = 0;
	g_planZero = 0;
	g_attempts = 0;
	g_speedRefusals = 0;
	g_budgetSkips = 0;
	g_waterRequests = 0;
	g_waterServed = 0;
	g_createdByKind[0] = 0;
	g_createdByKind[1] = 0;
	lastPlayerCell = -1;
	g_tickSeconds = 0.0;
	g_refreshSeconds = 0.0;
	g_refreshes = 0;

	for (frame = 0; frame < DRIVE_FRAMES; frame++)
		DriveFrame(useDirector, true, &lastPlayerCell, &g_refreshSeconds);
	/* The drive's own metric row: the per-cell minimum of the active street
	 * cells, read before the active set moves again. */
	g_trackCarCells = g_director.numActive[SPAWN_DIRECTOR_TRACK_CAR];
	g_trackPedCells = g_director.numActive[SPAWN_DIRECTOR_TRACK_PED];
	SpawnDirectorNoteRefresh(&g_directorTel, &g_director);
	SpawnDirectorNoteTick(&g_directorTel, &g_director);
}

/* The distribution of a track's per-cell minima over the active cells: the
 * minimum, the median and the maximum of the minima. */
static void
MinDistribution(int32_t track, int *outMin, int *outMedian, int *outMax, int *outCells, int *outZeroCells)
{
	int values[SPAWN_DIRECTOR_MAX_ACTIVE];
	int count = 0, i, j;

	for (i = 0; i < g_director.numActive[track]; i++) {
		int32_t cell = g_director.aActive[track][i];
		const SSpawnDirectorTrackState &state = g_director.aState[cell].track[track];

		if (!state.minSeen)
			continue;
		if (track == SPAWN_DIRECTOR_TRACK_CAR && !g_director.aState[cell].street && !g_director.aState[cell].water)
			continue;
		values[count++] = state.minCount;
	}
	*outCells = count;
	for (i = 1; i < count; i++) {
		int v = values[i];

		for (j = i; j > 0 && values[j - 1] > v; j--)
			values[j] = values[j - 1];
		values[j] = v;
	}
	*outMin = count ? values[0] : 0;
	*outMax = count ? values[count - 1] : 0;
	*outMedian = count ? values[count / 2] : 0;
	*outZeroCells = 0;
	for (i = 0; i < count; i++)
		if (values[i] == 0)
			(*outZeroCells)++;
}

/* ------------------------------------------------------------------ *
 * the checks
 * ------------------------------------------------------------------ */

static void
CheckTargetMath(void)
{
	/* The engine's own formula, mirrored: the stock sliders and the owner's. */
	float stock = RandomCarTarget(1.0f, 2.0f, 1.0f);
	float owner = RandomCarTarget(1.0f, 3.4f, 2.0f);

	CheckInt((int)stock, 60, "the stock car target is 60");
	CheckInt((int)owner, 408, "the owner's car target is 408");
	Check(owner <= (float)MAX_RANDOM_CARS, "the car target is capped by MAX_RANDOM_CARS");

	/* A degenerate case: the sum of the ceilings below the total leaves the
	 * sum short (the breach row), never a sum above the total. */
	{
		float weights[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		uint16_t ceilings[4] = { 1, 1, 1, 1 };
		uint16_t targets[4];
		int32_t sum = SpawnDirectorApportion(weights, ceilings, 4, 10, targets);

		CheckInt(sum, 4, "a total above the ceilings leaves the sum short, never above");
	}
	{
		float weights[4] = { 3.0f, 2.0f, 1.0f, 1.0f };
		uint16_t ceilings[4] = { 100, 100, 100, 100 };
		uint16_t targets[4];
		int32_t sum = SpawnDirectorApportion(weights, ceilings, 4, 100, targets);

		CheckInt(sum, 100, "the largest-remainder apportionment sums to the total");
	}
	{
		float weights[3] = { 0.0f, 1.0f, 1.0f };
		uint16_t ceilings[3] = { 100, 100, 100 };
		uint16_t targets[3];
		int32_t sum = SpawnDirectorApportion(weights, ceilings, 3, 7, targets);

		CheckInt(sum, 7, "a zero weight carries no target and the sum still lands");
		CheckInt(targets[0], 0, "the zero-weight cell carries no target");
	}
}

static void
CheckGrid(void)
{
	int32_t cell;

	Check(SpawnDirectorCellAt(-2400.0f, -2000.0f, &cell), "the grid's corner is a cell");
	CheckInt(cell, 0, "the grid's first cell is 0");
	Check(SpawnDirectorCellAt(1599.0f, 1999.0f, &cell), "the map's own corner is inside the grid");
	Check(!SpawnDirectorCellAt(2000.0f, 0.0f, &cell), "a position past the map is off the grid");
	Check(!SpawnDirectorCellAt(0.0f, -3000.0f, &cell), "a position off the map's south is off the grid");
	CheckInt(SpawnDirectorCellOf(-2400.0f + 64.0f, -2000.0f + 64.0f), 0, "the first cell's centre maps to 0");
}

static void
CheckLedger(void)
{
	SSpawnServiceTelemetry tel;
	SSpawnDirectorTelemetry dtel;

	SpawnTelemetryReset(&tel);
	SpawnNoteCreated(&tel, SPAWN_PURPOSE_FAR);
	SpawnNoteCreated(&tel, SPAWN_PURPOSE_FAR);
	SpawnNoteRemoval(&tel, SPAWN_PURPOSE_FAR);
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_FAR], 2, "the far creations land in the far row");
	CheckInt((int)tel.m_nRemovedByPurpose[SPAWN_PURPOSE_FAR], 1, "the far removals land in the far row");
	CheckInt((int)tel.m_nCreatedByPurpose[SPAWN_PURPOSE_AMBIENT], 0, "the far row does not touch the ambient row");

	SpawnDirectorTelemetryReset(&dtel);
	CheckInt((int)dtel.m_nCarBreaches, 0, "the director's telemetry starts clean");
}

static void
CheckModes(void)
{
	/* The interior: the car target is zero (no road planning) and the ped
	 * target is the flat interior budget; the off-map: no active set. */
	SSpawnDirector d;

	SpawnDirectorReset(&d);
	SpawnDirectorCollectSiteFacts(g_index, d.aFacts);
	SpawnDirectorRefresh(&d, 1000, g_playerX, g_playerY, ROAD_DENSITY,
		CAR_DESPAWN, PED_RADIUS, 0, 60, SPAWN_DIRECTOR_MODE_INTERIOR, NULL, NULL);
	CheckInt(d.targetSum[SPAWN_DIRECTOR_TRACK_CAR], 0, "the interior mode plans no road traffic");
	CheckInt(d.targetSum[SPAWN_DIRECTOR_TRACK_PED], 60, "the interior mode spreads the flat ped budget");
	{
		float x, y;
		uint8_t water;

		Check(!SpawnDirectorNextRequest(&d, g_playerX, g_playerY, 1.0f, 0.0f, &x, &y, &water),
			"the interior mode hands out no request");
	}

	SpawnDirectorReset(&d);
	SpawnDirectorCollectSiteFacts(g_index, d.aFacts);
	SpawnDirectorRefresh(&d, 1000, 5000.0f, 5000.0f, ROAD_DENSITY,
		CAR_DESPAWN, PED_RADIUS, 100, 100, SPAWN_DIRECTOR_MODE_OFF_MAP, NULL, NULL);
	CheckInt(d.numActive[SPAWN_DIRECTOR_TRACK_CAR], 0, "the off-map mode has no active set");
	CheckInt(d.targetSum[SPAWN_DIRECTOR_TRACK_CAR], 0, "the off-map mode has no target");
	{
		float x, y;
		uint8_t water;

		Check(!SpawnDirectorNextRequest(&d, 5000.0f, 5000.0f, 1.0f, 0.0f, &x, &y, &water),
			"the off-map mode hands out no request");
	}
}

int
main(void)
{
	int carMin, carMedian, carMax, carCells, carZero;
	int pedMin, pedMedian, pedMax, pedCells, pedZero;
	int beforeMin, beforeMedian, beforeMax, beforeCells, beforeZero;
	int32_t i;
	SSpawnSiteBuildStats stats;

	printf("The population director's evidence (VICE-152)\n\n");

	/* A. The city and the site facts. */
	printf("A. The city and the site facts\n");
	BuildCity();
	g_index.Build(CityGraph());
	stats = g_index.GetStats();
	printf("   the index: %d sites of %d connections (%d boat), the hash %s with %d cells\n",
		stats.sites, stats.connections, stats.boatSites,
		g_index.IsHashValid() ? "built" : "plain", stats.hashCells);
	CheckInt(stats.sites, stats.connections - stats.droppedNoLanes - stats.droppedMixed - stats.droppedUnresolved,
		"every usable connection is a site");
	{
		int streets = 0, waters = 0;
		int32_t lanesLand = 0, lanesWater = 0;
		SSpawnDirectorCellFacts facts[SPAWN_DIRECTOR_CELLS];

		memset(facts, 0, sizeof(facts));
		SpawnDirectorCollectSiteFacts(g_index, facts);
		for (i = 0; i < SPAWN_DIRECTOR_CELLS; i++) {
			if (facts[i].street)
				streets++;
			if (facts[i].water)
				waters++;
			lanesLand += facts[i].lanesLand;
			lanesWater += facts[i].lanesWater;
		}
		printf("   the cells: %d with streets, %d with water lanes; %d land lanes, %d water lanes\n",
			streets, waters, lanesLand, lanesWater);
		Check(streets > 100, "the city fills the grid with streets");
		Check(waters > 0, "the bay's cells carry water lanes");
	}

	/* B. The target math. */
	printf("\nB. The target math\n");
	CheckTargetMath();
	CheckGrid();
	printf("   the stock car target 60, the owner's 408; the apportionment lands exactly\n");

	/* The drive, before and after. */
	printf("\nC. The after drive (the director's maintenance)\n");
	RunDrive(true);
	MinDistribution(SPAWN_DIRECTOR_TRACK_CAR, &carMin, &carMedian, &carMax, &carCells, &carZero);
	printf("   the drive: %d frames, %d refreshes, %d active car cells, %d active ped cells\n",
		DRIVE_FRAMES, g_refreshes, g_trackCarCells, g_trackPedCells);
	printf("   the car target %d (sum %d), the ped target %d (sum %d)\n",
		g_director.target[SPAWN_DIRECTOR_TRACK_CAR], g_director.targetSum[SPAWN_DIRECTOR_TRACK_CAR],
		g_director.target[SPAWN_DIRECTOR_TRACK_PED], g_director.targetSum[SPAWN_DIRECTOR_TRACK_PED]);
	printf("   the car track: requests %d (none left %d), releases %d, land creations %d, water creations %d\n",
		g_directorRequests, g_directorEmpty, g_directorReleases, g_createdByKind[0], g_createdByKind[1]);
	printf("   the service: %d placements over %d plans, %d empty plans, %d attempts, %d refused by the speed test\n",
		g_planPlacements, g_directorRequests, g_planZero, g_attempts, g_speedRefusals);
	printf("   the per-cell minimum over the drive: min %d, median %d, max %d over %d street cells (%d dry)\n",
		carMin, carMedian, carMax, carCells, carZero);
	printf("   the car cells below their floor now %d, the defects since the reset %d, the worst minimum %d\n",
		SpawnDirectorStarvedCells(&g_director, SPAWN_DIRECTOR_TRACK_CAR),
		SpawnDirectorDefects(&g_director, SPAWN_DIRECTOR_TRACK_CAR),
		SpawnDirectorWorstMin(&g_director, SPAWN_DIRECTOR_TRACK_CAR));
	{
		int32_t belowFloor = SpawnDirectorStarvedCells(&g_director, SPAWN_DIRECTOR_TRACK_CAR);
		int32_t defects = SpawnDirectorDefects(&g_director, SPAWN_DIRECTOR_TRACK_CAR);

		CheckInt(defects, 0, "no car cell stayed below its floor past the fill bound (the acceptance)");
		Check(belowFloor <= 4, "the after drive ends with at most the last frames' lag of starved cells");
		printf("   the dry minima are the cells that entered the active set empty (the director creates");
		printf(" only inside it); the gate is the duration, the fill bound\n");
	}
	{
		/* The cells that starved the most: the per-cell defect events. */
		int32_t cell;

		printf("   the defect cells:");
		for (cell = 0; cell < SPAWN_DIRECTOR_CELLS; cell++) {
			if (g_director.aState[cell].track[SPAWN_DIRECTOR_TRACK_CAR].defectEvents > 0)
				printf(" (%.0f,%.0f) x%d", SpawnDirectorCellCentreX(cell), SpawnDirectorCellCentreY(cell),
					g_director.aState[cell].track[SPAWN_DIRECTOR_TRACK_CAR].defectEvents);
		}
		printf("\n");
	}
	{
		int32_t waterTargets = 0;

		for (i = 0; i < g_director.numActive[SPAWN_DIRECTOR_TRACK_CAR]; i++) {
			int32_t cell = g_director.aActive[SPAWN_DIRECTOR_TRACK_CAR][i];

			if (g_director.aState[cell].water)
				waterTargets += g_director.aState[cell].track[SPAWN_DIRECTOR_TRACK_CAR].target;
		}
		{
			int32_t cell, waterCells = 0, waterEmpty = 0;

			for (cell = 0; cell < SPAWN_DIRECTOR_CELLS; cell++) {
				if (!g_director.aState[cell].water ||
				    !(g_director.aState[cell].active & (1 << SPAWN_DIRECTOR_TRACK_CAR)))
					continue;
				waterCells++;
				if (g_director.aState[cell].track[SPAWN_DIRECTOR_TRACK_CAR].minCount == 0)
					waterEmpty++;
			}
			printf("   the water cells: %d in the last active set, %d of the car target, %d water requests, %d boats placed (%d dry)\n",
				waterCells, waterTargets, g_waterRequests, g_waterServed, waterEmpty);
		}
		Check(waterTargets > 0, "the water cells carry their own share of the car target");
		Check(g_createdByKind[1] > 0, "the water cells produced boats");
	}

	printf("\nD. The before drive (the old ring placement, the contrast)\n");
	RunDrive(false);
	MinDistribution(SPAWN_DIRECTOR_TRACK_CAR, &beforeMin, &beforeMedian, &beforeMax, &beforeCells, &beforeZero);
	printf("   the per-cell minimum over the drive: min %d, median %d, max %d over %d street cells (%d dry)\n",
		beforeMin, beforeMedian, beforeMax, beforeCells, beforeZero);
	printf("   the car cells below their floor now %d, the defects since the reset %d, the worst minimum %d\n",
		SpawnDirectorStarvedCells(&g_director, SPAWN_DIRECTOR_TRACK_CAR),
		SpawnDirectorDefects(&g_director, SPAWN_DIRECTOR_TRACK_CAR),
		SpawnDirectorWorstMin(&g_director, SPAWN_DIRECTOR_TRACK_CAR));
	Check(SpawnDirectorDefects(&g_director, SPAWN_DIRECTOR_TRACK_CAR) > 0,
		"the ring dice leaves the trace with defects (the before/after)");

	printf("\nE. The ped track (the targets, the metric)\n");
	MinDistribution(SPAWN_DIRECTOR_TRACK_PED, &pedMin, &pedMedian, &pedMax, &pedCells, &pedZero);
	printf("   the per-cell minimum over the drive: min %d, median %d, max %d over %d ped cells (%d dry)\n",
		pedMin, pedMedian, pedMax, pedCells, pedZero);
	printf("   the ped requests stay the population's own ring trickle until their migration;\n");
	printf("   the director owns the spread and the metric\n");
	Check(g_trackPedCells > 0, "the ped track has active cells");

	printf("\nF. The modes\n");
	CheckModes();
	printf("   the interior: no road planning, the flat ped budget; the off-map: no active set\n");

	printf("\nG. The ledger\n");
	CheckLedger();
	printf("   the far layer's class (SPAWN_PURPOSE_FAR) carries its own creations and removals\n");

	printf("\nH. The cost\n");
	printf("   the tick %.1f us a frame, the refresh %.1f us a call over %d refreshes\n",
		g_tickSeconds / DRIVE_FRAMES * 1e6, g_refreshSeconds / (g_refreshes ? g_refreshes : 1) * 1e6,
		g_refreshes);
	printf("   the refusals: no site %d, occupancy %d, visibility %d, zone %d, readiness %d, budget %d, pool %d, reach %d\n",
		g_refusals[SPAWN_REFUSAL_NO_SITE], g_refusals[SPAWN_REFUSAL_OCCUPANCY],
		g_refusals[SPAWN_REFUSAL_VISIBILITY], g_refusals[SPAWN_REFUSAL_ZONE],
		g_refusals[SPAWN_REFUSAL_READINESS], g_refusals[SPAWN_REFUSAL_BUDGET],
		g_refusals[SPAWN_REFUSAL_POOL], g_refusals[SPAWN_REFUSAL_REACHABILITY]);

	printf("\n%d checks, %d failures\n", (int)g_checks, g_failures);
	return g_failures ? 1 : 0;
}
