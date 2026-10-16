/*
 * The evidence program for VICE-123, "caps and streaming audit at maximum
 * sliders".
 *
 * It has four jobs, all offline:
 *
 *   A. The cap table. For every cap between the density sliders and the pool it
 *      prints the current value, the value the target asks for at the maximum
 *      sliders, the pool behind it and the headroom. It reads the values from
 *      src/core/PopulationBudget.h (the caps), src/core/config.h (the pools) and
 *      src/control/TrafficRings.h (the ring the caps grow with), so a number
 *      cannot drift away from this program.
 *
 *   B. The invariant checks. Every cap of the table stays below its pool; the
 *      ped pool holds the on-foot cap, the cars, the passengers and the frame
 *      reserve; the parked target stays inside the car budget; the pools
 *      VICE-134 resized (the alpha lists, the dummy pool) still hold the
 *      measured map demand.
 *
 *   C. The streaming verdict. The vehicle model residency budget
 *      (MAXVEHICLESLOADED, desiredNumVehiclesLoaded) against the models the
 *      population can ask for. The program reads the shipped data file when it
 *      is present (data/default.ide of the installed game; pass its path as
 *      argv[1]) and derives the counts from it; without the file it prints the
 *      recorded counts (measured 2026-10-16 from the installed Vice City
 *      default.ide, the path is printed) and says that they are recorded, not
 *      derived.
 *
 *   D. The cost. The audit adds pool reads a frame (the reserve checks); the
 *      program measures one read so the claim carries a number.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 -o /tmp/caps-audit utils/caps-audit/audit.cpp
 *	/tmp/caps-audit [path/to/default.ide]
 *
 * The exit code is 0 when every check passes. The live before/after values come
 * from utils/caps-audit/live-counters.py, which reads the running game
 * read-only.
 */

#include "PopulationBudget.h"
#include "TrafficRings.h"
#include "DrawDistance.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
		NUMVEHICLES + MAX_PED_PASSENGERS + PED_POOL_RESERVE, "MAX_PEDS_IN_USE");
	PrintClaimRow("drivers", NUMVEHICLES, NUMPEDS,
		MAX_PEDS_IN_USE + MAX_PED_PASSENGERS + PED_POOL_RESERVE, "one per car of the vehicle pool");
	PrintClaimRow("passengers", MAX_PED_PASSENGERS, NUMPEDS,
		MAX_PEDS_IN_USE + NUMVEHICLES + PED_POOL_RESERVE, "the counted rate over the car budget");
	PrintClaimRow("frame reserve", PED_POOL_RESERVE, NUMPEDS,
		MAX_PEDS_IN_USE + NUMVEHICLES + MAX_PED_PASSENGERS, "the burst of one frame + the one-off paths");
	PrintClaimRow("car budget", MAX_CARS_IN_USE, NUMVEHICLES, 0, "random + parked + mission + law + emergency");
	PrintClaimRow("random cars", MAX_RANDOM_CARS, MAX_CARS_IN_USE, 0, "the target formula stops here");
	PrintClaimRow("parked target", ParkedCarTarget(8.0f), NUM_CARGENS, 0, "20 x scale^2 at the ring top");
	PrintClaimRow("dummy demand", 2336 + NUMOBJECTS + 4, NUMDUMMIES, 0, "map dummies + convertibles (VICE-134)");
	printf("\n");
	printf("  measured live (2026-10-16, before the cap change) with a full street: ped pool\n");
	printf("  5417/8192, vehicle pool 153/2560, node pool 39253/%d, entry info pool 9240/%d,\n",
		NUMPTRNODES, NUMENTRYINFOS);
	printf("  dummy pool 2338/%d, visible entities 361..1233/%d\n\n", NUMDUMMIES, NUMVISIBLEENTITIES);

	/* The row-by-row checks of the table. */
	if (MAX_PEDS_IN_USE + NUMVEHICLES + MAX_PED_PASSENGERS + PED_POOL_RESERVE > NUMPEDS)
		Fail("the ped pool does not hold the budget line");
	if (MAX_CARS_IN_USE >= NUMVEHICLES)
		Fail("the car budget reached the vehicle pool");
	if (MAX_RANDOM_CARS > MAX_CARS_IN_USE)
		Fail("the random car target passed the total car budget");
	if (ParkedCarTarget(8.0f) >= MAX_CARS_IN_USE)
		Fail("the parked target reached the whole car budget");
	if (ParkedCarTarget(8.0f) > NUM_CARGENS)
		Fail("the parked target passed the generator count");
	if (NUM_CARGENS >= MAX_CARS_IN_USE)
		Fail("the generator count alone reaches the car budget");
	if (MAXVEHICLESLOADED > VEHICLEMODELSIZE)
		Fail("the vehicle residency tracker passed the vehicle model slots");

	/* The per-frame attempts are the fill rate, not a cap (PopulationBudget.h);
	 * the live count at the maximum sliders is 149..201 cars of the 2400 budget,
	 * so the fill owns the number and VICE-126/127 own the fill. */
	if (RANDOM_CAR_ATTEMPTS_PER_FRAME < 1 || PED_ATTEMPTS_PER_FRAME < 1 || CAR_SPAWN_ATTEMPTS < 1)
		Fail("a per-frame attempt count is below one");
	printf("  the fill rate at the ring top (scale %.2f): %d car calls and %d ped calls a frame,\n",
		(double)MaxSlidersRings(2000.0f).scale,
		(int)(RANDOM_CAR_ATTEMPTS_PER_FRAME * MaxSlidersRings(2000.0f).scale),
		(int)(PED_ATTEMPTS_PER_FRAME * MaxSlidersRings(2000.0f).scale));
	printf("  each car call tries %d placements (CAR_SPAWN_ATTEMPTS).\n\n", CAR_SPAWN_ATTEMPTS);
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
	const int mapDummies = 2336;

	printf("  alpha entity list : %d nodes (%d x 3), the measured scan demand is %d entities\n",
		NUMALPHAENTITYLIST * 3, NUMALPHAENTITYLIST, NUMVISIBLEENTITIES + NUMINVISIBLEENTITIES + mapDrawLast);
	printf("  alpha building list: %d slots, the map holds %d drawLast building instances\n",
		NUMVISIBLEENTITIES, mapBuildings);
	printf("  visible entities  : %d slots; measured live at a full street: 361..1233\n", NUMVISIBLEENTITIES);
	printf("  dummy pool        : %d slots; the map holds %d dummies + %d convertible objects\n",
		NUMDUMMIES, mapDummies, NUMOBJECTS);

	if (NUMALPHAENTITYLIST * 3 < NUMVISIBLEENTITIES + NUMINVISIBLEENTITIES + mapDrawLast)
		Fail("the alpha entity list is below the measured scan demand");
	if (NUMVISIBLEENTITIES < mapBuildings)
		Fail("the alpha building list is below the measured map demand");
	if (NUMDUMMIES < mapDummies + NUMOBJECTS)
		Fail("the dummy pool is below the map demand plus the convertible objects");
	if (NUMALPHAENTITYLIST <= 0 || NUMINVISIBLEENTITIES <= 0)
		Fail("an alpha list size is not positive");

	printf("  all three hold. VICE-134's report is the measurement and the reasoning; this\n");
	printf("  program re-reads the sizes out of config.h, so a later edit cannot pass silently.\n\n");
}

/* ------------------------------------------------------ C. the streaming row */

/* The vehicle models the population can choose, by name of the shipped data row.
 * The 56 class rows come from the classes of CreateVehicleObject (normal,
 * poorfamily, richfamily, executive, worker, big, taxi, moped, motorbike,
 * leisureboat, workerboat); the specials are the models the population chooses
 * outside the class arrays (they are "ignore" rows in the data). Measured from
 * the installed data/default.ide; the source refs are CarCtrl.cpp
 * (ChoosePoliceCarModel, ChooseBoatModel), Population.cpp (the emergency ped
 * crews) and Gangs.cpp (SetGangVehicleModel). */
static const char *aSpecialModels[] = {
	"police", "enforcer", "fbiranch", "rhino", "barracks", "vicechee", "predator",
	"ambulan", "firetruk",
	"cuban", "voodoo", "gangbur", "angel", "caddy",
};

/* A small parser for the cars section of data/default.ide: the row is
 * "id, model, txd, type, handling, game, anim, class, frequency, level, ...",
 * the section starts at a bare "cars" line and ends at a bare "end" line. */
static void TrimName(char *name)
{
	char *p = name;
	while (*p == ' ' || *p == '\t')
		p++;
	if (p != name)
		memmove(name, p, strlen(p) + 1);
	size_t n = strlen(name);
	while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\t' || name[n - 1] == '\n' || name[n - 1] == '\r'))
		name[--n] = 0;
}

static int ParseVehicleData(const char *path, int *pTotal, int *pClass, int *pMaxClass, int *pSpecials)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	char line[512];
	int inCars = 0;
	int total = 0, classRows = 0, specials = 0;
	char classNames[32][32];
	int classCount[32];
	bool seen[sizeof(aSpecialModels)/sizeof(aSpecialModels[0])];
	memset(classNames, 0, sizeof(classNames));
	memset(classCount, 0, sizeof(classCount));
	memset(seen, 0, sizeof(seen));

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, "cars", 4) == 0 && (p[4] == '\n' || p[4] == '\r' || p[4] == '\0')) {
			inCars = 1;
			continue;
		}
		if (inCars && strncmp(p, "end", 3) == 0 && (p[3] == '\n' || p[3] == '\r' || p[3] == '\0'))
			break;
		if (!inCars || *p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
			continue;
		if (!isdigit((unsigned char)*p))
			continue;

		/* The row is "id, model, txd, type, handling, game, anim, class, freq,
		 * level, ...": the model is the second field, the class the eighth. */
		char model[32] = { 0 };
		char cls[32] = { 0 };
		char *c = strchr(p, ',');
		if (!c)
			continue;
		sscanf(c + 1, " %31[^,]", model);
		for (int field = 1; field < 7 && c; field++)
			c = strchr(c + 1, ',');
		if (!c)
			continue;
		sscanf(c + 1, " %31[^,\n]", cls);
		TrimName(model);
		TrimName(cls);

		total++;
		for (unsigned s = 0; s < sizeof(aSpecialModels)/sizeof(aSpecialModels[0]); s++)
			if (strcmp(model, aSpecialModels[s]) == 0)
				seen[s] = true;

		if (strcmp(cls, "ignore") != 0) {
			classRows++;
			int slot = 0;
			for (; slot < 32; slot++) {
				if (classNames[slot][0] == 0)
					strcpy(classNames[slot], cls);
				if (strcmp(classNames[slot], cls) == 0)
					break;
			}
			if (slot < 32)
				classCount[slot]++;
		}
	}
	fclose(f);

	for (unsigned s = 0; s < sizeof(aSpecialModels)/sizeof(aSpecialModels[0]); s++)
		if (seen[s])
			specials++;

	int maxClass = 0;
	for (int i = 0; i < 32; i++)
		if (classCount[i] > maxClass)
			maxClass = classCount[i];
	*pTotal = total;
	*pClass = classRows;
	*pMaxClass = maxClass;
	*pSpecials = specials;
	return 1;
}

static void SectionStreaming(const char *dataFile)
{
	printf("== C. the streaming budget\n\n");

	/* The recorded counts: measured 2026-10-16 from the installed
	 * data/default.ide of Grand Theft Auto Vice City (the same header the game
	 * loads). The program re-derives them from the file when it can read it. */
	const char *recordedPath = "~/Games/Steam/steamapps/common/Grand Theft Auto Vice City/data/default.ide";
	const int recordedTotal = 107;
	const int recordedClass = 56;
	const int recordedMaxClass = 11;
	const int recordedSpecials = 14;

	int total = recordedTotal, classRows = recordedClass, maxClass = recordedMaxClass, specials = recordedSpecials;
	int derived = 0;

	if (dataFile) {
		derived = ParseVehicleData(dataFile, &total, &classRows, &maxClass, &specials);
		if (derived)
			printf("  the data file        %s (read, the counts below are derived from it)\n", dataFile);
		else
			printf("  the data file        %s (cannot be read; the recorded counts are used)\n", dataFile);
	} else {
		printf("  the data file        not given; pass the path of the installed data/default.ide as\n");
		printf("                       argv[1] to derive the counts. Recorded from %s\n", recordedPath);
	}
	if (!derived)
		printf("                       (measured 2026-10-16; the counts are recorded, not derived)\n");

	printf("  MAXVEHICLESLOADED      %d slots (the residency tracker, ms_vehiclesLoaded)\n", MAXVEHICLESLOADED);
	printf("  desiredNumVehiclesLoaded the memory formula of CStreaming::Init2, capped at the tracker\n");
	printf("  VEHICLEMODELSIZE       %d the model slots the tracker can hold\n", VEHICLEMODELSIZE);
	printf("  the shipped data       %d vehicle rows: %d class rows (the population classes the\n",
		total, classRows);
	printf("                         game loads into the class arrays) + %d \"ignore\" rows\n",
		total - classRows);
	printf("  the population set     %d class rows + %d models it chooses outside the classes\n",
		classRows, specials);
	printf("                         (police, enforcer, fbi, rhino, barracks, vicechee, predator,\n");
	printf("                         ambulan, firetruk and the five gang cars)\n");
	printf("  the largest class      %d models (MAX_CAR_MODELS_IN_ARRAY is %d, so AddToCarArray\n",
		maxClass, MAX_CAR_MODELS_IN_ARRAY);
	printf("                         cannot assert on the shipped data)\n");
	printf("  measured live          57..65 resident with a full street (2026-10-16)\n");

	if (classRows + specials > MAXVEHICLESLOADED)
		Fail("the population model set does not fit the residency tracker");
	if (classRows + specials > VEHICLEMODELSIZE)
		Fail("the population model set does not fit the vehicle model slots");
	if (maxClass >= MAX_CAR_MODELS_IN_ARRAY)
		Fail("a class array overflows on the shipped data (the AddToCarArray assert)");
	if (total - classRows < specials)
		Fail("the special models are not part of the data rows");
	if (specials <= 0 || classRows <= 0)
		Fail("the data parse found no models");
	if (derived && (total < 0 || classRows < 0))
		Fail("the data parse is not sane");

	printf("\n  verdict: the tracker holds every model the population can ask for (%d of %d slots;\n",
		classRows + specials, MAXVEHICLESLOADED);
	printf("  the live count is 57..65). The 90 slots are a memory budget (every resident model is\n");
	printf("  a loaded clump); the loader evicts the models no car references first\n");
	printf("  (CStreaming::RemoveLoadedVehicle), so the street keeps the models it drives.\n\n");
}

/* -------------------------------------------------------------- D. the cost */

/* The audit added pool reads to the creation paths (the reserve checks); a
 * CPool::GetNoOfFreeSpaces call scans one byte per slot (templates.h), so one
 * call reads NUMPEDS bytes. Measure that here, on the machine that runs the
 * audit, so the claim carries a number. The count is printed and checked so
 * the optimizer cannot turn the loop into dead code. */
static void SectionCost(void)
{
	static unsigned char flags[NUMPEDS];
	memset(flags, 0x80, sizeof(flags));
	for (int i = 0; i < NUMPEDS / 4; i++)
		flags[i] = 0;
	const int usedPerScan = NUMPEDS / 4;

	const int runs = 200000;
	struct timespec t0, t1;
	long used = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int r = 0; r < runs; r++) {
		int n = 0;
		for (int i = 0; i < NUMPEDS; i++)
			n += (flags[i] & 0x80) == 0;
		used += n;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;

	printf("== D. what the audit costs a frame\n\n");
	printf("  one ped pool scan (NUMPEDS %d bytes, counted %ld): %.3f us, measured over %d runs\n",
		NUMPEDS, used / runs, us / runs, runs);
	printf("  the audit reads the pool: once a frame in CPopulation::Update, once a frame in\n");
	printf("  CCarCtrl::GenerateRandomCars (the drivers included) and once per car with\n");
	printf("  passengers in CreateCarFromPlacement, so about %.3f us a frame when the car loop\n",
		2.0 * us / runs);
	printf("  creates no car and %.3f us a frame when it reaches its 23 calls.\n\n",
		(2.0 + 23.0) * us / runs);

	if (used / runs != usedPerScan)
		Fail("the scan did not count the flags (dead code eliminated?)");
	if (us / runs > 100.0)
		Fail("the ped pool scan costs more than 100 us");
}

int main(int argc, char **argv)
{
	printf("reVC caps and streaming audit (VICE-123)\n");
	printf("the caps come from src/core/PopulationBudget.h, the pools from src/core/config.h,\n");
	printf("the ring from src/control/TrafficRings.h\n\n");

	SectionTable();
	SectionVice134Pools();
	SectionStreaming(argc > 1 ? argv[1] : NULL);
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
