/*
 * The evidence program for the pedestrian standable cells (VICE-154).
 *
 * It compiles the cell store's own engine-free translation unit
 * (src/peds/PedSitesCore.cpp - the real lattice, the real predicate, the real
 * queue and cache) and drives it over synthetic maps. No engine header is
 * included anywhere: the program fills the same SPedSiteProbe the engine side
 * fills from CWorld/CWaterLevel, and that is the only seam.
 *
 *	A. The grid. The cell mapping over the corners, the cell edges and the
 *	   full width (a NaN or a huge value has no cell), and the probe
 *	   lattice: 16 distinct points per cell, a 4 x 4 row-by-row grid, every
 *	   point inside its own cell.
 *
 *	B. The predicate. The six rules one at a time and at the boundary: no
 *	   ground, the water line and its margin, the level margin, the roof step,
 *	   the clearance, the slope. Every case checks the *verdict* (the reason),
 *	   not just "rejected", and the order of the rules is checked with probes
 *	   that are bad in two ways at once. The slope predicate is then driven
 *	   alone over a table of normals (flat, the limit, just past it, a wall, a
 *	   NaN), and the water rule alone over the margin's two sides.
 *
 *	C. The build. A synthetic 600 x 256 m map with a sea and its shore, a
 *	   beach, a city of blocks with roofs, a wall segment, a hill, a cliff
 *	   face, a plateau and a void patch. Every cell of the map is requested,
 *	   queued and built through the real request/queue/report cycle; the
 *	   sweep then checks the ticket's acceptance on the built cells: no
 *	   stored point is in the water (ground at or below the line), on a roof
 *	   (inside a block's footprint), on the cliff face, in the wall or in
 *	   the void, every sunken cell (sea, shore, roof, cliff, void) answers
 *	   dead, the sand and the streets answer standable, and the counters
 *	   carry every reject reason (the ticket's telemetry) with the verdicts
 *	   summing to the probes.
 *
 *	D. The store. The answer kinds (a spot, an unknown-and-queued cell, a
 *	   dead cell, an off-grid position), the level staleness (an entry built
 *	   for one level is dropped and re-queued for another), the queued
 *	   entry's level update, the queue's nearest-first pop and its full-queue
 *	   eviction (a nearer cell displaces the farthest, a farther one is
 *	   refused), the cache's eviction (the farthest entry goes, the evicted
 *	   cell is unvalidated again and the next request re-queues it), and the
 *	   reads.
 *
 *	E. The bounded build. The frame budget (a pump spends at most
 *	   PED_SITE_CELLS_PER_FRAME builds, whatever the queue holds), the
 *	   arithmetic's cost (a request on a built cell, a queue pop at the full
 *	   queue, a report with its 16 probes and the spot selection, the worst
 *	   case's cache eviction, a whole capped frame) and the world-query
 *	   budget: the cap times the probes times the four queries of one probe,
 *	   a constant per frame, against the old per-attempt path's worst frame.
 *
 *	F. The coverage. The same synthetic city/beach/airfield/sea map, and the
 *	   same seeded draws, through the old VICE-122 policy (the per-attempt
 *	   random search with its node reach bound of 150 m and 5 m of level) and
 *	   through the cells (the same draws, the drawn cell's answer, the band
 *	   and the level re-checked). Per area: the standable share of the cells,
 *	   the per-draw share before and after, and the number of samples. The
 *	   areas beyond the node reach - the beach's far sand, the airfield - go
 *	   from nothing to the cells' share: the ticket's coverage acceptance.
 *
 * What this program cannot reach, stated plainly: the world queries
 * themselves (the two casts, the water line, the clearance sphere, and the
 * collision normal) are the engine's, in PedSites.cpp; here the synthetic map
 * answers them. So the probe *prices* are a model: the program prices the
 * store's own arithmetic exactly and the world work by the constant count of
 * queries a frame can spend, not by a clock on CWorld. The population's
 * wiring (the target ratio, the choice of the cell path per spawn) and the
 * creation loop's dynamic clearance are the game's; the build and the live
 * soak cover them.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/peds -O2 \
 *	    -o /tmp/spawn-ped-sites utils/spawn-service/ped-sites.cpp \
 *	    src/peds/PedSitesCore.cpp
 *	/tmp/spawn-ped-sites
 *
 * The exit code is 0 when every check passes. The output goes into the
 * ticket.
 */

#include "PedSites.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PI 3.14159265358979323846f

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

static double
NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ the rng */

static uint32_t g_seed = 20261018u;

static uint32_t
Rand(void)
{
	g_seed = g_seed * 1664525u + 1013904223u;
	return g_seed;
}

static float
RandFloat(float lo, float hi)
{
	return lo + (hi - lo) * (float)((Rand() >> 8) * (1.0 / 16777216.0));
}

/* ------------------------------------------------------- the build's map (C)
 *
 * A 600 x 256 m strip, west to east:
 *
 *	x < 0		the sea (its bed at -6, the water line 0.5)
 *	0 .. 8		the shore (the sand at 0.35, under the line: water)
 *	8 .. 64		the beach (the sand at 1.2, above the line)
 *	64 .. 256	the city (the street at 0; a 32 m block with a 20 m roof
 *			every 64 m, from +16 to +48 of the block)
 *	256 .. 384	the hill (the ground rises 0.2 a metre: a walkable 11
 *			degrees, the normal 0.98)
 *	384 .. 416	the cliff face (rises 1.5 a metre: 56 degrees, the
 *			normal 0.55: past the walk limit)
 *	416 .. 480	the plateau (at 73.6)
 *	480 .. 544	the void patch (no collision at all)
 *
 * A 3 m wall segment sits at x 118..121, y 80..160: the clearance sphere's
 * test. The wall's ground and surface are the street's (a thin fence-like
 * obstacle), so the clearance is the rule that fires there and the sweep can
 * tell it from the roof's step.
 */
#define MAP_MIN_X	(-32.0f)
#define MAP_MAX_X	576.0f
#define MAP_MIN_Y	0.0f
#define MAP_MAX_Y	256.0f
#define MAP_SEA_EDGE	0.0f
#define MAP_SHORE_EDGE	8.0f
#define MAP_BEACH_EDGE	64.0f
#define MAP_CITY_EDGE	256.0f
#define MAP_HILL_EDGE	384.0f
#define MAP_CLIFF_EDGE	416.0f
#define MAP_PLATEAU_EDGE 480.0f
#define MAP_VOID_EDGE	544.0f
#define MAP_WATER_Z	0.5f
#define MAP_ROOF_Z	20.0f
#define MAP_SEABED_Z	(-6.0f)
#define MAP_SHORE_Z	0.35f
#define MAP_BEACH_Z	1.2f
#define MAP_PLATEAU_Z	73.6f
#define MAP_HILL_SLOPE	0.2f
#define MAP_HILL_NORMAL	0.98058f	/* cos(atan(0.2)) */
#define MAP_CLIFF_SLOPE	1.5f
#define MAP_CLIFF_NORMAL 0.55470f	/* cos(atan(1.5)) */

/* The city's blocks: from 64 m, one block every 64 m, the roof over its own
 * 32 m (the block's +16 .. +48). */
static bool
MapInBlock(float x)
{
	int32_t k;
	float off;

	if (x < MAP_BEACH_EDGE || x >= MAP_CITY_EDGE)
		return false;
	k = (int32_t)((x - MAP_BEACH_EDGE) / 64.0f);
	off = x - (MAP_BEACH_EDGE + k * 64.0f);
	return off >= 16.0f && off < 48.0f;
}

/* The wall segment's footprint, widened by the clearance sphere's radius: a
 * probe whose sphere reaches it is blocked. */
static bool
MapInWall(float x, float y)
{
	return x >= 118.0f - PED_SITE_CLEARANCE_RADIUS && x < 121.0f + PED_SITE_CLEARANCE_RADIUS &&
		y >= 80.0f - PED_SITE_CLEARANCE_RADIUS && y < 160.0f + PED_SITE_CLEARANCE_RADIUS;
}

/* The terrain's own surface: what a player standing on the ground there
 * stands on (the city's roofs are not a level a pedestrian spawns from: the
 * player is in the street). This is the level a cell is built for. */
static float
MapLevelAt(float x, float y)
{
	(void)y;
	if (x < MAP_SEA_EDGE)
		return MAP_WATER_Z;
	if (x < MAP_SHORE_EDGE)
		return MAP_SHORE_Z;
	if (x < MAP_BEACH_EDGE)
		return MAP_BEACH_Z;
	if (x < MAP_CITY_EDGE)
		return 0.0f;
	if (x < MAP_HILL_EDGE)
		return (x - MAP_CITY_EDGE) * MAP_HILL_SLOPE;
	if (x < MAP_CLIFF_EDGE)
		return (MAP_HILL_EDGE - MAP_CITY_EDGE) * MAP_HILL_SLOPE + (x - MAP_HILL_EDGE) * MAP_CLIFF_SLOPE;
	if (x < MAP_PLATEAU_EDGE)
		return MAP_PLATEAU_Z;
	return MAP_WATER_Z;
}

/* The ground the down cast finds: the topmost surface at or below the
 * request's level. A block's roof above the level is missed (the cast starts
 * in the street), exactly as the old placement's cast missed it. */
static bool
MapGroundAt(float x, float y, float levelZ, float *pZ, float *pNormalZ)
{
	if (x < MAP_SEA_EDGE) {
		*pZ = MAP_SEABED_Z;
		*pNormalZ = 1.0f;
		return true;
	}
	if (x < MAP_SHORE_EDGE) {
		*pZ = MAP_SHORE_Z;
		*pNormalZ = 1.0f;
		return true;
	}
	if (x < MAP_BEACH_EDGE) {
		*pZ = MAP_BEACH_Z;
		*pNormalZ = 1.0f;
		return true;
	}
	if (x < MAP_CITY_EDGE) {
		*pNormalZ = 1.0f;
		*pZ = (MapInBlock(x) && levelZ >= MAP_ROOF_Z) ? MAP_ROOF_Z : 0.0f;
		return true;
	}
	if (x < MAP_HILL_EDGE) {
		*pZ = (x - MAP_CITY_EDGE) * MAP_HILL_SLOPE;
		*pNormalZ = MAP_HILL_NORMAL;
		return true;
	}
	if (x < MAP_CLIFF_EDGE) {
		*pZ = (MAP_HILL_EDGE - MAP_CITY_EDGE) * MAP_HILL_SLOPE + (x - MAP_HILL_EDGE) * MAP_CLIFF_SLOPE;
		*pNormalZ = MAP_CLIFF_NORMAL;
		return true;
	}
	if (x < MAP_PLATEAU_EDGE) {
		*pZ = MAP_PLATEAU_Z;
		*pNormalZ = 1.0f;
		return true;
	}
	(void)y;
	return false;
}

/* The top surface below the cast: the roof over the city's blocks, the
 * terrain's own surface elsewhere. */
static bool
MapSurfaceAt(float x, float y, float levelZ, float *pZ)
{
	float ground, normal;

	if (MapInBlock(x)) {
		*pZ = MAP_ROOF_Z;
		return true;
	}
	return MapGroundAt(x, y, levelZ, &ground, &normal) && (*pZ = ground, true);
}

/* The water line: the sea and its shore, and the beach's own block (the sand
 * stands above it); the city and beyond have none. */
static bool
MapWaterAt(float x, float y, float *pZ)
{
	(void)y;
	if (x < MAP_BEACH_EDGE) {
		*pZ = MAP_WATER_Z;
		return true;
	}
	return false;
}

static void
MapProbeAt(float x, float y, float levelZ, SPedSiteProbe &probe)
{
	probe.groundFound = MapGroundAt(x, y, levelZ, &probe.groundZ, &probe.groundNormalZ);
	probe.surfaceFound = MapSurfaceAt(x, y, levelZ, &probe.surfaceZ);
	probe.waterFound = MapWaterAt(x, y, &probe.waterZ);
	probe.clearance = !MapInWall(x, y);
}

/* --------------------------------------------------------- the flat map (D)
 *
 * The store's own contracts (D) need no terrain: a flat ground at 0, no
 * water, the clearance free. The level the caller passes is what the level
 * rules read, so a request at another level finds the ground off its level
 * and the cell answers dead - which is the staleness cycle the section
 * checks. */
static void
FlatProbeAt(float x, float y, float levelZ, SPedSiteProbe &probe)
{
	(void)x;
	(void)y;
	(void)levelZ;
	probe.groundFound = true;
	probe.groundZ = 0.0f;
	probe.groundNormalZ = 1.0f;
	probe.surfaceFound = true;
	probe.surfaceZ = 0.0f;
	probe.waterFound = false;
	probe.waterZ = 0.0f;
	probe.clearance = true;
}

/* The pump's shape at a stated budget: the engine side's Pump with its world
 * queries replaced by the map. Returns the builds spent. */
typedef void (*TerrainProbe)(float x, float y, float levelZ, SPedSiteProbe &probe);

static int32_t
BuildQueue(CPedSiteCells &cells, TerrainProbe probe, int32_t budget)
{
	int32_t built = 0;

	for (int32_t i = 0; i < budget; i++) {
		uint16_t cell;
		float levelZ;
		SPedSiteProbe aProbes[PED_SITE_PROBES];

		if (!cells.PopNearestQueued(cell, levelZ))
			break;
		for (int32_t k = 0; k < PED_SITE_PROBES; k++) {
			float x, y;
			CPedSiteCells::ProbePoint(cell, k, &x, &y);
			probe(x, y, levelZ, aProbes[k]);
		}
		cells.ReportCellProbed(cell, levelZ, aProbes);
		built++;
	}
	return built;
}

/* Queue the cell under (x, y) at the level of the request: the shape the
 * population's request has (a drawn position, the ring's level). */
static EPedSiteAnswer
RequestAt(CPedSiteCells &cells, float x, float y, float levelZ)
{
	SPedSitePoint spot;
	return cells.RequestSpot(x, y, levelZ, &spot);
}

/* Every queued cell, in slices of the frame budget, until the queue is
 * empty. */
static int32_t
DrainQueue(CPedSiteCells &cells, TerrainProbe probe)
{
	int32_t total = 0, n;
	while ((n = BuildQueue(cells, probe, PED_SITE_CELLS_PER_FRAME * 64)) > 0)
		total += n;
	return total;
}

/* The level a cell of the build map is built for: the terrain's own surface
 * at the cell's centre. */
static float
MapCellLevel(uint16_t cell)
{
	float x, y;
	CPedSiteCells::CellCentre(cell, &x, &y);
	return MapLevelAt(x, y);
}

/* ------------------------------------------------------------------ A. the grid */

static void
SectionGrid(void)
{
	uint16_t cell;
	float xs[PED_SITE_PROBES], ys[PED_SITE_PROBES];
	int32_t duplicates = 0;

	printf("A. the grid (16 m cells; %d x %d over the world)\n", PED_SITE_CELLS_X, PED_SITE_CELLS_Y);

	Check(CPedSiteCells::CellAt(PED_SITE_GRID_MIN_X, PED_SITE_GRID_MIN_Y, &cell) && cell == 0,
		"the world's corner must be cell 0");
	Check(CPedSiteCells::CellAt(PED_SITE_GRID_MIN_X + PED_SITE_CELL_SIZE - 0.01f,
		PED_SITE_GRID_MIN_Y + PED_SITE_CELL_SIZE - 0.01f, &cell) && cell == 0,
		"the first cell must end at its edge");
	Check(CPedSiteCells::CellAt(PED_SITE_GRID_MIN_X + PED_SITE_CELL_SIZE,
		PED_SITE_GRID_MIN_Y + PED_SITE_CELL_SIZE, &cell) && cell == PED_SITE_CELLS_X + 1,
		"the cell after the first edge must be (1, 1)");
	Check(CPedSiteCells::CellAt(PED_SITE_GRID_MIN_X + PED_SITE_CELLS_X * PED_SITE_CELL_SIZE - 0.01f,
		PED_SITE_GRID_MIN_Y + PED_SITE_CELLS_Y * PED_SITE_CELL_SIZE - 0.01f, &cell) &&
		cell == PED_SITE_CELLS - 1, "the world's far corner must be the last cell");
	Check(!CPedSiteCells::CellAt(PED_SITE_GRID_MIN_X - 0.5f, 0.0f, &cell), "a position past the west edge must have no cell");
	Check(!CPedSiteCells::CellAt(0.0f, PED_SITE_GRID_MIN_Y - 0.5f, &cell), "a position past the south edge must have no cell");
	Check(!CPedSiteCells::CellAt(PED_SITE_GRID_MIN_X + PED_SITE_CELLS_X * PED_SITE_CELL_SIZE, 0.0f, &cell),
		"a position at the east edge must have no cell");
	Check(!CPedSiteCells::CellAt(0.0f, PED_SITE_GRID_MIN_Y + PED_SITE_CELLS_Y * PED_SITE_CELL_SIZE, &cell),
		"a position at the north edge must have no cell");
	Check(!CPedSiteCells::CellAt(NAN, 0.0f, &cell), "a NaN x has no cell");
	Check(!CPedSiteCells::CellAt(0.0f, NAN, &cell), "a NaN y has no cell");
	Check(!CPedSiteCells::CellAt(1e30f, 0.0f, &cell), "a huge x has no cell");
	Check(!CPedSiteCells::CellAt(0.0f, -1e30f, &cell), "a huge negative y has no cell");

	/* The lattice: 16 distinct points, row by row, all inside their cell. */
	Check(CPedSiteCells::CellAt(100.0f, 100.0f, &cell), "the lattice's cell resolves");
	for (int32_t i = 0; i < PED_SITE_PROBES; i++)
		CPedSiteCells::ProbePoint(cell, i, &xs[i], &ys[i]);
	{
		float cx, cy;
		CPedSiteCells::CellCentre(cell, &cx, &cy);
		cx -= PED_SITE_CELL_SIZE * 0.5f;
		cy -= PED_SITE_CELL_SIZE * 0.5f;
		for (int32_t i = 0; i < PED_SITE_PROBES; i++) {
			Check(xs[i] > cx && xs[i] < cx + PED_SITE_CELL_SIZE &&
				ys[i] > cy && ys[i] < cy + PED_SITE_CELL_SIZE,
				"every probe must be inside its cell");
			Check(xs[i] - cx >= 1.0f && cx + PED_SITE_CELL_SIZE - xs[i] >= 1.0f &&
				ys[i] - cy >= 1.0f && cy + PED_SITE_CELL_SIZE - ys[i] >= 1.0f,
				"no probe may sit on the cell's edge");
			for (int32_t k = 0; k < i; k++)
				if (xs[k] == xs[i] && ys[k] == ys[i])
					duplicates++;
		}
	}
	Check(duplicates == 0, "every lattice point must be distinct");
	Check(xs[1] - xs[0] == 4.0f && xs[2] - xs[1] == 4.0f && xs[3] - xs[2] == 4.0f,
		"the lattice's columns must be 4 m apart");
	Check(ys[4] - ys[0] == 4.0f && ys[8] - ys[4] == 4.0f && ys[12] - ys[8] == 4.0f,
		"the lattice's rows must be 4 m apart");
	printf("  the corners, the edges, the off-grid values and the 4 x 4 lattice: ok\n");
}

/* -------------------------------------------------------------- B. the predicate */

/* A probe that passes at the level 5: a flat street, no water, free. */
static SPedSiteProbe
PassProbe(void)
{
	SPedSiteProbe p;
	p.groundFound = true;
	p.groundZ = 5.0f;
	p.groundNormalZ = 1.0f;
	p.surfaceFound = true;
	p.surfaceZ = 5.0f;
	p.waterFound = false;
	p.waterZ = 0.0f;
	p.clearance = true;
	return p;
}

static void
CheckVerdict(EPedSiteVerdict expected, float levelZ, const SPedSiteProbe &probe, const char *what)
{
	float standZ = -999.0f;
	EPedSiteVerdict got = PedSiteEvaluateProbe(levelZ, probe, &standZ);

	g_checks++;
	if (got != expected) {
		printf("  FAIL: %s (got %d, expected %d)\n", what, (int)got, (int)expected);
		g_failures++;
	}
	if (expected == PED_SITE_PASS && standZ != probe.groundZ) {
		printf("  FAIL: %s (the pass must report the ground %f, got %f)\n", what, probe.groundZ, standZ);
		g_failures++;
	}
}

static void
SectionPredicate(void)
{
	printf("B. the predicate (the six rules and the slope predicate)\n");

	/* The pass. */
	{
		SPedSiteProbe p = PassProbe();
		CheckVerdict(PED_SITE_PASS, 5.0f, p, "a flat street at the level must pass");
	}

	/* 1. No ground: the cast found nothing. */
	{
		SPedSiteProbe p = PassProbe();
		p.groundFound = false;
		CheckVerdict(PED_SITE_NO_GROUND, 5.0f, p, "a cast with no ground must reject as no ground");
	}

	/* 2. Water and its margin: the ground must stand above the line plus
	 * the margin; at the line and at the margin it is still water. */
	{
		SPedSiteProbe p = PassProbe();
		p.waterFound = true;
		p.waterZ = 4.9f;
		CheckVerdict(PED_SITE_WATER, 5.0f, p, "a ground at the margin must reject as water");
		p.waterZ = 4.89f;
		CheckVerdict(PED_SITE_PASS, 5.0f, p, "a ground clear of the margin must pass");
		p.waterZ = 5.0f;
		CheckVerdict(PED_SITE_WATER, 5.0f, p, "a ground at the line must reject as water");
		p.waterZ = 6.0f;
		CheckVerdict(PED_SITE_WATER, 5.0f, p, "a ground below the line must reject as water");
		p.waterFound = false;
		p.waterZ = 0.0f;
		CheckVerdict(PED_SITE_PASS, 5.0f, p, "a block without water must pass");
	}

	/* 3. The level margin: 3 m either way, the boundary inside. The engine's
	 * cast starts at the level and finds the ground at or below it, so the
	 * passing side is the ground below; the upper side is defensive (a probe
	 * the game cannot produce). */
	{
		SPedSiteProbe p = PassProbe();
		p.groundZ = 5.0f - PED_SITE_LEVEL_MARGIN;
		p.surfaceZ = p.groundZ;
		CheckVerdict(PED_SITE_PASS, 5.0f, p, "a ground one margin below the level must pass");
		p.groundZ = 5.0f - PED_SITE_LEVEL_MARGIN - 0.01f;
		p.surfaceZ = p.groundZ;
		CheckVerdict(PED_SITE_LEVEL, 5.0f, p, "a ground below the level margin must reject as level");
		p.groundZ = 5.0f + PED_SITE_LEVEL_MARGIN + 0.01f;
		p.surfaceZ = p.groundZ;
		CheckVerdict(PED_SITE_LEVEL, 5.0f, p, "a ground above the level margin must reject as level");
	}

	/* 4. The roof step: the top surface may stand a step above the level,
	 * not more; no surface at all rejects as a roof. */
	{
		SPedSiteProbe p = PassProbe();
		p.surfaceZ = 5.0f + PED_SITE_STEP;
		CheckVerdict(PED_SITE_PASS, 5.0f, p, "a surface at the step must pass");
		p.surfaceZ = 5.0f + PED_SITE_STEP + 0.01f;
		CheckVerdict(PED_SITE_ROOF, 5.0f, p, "a surface past the step must reject as a roof");
		p.surfaceZ = 20.0f;
		CheckVerdict(PED_SITE_ROOF, 5.0f, p, "a roof over the street must reject as a roof");
		p.surfaceFound = false;
		CheckVerdict(PED_SITE_ROOF, 5.0f, p, "a missing surface cast must reject as a roof");
	}

	/* 5. The clearance. */
	{
		SPedSiteProbe p = PassProbe();
		p.clearance = false;
		CheckVerdict(PED_SITE_CLEARANCE, 5.0f, p, "a blocked sphere must reject as clearance");
	}

	/* 6. The slope: the ground at the level, steep. */
	{
		SPedSiteProbe p = PassProbe();
		p.groundNormalZ = PED_SITE_SLOPE_COS_MIN;
		CheckVerdict(PED_SITE_PASS, 5.0f, p, "a normal at the walk limit must pass");
		p.groundNormalZ = PED_SITE_SLOPE_COS_MIN - 0.0001f;
		CheckVerdict(PED_SITE_SLOPE, 5.0f, p, "a normal past the walk limit must reject as a slope");
		p.groundNormalZ = 0.0f;
		CheckVerdict(PED_SITE_SLOPE, 5.0f, p, "a vertical face must reject as a slope");
		p.groundNormalZ = -1.0f;
		CheckVerdict(PED_SITE_SLOPE, 5.0f, p, "an upside-down normal must reject as a slope");
	}

	/* The order of the rules: a probe bad in two ways reports the first. */
	{
		SPedSiteProbe p = PassProbe();
		p.groundFound = false;
		p.waterFound = true;
		CheckVerdict(PED_SITE_NO_GROUND, 5.0f, p, "no ground must win over water");
		p = PassProbe();
		p.waterFound = true;
		p.waterZ = 6.0f;
		p.surfaceZ = 20.0f;
		CheckVerdict(PED_SITE_WATER, 5.0f, p, "water must win over the roof");
		p = PassProbe();
		p.groundZ = 20.0f;
		p.surfaceZ = 20.0f;
		CheckVerdict(PED_SITE_LEVEL, 5.0f, p, "the level must win over the roof");
		p = PassProbe();
		p.surfaceZ = 20.0f;
		p.clearance = false;
		CheckVerdict(PED_SITE_ROOF, 5.0f, p, "the roof must win over the clearance");
		p = PassProbe();
		p.clearance = false;
		p.groundNormalZ = 0.0f;
		CheckVerdict(PED_SITE_CLEARANCE, 5.0f, p, "the clearance must win over the slope");
	}

	/* The slope predicate alone. */
	{
		static const struct { float normalZ; bool pass; const char *what; } aTable[] = {
			{ 1.0f, true, "a flat ground" },
			{ 0.9f, true, "a gentle slope" },
			{ PED_SITE_SLOPE_COS_MIN, true, "the walk limit itself" },
			{ PED_SITE_SLOPE_COS_MIN - 0.001f, false, "just past the walk limit" },
			{ 0.5f, false, "a 60 degree slope" },
			{ 0.196f, false, "a 79 degree slope" },
			{ 0.0f, false, "a vertical wall" },
			{ -1.0f, false, "an upside-down normal" },
			{ NAN, false, "a degenerate normal" },
		};
		for (size_t i = 0; i < sizeof(aTable) / sizeof(aTable[0]); i++)
			Check(PedSiteGroundIsWalkable(aTable[i].normalZ) == aTable[i].pass, aTable[i].what);
	}

	/* The water rule alone: the no-water block, the margin's two sides. */
	{
		Check(PedSiteGroundIsClearOfWater(false, 5.0f, 0.0f), "a block without water is clear");
		Check(PedSiteGroundIsClearOfWater(true, 5.0f, 5.11f), "clear above the margin");
		Check(!PedSiteGroundIsClearOfWater(true, 5.0f, 5.1f), "at the margin the ground is not clear");
		Check(!PedSiteGroundIsClearOfWater(true, 5.0f, 5.0f), "at the line the ground is not clear");
		Check(!PedSiteGroundIsClearOfWater(true, 5.0f, 4.0f), "under the line the ground is not clear");
	}

	/* A nil standZ pointer is allowed (the harness's own "before" model). */
	{
		SPedSiteProbe p = PassProbe();
		Check(PedSiteEvaluateProbe(5.0f, p, NULL) == PED_SITE_PASS, "a nil standZ pointer is allowed");
	}
	printf("  the six rules, their order, the slope table and the water margins: ok\n");
}

/* ---------------------------------------------------------------- C. the build */

static void
SectionBuild(void)
{
	CPedSiteCells cells;
	SPedSiteStats before, after;
	uint16_t cell;
	int32_t built, requested = 0;
	uint32_t sum = 0;

	printf("C. the build (the synthetic map: sea, shore, beach, city, wall, hill, cliff, plateau, void)\n");

	/* The seat: every cell of the map is requested at the level of the
	 * terrain under it (the player standing on the ground), which is what
	 * the population passes. The requests come in slices, because the queue
	 * is bounded (PED_SITE_QUEUE) and a burst of more requests than that
	 * would drop the far ones. */
	cells.Reset();
	cells.SetAnchor(256.0f, 128.0f);
	built = 0;
	for (float y = MAP_MIN_Y; y < MAP_MAX_Y; y += PED_SITE_CELL_SIZE) {
		for (float x = MAP_MIN_X; x < MAP_MAX_X; x += PED_SITE_CELL_SIZE) {
			float cx = x + PED_SITE_CELL_SIZE * 0.5f;
			float cy = y + PED_SITE_CELL_SIZE * 0.5f;
			RequestAt(cells, cx, cy, MapLevelAt(cx, cy));
			requested++;
			if (cells.GetNumQueued() >= 256)
				built += DrainQueue(cells, MapProbeAt);
		}
	}
	built += DrainQueue(cells, MapProbeAt);
	Check(built == requested, "every requested cell must be built by the drain");
	Check(cells.GetNumQueued() == 0, "the drain must empty the queue");
	Check((int32_t)cells.GetStats().cellsBuilt == requested, "the cells-built counter must follow the drain");
	Check((int32_t)cells.GetStats().probes == requested * PED_SITE_PROBES, "every build must run the whole lattice");
	Check(cells.GetNumCached() == requested, "every built cell must hold a cache entry");

	/* The counters: the verdicts sum to the probes and every reject reason
	 * of the ticket fired somewhere on this map. */
	for (int32_t v = 0; v < PED_SITE_VERDICTS; v++)
		sum += cells.GetStats().verdicts[v];
	Check((uint32_t)sum == cells.GetStats().probes, "the verdicts must sum to the probes");
	Check(cells.GetStats().verdicts[PED_SITE_PASS] > 0, "the map must pass some probes");
	Check(cells.GetStats().verdicts[PED_SITE_NO_GROUND] > 0, "the void must count a no-ground reject");
	Check(cells.GetStats().verdicts[PED_SITE_WATER] > 0, "the sea and the shore must count water rejects");
	Check(cells.GetStats().verdicts[PED_SITE_LEVEL] > 0, "the cliff's low probes must count level rejects");
	Check(cells.GetStats().verdicts[PED_SITE_ROOF] > 0, "the blocks must count roof rejects");
	Check(cells.GetStats().verdicts[PED_SITE_CLEARANCE] > 0, "the wall must count clearance rejects");
	Check(cells.GetStats().verdicts[PED_SITE_SLOPE] > 0, "the cliff face must count slope rejects");

	/* The sweep: every built cell, every stored point. */
	{
		int32_t standable = 0, dead = 0;
		int32_t waterSpots = 0, roofSpots = 0, cliffSpots = 0, wallSpots = 0, voidSpots = 0, beachSpots = 0;
		for (int32_t c = 0; c < PED_SITE_CELLS; c++) {
			if (!cells.CellIsBuilt((uint16_t)c))
				continue;
			if (cells.CellIsStandable((uint16_t)c))
				standable++;
			else
				dead++;
			for (int32_t i = 0; i < cells.CellGetSpots((uint16_t)c); i++) {
				const SPedSitePoint *p = cells.CellGetSpot((uint16_t)c, i);
				if (p == NULL) {
					Fail("a stored spot index must resolve");
					continue;
				}
				/* The sea and the shore must never store a point. */
				if (p->x < MAP_SHORE_EDGE)
					waterSpots++;
				/* The beach's points stand above the water line and its
				 * margin. */
				if (p->x < MAP_BEACH_EDGE && p->z <= MAP_WATER_Z + PED_SITE_WATER_MARGIN)
					waterSpots++;
				/* No point on a block's roof, on the cliff face, in the
				 * wall's footprint or in the void. */
				if (MapInBlock(p->x))
					roofSpots++;
				if (p->x >= MAP_HILL_EDGE && p->x < MAP_CLIFF_EDGE)
					cliffSpots++;
				if (MapInWall(p->x, p->y))
					wallSpots++;
				if (p->x >= MAP_VOID_EDGE)
					voidSpots++;
				if (p->x >= MAP_SHORE_EDGE && p->x < MAP_BEACH_EDGE)
					beachSpots++;
			}
		}
		Check(waterSpots == 0, "no stored point may stand in the water or at its margin");
		Check(roofSpots == 0, "no stored point may stand on a block's roof");
		Check(wallSpots == 0, "no stored point may stand in the wall's footprint");
		Check(cliffSpots == 0, "no stored point may stand on the cliff face");
		Check(voidSpots == 0, "no stored point may stand in the void patch");
		Check(standable > 0 && dead > 0, "the map must build standable and dead cells");
		Check(beachSpots > 0, "the beach must store its points");
		printf("  %d cells built (%d standable, %d dead); the sweep: no point in the water, on a roof, on the cliff, in the wall or in the void\n",
			requested, standable, dead);
	}

	/* The sunken cells answer dead, the sand and the streets answer spots.
	 * The cases sit on cell centres, so the request's level is the level the
	 * cell was built for. */
	{
		struct SCase { float x, y; bool standable; const char *what; } aCases[] = {
			{ -8.0f, 128.0f, false, "a sea cell" },
			{ 24.0f, 128.0f, true, "a beach cell" },
			{ 88.0f, 128.0f, false, "a block's cell" },
			{ 72.0f, 128.0f, true, "a street cell" },
			{ 328.0f, 128.0f, true, "a hill cell" },
			{ 408.0f, 128.0f, false, "a cliff cell" },
			{ 456.0f, 128.0f, true, "a plateau cell" },
			{ 520.0f, 128.0f, false, "a void cell" },
		};
		for (size_t i = 0; i < sizeof(aCases) / sizeof(aCases[0]); i++) {
			SPedSitePoint spot;
			EPedSiteAnswer answer;
			Check(CPedSiteCells::CellAt(aCases[i].x, aCases[i].y, &cell), "a case's cell resolves");
			answer = cells.RequestSpot(aCases[i].x, aCases[i].y, MapCellLevel(cell), &spot);
			if (aCases[i].standable) {
				Check(answer == PED_SITE_ANSWER_SPOT, aCases[i].what);
				Check(cells.CellIsStandable(cell), "a standable case's cell must read standable");
			} else {
				Check(answer == PED_SITE_ANSWER_DEAD, aCases[i].what);
			}
		}
	}

	/* The shore's own cell spans the shore and the beach (the shore strip is
	 * 8 m, narrower than a cell): the cell stands on its sand, and the point
	 * it answers with stands above the water line's margin. The water rejects
	 * of the shore's own probes are in the counters above. */
	{
		SPedSitePoint spot;
		Check(CPedSiteCells::CellAt(4.0f, 128.0f, &cell), "the shore cell resolves");
		Check(cells.RequestSpot(4.0f, 128.0f, MapCellLevel(cell), &spot) == PED_SITE_ANSWER_SPOT,
			"the shore's cell must stand on its sand");
		Check(spot.z > MAP_WATER_Z + PED_SITE_WATER_MARGIN, "the shore's answer must stand above the water");
	}

	/* A built cell's answer is one of its own stored points, and the request
	 * counts. */
	{
		SPedSitePoint spot;
		before = cells.GetStats();
		Check(CPedSiteCells::CellAt(24.0f, 128.0f, &cell), "the beach cell resolves");
		Check(cells.RequestSpot(24.0f, 128.0f, MAP_BEACH_Z, &spot) == PED_SITE_ANSWER_SPOT,
			"a built beach cell must answer");
		after = cells.GetStats();
		Check(after.requests == before.requests + 1, "a request must count");
		Check(after.answered == before.answered + 1, "an answered request must count");
		Check(spot.z > MAP_WATER_Z + PED_SITE_WATER_MARGIN, "the answered point stands above the water");
		Check(cells.CellGetSpots(cell) >= 1 && cells.CellGetSpots(cell) <= PED_SITE_SPOTS,
			"a cell keeps between one and the cap's spots");
	}

	/* A cell's dead answer counts too. */
	{
		SPedSitePoint spot;
		before = cells.GetStats();
		Check(CPedSiteCells::CellAt(88.0f, 128.0f, &cell), "the block cell resolves");
		Check(cells.RequestSpot(88.0f, 128.0f, 0.0f, &spot) == PED_SITE_ANSWER_DEAD, "a built dead cell must answer dead");
		after = cells.GetStats();
		Check(after.deadAnswers == before.deadAnswers + 1, "a dead answer must count");
		Check(after.answered == before.answered, "a dead answer must not count as answered");
	}

	printf("  the answer kinds and the counters: ok\n");
}

/* ---------------------------------------------------------------- D. the store */

static void
SectionStore(void)
{
	CPedSiteCells cells;
	SPedSitePoint spot;
	uint16_t cell, cell2;
	SPedSiteStats before, after;

	printf("D. the store (the answer kinds, the level, the queue and the cache)\n");

	/* The unvalidated cell queues and answers unknown; a second request at
	 * another level moves the queued level (the last request wins). */
	cells.Reset();
	cells.SetAnchor(100.0f, 100.0f);
	Check(CPedSiteCells::CellAt(100.0f, 100.0f, &cell), "the cell resolves");
	Check(cells.RequestSpot(100.0f, 100.0f, 0.0f, &spot) == PED_SITE_ANSWER_UNKNOWN,
		"an unvalidated cell must answer unknown");
	Check(cells.CellIsQueued(cell), "the request must queue the cell");
	Check(cells.GetNumQueued() == 1, "the queue must hold the cell");
	Check(cells.RequestSpot(100.0f, 100.0f, 40.0f, &spot) == PED_SITE_ANSWER_UNKNOWN,
		"a queued cell must answer unknown");
	{
		uint16_t popped;
		float levelZ = -1.0f;
		Check(cells.PopNearestQueued(popped, levelZ), "the queue must pop");
		Check(popped == cell && levelZ == 40.0f, "the last request's level must be the queued one");
		Check(cells.GetNumQueued() == 0, "the pop must empty the queue");
		Check(!cells.CellIsQueued(cell) && !cells.CellIsBuilt(cell), "a popped cell is unvalidated until the report");
	}

	/* The off-grid guard. */
	Check(cells.RequestSpot(1e6f, 0.0f, 0.0f, &spot) == PED_SITE_ANSWER_OFF_GRID, "a position off the grid must answer off-grid");
	Check(cells.RequestSpot(NAN, 0.0f, 0.0f, &spot) == PED_SITE_ANSWER_OFF_GRID, "a NaN position must answer off-grid");

	/* The flat map: a built cell answers a spot, and a request at another
	 * level drops the entry and re-queues it (the level staleness). */
	cells.Reset();
	cells.SetAnchor(0.0f, 0.0f);
	Check(CPedSiteCells::CellAt(0.0f, 0.0f, &cell), "the flat cell resolves");
	Check(cells.RequestSpot(0.0f, 0.0f, 0.0f, &spot) == PED_SITE_ANSWER_UNKNOWN, "the flat cell queues");
	DrainQueue(cells, FlatProbeAt);
	Check(cells.RequestSpot(0.0f, 0.0f, 0.0f, &spot) == PED_SITE_ANSWER_SPOT, "the flat cell answers a spot");
	Check(cells.CellGetLevel(cell) == 0.0f, "the entry keeps its build level");
	before = cells.GetStats();
	Check(cells.RequestSpot(0.0f, 0.0f, PED_SITE_LEVEL_MARGIN + 1.0f, &spot) == PED_SITE_ANSWER_UNKNOWN,
		"a request past the level margin must drop the entry and answer unknown");
	after = cells.GetStats();
	Check(after.cellsEvicted == before.cellsEvicted + 1, "the stale entry must count as evicted");
	Check(cells.CellIsQueued(cell), "the stale cell must be queued for the new level");
	before = after;
	DrainQueue(cells, FlatProbeAt);
	after = cells.GetStats();
	Check(after.cellsBuilt == before.cellsBuilt + 1, "the stale cell must be rebuilt");
	Check(cells.CellGetLevel(cell) == PED_SITE_LEVEL_MARGIN + 1.0f, "the rebuilt entry carries the new level");
	Check(cells.RequestSpot(0.0f, 0.0f, PED_SITE_LEVEL_MARGIN + 1.0f, &spot) == PED_SITE_ANSWER_DEAD,
		"the flat ground off the new level must answer dead");

	/* The queue's nearest-first pop: three cells, the nearest answers first. */
	cells.Reset();
	cells.SetAnchor(0.0f, 0.0f);
	Check(CPedSiteCells::CellAt(0.0f, 0.0f, &cell), "the near cell resolves");
	Check(CPedSiteCells::CellAt(64.0f, 0.0f, &cell2), "the far cell resolves");
	RequestAt(cells, 64.0f, 0.0f, 0.0f);
	RequestAt(cells, 0.0f, 0.0f, 0.0f);
	RequestAt(cells, 32.0f, 0.0f, 0.0f);
	{
		uint16_t popped;
		float levelZ;
		Check(cells.GetNumQueued() == 3, "the three cells must queue");
		Check(cells.PopNearestQueued(popped, levelZ) && popped == cell, "the nearest cell must pop first");
		Check(cells.PopNearestQueued(popped, levelZ), "the second cell pops");
		Check(cells.PopNearestQueued(popped, levelZ) && popped == cell2, "the farthest cell must pop last");
	}

	/* The full queue: a nearer admission displaces the farthest, a farther
	 * one is refused. */
	cells.Reset();
	cells.SetAnchor(0.0f, 0.0f);
	{
		int32_t queued = 0;
		uint16_t farCell = 0, nearCell, refusedCell;
		for (float y = 0.0f; y < 32.0f * PED_SITE_CELL_SIZE && queued < PED_SITE_QUEUE; y += PED_SITE_CELL_SIZE) {
			for (float x = 0.0f; x < 32.0f * PED_SITE_CELL_SIZE && queued < PED_SITE_QUEUE; x += PED_SITE_CELL_SIZE) {
				float cx = x + 1.0f, cy = y + 1.0f;
				if (queued == PED_SITE_QUEUE - 1)
					Check(CPedSiteCells::CellAt(cx, cy, &farCell), "the far cell resolves");
				RequestAt(cells, cx, cy, 0.0f);
				queued++;
			}
		}
		Check(cells.GetNumQueued() == PED_SITE_QUEUE, "the queue must fill");
		/* A nearer cell than every queued one: it displaces the farthest. */
		Check(CPedSiteCells::CellAt(-4.0f, -4.0f, &nearCell), "the near admission resolves");
		before = cells.GetStats();
		Check(cells.RequestSpot(-4.0f, -4.0f, 0.0f, &spot) == PED_SITE_ANSWER_UNKNOWN, "the admission answers unknown");
		after = cells.GetStats();
		Check(after.cellsDropped == before.cellsDropped + 1, "the displaced cell must count as dropped");
		Check(cells.GetNumQueued() == PED_SITE_QUEUE, "the queue stays full");
		Check(cells.CellIsQueued(nearCell), "the nearer cell must be queued");
		Check(!cells.CellIsQueued(farCell), "the farthest cell must be displaced");
		/* A farther cell than every queued one: refused. */
		Check(CPedSiteCells::CellAt(400.0f, 400.0f, &refusedCell), "the far admission resolves");
		before = cells.GetStats();
		Check(cells.RequestSpot(400.0f, 400.0f, 0.0f, &spot) == PED_SITE_ANSWER_UNKNOWN, "the far admission answers unknown");
		after = cells.GetStats();
		Check(after.cellsDropped == before.cellsDropped + 1, "the refused admission must count as dropped");
		Check(!cells.CellIsQueued(refusedCell), "the farther cell must not be queued");
	}

	/* The cache: PED_SITE_CACHE + 10 cells build; the farthest evict and go
	 * back to unvalidated; the next request queues them again. The anchor
	 * sits at the block's south-west corner, so the far corner is what goes.
	 * The requests come in slices (the queue's cap is smaller than the cache's
	 * cap). */
	cells.Reset();
	cells.SetAnchor(0.0f, 0.0f);
	{
		int32_t asked = 0;
		uint16_t farCell, lastCell;
		for (float y = 0.0f; y < 33.0f * PED_SITE_CELL_SIZE && asked < PED_SITE_CACHE + 10; y += PED_SITE_CELL_SIZE) {
			for (float x = 0.0f; x < 33.0f * PED_SITE_CELL_SIZE && asked < PED_SITE_CACHE + 10; x += PED_SITE_CELL_SIZE) {
				RequestAt(cells, x + 1.0f, y + 1.0f, 0.0f);
				asked++;
				if (cells.GetNumQueued() >= 256)
					DrainQueue(cells, FlatProbeAt);
			}
		}
		Check(asked == PED_SITE_CACHE + 10, "the cache stream asks for the cap plus ten");
		DrainQueue(cells, FlatProbeAt);
		Check(cells.GetNumCached() == PED_SITE_CACHE, "the cache holds its cap");
		/* The first cap's asks are the rows 0..480 of the block (31 rows of
		 * 33) plus one: the farthest of them is the row's east end, the cell
		 * at (512, 480) - the one the first eviction takes. The last ask is
		 * the row 496's own column 160, the cell at (160, 496). */
		Check(CPedSiteCells::CellAt(520.0f, 488.0f, &farCell), "the far cell resolves");
		Check(CPedSiteCells::CellAt(168.0f, 504.0f, &lastCell), "the last cell resolves");
		Check(!cells.CellIsBuilt(farCell), "the farthest cell must be evicted");
		Check(cells.CellIsBuilt(lastCell), "the last cell stays cached");
		before = cells.GetStats();
		{
			float fx, fy;
			CPedSiteCells::CellCentre(farCell, &fx, &fy);
			Check(cells.RequestSpot(fx, fy, 0.0f, &spot) == PED_SITE_ANSWER_UNKNOWN,
				"an evicted cell must answer unknown and queue again");
		}
		after = cells.GetStats();
		Check(after.cellsQueued == before.cellsQueued + 1, "the re-request must count as a new admission");
	}

	/* The reads: an unbuilt cell reads nothing, a built dead one reads dead. */
	cells.Reset();
	Check(!cells.CellIsBuilt((uint16_t)0) && !cells.CellIsQueued((uint16_t)0), "a fresh cell reads unvalidated");
	Check(cells.CellGetSpots((uint16_t)0) == 0, "a fresh cell has no spots");
	Check(cells.CellGetSpot((uint16_t)0, 0) == NULL, "a fresh cell's spot is nil");
	printf("  unknown/dead/off-grid, the level staleness, nearest-first, the full queue and the cache: ok\n");
}

/* ----------------------------------------------------------------- E. the cost */

#define COST_N 200000

static volatile int g_sink;
static float ms_aCostX[COST_N];
static float ms_aCostY[COST_N];

static void
CostSetup(void)
{
	for (int32_t i = 0; i < COST_N; i++) {
		/* The build map's span: the draws land in different cells, so the
		 * requests and the reports are not one cell. */
		ms_aCostX[i] = RandFloat(MAP_MIN_X, MAP_MAX_X);
		ms_aCostY[i] = RandFloat(MAP_MIN_Y, MAP_MAX_Y);
	}
}

/* Every cell of the build map, built: the request stream of the cost section
 * hits built cells (the ring of a played frame). */
static void
CostBuildMap(CPedSiteCells &cells)
{
	cells.Reset();
	cells.SetAnchor(256.0f, 128.0f);
	for (float y = MAP_MIN_Y; y < MAP_MAX_Y; y += PED_SITE_CELL_SIZE)
		for (float x = MAP_MIN_X; x < MAP_MAX_X; x += PED_SITE_CELL_SIZE) {
			float cx = x + PED_SITE_CELL_SIZE * 0.5f;
			float cy = y + PED_SITE_CELL_SIZE * 0.5f;
			RequestAt(cells, cx, cy, MapLevelAt(cx, cy));
		}
	DrainQueue(cells, MapProbeAt);
}

static void
SectionCost(void)
{
	CPedSiteCells cells;
	SPedSitePoint spot;
	double t0, t1;
	int32_t i;

	printf("E. the bounded build (the frame budget and the arithmetic; %d operations per stream)\n", COST_N);
	CostSetup();

	/* The frame budget: a queue of 100, one pump spends the cap. */
	cells.Reset();
	cells.SetAnchor(0.0f, 0.0f);
	for (i = 0; i < 100; i++)
		RequestAt(cells, (float)(i % 10) * PED_SITE_CELL_SIZE + 1.0f,
			(float)(i / 10) * PED_SITE_CELL_SIZE + 1.0f, 0.0f);
	Check(cells.GetNumQueued() == 100, "the budget stream queues 100 cells");
	Check(BuildQueue(cells, FlatProbeAt, PED_SITE_CELLS_PER_FRAME) == PED_SITE_CELLS_PER_FRAME,
		"one pump must spend exactly the cap");
	Check(cells.GetNumQueued() == 100 - PED_SITE_CELLS_PER_FRAME, "the pump must take the cap off the queue");

	/* A request on a built cell: the ring is built, the draws hit it. */
	CostBuildMap(cells);
	t0 = NowSeconds();
	for (i = 0; i < COST_N; i++)
		g_sink += cells.RequestSpot(ms_aCostX[i], ms_aCostY[i], 0.0f, &spot);
	t1 = NowSeconds();
	printf("  a request on a built ring:         %6.1f ns\n", (t1 - t0) / COST_N * 1e9);

	/* A report into a full cache: every report is a new cell, so every
	 * insert evicts (the 1024-entry farthest scan is the worst case). */
	{
		static SPedSiteProbe aProbes[PED_SITE_PROBES];
		for (i = 0; i < PED_SITE_PROBES; i++)
			aProbes[i] = PassProbe();
		cells.Reset();
		cells.SetAnchor(0.0f, 0.0f);
		for (i = 0; i < PED_SITE_CACHE; i++)
			cells.ReportCellProbed((uint16_t)i, 5.0f, aProbes);
		t0 = NowSeconds();
		for (i = 0; i < COST_N; i++)
			cells.ReportCellProbed((uint16_t)(PED_SITE_CACHE + i % (PED_SITE_CELLS - PED_SITE_CACHE)), 5.0f, aProbes);
		t1 = NowSeconds();
		g_sink += cells.GetNumCached();
		printf("  a report into a full cache:        %6.1f ns\n", (t1 - t0) / COST_N * 1e9);
	}

	/* A report that lands on the entry the cell already has: the cache
	 * lookup's own path, no eviction. */
	{
		static SPedSiteProbe aProbes[PED_SITE_PROBES];
		for (i = 0; i < PED_SITE_PROBES; i++)
			aProbes[i] = PassProbe();
		cells.Reset();
		cells.SetAnchor(0.0f, 0.0f);
		t0 = NowSeconds();
		for (i = 0; i < COST_N; i++)
			cells.ReportCellProbed((uint16_t)(i % 100), 5.0f, aProbes);
		t1 = NowSeconds();
		g_sink += cells.GetNumCached();
		printf("  a report on its own entry:         %6.1f ns\n", (t1 - t0) / COST_N * 1e9);
	}

	/* A frame in a built ring (the steady state): the anchor and the frame's
	 * two requests; the cells are cached, so nothing queues or builds. */
	{
		static SPedSiteProbe aProbes[PED_SITE_PROBES];
		for (i = 0; i < PED_SITE_PROBES; i++)
			aProbes[i] = PassProbe();
		CostBuildMap(cells);
		t0 = NowSeconds();
		for (i = 0; i < COST_N; i++) {
			cells.SetAnchor(ms_aCostX[i], ms_aCostY[i]);
			for (int32_t b = 0; b < PED_SITE_CELLS_PER_FRAME; b++)
				g_sink += cells.RequestSpot(ms_aCostX[i] + b, ms_aCostY[i], 0.0f, &spot);
		}
		t1 = NowSeconds();
		printf("  a frame in a built ring:           %6.1f ns\n", (t1 - t0) / COST_N * 1e9);
	}

	/* A frame with fresh cells (the busy case): every request is a cell the
	 * store has not built - it queues, the pump's pop builds it and the
	 * insert stores it. The full cache's own price (the eviction scan) is the
	 * row above; the ring that stays cached is the row above that. */
	{
		static SPedSiteProbe aProbes[PED_SITE_PROBES];
		int32_t cursor = 0;
		for (i = 0; i < PED_SITE_PROBES; i++)
			aProbes[i] = PassProbe();
		CostBuildMap(cells);
		t0 = NowSeconds();
		for (i = 0; i < COST_N; i++) {
			cells.SetAnchor(ms_aCostX[i], ms_aCostY[i]);
			for (int32_t b = 0; b < PED_SITE_CELLS_PER_FRAME; b++) {
				uint16_t cell;
				float levelZ, cx, cy;
				/* A cell of the map's far side, a fresh one each step. */
				cursor = (cursor + 1) % (PED_SITE_CELLS / 2);
				CPedSiteCells::CellCentre((uint16_t)(PED_SITE_CELLS / 2 + cursor), &cx, &cy);
				g_sink += cells.RequestSpot(cx, cy, 0.0f, &spot);
				if (cells.PopNearestQueued(cell, levelZ))
					cells.ReportCellProbed(cell, levelZ, aProbes);
			}
		}
		t1 = NowSeconds();
		g_sink += cells.GetNumCached() + cells.GetNumQueued();
		printf("  a frame with fresh cells (2):      %6.1f ns\n", (t1 - t0) / COST_N * 1e9);
	}

	/* The world's side is a constant: the cap times the lattice times the
	 * four queries of a probe (the ground and the roof casts, the water line,
	 * the clearance sphere). The old path's worst frame is the hidden
	 * burst's (FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME = 100 attempts, the
	 * 0.15 share, the 5 positions of one spawn and its 5 queries - the
	 * casts, the line, the sphere and the node search); the trickle's is
	 * the sixth of it. The cells' frame is below both and does not move
	 * with the crowd. */
	{
		int32_t probes = PED_SITE_CELLS_PER_FRAME * PED_SITE_PROBES;
		int32_t queries = probes * 4;
		int32_t oldBurst = (int32_t)(100 * 0.15f * 5 * 5);
		int32_t oldTrickle = (int32_t)(6 * 0.15f * 5 * 5);
		printf("  the world budget per frame:        %d probes = %d queries (the cap x %d); the old path: %d (hidden burst), %d (trickle)\n",
			probes, queries, PED_SITE_PROBES, oldBurst, oldTrickle);
	}

	printf("  sizeof(CPedSiteCells):             %6.1f KB (the state %d x 2 B, the cache %d x 56 B, the queue %d x 6 B)\n",
		sizeof(CPedSiteCells) / 1024.0, PED_SITE_CELLS, PED_SITE_CACHE, PED_SITE_QUEUE);
}

/* ------------------------------------------------------------- F. the coverage */

/*
 * The coverage map: a flat coastal strip and an offshore airfield, with the
 * pedestrian nodes only along the promenade and the city's streets.
 *
 *	x < 0		the sea
 *	0 .. 8		the shore (under the water line)
 *	8 .. 208	the beach (the sand at 1.2): the sand west of 58 m is
 *			past the node reach of the promenade (at 200 m)
 *	208 .. 384	the city (the street at 0, blocks with a 20 m roof every
 *			64 m from 224); the nodes along the street and the
 *			promenade
 *	384 .. 592	the sea between the city and the airfield
 *	592 .. 752	the airfield (a flat field at 0.5, no nodes: more than
 *			200 m from the city's last node)
 *
 * The sampler draws the VICE-122 way (an angle, a distance over the off-screen
 * band's [35, 130] union - the common case, the spawn is behind the player),
 * keeps only the draws whose point lands in the area, and asks the old policy
 * and the cells the same question: does this draw produce a standable position
 * in the area? The old policy is the VICE-122 checks (the predicate without
 * the slope - the old path had none - the invisible band on the drawn point,
 * and the node reach: a node within 150 m and 5 m of the level), the cells
 * are the built ring's answer with the caller's band and level rules.
 *
 * One reading note on the table: the cells snap the draw to a stored point of
 * the drawn cell, so a point near the band's or the area's edge can land on
 * the other side of it. A standable area that was already fully covered (the
 * near beach) therefore reads a few points lower than the old policy, while
 * the areas beyond the node reach (the far sand, the airfield) go from
 * nothing to the cells' share - the ticket's own measure.
 */
#define COV_MAX_DIST	130.0f
#define COV_MIN_DIST_OFFSCREEN 35.0f
#define COV_MAX_DIST_OFFSCREEN 65.0f
#define COV_LEVEL	1.0f

struct SCovNode
{
	float x, y, z;
};

static SCovNode g_aNodes[] = {
	/* The promenade, along the beach's east edge. */
	{ 200.0f, 8.0f, 1.2f }, { 200.0f, 48.0f, 1.2f }, { 200.0f, 88.0f, 1.2f },
	{ 200.0f, 128.0f, 1.2f }, { 200.0f, 168.0f, 1.2f }, { 200.0f, 208.0f, 1.2f },
	{ 200.0f, 248.0f, 1.2f },
	/* The city's street. */
	{ 208.0f, 8.0f, 0.0f }, { 248.0f, 8.0f, 0.0f }, { 288.0f, 8.0f, 0.0f },
	{ 328.0f, 8.0f, 0.0f }, { 368.0f, 8.0f, 0.0f },
};

/* The old path's node rule: the closest node within 150 m, on the level
 * within 5 m. The harness's table is small, so the closest is a scan (the
 * game's own search is the node grid's walk). */
static bool
CovNodeReachable(float x, float y, float z)
{
	float best = 1.0e9f;
	float bestZ = 0.0f;

	for (size_t i = 0; i < sizeof(g_aNodes) / sizeof(g_aNodes[0]); i++) {
		float dx = g_aNodes[i].x - x;
		float dy = g_aNodes[i].y - y;
		float d = dx * dx + dy * dy;
		if (d < best) {
			best = d;
			bestZ = g_aNodes[i].z;
		}
	}
	if (best > 150.0f * 150.0f)
		return false;
	return fabsf(bestZ - z) <= 5.0f;
}

static bool
CovInBlock(float x)
{
	int32_t k;
	float off;

	if (x < 208.0f || x >= 384.0f)
		return false;
	k = (int32_t)((x - 208.0f) / 64.0f);
	off = x - (208.0f + k * 64.0f);
	return off >= 16.0f && off < 48.0f;
}

static void
CovProbeAt(float x, float y, float levelZ, SPedSiteProbe &probe)
{
	(void)levelZ;
	(void)y;
	probe.clearance = true;
	probe.waterFound = false;
	probe.waterZ = 0.0f;
	probe.groundNormalZ = 1.0f;
	probe.surfaceFound = true;
	if (x < 0.0f) {
		probe.groundFound = true;
		probe.groundZ = -6.0f;
		probe.surfaceZ = -6.0f;
		probe.waterFound = true;
		probe.waterZ = 0.5f;
	} else if (x < 8.0f) {
		probe.groundFound = true;
		probe.groundZ = 0.35f;
		probe.surfaceZ = 0.35f;
		probe.waterFound = true;
		probe.waterZ = 0.5f;
	} else if (x < 208.0f) {
		probe.groundFound = true;
		probe.groundZ = 1.2f;
		probe.surfaceZ = 1.2f;
		probe.waterFound = true;
		probe.waterZ = 0.5f;
	} else if (x < 384.0f) {
		probe.groundFound = true;
		probe.groundZ = 0.0f;
		probe.surfaceZ = CovInBlock(x) ? 20.0f : 0.0f;
	} else if (x < 592.0f) {
		probe.groundFound = true;
		probe.groundZ = -6.0f;
		probe.surfaceZ = -6.0f;
		probe.waterFound = true;
		probe.waterZ = 0.5f;
	} else if (x < 752.0f) {
		probe.groundFound = true;
		probe.groundZ = 0.5f;
		probe.surfaceZ = 0.5f;
	} else {
		probe.groundFound = false;
		probe.surfaceFound = false;
	}
}

/* The old policy's acceptance at one drawn point: the VICE-122 checks (the
 * invisible band's own rule on the drawn distance - the harness samples the
 * off-screen case), without the slope, with the node reach. */
static bool
CovOldAccepts(float x, float y, float dist)
{
	SPedSiteProbe probe;
	EPedSiteVerdict verdict;

	if (!(COV_MIN_DIST_OFFSCREEN < dist && dist < COV_MAX_DIST_OFFSCREEN))
		return false;
	CovProbeAt(x, y, COV_LEVEL, probe);
	verdict = PedSiteEvaluateProbe(COV_LEVEL, probe, NULL);
	if (verdict == PED_SITE_SLOPE)
		verdict = PED_SITE_PASS;	/* the old path had no slope rule */
	if (verdict != PED_SITE_PASS)
		return false;
	if (!CovNodeReachable(x, y, probe.groundZ))
		return false;
	return true;
}

struct SCovArea
{
	const char *name;
	float minX, maxX, minY, maxY;
	bool expectEmptyBefore;		/* no node in reach: the old policy is nothing there */
	int32_t minAfterPercent;	/* the cells' floor: what a standable area answers */
};

static SCovArea g_aAreas[] = {
	{ "the sea",       -192.0f, 0.0f,   0.0f, 256.0f, true,  0 },
	{ "the far sand",  8.0f,    48.0f,  0.0f, 256.0f, true,  25 },
	{ "the near beach", 48.0f,  208.0f, 0.0f, 256.0f, false, 25 },
	{ "the city",      208.0f,  384.0f, 0.0f, 256.0f, false, 15 },
	{ "the airfield",  592.0f,  752.0f, 0.0f, 256.0f, true,  25 },
};

/* The band's cells around a centre, built once per area. */
static void
CoverageBuildRing(CPedSiteCells &cells, float centreX, float centreY)
{
	uint16_t minCell, maxCell;
	float minX = centreX - COV_MAX_DIST_OFFSCREEN - PED_SITE_CELL_SIZE;
	float maxX = centreX + COV_MAX_DIST_OFFSCREEN + PED_SITE_CELL_SIZE;
	float minY = centreY - COV_MAX_DIST_OFFSCREEN - PED_SITE_CELL_SIZE;
	float maxY = centreY + COV_MAX_DIST_OFFSCREEN + PED_SITE_CELL_SIZE;
	float x, y;

	cells.Reset();
	cells.SetAnchor(centreX, centreY);
	CPedSiteCells::CellAt(minX, minY, &minCell);
	CPedSiteCells::CellAt(maxX, maxY, &maxCell);
	for (int32_t cy = minCell / PED_SITE_CELLS_X; cy <= (int32_t)(maxCell / PED_SITE_CELLS_X); cy++)
		for (int32_t cx = minCell % PED_SITE_CELLS_X; cx <= (int32_t)(maxCell % PED_SITE_CELLS_X); cx++) {
			uint16_t cell = (uint16_t)(cy * PED_SITE_CELLS_X + cx);
			CPedSiteCells::CellCentre(cell, &x, &y);
			RequestAt(cells, x, y, COV_LEVEL);
		}
	DrainQueue(cells, CovProbeAt);
}

static void
SectionCoverage(void)
{
	printf("F. the coverage (the same seeded draws, the old policy and the cells)\n");
	printf("  %-14s %6s %10s %10s %10s\n", "area", "cells", "standable", "before", "after");

	for (size_t a = 0; a < sizeof(g_aAreas) / sizeof(g_aAreas[0]); a++) {
		SCovArea &area = g_aAreas[a];
		CPedSiteCells cells;
		float centreX = (area.minX + area.maxX) * 0.5f;
		float centreY = (area.minY + area.maxY) * 0.5f;
		int32_t samples = 0, oldPass = 0, newPass = 0, cellsInArea = 0, standable = 0;
		uint32_t savedSeed = g_seed;

		CoverageBuildRing(cells, centreX, centreY);

		for (int32_t c = 0; c < PED_SITE_CELLS; c++) {
			float x, y;
			if (!cells.CellIsBuilt((uint16_t)c))
				continue;
			CPedSiteCells::CellCentre((uint16_t)c, &x, &y);
			if (x < area.minX || x >= area.maxX || y < area.minY || y >= area.maxY)
				continue;
			cellsInArea++;
			if (cells.CellIsStandable((uint16_t)c))
				standable++;
		}

		/* The same draws through both policies: only the draws that land in
		 * the area count (the area's own coverage). */
		g_seed = savedSeed;
		for (int32_t i = 0; i < 200000; i++) {
			float angle = RandFloat(0.0f, 2.0f * PI);
			float dist = RandFloat(COV_MIN_DIST_OFFSCREEN, COV_MAX_DIST);
			float x = centreX + dist * sinf(angle);
			float y = centreY + dist * cosf(angle);
			bool oldOk, newOk = false;

			if (x < area.minX || x >= area.maxX || y < area.minY || y >= area.maxY)
				continue;
			samples++;

			oldOk = CovOldAccepts(x, y, dist);

			/* The cells: the drawn position's answer, the caller's band
			 * and level rules on the returned point. */
			{
				SPedSitePoint spot;
				if (cells.RequestSpot(x, y, COV_LEVEL, &spot) == PED_SITE_ANSWER_SPOT) {
					float spotDist = sqrtf((spot.x - centreX) * (spot.x - centreX) +
						(spot.y - centreY) * (spot.y - centreY));
					if (spotDist > COV_MIN_DIST_OFFSCREEN && spotDist < COV_MAX_DIST_OFFSCREEN &&
						fabsf(spot.z - COV_LEVEL) <= PED_SITE_LEVEL_MARGIN &&
						spot.x >= area.minX && spot.x < area.maxX &&
						spot.y >= area.minY && spot.y < area.maxY)
						newOk = true;
				}
			}

			if (oldOk)
				oldPass++;
			if (newOk)
				newPass++;
		}

		if (samples > 0) {
			printf("  %-14s %6d %9.1f%% %9.1f%% %9.1f%%\n", area.name, cellsInArea,
				cellsInArea > 0 ? 100.0f * standable / cellsInArea : 0.0f,
				100.0f * oldPass / samples, 100.0f * newPass / samples);
			/* The ticket's acceptance: the areas beyond the node reach (the
			 * far sand, the airfield) are nothing under the old policy and
			 * stand on validated ground through the cells; the sea is
			 * refused by both. */
			if (area.expectEmptyBefore)
				Check(oldPass == 0, "an area beyond the node reach must be empty under the old policy");
			if (area.minAfterPercent > 0)
				Check(newPass * 100 / samples >= area.minAfterPercent, "the cells must answer in the area");
			if (strcmp(area.name, "the sea") == 0)
				Check(oldPass == 0 && newPass == 0, "the sea must stay empty both ways");
		}
	}

	printf("  the per-spawn share of five attempts is 1 - (1 - p)^5 of the rows above\n");
}

/* ------------------------------------------------------------------ main */

int
main(void)
{
	printf("The pedestrian standable cells (VICE-154): the grid, the predicate, the build, the store, the cost, the coverage.\n\n");
	SectionGrid();
	printf("\n");
	SectionPredicate();
	printf("\n");
	SectionBuild();
	printf("\n");
	SectionStore();
	printf("\n");
	SectionCost();
	printf("\n");
	SectionCoverage();

	printf("\nchecks: %lld\n", g_checks);
	if (g_failures == 0)
		printf("RESULT: every check passed.\n");
	else
		printf("RESULT: %d check(s) failed.\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
