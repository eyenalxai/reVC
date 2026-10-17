/*
 * The evidence program for the spawn site index (VICE-148).
 *
 * It compiles the index's own engine-free translation unit
 * (src/control/SpawnSitesIndex.cpp - the real build, the real refresh, the
 * real hash query) and drives it over a synthetic car path graph of the
 * shipped node counts. No engine header is included anywhere.
 *
 *	A. The graph. The synthetic city: the shipped split of the car nodes
 *	   (2,973 road + 378 water = 3,351) over a sparse street network, with
 *	   the one-way streets, the cul-de-sacs, the disabled and between-levels
 *	   nodes, the harbour ramps (mixed land/water connections) and the
 *	   capacities' own bounds (the links and the connections below
 *	   NUM_CARPATHLINKS / NUM_PATHCONNECTIONS). The graph is synthetic on
 *	   purpose: the shipped graph is a dump of the running game
 *	   (tools/pathfind_check), and the live read is the acceptance; this
 *	   program proves the index's rules and its bounds, and the numbers it
 *	   cannot reach are named below.
 *
 *	B. The build. The total and the usable site counts, the drops (the
 *	   one-way directions with no lanes, the mixed pairs), the boat/road
 *	   split, the spatial hash's cells, and the build time.
 *
 *	C. The field spot-check. Every field of a deterministic sample of sites
 *	   is recomputed from the raw node/link/connection arrays by this
 *	   program's own reading of the primitive's rule
 *	   (CCarCtrl::CreateCarFromPlacement: the travelled side's lanes, the
 *	   travel direction, the rarity), and compared field by field. The
 *	   sample is forced to hold the first site of every class (boat, water,
 *	   small boats, dead end, disabled, between levels) and the first plain
 *	   one, so no rule is checked on plain sites alone.
 *
 *	D. The spatial hash. Every sampled site is found at its own position (a
 *	   point query), and the ring walk's result is compared against a plain
 *	   radius scan for a spread of query points and radii: the sets must
 *	   match element for element.
 *
 *	E. The flags refresh. A script road toggle of the engine's own shape
 *	   (SwitchOffNodeAndNeighbours' recursion, and the between-levels mark)
 *	   is simulated: the graph's own flag version moves, the index's
 *	   RefreshFlags re-derives the sites' flags, and the checks are the
 *	   ticket's: the build count stands (no rebuild), the sites' structural
 *	   fields and their order hash the same before and after, the flags
 *	   follow the nodes, and the refresh's own time is measured - the gate
 *	   with no toggle is one comparison, and the refresh itself is a
 *	   fraction of the build and of a frame (the "no frame spike" row).
 *
 *	F. The boat sweep, and the storage's worst case. Every mixed
 *	   land/water connection of the city is rejected (counted by this
 *	   program and by the index), and the index is built over a graph that
 *	   fills the capacities exactly (NUM_PATHCONNECTIONS directed
 *	   connections over 3,351 nodes and NUM_CARPATHLINKS link records) to
 *	   show the arrays hold it and what the worst-case build costs.
 *
 *	G. The hash fallback. A site outside the 4 km grid leaves the hash
 *	   unbuilt and the query keeps the plain scan - the node grid's own
 *	   contract (ADR 0024).
 *
 * What this program cannot reach, stated plainly: the engine side's own
 * snapshot (SpawnSites.cpp) is not linked, so the node flags and the
 * info-zone indices here are the synthetic graph's, laid out the way the
 * engine side lays them out (the flags from the node bits, the zone index
 * from the connection's midpoint); the shipped graph itself needs a running
 * game; and the loader (CGame::Initialise) is the game's, not this program's.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 \
 *	    -o /tmp/spawn-service utils/spawn-service/harness.cpp \
 *	    src/control/SpawnSitesIndex.cpp
 *	/tmp/spawn-service
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "SpawnSites.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures;

static void
Fail(const char *what)
{
	g_failures++;
	printf("  FAIL: %s\n", what);
}

static void
Check(bool ok, const char *what)
{
	if (!ok)
		Fail(what);
}

static volatile int g_sink;

static double
NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ *
 * the synthetic city
 * ------------------------------------------------------------------ */

/*
 * The shipped split: 3,351 car nodes = 2,973 road + 378 water (ADR 0024's
 * live census). The city is a sparse network: 49 streets of 60 road nodes
 * plus a 33 node tail, a serpentine that links the streets, a few avenues
 * that close cycles, and cul-de-sacs where a street's end is left free. The
 * link capacity (NUM_CARPATHLINKS, 3,500) cannot describe a full grid of
 * 2,973 nodes either; the network's counts are printed below and checked
 * against the capacities.
 *
 * The water is a separate bay of 378 nodes in the south-east, its own
 * channel network, and a handful of harbour ramps connect the two: the mixed
 * land/water pairs the index must reject.
 */
#define ROAD_FULL_ROWS 49		/* 49 x 60 = 2,940 road nodes */
#define ROAD_ROW_NODES 60
#define ROAD_TAIL_NODES 33		/* + 33: 2,973 road nodes */
#define NUM_ROAD_NODES (ROAD_FULL_ROWS * ROAD_ROW_NODES + ROAD_TAIL_NODES)
#define WATER_COLS 21
#define WATER_ROWS 18			/* 378 water nodes */
#define NUM_WATER_NODES (WATER_COLS * WATER_ROWS)
#define NUM_CITY_NODES (NUM_ROAD_NODES + NUM_WATER_NODES)	/* 3,351 */

#define ROAD_ORIGIN_X 200.0f
#define ROAD_ORIGIN_Y 200.0f
#define WATER_ORIGIN_X 2200.0f
#define WATER_ORIGIN_Y 2600.0f
#define CITY_SPACING 32.0f
#define CITY_NODE_HEIGHT 10.0f

#define MAX_EDGES 11000

static SSpawnSiteNode g_nodes[NUM_PATHNODES];
static SSpawnSiteLink g_links[NUM_CARPATHLINKS];
static uint16_t g_connectionNode[NUM_PATHCONNECTIONS];
static int16_t g_connectionLink[NUM_PATHCONNECTIONS];
static int16_t g_connectionZone[NUM_PATHCONNECTIONS];

static int g_numNodes;
static int g_numLinks;
static int g_numConnections;

/* One road segment before the graph is laid out: `oneWay` is 0 two-way, 1 the
 * a -> b direction only, 2 the b -> a direction only. */
struct SEdge
{
	int a, b;
	int oneWay;
};
static SEdge g_edges[MAX_EDGES];
static int g_numEdges;

static void
AddEdge(int a, int b, int oneWay)
{
	if (g_numEdges >= MAX_EDGES) {
		printf("the edge list is full\n");
		exit(1);
	}
	g_edges[g_numEdges].a = a;
	g_edges[g_numEdges].b = b;
	g_edges[g_numEdges].oneWay = oneWay;
	g_numEdges++;
}

/* The row a street connector skips: its two ends become cul-de-sacs. */
static bool
CulDeSacRow(int row)
{
	return row % 7 == 5;
}

/* The one-way mix of a segment: one in five is a one-way street, the
 * direction alternating. */
static int
EdgeOneWay(int edge)
{
	int k = edge % 10;
	if (k == 3)
		return 1;
	if (k == 7)
		return 2;
	return 0;
}

static inline int
RoadNode(int row, int col)
{
	return row * ROAD_ROW_NODES + col;
}

static inline int
WaterNode(int row, int col)
{
	return NUM_ROAD_NODES + row * WATER_COLS + col;
}

/* The synthetic information zones (the engine side's own rule is
 * CTheZones::FindInformationZoneForPosition, which the index takes as an
 * index): four quadrants around (1000, 1000) and the bay as the default
 * zone 0. A segment that straddles a quadrant's edge therefore has its
 * endpoint nodes in one zone and the connection's midpoint (the site's own
 * position) in another - the rule the ticket asks for. */
static int
ZoneAt(float x, float y)
{
	if (x > 2100.0f)
		return 0;		/* the bay (the default zone) */
	if (x < 1000.0f)
		return y < 1000.0f ? 1 : 2;
	return y < 1000.0f ? 3 : 4;
}

/* The city's nodes, from the shipped counts. */
static void
BuildCityNodes(void)
{
	int i;

	memset(g_nodes, 0, sizeof(g_nodes));

	for (i = 0; i < NUM_ROAD_NODES; i++) {
		int row = i / ROAD_ROW_NODES;
		int col = i % ROAD_ROW_NODES;
		g_nodes[i].x = ROAD_ORIGIN_X + col * CITY_SPACING;
		g_nodes[i].y = ROAD_ORIGIN_Y + row * CITY_SPACING;
		g_nodes[i].z = CITY_NODE_HEIGHT + (float)(i % 7);
		g_nodes[i].spawnRate = 3 + (i * 7) % 13;
		if (i % 97 == 3)
			g_nodes[i].flags |= SPAWN_SITE_NODE_DISABLED;
		if (i % 211 == 5)
			g_nodes[i].flags |= SPAWN_SITE_NODE_BETWEENLEVELS;
	}
	for (i = 0; i < NUM_WATER_NODES; i++) {
		int row = i / WATER_COLS;
		int col = i % WATER_COLS;
		int node = NUM_ROAD_NODES + i;
		g_nodes[node].x = WATER_ORIGIN_X + col * CITY_SPACING;
		g_nodes[node].y = WATER_ORIGIN_Y + row * CITY_SPACING;
		g_nodes[node].z = 0.0f;
		g_nodes[node].spawnRate = 3 + (i * 5) % 13;
		g_nodes[node].flags |= SPAWN_SITE_NODE_WATER;
		if (i % 40 == 0)
			g_nodes[node].flags |= SPAWN_SITE_NODE_SMALLBOATS;
	}
	g_numNodes = NUM_CITY_NODES;
}

/* The city's segments: the streets, the serpentine, the avenues, the bay's
 * channels and the harbour ramps. */
static void
BuildCityEdges(void)
{
	int r, c, edge;

	g_numEdges = 0;

	/* The streets (each street a chain) and the tail. */
	for (r = 0; r < ROAD_FULL_ROWS; r++)
		for (c = 0; c + 1 < ROAD_ROW_NODES; c++)
			AddEdge(RoadNode(r, c), RoadNode(r, c + 1), EdgeOneWay(g_numEdges));
	for (c = 0; c + 1 < ROAD_TAIL_NODES; c++)
		AddEdge(RoadNode(ROAD_FULL_ROWS, c), RoadNode(ROAD_FULL_ROWS, c + 1), EdgeOneWay(g_numEdges));

	/* The serpentine: a street's end links to the next street's start, and
	 * the tail hangs off the last full street. The skipped rows leave the
	 * two ends as cul-de-sacs (a node with one link, the graph's own dead
	 * ends). */
	for (r = 0; r + 1 < ROAD_FULL_ROWS; r++)
		if (!CulDeSacRow(r) && !CulDeSacRow(r + 1))
			AddEdge(RoadNode(r, ROAD_ROW_NODES - 1), RoadNode(r + 1, 0), EdgeOneWay(g_numEdges));
	AddEdge(RoadNode(ROAD_FULL_ROWS - 1, ROAD_ROW_NODES - 1), RoadNode(ROAD_FULL_ROWS, 0), EdgeOneWay(g_numEdges));

	/* The avenues: every other street start links two rows on, closing
	 * cycles (the real graph is no forest). The cul-de-sac rows are left
	 * out, so their ends stay dead. */
	for (r = 0; r + 2 < ROAD_FULL_ROWS; r += 2)
		if (!CulDeSacRow(r) && !CulDeSacRow(r + 2))
			AddEdge(RoadNode(r, 0), RoadNode(r + 2, 0), EdgeOneWay(g_numEdges));

	/* The bay's channels: the rows, the row connectors and two loops. */
	for (r = 0; r < WATER_ROWS; r++)
		for (c = 0; c + 1 < WATER_COLS; c++)
			AddEdge(WaterNode(r, c), WaterNode(r, c + 1), EdgeOneWay(g_numEdges));
	for (r = 0; r + 1 < WATER_ROWS; r++)
		AddEdge(WaterNode(r, WATER_COLS - 1), WaterNode(r + 1, 0), EdgeOneWay(g_numEdges));
	for (r = 0; r + 2 < WATER_ROWS; r += 3)
		AddEdge(WaterNode(r, 0), WaterNode(r + 2, 0), EdgeOneWay(g_numEdges));

	/* The harbour ramps: the mixed land/water pairs the index must reject
	 * (a tail street end to the nearest bay nodes). */
	for (edge = 0; edge < 8; edge++)
		AddEdge(RoadNode(ROAD_FULL_ROWS, ROAD_TAIL_NODES - 1 - edge), WaterNode(0, edge), EdgeOneWay(g_numEdges));
}

/* The lanes of a segment: the two-way roads carry two lanes each way (a side
 * street one), a one-way street two lanes its own way and none against it, so
 * the index drops that direction. The water carries one lane each way. */
static void
EdgeLanes(const SEdge &edge, int8_t *pLeft, int8_t *pRight)
{
	int k = edge.oneWay;
	if (edge.a >= NUM_ROAD_NODES || edge.b >= NUM_ROAD_NODES) {
		*pLeft = k == 2 ? 0 : 1;
		*pRight = k == 1 ? 0 : 1;
		return;
	}
	*pLeft = k == 1 ? 0 : 2;
	*pRight = k == 2 ? 0 : 2;
}

/* The graph's links and connections, out of the edge list: one link record
 * per segment (both directions share it, as the graph shares it), the link's
 * own node the smaller of the pair (the graph's own rule: the direction runs
 * from the higher index to the lower), and the connections laid out per node
 * in node order. */
static void
LayGraph(void)
{
	static int cursor[NUM_PATHNODES];
	int i, conn;

	if (g_numEdges > NUM_CARPATHLINKS) {
		printf("the synthetic graph overflows the link capacity\n");
		exit(1);
	}
	if (g_numEdges * 2 > NUM_PATHCONNECTIONS) {
		printf("the synthetic graph overflows the connection capacity\n");
		exit(1);
	}

	for (i = 0; i < g_numNodes; i++) {
		g_nodes[i].firstLink = 0;
		g_nodes[i].numLinks = 0;
	}
	for (i = 0; i < g_numEdges; i++) {
		g_nodes[g_edges[i].a].numLinks++;
		g_nodes[g_edges[i].b].numLinks++;
	}
	conn = 0;
	for (i = 0; i < g_numNodes; i++) {
		g_nodes[i].firstLink = (int16_t)conn;
		cursor[i] = conn;
		conn += g_nodes[i].numLinks;
	}
	g_numConnections = conn;

	/* The connections: every segment owns both directions (a one-way
	 * street's wrong direction is a connection with no lanes - the graph
	 * stores it, the index drops it). */
	for (i = 0; i < g_numEdges; i++) {
		const SEdge &e = g_edges[i];
		g_connectionNode[cursor[e.a]] = (uint16_t)e.b;
		g_connectionLink[cursor[e.a]] = (int16_t)i;
		cursor[e.a]++;
		g_connectionNode[cursor[e.b]] = (uint16_t)e.a;
		g_connectionLink[cursor[e.b]] = (int16_t)i;
		cursor[e.b]++;
	}

	/* The link records. */
	for (i = 0; i < g_numEdges; i++) {
		const SEdge &e = g_edges[i];
		int low = e.a < e.b ? e.a : e.b;
		int high = e.a < e.b ? e.b : e.a;
		float dx = g_nodes[low].x - g_nodes[high].x;
		float dy = g_nodes[low].y - g_nodes[high].y;
		float mag = (float)sqrt(dx * dx + dy * dy);
		int8_t left, right;

		EdgeLanes(e, &left, &right);
		/* The link record's own position: the graph's external links carry
		 * the synthetic midpoint of the merged external nodes, which is not
		 * the node pair's midpoint - the sites must read the node pair's own
		 * (SpawnSites.h). The offset below stands for that. */
		g_links[i].x = (g_nodes[e.a].x + g_nodes[e.b].x) * 0.5f + 7.0f;
		g_links[i].y = (g_nodes[e.a].y + g_nodes[e.b].y) * 0.5f - 5.0f;
		g_links[i].dirX = mag > 0.0f ? (int8_t)(dx / mag * 100.0f) : 0;
		g_links[i].dirY = mag > 0.0f ? (int8_t)(dy / mag * 100.0f) : 0;
		g_links[i].numLeftLanes = left;
		g_links[i].numRightLanes = right;
		g_links[i].pathNodeIndex = (int16_t)low;
		g_links[i].width = (uint8_t)(40 * (left + right));
	}
	g_numLinks = g_numEdges;

	/* The info-zone indices, the engine side's own pass: every connection's
	 * midpoint's zone, walked in node order (the site stores the index, never
	 * the info). */
	for (i = 0; i < g_numNodes; i++) {
		int j;
		for (j = 0; j < g_nodes[i].numLinks; j++) {
			int c = g_nodes[i].firstLink + j;
			int other = g_connectionNode[c];
			float mx = (g_nodes[i].x + g_nodes[other].x) * 0.5f;
			float my = (g_nodes[i].y + g_nodes[other].y) * 0.5f;
			g_connectionZone[c] = (int16_t)ZoneAt(mx, my);
		}
	}
}

/* The dead ends, the graph's own rule (PathFind.cpp PreparePathDataForType): a
 * node with fewer than two neighbours that are not dead ends is a dead end,
 * iterated to the fixed point. The rule is the graph's, not the index's; the
 * sites read the flag as it stands. */
static void
MarkDeadEnds(void)
{
	int cont, i, j;

	do {
		cont = 0;
		for (i = 0; i < g_numNodes; i++) {
			int alive = 0;
			if (g_nodes[i].flags & SPAWN_SITE_NODE_DEADEND)
				continue;
			for (j = 0; j < g_nodes[i].numLinks; j++)
				if (!(g_nodes[g_connectionNode[g_nodes[i].firstLink + j]].flags & SPAWN_SITE_NODE_DEADEND))
					alive++;
			if (alive < 2) {
				g_nodes[i].flags |= SPAWN_SITE_NODE_DEADEND;
				cont = 1;
			}
		}
	} while (cont);
}

/* ------------------------------------------------------------------ *
 * the checklist: this program's own reading of the raw structures
 * ------------------------------------------------------------------ */

/* The primitive's own field rules (CarCtrl.cpp CreateCarFromPlacement) and the
 * ticket's field list, read from the raw arrays. Returns false when the pair
 * is not a site (why names the drop). */
static bool
ReferenceSite(int node1, int conn, SSpawnSite *out, const char **why)
{
	int node2 = g_connectionNode[conn];
	int link = g_connectionLink[conn];
	const SSpawnSiteNode *n1, *n2;
	const SSpawnSiteLink *l;
	int lanes;
	int8_t sign;

	*why = "";
	if (node2 < 0 || node2 >= g_numNodes) {
		*why = "unresolved";
		return false;
	}
	if (link < 0 || link >= g_numLinks) {
		*why = "unresolved";
		return false;
	}
	n1 = &g_nodes[node1];
	n2 = &g_nodes[node2];
	l = &g_links[link];
	if ((n1->flags ^ n2->flags) & SPAWN_SITE_NODE_WATER) {
		*why = "mixed";
		return false;
	}
	lanes = (l->pathNodeIndex == node2) ? l->numLeftLanes : l->numRightLanes;
	if (lanes <= 0) {
		*why = "no lanes";
		return false;
	}

	memset(out, 0, sizeof(*out));
	out->x = (n1->x + n2->x) * 0.5f;
	out->y = (n1->y + n2->y) * 0.5f;
	out->z = (n1->z + n2->z) * 0.5f;
	out->connection = (uint16_t)conn;
	out->node1 = (int16_t)node1;
	out->node2 = (int16_t)node2;
	out->link = (int16_t)link;
	out->zoneIndex = g_connectionZone[conn];
	sign = (node1 >= node2) ? 1 : -1;
	out->dirX = (int8_t)(sign * l->dirX);
	out->dirY = (int8_t)(sign * l->dirY);
	out->lanes = (uint8_t)lanes;
	out->width = l->width;
	out->spawnRate = n1->spawnRate < n2->spawnRate ? n1->spawnRate : n2->spawnRate;
	if (n1->flags & SPAWN_SITE_NODE_WATER)
		out->flags |= SPAWN_SITE_WATER1;
	if (n2->flags & SPAWN_SITE_NODE_WATER)
		out->flags |= SPAWN_SITE_WATER2;
	if ((n1->flags & n2->flags) & SPAWN_SITE_NODE_WATER)
		out->flags |= SPAWN_SITE_BOAT;
	if (n1->flags & SPAWN_SITE_NODE_SMALLBOATS)
		out->flags |= SPAWN_SITE_SMALLBOATS1;
	if (n2->flags & SPAWN_SITE_NODE_SMALLBOATS)
		out->flags |= SPAWN_SITE_SMALLBOATS2;
	if ((n1->flags | n2->flags) & SPAWN_SITE_NODE_DEADEND)
		out->flags |= SPAWN_SITE_DEADEND;
	if ((n1->flags | n2->flags) & SPAWN_SITE_NODE_DISABLED)
		out->flags |= SPAWN_SITE_DISABLED;
	if ((n1->flags | n2->flags) & SPAWN_SITE_NODE_BETWEENLEVELS)
		out->flags |= SPAWN_SITE_BETWEENLEVELS;
	return true;
}

static void
PrintSiteDiff(const SSpawnSite &got, const SSpawnSite &want, int conn)
{
	printf("    connection %d:\n", conn);
	printf("      stored: x %.6f y %.6f z %.6f conn %d node %d->%d link %d zone %d dir %d,%d lanes %d width %d rate %d flags %02x\n",
		(double)got.x, (double)got.y, (double)got.z, got.connection, got.node1, got.node2, got.link,
		got.zoneIndex, got.dirX, got.dirY, got.lanes, got.width, got.spawnRate, got.flags);
	printf("      raw:    x %.6f y %.6f z %.6f conn %d node %d->%d link %d zone %d dir %d,%d lanes %d width %d rate %d flags %02x\n",
		(double)want.x, (double)want.y, (double)want.z, want.connection, want.node1, want.node2, want.link,
		want.zoneIndex, want.dirX, want.dirY, want.lanes, want.width, want.spawnRate, want.flags);
}

/* The number of sites whose flags hold a bit. */
static int
CountFlag(const CSpawnSiteIndex &index, uint8_t bit)
{
	int i, n = 0;
	for (i = 0; i < index.GetNumSites(); i++)
		if (index.GetSites()[i].flags & bit)
			n++;
	return n;
}

/* ------------------------------------------------------------------ *
 * A. the graph
 * ------------------------------------------------------------------ */

/* The graph's own make-up, counted from the raw arrays. */
static void
Section1Graph(void)
{
	int i, j;
	int oneWayEdges = 0, waterEdges = 0, mixedEdges = 0;
	int disabledNodes = 0, betweenNodes = 0, waterNodes = 0, deadEndNodes = 0;

	for (i = 0; i < g_numEdges; i++) {
		if (g_edges[i].oneWay)
			oneWayEdges++;
		if (g_edges[i].a >= NUM_ROAD_NODES && g_edges[i].b >= NUM_ROAD_NODES)
			waterEdges++;
		if ((g_edges[i].a >= NUM_ROAD_NODES) != (g_edges[i].b >= NUM_ROAD_NODES))
			mixedEdges++;
	}
	for (i = 0; i < g_numNodes; i++) {
		if (g_nodes[i].flags & SPAWN_SITE_NODE_DISABLED)
			disabledNodes++;
		if (g_nodes[i].flags & SPAWN_SITE_NODE_BETWEENLEVELS)
			betweenNodes++;
		if (g_nodes[i].flags & SPAWN_SITE_NODE_WATER)
			waterNodes++;
		if (g_nodes[i].numLinks == 1)
			deadEndNodes++;
	}
	for (i = 0; i < g_numNodes; i++) {
		for (j = 0; j < g_nodes[i].numLinks; j++) {
			int c = g_nodes[i].firstLink + j;
			if (g_connectionNode[c] >= g_numNodes)
				Fail("a connection points outside the graph");
		}
	}

	printf("== 1. the graph (synthetic, the shipped node counts)\n\n");
	printf("  the car nodes: %d (the shipped 3,351 = 2,973 road + 378 water)\n", g_numNodes);
	printf("  the links: %d of the capacity %d, the connections: %d of the capacity %d\n",
		g_numLinks, NUM_CARPATHLINKS, g_numConnections, NUM_PATHCONNECTIONS);
	printf("  the make-up: %d one-way segments, %d water segments, %d harbour ramps (mixed),\n",
		oneWayEdges, waterEdges, mixedEdges);
	printf("  %d water nodes, %d disabled, %d between-levels, %d cul-de-sacs (one link)\n\n",
		waterNodes, disabledNodes, betweenNodes, deadEndNodes);
	Check(g_numLinks <= NUM_CARPATHLINKS, "the synthetic graph must fit the link capacity");
	Check(g_numConnections <= NUM_PATHCONNECTIONS, "the synthetic graph must fit the connection capacity");
	Check(oneWayEdges > 0 && waterEdges > 0 && mixedEdges > 0, "the graph must hold one-way, water and mixed segments");
	Check(disabledNodes > 0 && betweenNodes > 0 && deadEndNodes > 0, "the graph must hold disabled, between-levels and dead-end nodes");
}

/* ------------------------------------------------------------------ *
 * B. the build
 * ------------------------------------------------------------------ */

/* The graph view the index is built over: the shape SpawnSites.cpp fills from
 * ThePaths. */
static SSpawnSiteGraph
GraphView(void)
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

static double
Section2Build(CSpawnSiteIndex &index)
{
	const int rounds = 20;
	double best = 1e9, mean = 0.0;
	int i;
	const SSpawnSiteBuildStats *stats;

	printf("== 2. the build: the sites over the directed connections\n\n");

	for (i = 0; i < rounds; i++) {
		double t0 = NowSeconds();
		index.Build(GraphView());
		double t1 = NowSeconds();
		g_sink += index.GetNumSites();
		if (t1 - t0 < best)
			best = t1 - t0;
		mean += t1 - t0;
	}
	mean /= rounds;
	stats = &index.GetStats();

	printf("  the directed connections examined: %d\n", stats->connections);
	printf("  the usable sites: %d (%.1f %% of the connections)\n",
		stats->sites, stats->sites * 100.0 / stats->connections);
	printf("  dropped: %d with no lanes on the travelled side (a one-way's wrong direction),\n",
		stats->droppedNoLanes);
	printf("    %d mixed land/water pairs, %d unresolved\n",
		stats->droppedMixed, stats->droppedUnresolved);
	printf("  the boats: %d sites (%.1f %% of the sites), the road sites: %d\n",
		stats->boatSites, stats->boatSites * 100.0 / stats->sites,
		stats->sites - stats->boatSites);
	printf("  the flags: %d dead-end, %d disabled, %d between-levels, %d small-boat sites\n",
		CountFlag(index, SPAWN_SITE_DEADEND), CountFlag(index, SPAWN_SITE_DISABLED),
		CountFlag(index, SPAWN_SITE_BETWEENLEVELS),
		CountFlag(index, SPAWN_SITE_SMALLBOATS1) + CountFlag(index, SPAWN_SITE_SMALLBOATS2));
	printf("  the spatial hash: %s, %d of the %d cells hold sites\n",
		index.IsHashValid() ? "built" : "not built (the plain scan)",
		stats->hashCells, SPAWN_SITE_GRID_CELLS);
	printf("  the build time: %.2f ms (the best of %d runs; the mean %.2f ms)\n",
		best * 1000.0, rounds, mean * 1000.0);
	printf("    %.0f ns for each of the %d directed connections\n\n",
		best * 1e9 / stats->connections, stats->connections);

	Check(stats->connections == g_numConnections, "the build must examine every directed connection");
	Check(stats->sites + stats->droppedNoLanes + stats->droppedMixed + stats->droppedUnresolved == stats->connections,
		"every connection must be a site or one of the drops");
	Check(stats->sites > 0 && stats->sites <= NUM_PATHCONNECTIONS, "the sites must fit the array");
	Check(stats->boatSites > 0, "the water-water pairs must give boat sites");
	Check(stats->droppedNoLanes > 0, "the one-way directions must be dropped");
	Check(stats->droppedMixed > 0, "the harbour ramps must be dropped");
	Check(stats->droppedUnresolved == 0, "the graph must resolve every connection");
	Check(index.IsHashValid(), "the city's hash must be built");
	return best;
}

/* ------------------------------------------------------------------ *
 * C. the field spot-check
 * ------------------------------------------------------------------ */

static void
Section3SpotCheck(CSpawnSiteIndex &index)
{
	const SSpawnSite *sites = index.GetSites();
	const int numSites = index.GetNumSites();
	static int siteOfConnection[NUM_PATHCONNECTIONS];
	static bool sampled[NUM_PATHCONNECTIONS];
	const uint8_t classes[] = {
		SPAWN_SITE_BOAT, SPAWN_SITE_DISABLED, SPAWN_SITE_BETWEENLEVELS, SPAWN_SITE_DEADEND,
		SPAWN_SITE_SMALLBOATS1, SPAWN_SITE_SMALLBOATS2, SPAWN_SITE_WATER1, SPAWN_SITE_WATER2
	};
	const int nClasses = sizeof(classes) / sizeof(classes[0]);
	int gotSites = 0, gotNoLanes = 0, gotMixed = 0, gotUnresolved = 0;
	int mismatches = 0, zoneRule = 0, zoneMidpoint = 0, zoneEntryNode = 0;
	int linkPositionSites = 0;
	int sample = 0, stride;
	int i, j, node, conn;

	printf("== 3. the field spot-check against the raw structures\n\n");

	/* Every connection of the graph is classified by this program's own
	 * reading of the raw node/link/connection arrays, and the site array is
	 * mapped by the connection id: the site's key. */
	for (i = 0; i < NUM_PATHCONNECTIONS; i++)
		siteOfConnection[i] = -1;
	for (i = 0; i < numSites; i++) {
		conn = sites[i].connection;
		if (siteOfConnection[conn] != -1)
			Fail("two sites hold one connection");
		siteOfConnection[conn] = i;
	}
	for (node = 0; node < g_numNodes; node++) {
		for (j = 0; j < g_nodes[node].numLinks; j++) {
			SSpawnSite want;
			const char *why;
			conn = g_nodes[node].firstLink + j;
			if (ReferenceSite(node, conn, &want, &why)) {
				gotSites++;
				if (siteOfConnection[conn] < 0)
					Fail("a usable connection has no site");
				else if (sites[siteOfConnection[conn]].node1 != node ||
				         sites[siteOfConnection[conn]].node2 != g_connectionNode[conn])
					Fail("a site's node pair is not the connection's own");
			} else if (strcmp(why, "no lanes") == 0) {
				gotNoLanes++;
				if (siteOfConnection[conn] >= 0)
					Fail("a no-lane connection has a site");
			} else if (strcmp(why, "mixed") == 0) {
				gotMixed++;
				if (siteOfConnection[conn] >= 0)
					Fail("a mixed connection has a site");
			} else {
				gotUnresolved++;
			}
		}
	}
	printf("  the classification over every connection: %d usable, %d no lanes, %d mixed, %d unresolved\n",
		gotSites, gotNoLanes, gotMixed, gotUnresolved);
	printf("  the index's own counters:                  %d usable, %d no lanes, %d mixed, %d unresolved\n",
		index.GetStats().sites, index.GetStats().droppedNoLanes,
		index.GetStats().droppedMixed, index.GetStats().droppedUnresolved);
	Check(gotSites == index.GetStats().sites, "the usable counts must agree");
	Check(gotNoLanes == index.GetStats().droppedNoLanes, "the no-lane counts must agree");
	Check(gotMixed == index.GetStats().droppedMixed, "the mixed counts must agree");
	Check(gotUnresolved == 0, "every connection must resolve");

	/* The field sample: a stride over the site array, forced to hold the
	 * first site of every class and the first plain one, so no rule is
	 * checked on plain sites alone. */
	memset(sampled, 0, sizeof(sampled));
	stride = numSites / 512;
	if (stride < 1)
		stride = 1;
	for (i = 0; i < numSites; i += stride)
		sampled[i] = true;
	for (j = 0; j < nClasses; j++) {
		int first = -1;
		for (i = 0; i < numSites; i++)
			if (sites[i].flags & classes[j]) {
				first = i;
				break;
			}
		if (first < 0)
			Fail("a site class has no site in the city");
		else
			sampled[first] = true;
	}
	for (i = 0; i < numSites; i++)
		if (sites[i].flags == 0) {
			sampled[i] = true;
			break;
		}

	for (i = 0; i < numSites; i++) {
		SSpawnSite want;
		const char *why;
		if (!sampled[i])
			continue;
		sample++;
		if (!ReferenceSite(sites[i].node1, sites[i].connection, &want, &why)) {
			Fail("a sampled site's connection is not usable");
			continue;
		}
		if (memcmp(&sites[i], &want, sizeof(SSpawnSite)) != 0) {
			mismatches++;
			if (mismatches <= 3)
				PrintSiteDiff(sites[i], want, sites[i].connection);
		}
	}
	printf("  the sample: %d of the %d sites, every field recomputed from the raw arrays\n",
		sample, numSites);
	printf("  the fields: the midpoint (the node pair's own), the node pair, the connection and\n");
	printf("  its link, the travel direction, the lanes of the travelled side, the width, the\n");
	printf("  rarity (the smaller of the two nodes) and the eight flag bits\n");
	printf("  a field that differs: %d\n\n", mismatches);
	Check(mismatches == 0, "every sampled site must equal the raw structures' own reading");
	Check(sample > 512, "the sample must be a real spread");

	/* A site's midpoint is the node pair's own, never the link record's
	 * position (the graph's external links carry a synthetic position). The
	 * harness's link records carry one on purpose, so this check has teeth. */
	for (i = 0; i < numSites; i++) {
		const SSpawnSite &s = sites[i];
		if (s.x == g_links[s.link].x && s.y == g_links[s.link].y)
			linkPositionSites++;
	}
	printf("  the sites whose midpoint equals their link record's own position: %d of %d\n",
		linkPositionSites, numSites);
	printf("    (the link positions here are synthetic, as the external links' are; a site's\n");
	printf("    midpoint is the node pair's own)\n\n");
	Check(linkPositionSites == 0, "a site's midpoint must never be the link record's position");

	/* The zone index: the midpoint's own zone (never the entry node's) and the
	 * index itself, never the info. */
	for (i = 0; i < numSites; i++) {
		const SSpawnSite &s = sites[i];
		int zoneAtMid = ZoneAt(s.x, s.y);
		if (s.zoneIndex == (int16_t)zoneAtMid)
			zoneRule++;
		if (zoneAtMid != ZoneAt(g_nodes[s.node1].x, g_nodes[s.node1].y))
			zoneMidpoint++;
		if (s.zoneIndex != (int16_t)ZoneAt(g_nodes[s.node1].x, g_nodes[s.node1].y))
			zoneEntryNode++;
	}
	printf("  the zone index, against the midpoint's own zone: %d of %d sites agree\n",
		zoneRule, numSites);
	printf("    %d sites whose midpoint lies in another zone than the entry node's (the rule is\n",
		zoneMidpoint);
	printf("    not vacuous); %d sites where the entry node's zone would be stored instead)\n\n",
		zoneEntryNode);
	Check(zoneRule == numSites, "every site's zone index must be its midpoint's");
	Check(zoneMidpoint > 0, "the zones must straddle some segments");
	Check(zoneEntryNode == zoneMidpoint, "the entry node's zone must not be what is stored");
}

/* ------------------------------------------------------------------ *
 * D. the spatial hash
 * ------------------------------------------------------------------ */

/* The plain radius scan, the reference the hash's ring walk is compared
 * against. */
static int
ScanSitesNear(const CSpawnSiteIndex &index, float x, float y, float radius, int *out, int max)
{
	const SSpawnSite *sites = index.GetSites();
	const float radiusSq = radius * radius;
	int i, n = 0;
	for (i = 0; i < index.GetNumSites() && n < max; i++) {
		float dx = sites[i].x - x, dy = sites[i].y - y;
		if (dx * dx + dy * dy <= radiusSq)
			out[n++] = i;
	}
	return n;
}

static int
CompareInts(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static void
Section4Hash(CSpawnSiteIndex &index)
{
	static int outHash[NUM_PATHCONNECTIONS], outScan[NUM_PATHCONNECTIONS];
	const SSpawnSite *sites = index.GetSites();
	const int numSites = index.GetNumSites();
	const int stride = numSites / 512 > 0 ? numSites / 512 : 1;
	const float radii[] = { 0.0f, 8.0f, 33.0f, 100.0f, 400.0f, 5000.0f };
	const int nRadii = sizeof(radii) / sizeof(radii[0]);
	int inCellMisses = 0, badIds = 0, mismatches = 0, queries = 0, hits = 0;
	int i, q, r;

	printf("== 4. the spatial hash\n\n");

	/* Every sampled site is in its own cell: a point query at the site's own
	 * position (the ring walk starts at the query's cell) finds it, and every
	 * id the walk answers is a valid site. */
	for (i = 0; i < numSites; i += stride) {
		int n = index.CollectSitesNear(sites[i].x, sites[i].y, 0.01f, outHash, NUM_PATHCONNECTIONS);
		int found = 0;
		for (q = 0; q < n; q++) {
			if (outHash[q] < 0 || outHash[q] >= numSites) {
				badIds++;
				continue;
			}
			if (outHash[q] == i)
				found = 1;
		}
		if (!found)
			inCellMisses++;
	}
	printf("  every sampled site found at its own position (a point query): %d misses, %d bad ids\n",
		inCellMisses, badIds);
	Check(inCellMisses == 0, "every site must be found in the cell its position falls in");
	Check(badIds == 0, "the query must only answer valid site ids");

	/* The queries: a deterministic spread over the map (inside and outside the
	 * grid) times the radii, against the plain scan. The sets must match
	 * element for element. */
	for (q = 0; q < 300; q++) {
		float x = -500.0f + (float)((q * 37) % 5000);
		float y = -500.0f + (float)((q * 61) % 4200);
		for (r = 0; r < nRadii; r++) {
			int nHash = index.CollectSitesNear(x, y, radii[r], outHash, NUM_PATHCONNECTIONS);
			int nScan = ScanSitesNear(index, x, y, radii[r], outScan, NUM_PATHCONNECTIONS);
			queries++;
			if (nHash > 0)
				hits++;
			qsort(outHash, nHash, sizeof(int), CompareInts);
			qsort(outScan, nScan, sizeof(int), CompareInts);
			if (nHash != nScan || memcmp(outHash, outScan, sizeof(int) * nHash) != 0) {
				mismatches++;
				if (mismatches <= 3) {
					int a, listed = 0;
					printf("    the query (%.0f, %.0f) at r %.0f: the hash answers %d sites, the plain scan %d\n",
						(double)x, (double)y, (double)radii[r], nHash, nScan);
					for (a = 0; a < nScan && listed < 4; a++) {
						int b, inHash = 0;
						for (b = 0; b < nHash; b++)
							if (outHash[b] == outScan[a])
								inHash = 1;
						if (!inHash) {
							printf("      only in the scan: site %d at (%.1f, %.1f)\n",
								outScan[a], (double)sites[outScan[a]].x, (double)sites[outScan[a]].y);
							listed++;
						}
					}
				}
			}
		}
	}
	printf("  the queries: %d (300 points x %d radii), the sets that differ from the plain\n",
		queries, nRadii);
	printf("  radius scan: %d; the queries that found sites: %d\n\n", mismatches, hits);
	Check(mismatches == 0, "the ring walk must answer what the plain scan answers");
	Check(hits > 0, "the queries must find sites");
}

/* ------------------------------------------------------------------ *
 * E. the flags refresh
 * ------------------------------------------------------------------ */

/* The engine side's gate, mirrored (CSpawnSites::RefreshFlags): the graph's own
 * version moves the refresh, and a frame with no toggle reads it only. The two
 * versions are volatile so the gate's own measurement below is real (the engine
 * side's own are plain globals, of course). */
static volatile uint32_t g_graphFlagsVersion;	/* CPathFind::m_nodeFlagsVersion */
static volatile uint32_t g_siteFlagsVersion;	/* the version the index's flags hold */
static int g_lastFlagChanges;

/* The engine side's gate, mirrored (CSpawnSites::RefreshFlags): the graph's own
 * version moves the refresh, and a frame with no toggle reads it only. */
static void
HarnessRefreshFlags(CSpawnSiteIndex &index)
{
	if (g_siteFlagsVersion == g_graphFlagsVersion)
		return;
	g_siteFlagsVersion = g_graphFlagsVersion;
	g_lastFlagChanges = index.RefreshFlags(g_nodes);
}

/* CPathFind::SwitchOffNodeAndNeighbours' own shape (PathFind.cpp): the node
 * off, then the neighbours of a node with fewer than three links. */
static void
SwitchOffNodeAndNeighbours(int nodeId, bool disable, int *pVisited)
{
	int i;

	(*pVisited)++;
	if (disable)
		g_nodes[nodeId].flags |= SPAWN_SITE_NODE_DISABLED;
	else
		g_nodes[nodeId].flags &= ~SPAWN_SITE_NODE_DISABLED;
	if (g_nodes[nodeId].numLinks < 3)
		for (i = 0; i < g_nodes[nodeId].numLinks; i++) {
			int next = g_connectionNode[g_nodes[nodeId].firstLink + i];
			if (!!(g_nodes[next].flags & SPAWN_SITE_NODE_DISABLED) != disable &&
			    g_nodes[next].numLinks < 3)
				SwitchOffNodeAndNeighbours(next, disable, pVisited);
		}
}

/* CPathFind::MarkRoadsBetweenLevelsNodeAndNeighbours' own shape. */
static void
MarkRoadsBetweenLevelsNodeAndNeighbours(int nodeId, bool mark, int *pVisited)
{
	int i;

	(*pVisited)++;
	if (mark)
		g_nodes[nodeId].flags |= SPAWN_SITE_NODE_BETWEENLEVELS;
	else
		g_nodes[nodeId].flags &= ~SPAWN_SITE_NODE_BETWEENLEVELS;
	if (g_nodes[nodeId].numLinks < 3)
		for (i = 0; i < g_nodes[nodeId].numLinks; i++) {
			int next = g_connectionNode[g_nodes[nodeId].firstLink + i];
			if (!!(g_nodes[next].flags & SPAWN_SITE_NODE_BETWEENLEVELS) != mark &&
			    g_nodes[next].numLinks < 3)
				MarkRoadsBetweenLevelsNodeAndNeighbours(next, mark, pVisited);
		}
}

/* Everything of a site but its flags: what a rebuild recomputes and a refresh
 * must not touch. */
static uint32_t
HashStructural(const CSpawnSiteIndex &index)
{
	uint32_t h = 2166136261u;
	int i;
	for (i = 0; i < index.GetNumSites(); i++) {
		const unsigned char *p = (const unsigned char *)&index.GetSites()[i];
		size_t k;
		for (k = 0; k < sizeof(SSpawnSite); k++) {
			if (k == offsetof(SSpawnSite, flags))
				continue;
			h ^= p[k];
			h *= 16777619u;
		}
	}
	return h;
}

/* The sites' flags against the nodes' own bits, this program's own reading. */
static int
FlagMismatches(const CSpawnSiteIndex &index)
{
	int i, bad = 0;
	for (i = 0; i < index.GetNumSites(); i++) {
		const SSpawnSite &s = index.GetSites()[i];
		SSpawnSite want;
		const char *why;
		ReferenceSite(s.node1, s.connection, &want, &why);
		if (want.flags != s.flags)
			bad++;
	}
	return bad;
}

static void
Section5Refresh(CSpawnSiteIndex &index, double buildSec)
{
	const int gateRounds = 1000000;
	const int refreshRounds = 200;
	uint32_t structuralBefore, structuralAfter;
	int buildsBefore = index.GetNumBuilds();
	int refreshesBefore = index.GetNumFlagRefreshes();
	int disabledBefore = CountFlag(index, SPAWN_SITE_DISABLED);
	int betweenBefore = CountFlag(index, SPAWN_SITE_BETWEENLEVELS);
	int disabledNodes = 0, betweenNodes = 0;
	int disabledAfter, betweenAfter, changed;
	double t0, t1, gateNs, refreshUs = 0.0;
	int i;

	printf("== 5. the flags refresh: a script's road toggle\n\n");
	structuralBefore = HashStructural(index);

	/* The frame's gate with no toggle: the call, the version's own load and the
	 * comparison (the load is kept observable so the loop cannot be folded
	 * away). */
	t0 = NowSeconds();
	for (i = 0; i < gateRounds; i++) {
		HarnessRefreshFlags(index);
		g_sink += (int)g_siteFlagsVersion;
	}
	t1 = NowSeconds();
	gateNs = (t1 - t0) / gateRounds * 1e9;
	Check(index.GetNumFlagRefreshes() == refreshesBefore, "the gate with no toggle must not reach the refresh");

	/* The toggle: a street switched off and a between-levels mark, the two
	 * writes the script makes after boot; the graph's own version moves. */
	SwitchOffNodeAndNeighbours(1500, true, &disabledNodes);
	MarkRoadsBetweenLevelsNodeAndNeighbours(200, true, &betweenNodes);
	g_graphFlagsVersion++;

	HarnessRefreshFlags(index);	/* the refresh the toggle asks for */
	changed = g_lastFlagChanges;
	disabledAfter = CountFlag(index, SPAWN_SITE_DISABLED);
	betweenAfter = CountFlag(index, SPAWN_SITE_BETWEENLEVELS);

	/* The refresh's own cost (the same call the engine side's gate makes). */
	for (i = 0; i < refreshRounds; i++) {
		double r0 = NowSeconds();
		g_sink += index.RefreshFlags(g_nodes);
		double r1 = NowSeconds();
		refreshUs += (r1 - r0) * 1e6;
	}
	refreshUs /= refreshRounds;

	structuralAfter = HashStructural(index);

	printf("  the toggle: %d nodes switched off (SwitchOffNodeAndNeighbours' recursion),\n",
		disabledNodes);
	printf("    %d nodes marked between levels\n", betweenNodes);
	printf("  the sites whose flags changed: %d (the disabled sites %d -> %d,\n",
		changed, disabledBefore, disabledAfter);
	printf("    the between-levels sites %d -> %d)\n", betweenBefore, betweenAfter);
	printf("  the builds: %d -> %d (a rebuild would show here; the refresh never builds)\n",
		buildsBefore, index.GetNumBuilds());
	printf("  the flag refreshes since the section began: %d (the toggle's own + %d timed calls)\n",
		index.GetNumFlagRefreshes() - refreshesBefore, refreshRounds);
	printf("  the structural fields (midpoint, node pair, connection, link, direction, lanes,\n");
	printf("    width, rarity, zone, the site order): the hash %s\n",
		structuralBefore == structuralAfter ? "stands (no rebuild)" : "MOVED");
	printf("  the sites' flags against the nodes' bits after the refresh: %d mismatches\n",
		FlagMismatches(index));
	printf("  the frame's gate (no toggle): %.0f ns;  the refresh itself: %.0f us\n",
		gateNs, refreshUs);
	printf("    the refresh is %.4f %% of a 33 ms frame and %.2f %% of the build (%.2f ms)\n\n",
		refreshUs / 33000.0 * 100.0, refreshUs / 1e6 / buildSec * 100.0, buildSec * 1000.0);

	Check(index.GetNumBuilds() == buildsBefore, "the refresh must never rebuild");
	Check(structuralBefore == structuralAfter, "the refresh must touch nothing but the flags");
	Check(changed > 0, "the toggle must move some sites' flags");
	Check(disabledAfter > disabledBefore, "the switched-off nodes must move the disabled flags");
	Check(betweenAfter > betweenBefore, "the between-levels mark must move the between-levels flags");
	Check(FlagMismatches(index) == 0, "every site's flags must follow its nodes");
	Check(index.GetNumFlagRefreshes() - refreshesBefore > 0, "the refresh path must have run");
	Check(refreshUs < 33000.0 * 0.01, "the refresh must stay under 1 % of a frame");

	/* The second toggle: the street back on, the flags return. */
	disabledBefore = disabledAfter;
	betweenBefore = betweenAfter;
	SwitchOffNodeAndNeighbours(1500, false, &disabledNodes);
	MarkRoadsBetweenLevelsNodeAndNeighbours(200, false, &betweenNodes);
	g_graphFlagsVersion++;
	HarnessRefreshFlags(index);
	disabledAfter = CountFlag(index, SPAWN_SITE_DISABLED);
	betweenAfter = CountFlag(index, SPAWN_SITE_BETWEENLEVELS);
	printf("  the street switched back on: the disabled sites %d -> %d, the between-levels\n",
		disabledBefore, disabledAfter);
	printf("    sites %d -> %d; the flags changed: %d\n", betweenBefore, betweenAfter, g_lastFlagChanges);
	Check(disabledAfter < disabledBefore, "the reverse toggle must clear the disabled flags");
	Check(FlagMismatches(index) == 0, "the reverse toggle must leave the flags on the nodes");

	/* A frame with no toggle: the gate's early return, no refresh call. */
	{
		int runsBefore = index.GetNumFlagRefreshes();
		HarnessRefreshFlags(index);
		Check(index.GetNumFlagRefreshes() == runsBefore, "a frame with no toggle must not refresh");
		printf("  a frame with the version standing still: the gate returns, %d refresh calls\n\n",
			index.GetNumFlagRefreshes() - runsBefore);
	}
}

/* ------------------------------------------------------------------ *
 * F. the boat sweep, and the storage's worst case
 * ------------------------------------------------------------------ */

/* The storage's worst case: NUM_PATHCONNECTIONS directed connections over
 * 3,351 nodes and NUM_CARPATHLINKS link records. The link records are shared
 * across the connections here (3,500 records cannot describe 10,200 segments
 * either way); the section's point is the arrays' bound and the worst-case
 * build time. */
static void
BuildCapacityGraph(void)
{
	const int numNodes = 3351;
	const int tail = NUM_PATHCONNECTIONS - numNodes * 6;	/* the first nodes take one more */
	int i, j, conn = 0;

	memset(g_nodes, 0, sizeof(g_nodes));
	g_numNodes = numNodes;
	g_numLinks = NUM_CARPATHLINKS;
	for (i = 0; i < g_numNodes; i++) {
		g_nodes[i].x = 200.0f + (i % 58) * 56.0f;
		g_nodes[i].y = 200.0f + (i / 58) * 56.0f;
		g_nodes[i].z = 10.0f;
		g_nodes[i].spawnRate = 3 + (i * 5) % 13;
		if (i % 101 == 0)
			g_nodes[i].flags |= SPAWN_SITE_NODE_DISABLED;
		if (i % 37 == 0)
			g_nodes[i].flags |= SPAWN_SITE_NODE_DEADEND;
		if (i >= g_numNodes - 378)
			g_nodes[i].flags |= SPAWN_SITE_NODE_WATER;
		g_nodes[i].firstLink = (int16_t)conn;
		g_nodes[i].numLinks = (uint8_t)(6 + (i < tail ? 1 : 0));
		for (j = 0; j < g_nodes[i].numLinks; j++) {
			int target = (i + 1 + j) % g_numNodes;
			if ((g_nodes[i].flags ^ g_nodes[target].flags) & SPAWN_SITE_NODE_WATER)
				target = (g_numNodes - 378) + ((i + j) % 378);
			g_connectionNode[conn] = (uint16_t)target;
			g_connectionLink[conn] = (int16_t)(conn % g_numLinks);
			g_connectionZone[conn] = (int16_t)(conn % 6);
			conn++;
		}
	}
	g_numConnections = conn;

	for (i = 0; i < g_numLinks; i++) {
		int a = (i * 7) % g_numNodes;
		int b = (i * 13 + 1) % g_numNodes;
		int low = a < b ? a : b;
		int high = a < b ? b : a;
		float dx = g_nodes[low].x - g_nodes[high].x;
		float dy = g_nodes[low].y - g_nodes[high].y;
		float mag = (float)sqrt(dx * dx + dy * dy);

		g_links[i].x = (g_nodes[a].x + g_nodes[b].x) * 0.5f;
		g_links[i].y = (g_nodes[a].y + g_nodes[b].y) * 0.5f;
		g_links[i].dirX = mag > 0.0f ? (int8_t)(dx / mag * 100.0f) : 0;
		g_links[i].dirY = mag > 0.0f ? (int8_t)(dy / mag * 100.0f) : 0;
		g_links[i].numLeftLanes = (i % 5 == 3) ? 0 : 2;
		g_links[i].numRightLanes = 2;
		g_links[i].pathNodeIndex = (int16_t)low;
		g_links[i].width = (uint8_t)(40 * (g_links[i].numLeftLanes + g_links[i].numRightLanes));
	}
}

static void
Section6SweepAndCapacity(CSpawnSiteIndex &cityIndex, CSpawnSiteIndex &capacityIndex)
{
	const int rounds = 5;
	const int sites = cityIndex.GetNumSites();
	const SSpawnSite *pSites = cityIndex.GetSites();
	double best = 1e9;
	int i, node, j;
	int mixedConnections = 0, mixedSites = 0, boatSites = 0, landSites = 0;
	int boatSitesWithoutWater = 0;

	printf("== 6. the boat sweep, and the storage's worst case\n\n");

	for (node = 0; node < g_numNodes; node++)
		for (j = 0; j < g_nodes[node].numLinks; j++) {
			int conn = g_nodes[node].firstLink + j;
			int other = g_connectionNode[conn];
			if ((g_nodes[node].flags ^ g_nodes[other].flags) & SPAWN_SITE_NODE_WATER)
				mixedConnections++;
		}
	for (i = 0; i < sites; i++) {
		const SSpawnSite &s = pSites[i];
		if ((g_nodes[s.node1].flags ^ g_nodes[s.node2].flags) & SPAWN_SITE_NODE_WATER)
			mixedSites++;
		if (s.flags & SPAWN_SITE_BOAT) {
			boatSites++;
			if (!(s.flags & (SPAWN_SITE_WATER1 | SPAWN_SITE_WATER2)))
				boatSitesWithoutWater++;
		} else {
			landSites++;
		}
	}
	printf("  the boat sweep over the city: %d mixed land/water connections; the index dropped\n",
		mixedConnections);
	printf("    %d of them, and a stored site whose ends disagree about water: %d\n",
		cityIndex.GetStats().droppedMixed, mixedSites);
	printf("  the boat sites: %d, every one a water-water pair (%d without the water flag);\n",
		boatSites, boatSitesWithoutWater);
	printf("    the road sites: %d\n\n", landSites);
	Check(mixedConnections == cityIndex.GetStats().droppedMixed, "the mixed sweep must count every mixed pair");
	Check(mixedSites == 0, "no stored site may be a mixed pair");
	Check(boatSites == cityIndex.GetStats().boatSites, "the boat sites must be the water-water pairs");
	Check(boatSitesWithoutWater == 0, "a boat site must carry the water flags");
	Check(landSites > 0 && boatSites > 0, "the city must hold both kinds of site");

	/* The storage's worst case. */
	BuildCapacityGraph();
	for (i = 0; i < rounds; i++) {
		double t0 = NowSeconds();
		capacityIndex.Build(GraphView());
		double t1 = NowSeconds();
		g_sink += capacityIndex.GetNumSites();
		if (t1 - t0 < best)
			best = t1 - t0;
	}
	printf("  the storage's worst case: %d nodes, %d links, %d directed connections\n",
		g_numNodes, g_numLinks, g_numConnections);
	printf("  the usable sites: %d of the capacity %d, the boats: %d\n",
		capacityIndex.GetStats().sites, NUM_PATHCONNECTIONS, capacityIndex.GetStats().boatSites);
	printf("  dropped: %d with no lanes, %d mixed, %d unresolved; the hash: %s, %d cells\n",
		capacityIndex.GetStats().droppedNoLanes, capacityIndex.GetStats().droppedMixed,
		capacityIndex.GetStats().droppedUnresolved,
		capacityIndex.IsHashValid() ? "built" : "not built (the plain scan)",
		capacityIndex.GetStats().hashCells);
	printf("  the build time: %.2f ms (the best of %d; %.0f ns a connection)\n\n",
		best * 1000.0, rounds, best * 1e9 / g_numConnections);
	Check(g_numConnections == NUM_PATHCONNECTIONS, "the worst case must fill the connection capacity");
	Check(g_numLinks == NUM_CARPATHLINKS, "the worst case must fill the link capacity");
	Check(capacityIndex.GetStats().sites > 0 && capacityIndex.GetStats().sites <= NUM_PATHCONNECTIONS,
		"the worst case must fit the site array");
	Check(capacityIndex.GetStats().droppedUnresolved == 0, "the worst case must resolve every connection");
	Check(capacityIndex.IsHashValid(), "the worst case's hash must be built");
}

/* ------------------------------------------------------------------ *
 * G. the hash fallback
 * ------------------------------------------------------------------ */

static void
Section7Fallback(CSpawnSiteIndex &index)
{
	static int out[NUM_PATHCONNECTIONS];
	int near, far;
	int i;

	printf("== 7. the hash fallback (a site outside the grid)\n\n");

	/* A three node graph with one node far past the 4 km grid: its
	 * connections' midpoints are past the grid, so the hash cannot hold them
	 * and must go unbuilt - and the query must still find every site (the
	 * node grid's own contract). */
	memset(g_nodes, 0, sizeof(g_nodes));
	g_numNodes = 3;
	g_numLinks = 2;
	for (i = 0; i < g_numNodes; i++) {
		g_nodes[i].z = 10.0f;
		g_nodes[i].spawnRate = 8;
	}
	g_nodes[0].x = 100.0f; g_nodes[0].y = 100.0f;
	g_nodes[1].x = 200.0f; g_nodes[1].y = 100.0f;
	g_nodes[2].x = 20000.0f; g_nodes[2].y = 100.0f;
	g_nodes[0].firstLink = 0; g_nodes[0].numLinks = 1;
	g_nodes[1].firstLink = 1; g_nodes[1].numLinks = 2;
	g_nodes[2].firstLink = 3; g_nodes[2].numLinks = 1;
	g_connectionNode[0] = 1; g_connectionLink[0] = 0; g_connectionZone[0] = 0;
	g_connectionNode[1] = 0; g_connectionLink[1] = 0; g_connectionZone[1] = 0;
	g_connectionNode[2] = 2; g_connectionLink[2] = 1; g_connectionZone[2] = 0;
	g_connectionNode[3] = 1; g_connectionLink[3] = 1; g_connectionZone[3] = 0;
	g_numConnections = 4;
	for (i = 0; i < g_numLinks; i++) {
		int a = i, b = i + 1;
		g_links[i].x = (g_nodes[a].x + g_nodes[b].x) * 0.5f;
		g_links[i].y = (g_nodes[a].y + g_nodes[b].y) * 0.5f;
		g_links[i].dirX = -100; g_links[i].dirY = 0;
		g_links[i].numLeftLanes = 1; g_links[i].numRightLanes = 1;
		g_links[i].pathNodeIndex = (int16_t)a;
		g_links[i].width = 80;
	}

	index.Build(GraphView());
	near = index.CollectSitesNear(150.0f, 100.0f, 1.0f, out, NUM_PATHCONNECTIONS);
	far = index.CollectSitesNear(10100.0f, 100.0f, 1.0f, out, NUM_PATHCONNECTIONS);
	printf("  the graph: 4 directed connections, 4 sites (one pair's midpoint past the 4 km grid)\n");
	printf("  the hash: %s\n", index.IsHashValid() ? "built" : "not built (the plain scan)");
	printf("  a query at the near pair: %d sites; at the far pair: %d sites\n\n", near, far);
	Check(!index.IsHashValid(), "a site outside the grid must leave the hash unbuilt");
	Check(index.GetStats().sites == 4, "the tiny graph must give four sites");
	Check(near == 2, "the plain scan must find the near pair");
	Check(far == 2, "the plain scan must find the far pair");
}

int
main(void)
{
	static CSpawnSiteIndex cityIndex;
	static CSpawnSiteIndex capacityIndex;
	static CSpawnSiteIndex fallbackIndex;
	double buildSec;

	BuildCityNodes();
	BuildCityEdges();
	LayGraph();
	MarkDeadEnds();

	Section1Graph();
	buildSec = Section2Build(cityIndex);
	Section3SpotCheck(cityIndex);
	Section4Hash(cityIndex);
	Section5Refresh(cityIndex, buildSec);
	Section6SweepAndCapacity(cityIndex, capacityIndex);
	Section7Fallback(fallbackIndex);

	printf("== the verdict\n\n");
	if (g_failures == 0)
		printf("  every check passed (the city's index: %d sites of %d connections)\n",
			cityIndex.GetNumSites(), NUM_PATHCONNECTIONS);
	else
		printf("  %d checks failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
