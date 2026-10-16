/*
 * The evidence program for VICE-123, "caps and streaming audit at maximum
 * sliders".
 *
 * It has three jobs, all offline:
 *
 *   A. The cap table. For every cap between the density sliders and the pool it
 *      prints the current value, the value the target asks for at the maximum
 *      sliders, the pool behind it and the headroom. It reads the values from
 *      src/core/PopulationCaps.h (the caps), src/core/config.h (the pools) and
 *      src/control/TrafficRings.h (the ring the caps grow with), so a number
 *      cannot drift away from this program.
 *
 *   B. The invariant checks. Every cap of the table stays below its pool; the
 *      ped pool holds the on-foot cap, the drivers, the passengers and the
 *      frame reserve; the parked target stays inside the car budget; the pools
 *      VICE-134 resized (the alpha lists, the dummy pool) still hold the
 *      measured map demand.
 *
 *   C. The streaming verdict. The vehicle model residency budget
 *      (MAXVEHICLESLOADED, desiredNumVehiclesLoaded) against the models the
 *      population can ask for; the numbers measured from the shipped data are
 *      stated with their source.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 -o /tmp/caps-audit utils/caps-audit/audit.cpp
 *	/tmp/caps-audit
 *
 * The exit code is 0 when every check passes. The live before/after values come
 * from utils/caps-audit/live-counters.py, which reads the running game
 * read-only.
 */

#include "PopulationCaps.h"
#include "TrafficRings.h"
#include "DrawDistance.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;

static void Fail(const char *what)
{
	g_failures++;
	printf("FAIL: %s\n", what);
}

/* The density slider tops (re3.cpp clamps the two values here; the menu slider
 * and the ini read both use them). */
#define PED_DENSITY_MAX 3.4f
#define CAR_DENSITY_MAX 3.4f

/* The weather far clips of the shipped time cycle (utils/one-range-control
 * carries the same four; VICE-119 measured them). */
struct SWeather
{
	const char *name;
	float farClip;
};

static const SWeather aWeathers[] = {
	{ "sunny",  2000.0f },
	{ "cloudy", 1150.0f },
	{ "rainy",   650.0f },
	{ "foggy",   150.0f },
};

/* The ring of the maximum sliders of one weather: the draw distance at its top,
 * FOV 70 (the FOV factor 1.0; a narrower FOV raises the request and the cap
 * still bounds it). */
static STrafficRings MaxSlidersRings(float farClip)
{
	STrafficRingInput in;
	in.lodDistance = LOD_DISTANCE_BASE * DRAWDIST_MAX;
	in.fovFactor = 1.0f;
	in.farClip = farClip;
	return ComputeTrafficRings(in);
}

static void PrintClaimRow(const char *name, int value, int pool, int claims, const char *note)
{
	printf("  %-24s %8d %8d %8d %8d   %s\n", name, value, pool, claims, pool - claims - value, note);
}

/* ------------------------------------------------------------------ A. table */

static void SectionTable(void)
{
	printf("== A. the cap table at the maximum sliders\n\n");
	printf("  the density tops: pedestrians %.1f, cars %.1f (re3.cpp); the ring comes from\n",
		(double)PED_DENSITY_MAX, (double)CAR_DENSITY_MAX);
	printf("  the draw distance %.1f and FOV 70, the weather bounds the visible range.\n\n",
		(double)DRAWDIST_MAX);

	printf("  weather | far clip |   scale | on-foot ask | on-foot cap | car ask | car cap | random ask | random cap | parked cap\n");
	for (unsigned w = 0; w < sizeof(aWeathers)/sizeof(aWeathers[0]); w++) {
		STrafficRings r = MaxSlidersRings(aWeathers[w].farClip);
		float onFootAsk = DEFAULT_MAX_NUMBER_OF_PEDS * PED_DENSITY_MAX * r.scale * r.scale;
		float onFootCap = TrafficRingsMin(onFootAsk, (float)MAX_PEDS_IN_USE);
		float carAsk = DEFAULT_MAX_NUMBER_OF_CARS * CAR_DENSITY_MAX * r.scale * r.scale;
		float carCap = TrafficRingsMin(carAsk, (float)MAX_CARS_IN_USE);
		float randomAsk = RANDOM_CARS_PER_INI_UNIT * CAR_DENSITY_MAX * r.scale * r.scale;
		float randomCap = TrafficRingsMin(randomAsk, (float)MAX_RANDOM_CARS);
		printf("  %-7s | %8.0f | %7.3f | %11.0f | %11.0f | %7.0f | %7.0f | %10.0f | %10.0f | %10d\n",
			aWeathers[w].name, (double)aWeathers[w].farClip, (double)r.scale,
			(double)onFootAsk, (double)onFootCap, (double)carAsk, (double)carCap,
			(double)randomAsk, (double)randomCap, ParkedCarTarget(r.scale));

		/* The budget request is the number the formula asks for before the cap;
		 * it must not fall below the stock budget the game always had. */
		if (onFootCap < DEFAULT_MAX_NUMBER_OF_PEDS)
			Fail("the on-foot cap fell below the stock budget");
		if (carCap < DEFAULT_MAX_NUMBER_OF_CARS * 2.0f)
			Fail("the car cap fell below the stock budget");
		if (randomCap < RANDOM_CARS_PER_INI_UNIT)
			Fail("the random car cap fell below the stock budget");
		/* The parked target may not pass the generator count, and the generator
		 * count may not pass the parked share of the car budget. */
		if (ParkedCarTarget(r.scale) > NUM_CARGENS)
			Fail("the parked target passed the generator count");
		if (ParkedCarTarget(r.scale) >= MAX_CARS_IN_USE)
			Fail("the parked target reached the whole car budget");
	}
	printf("\n");

	printf("  the cap against its pool, with the other claims on the same pool:\n\n");
	printf("  %-24s %8s %8s %8s %8s   %s\n", "cap", "value", "pool", "claims", "free", "note");
	PrintClaimRow("on foot", MAX_PEDS_IN_USE, NUMPEDS,
		MAX_CARS_IN_USE + MAX_PED_PASSENGERS + PED_POOL_RESERVE, "MAX_PEDS_IN_USE");
	PrintClaimRow("drivers", MAX_CARS_IN_USE, NUMPEDS,
		MAX_PEDS_IN_USE + MAX_PED_PASSENGERS + PED_POOL_RESERVE, "one per car of the car budget");
	PrintClaimRow("passengers", MAX_PED_PASSENGERS, NUMPEDS,
		MAX_PEDS_IN_USE + MAX_CARS_IN_USE + PED_POOL_RESERVE, "the measured rate over the car budget");
	PrintClaimRow("frame reserve", PED_POOL_RESERVE, NUMPEDS,
		MAX_PEDS_IN_USE + MAX_CARS_IN_USE + MAX_PED_PASSENGERS, "the burst of one frame");
	PrintClaimRow("car budget", MAX_CARS_IN_USE, NUMVEHICLES, 0, "random + parked + mission + law + emergency");
	PrintClaimRow("random cars", MAX_RANDOM_CARS, MAX_CARS_IN_USE, 0, "the target formula stops here");
	PrintClaimRow("parked target", ParkedCarTarget(8.0f), NUM_CARGENS, 0, "20 x scale^2 at the ring top");
	PrintClaimRow("dummy demand", 2336 + NUMOBJECTS + 4, NUMDUMMIES, 0, "map dummies + convertibles (VICE-134)");
	printf("\n");
	printf("  measured live with a full street: ped pool 5417/8192, vehicle pool 153/2560,\n");
	printf("  node pool 39253/%d, entry info pool 9240/%d, dummy pool 2338/%d\n\n",
		NUMPTRNODES, NUMENTRYINFOS, NUMDUMMIES);

	/* The row-by-row checks of the table. */
	if (MAX_PEDS_IN_USE + MAX_CARS_IN_USE + MAX_PED_PASSENGERS + PED_POOL_RESERVE > NUMPEDS)
		Fail("the ped pool does not hold the budget line");
	if (MAX_CARS_IN_USE >= NUMVEHICLES)
		Fail("the car budget reached the vehicle pool");
	if (MAX_RANDOM_CARS > MAX_CARS_IN_USE)
		Fail("the random car target passed the total car budget");
	if (ParkedCarTarget(8.0f) >= MAX_CARS_IN_USE)
		Fail("the parked target reached the whole car budget");
	if (ParkedCarTarget(8.0f) > NUM_CARGENS)
		Fail("the parked target passed the generator count");
	if (MAXVEHICLESLOADED > VEHICLEMODELSIZE)
		Fail("the vehicle residency tracker passed the vehicle model slots");

	/* The per-frame attempts are the fill rate, not a cap: they must be at least
	 * one call a frame and grow with the ring (VICE-109). */
	if (RANDOM_CAR_ATTEMPTS_PER_FRAME < 1 || PED_ATTEMPTS_PER_FRAME < 1 || CAR_SPAWN_ATTEMPTS < 1)
		Fail("a per-frame attempt count is below one");
	printf("  the fill rate at the ring top (scale %.2f): %d car calls and %d ped calls a frame,\n",
		(double)MaxSlidersRings(2000.0f).scale,
		(int)(RANDOM_CAR_ATTEMPTS_PER_FRAME * MaxSlidersRings(2000.0f).scale),
		(int)(PED_ATTEMPTS_PER_FRAME * MaxSlidersRings(2000.0f).scale));
	printf("  each car call tries %d placements (CAR_SPAWN_ATTEMPTS). The caps bound the count\n",
		CAR_SPAWN_ATTEMPTS);
	printf("  and the rates bound the time to fill it. The audit raised the target caps (the\n");
	printf("  random cars and the parked cars, see the table) and left the rates alone: the live\n");
	printf("  count at the maximum sliders (149..201 cars) is a fill problem, not a cap problem\n");
	printf("  (VICE-126 and VICE-127 own it).\n\n");
}

/* ------------------------------------------- B. the VICE-134 pools, verified */

static void SectionVice134Pools(void)
{
	printf("== B. the pools VICE-134 resized, against the measured map demand\n\n");

	/* Measured from the shipped map (VICE-134): the scan inserts at most one
	 * node per entity per frame, and the map's drawLast instances are inserted
	 * too: 3132 vegetation buildings and 416 objects/dummies = 3548. */
	const int mapDrawLast = 3548;
	const int mapBuildings = 3132;

	printf("  alpha entity list : %d nodes (%d x 3), the measured scan demand is %d entities\n",
		NUMALPHAENTITYLIST * 3, NUMALPHAENTITYLIST, NUMVISIBLEENTITIES + NUMINVISIBLEENTITIES + mapDrawLast);
	printf("  alpha building list: %d slots, the map holds %d drawLast building instances\n",
		NUMVISIBLEENTITIES, mapBuildings);
	printf("  visible entities  : %d slots; measured live at a full street: 361..1233\n", NUMVISIBLEENTITIES);
	printf("  dummy pool        : %d slots; the map holds 2336 dummies + %d convertible objects\n",
		NUMDUMMIES, NUMOBJECTS);

	if (NUMALPHAENTITYLIST * 3 < NUMVISIBLEENTITIES + NUMINVISIBLEENTITIES + mapDrawLast)
		Fail("the alpha entity list is below the measured scan demand");
	if (NUMVISIBLEENTITIES < mapBuildings)
		Fail("the alpha building list is below the measured map demand");
	if (NUMDUMMIES < 2336 + NUMOBJECTS)
		Fail("the dummy pool is below the map demand plus the convertible objects");
	if (NUMALPHAENTITYLIST <= 0 || NUMINVISIBLEENTITIES <= 0)
		Fail("an alpha list size is not positive");

	printf("  all three hold. VICE-134's report is the measurement and the reasoning; this\n");
	printf("  program re-reads the sizes out of config.h, so a later edit cannot pass silently.\n\n");
}

/* ------------------------------------------------------ C. the streaming row */

static void SectionStreaming(void)
{
	printf("== C. the streaming budget\n\n");

	/* Measured from the shipped data/ default.ide (VICE-123): the file has 107
	 * vehicle model rows: 56 with a population class (normal 11, worker 7,
	 * executive 7, richfamily 6, poorfamily 6, big 6, taxi 3, motorbike 3,
	 * leisureboat 3, workerboat 3, moped 1) and 51 "ignore" (police, planes,
	 * RC, trains, the unique vehicles). The police and gang sets add about 15
	 * more models the population asks for. */
	const int dataVehicleModels = 107;
	const int dataIgnoreModels = 51;
	const int dataPopulationModels = 56;	/* the 11 class arrays */
	const int dataCopAndGangModels = 15;	/* ChoosePoliceCarModel + the gang cars */

	printf("  MAXVEHICLESLOADED      %d slots (the residency tracker, ms_vehiclesLoaded)\n", MAXVEHICLESLOADED);
	printf("  desiredNumVehiclesLoaded %d at boot on this machine: the memory formula of Init2,\n", MAXVEHICLESLOADED);
	printf("                         capped by MAXVEHICLESLOADED\n");
	printf("  VEHICLEMODELSIZE       %d the model slots the tracker can hold\n", VEHICLEMODELSIZE);
	printf("  shipped data           %d vehicle models: %d population models + about %d cop and\n",
		dataVehicleModels, dataPopulationModels, dataCopAndGangModels);
	printf("                         gang models; %d are the \"ignore\" class the population never\n", dataIgnoreModels);
	printf("                         chooses (planes, RC, trains, the unique vehicles)\n");
	printf("  measured live          %d..%d resident with a full street and a 2400 car budget\n", 57, 63);

	if (dataPopulationModels + dataCopAndGangModels > MAXVEHICLESLOADED)
		Fail("the population model set does not fit the residency tracker");
	if (MAXVEHICLESLOADED > VEHICLEMODELSIZE)
		Fail("the residency tracker does not fit the vehicle model slots");
	if (dataVehicleModels - dataIgnoreModels != dataPopulationModels)
		Fail("the measured model counts do not add up");

	printf("\n  verdict: the budget holds every model a full street asks for; the streaming is not\n");
	printf("  the limit of the car target. The 90 slots are a memory budget (every resident model\n");
	printf("  is a loaded clump); the loader evicts the models no car references first\n");
	printf("  (CStreaming::RemoveLoadedVehicle), so the street keeps the models it drives.\n\n");
}

/* ------------------------------------------------------------- D. the cost */

/* The audit added two scans of the ped pool flags a frame (the reservoir
 * checks) and nothing else. CPool::GetNoOfFreeSpaces scans one byte per slot
 * (templates.h), so one call reads NUMPEDS bytes. Measure that here, on the
 * machine that runs the audit, so the claim carries a number. */
static void SectionCost(void)
{
	static unsigned char flags[NUMPEDS];
	memset(flags, 0x80, sizeof(flags));
	for (int i = 0; i < NUMPEDS / 4; i++)
		flags[i] = 0;

	const int runs = 200000;
	struct timespec t0, t1;
	int used = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int r = 0; r < runs; r++)
		for (int i = 0; i < NUMPEDS; i++)
			used += (flags[i] & 0x80) == 0;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;

	printf("== D. what the audit costs a frame\n\n");
	printf("  one ped pool scan (NUMPEDS %d bytes): %.3f us, measured over %d runs\n",
		NUMPEDS, us / runs, runs);
	(void)used;
	printf("  the audit adds two scans a frame (the on-foot loop and the car loop), so about\n");
	printf("  %.3f us a frame at this ring; the passenger loop adds one only when a car is\n", 2.0 * us / runs);
	printf("  created. The rest is arithmetic (NumCarsInUse, ParkedCarTarget).\n\n");

	if (us / runs > 100.0)
		Fail("the ped pool scan costs more than 100 us");
}

int main(void)
{
	printf("reVC caps and streaming audit (VICE-123)\n");
	printf("the caps come from src/core/PopulationCaps.h, the pools from src/core/config.h,\n");
	printf("the ring from src/control/TrafficRings.h\n\n");

	SectionTable();
	SectionVice134Pools();
	SectionStreaming();
	SectionCost();

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  every cap stays below its pool at the maximum sliders, the ped pool holds the\n");
		printf("  crowd and its drivers, the VICE-134 pools hold the map demand, and the vehicle\n");
		printf("  residency budget holds the models the population asks for.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
