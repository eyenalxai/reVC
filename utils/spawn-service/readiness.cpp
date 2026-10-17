/*
 * The evidence program for the spawn readiness and prewarm (ticket VICE-151).
 *
 * It compiles src/core/SpawnReadiness.h on its own (no engine header beyond
 * config.h and the budget table) and drives the rule, the whole-set
 * predicate, the plan and the budget the game runs:
 *
 *	A. The per-model rule. The ticket's predicate (the model's stream, its
 *	   texture dictionary and its animation block) over every combination,
 *	   the evicted animation block included: a "loaded" model whose anim
 *	   block was evicted reads not ready, a model without an anim block
 *	   needs none, a model without a txd slot is never ready.
 *
 *	B. The whole-set predicate. The car class arrays of the shipped shape
 *	   (the class rows in their ratings, packed, the counts separate) and
 *	   the pedestrian group (the 16 slots with their -1 holes): a healthy
 *	   set reads ready, an evicted animation block fails and names its
 *	   model, an unloaded model fails, the -1 holes are skipped, and an
 *	   empty set (no stored model) is never ready. The gang mask rule (the
 *	   rule CTheZones and StreamZoneModels share) over synthetic thresholds
 *	   of the shipped shape.
 *
 *	C. The plan. The global car set is requested once; a zone change drops
 *	   the zone-only pins and keeps the car pins (a shared model keeps its
 *	   car bit) through the plan's own ReleaseSet; the pin budget refuses
 *	   the vehicle pin at the budget and counts it deferred while a
 *	   pedestrian pin still fits; a duplicate model is one pin; Reset drops
 *	   the pins and keeps the counters.
 *
 *	D. The shipped sets, from the installed game's data when it is readable
 *	   (data/default.ide, data/pedgrp.dat, models/gta3.dir): the 56 class
 *	   models, a pedestrian group (the default one) and the gang set; the
 *	   pin counts against the budget and the tracker's headroom, and the
 *	   files, sectors and bytes the prewarm reads. An .ide anim name counts
 *	   only when the image holds the .ifp: the engine's anim index resolves
 *	   against the registered blocks (the peds' "man" has no file, so the
 *	   model has no block).
 *
 *	E. The stall. The blocking path's I/O share is measured against the real
 *	   image (models/gta3.img): every file of a set is read once at its own
 *	   size, the CdStreamRead + Sync pattern, and the wall time is printed -
 *	   the boot set (the car classes and the first zone), a transition's
 *	   zone set, and the union. The engine-free plan's own per-frame cost is
 *	   measured over the full pin set (the class-array walk, the group walk,
 *	   the pin find), so the pump's arithmetic is a number, not a claim.
 *
 * What this program cannot reach, stated plainly: the live arrays are the
 * engine's (CCarCtrl::LoadedCarsArray, CPopulation::ms_pPedGroups, the zone
 * info), so the checks drive the same predicates over arrays of the shipped
 * shape and the engine's own walk is compiled and inspected here, not driven; the streaming
 * calls (the requests, the pins as STREAMFLAGS_DONT_REMOVE, the blocking
 * LoadAllRequestedModels) are the engine's, driven by inspection here; the
 * file *processing* (the clump, txd and anim loads) needs RenderWare and
 * stays the game's, so the measured stall here is the I/O share and the
 * running game's own counter (CSpawnReadiness::ms_Plan.m_nLastStallMs, read
 * by utils/caps-audit/live-counters.py) carries the whole; and the zone gang
 * masks come from the script (main.scm's SET_ZONE_* opcodes), so the zone
 * set here is the representative one the program prints, not a read of a
 * save.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 \
 *	    -o /tmp/spawn-readiness utils/spawn-service/readiness.cpp
 *	/tmp/spawn-readiness                          # the recorded data
 *	/tmp/spawn-readiness --data ~/Games/Steam/steamapps/common/"Grand Theft Auto Vice City"
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "SpawnReadiness.h"
#include "PopulationBudget.h"

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

static double
NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static volatile int g_sink;

/* ------------------------------------------------------------------ *
 * the shipped data
 * ------------------------------------------------------------------ */

/*
 * The class rows the harness reads are the CCarCtrl classes in the enum's
 * own order (CarCtrl.cpp's LoadVehicleObject maps the handling.cfg names
 * into CCarCtrl::NORMAL..WORKERBOAT; the gang, cop and player classes are
 * "ignore" rows and never enter the class arrays). The count is the
 * capacity's own: a mirror, because CarCtrl.h is the engine's.
 */
#define NUM_CLASSES 11

static const char *g_classNames[NUM_CLASSES] = {
	"normal", "poorfamily", "richfamily", "executive", "worker", "big",
	"taxi", "moped", "motorbike", "leisureboat", "workerboat",
};

enum { DATA_CAR, DATA_PED };

struct SDataModel
{
	char name[32];
	char txd[32];
	char anim[32];		/* "null" when the model has none */
	uint8_t kind;		/* DATA_CAR / DATA_PED */
	uint8_t classIndex;	/* a class row's rating, 0xff otherwise */
};

#define MAX_DATA_MODELS 1024
static SDataModel g_models[MAX_DATA_MODELS];
static int g_numModels;
static bool g_dataDerived;	/* the installed data was read */

/* The class arrays of the shipped shape: the model indices per rating and
 * the per-rating counts (the shape of CCarCtrl::CarArrays /
 * NumOfLoadedCarsOfRating). */
static int32_t g_classModels[NUM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];
static int32_t g_classCounts[NUM_CLASSES];

/* The pedestrian groups (pedgrp.dat's lines, resolved to model indices). */
static int32_t g_pedGroups[NUMPEDGROUPS][NUMMODELSPERPEDGROUP];
static int g_numPedGroups;

/* The gang table: a mirror of CGangs::Initialise (the models are the game's
 * own constants; -1 when the gang has none). */
struct SGangData
{
	const char *ped1, *ped2, *car;
};
static const SGangData g_gangs[9] = {
	{ "CBA", "CBB", "CUBAN" },
	{ "HNA", "HNB", "VOODOO" },
	{ "SGA", "SGB", "GANGBUR" },
	{ "CLA", "CLB", NULL },
	{ "GDA", "GDB", NULL },
	{ "BKA", "BKB", "ANGEL" },
	{ "PGA", "PGB", NULL },
	{ "WFOGO", "WMOGO", "CADDY" },
	{ NULL, NULL, NULL },
};
static int32_t g_gangPed[9][2];
static int32_t g_gangCar[9];

/* The recorded shipped counts (the caps audit's own recorded row, measured
 * 2026-10-16 from the installed default.ide). The fallback when the data is
 * not readable. */
static const int g_recordedClassCounts[NUM_CLASSES] = { 11, 6, 6, 7, 7, 6, 3, 1, 3, 3, 3 };
#define RECORDED_CLASS_MODELS 56
#define RECORDED_PED_GROUPS 67
#define RECORDED_CAR_FILES 117
#define RECORDED_CAR_SECTORS 5916
#define RECORDED_ZONE_FILES 51		/* the default group (32) + the 3 gangs (19) */
#define RECORDED_ZONE_SECTORS 1341	/* 714 + 627 */
#define RECORDED_UNION_FILES 167
#define RECORDED_UNION_SECTORS 7218

/* The gta3 image directory: the file sizes the streaming reads. */
struct SDirEntry
{
	char name[32];
	uint32_t offset;	/* sectors */
	uint32_t size;		/* sectors */
};
#define MAX_DIR_ENTRIES 8192
static SDirEntry g_dir[MAX_DIR_ENTRIES];
static int g_numDirEntries;
static char g_dataDir[512];
static bool g_imageReadable;

static const char *
DefaultDataDir(void)
{
	static char path[512];
	const char *home = getenv("HOME");
	if (home == NULL)
		return NULL;
	snprintf(path, sizeof(path), "%s/Games/Steam/steamapps/common/Grand Theft Auto Vice City", home);
	return path;
}

static void
Trim(char *s)
{
	char *start = s;
	while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
		start++;
	if (start != s)
		memmove(s, start, strlen(start) + 1);
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';
}

static void
Lower(char *s)
{
	for (; *s; s++)
		if (*s >= 'A' && *s <= 'Z')
			*s += 'a' - 'A';
}

static int
FindDataModel(const char *name)
{
	char lower[32];
	snprintf(lower, sizeof(lower), "%s", name);
	Lower(lower);
	for (int i = 0; i < g_numModels; i++)
		if (strcmp(g_models[i].name, lower) == 0)
			return i;
	return -1;
}

static int
AddDataModel(const char *name, const char *txd, const char *anim, uint8_t kind, int classIndex)
{
	if (g_numModels >= MAX_DATA_MODELS)
		return -1;
	SDataModel *m = &g_models[g_numModels];
	snprintf(m->name, sizeof(m->name), "%s", name);
	snprintf(m->txd, sizeof(m->txd), "%s", txd);
	snprintf(m->anim, sizeof(m->anim), "%s", anim);
	Lower(m->name);
	Lower(m->txd);
	Lower(m->anim);
	m->kind = kind;
	m->classIndex = (uint8_t)classIndex;
	return g_numModels++;
}

/* Split a comma row into trimmed fields. */
static int
SplitFields(char *line, char fields[][32], int maxFields)
{
	int n = 0;
	char *p = line;

	while (n < maxFields) {
		char *comma = strchr(p, ',');
		if (comma)
			*comma = '\0';
		char *field = p;
		while (*field == ' ' || *field == '\t')
			field++;
		char *end = field + strlen(field);
		while (end > field && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
			*--end = '\0';
		snprintf(fields[n], 32, "%s", field);
		n++;
		if (comma == NULL)
			break;
		p = comma + 1;
	}
	return n;
}

static int
ClassIndex(const char *name)
{
	for (int i = 0; i < NUM_CLASSES; i++)
		if (strcmp(g_classNames[i], name) == 0)
			return i;
	return -1;
}

/*
 * data/default.ide: the peds and the cars sections. A car row is
 * "id, model, txd, type, handling, game, anim, class, freq, ..." and a ped
 * row "id, model, txd, type, stat, animGroup, carsCanDrive, animFile, ...".
 */
static void
ParseDefaultIde(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return;

	char line[1024];
	char fields[16][32];
	int section = -1;	/* 0 peds, 1 cars */

	while (fgets(line, sizeof(line), f) != NULL) {
		Trim(line);
		if (line[0] == '\0' || line[0] == '#')
			continue;
		if (strchr(line, ',') == NULL) {
			if (strcmp(line, "peds") == 0)
				section = 0;
			else if (strcmp(line, "cars") == 0)
				section = 1;
			else
				section = -1;
			continue;
		}
		int n = SplitFields(line, fields, 16);
		if (n < 3)
			continue;
		if (section == 0 && n >= 3) {
			const char *anim = n > 7 ? fields[7] : "null";
			if (FindDataModel(fields[1]) < 0)
				AddDataModel(fields[1], fields[2], anim, DATA_PED, 0xff);
		} else if (section == 1 && n >= 8) {
			int cls = ClassIndex(fields[7]);
			int index = FindDataModel(fields[1]);
			if (index < 0)
				index = AddDataModel(fields[1], fields[2], fields[6], DATA_CAR, cls < 0 ? 0xff : (uint8_t)cls);
			else
				g_models[index].classIndex = cls < 0 ? 0xff : (uint8_t)cls;
		}
	}
	fclose(f);
}

/*
 * data/pedgrp.dat: one line per pedestrian group, the first 16 names (the
 * commas and the spaces separate, the "//" comment after the 16th is never
 * read - the game's own parser stops at 16).
 */
static void
ParsePedGroups(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return;

	char line[1024];
	while (fgets(line, sizeof(line), f) != NULL) {
		char *hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		if (line[0] == '\0' || line[0] == '\n')
			continue;
		if (g_numPedGroups >= NUMPEDGROUPS)
			break;
		int count = 0;
		char *p = line;
		while (count < NUMMODELSPERPEDGROUP) {
			while (*p == ' ' || *p == '\t' || *p == ',')
				p++;
			if (*p == '\0' || *p == '\r' || *p == '\n' || *p == '/')
				break;
			char *start = p;
			while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '\r' && *p != '\n')
				p++;
			char saved = *p;
			*p = '\0';
			g_pedGroups[g_numPedGroups][count] = FindDataModel(start);
			*p = saved;
			count++;
		}
		if (count == NUMMODELSPERPEDGROUP)
			g_numPedGroups++;
	}
	fclose(f);
}

static void
BuildGangTable(void)
{
	for (int g = 0; g < 9; g++) {
		g_gangPed[g][0] = g_gangs[g].ped1 ? FindDataModel(g_gangs[g].ped1) : -1;
		g_gangPed[g][1] = g_gangs[g].ped2 ? FindDataModel(g_gangs[g].ped2) : -1;
		g_gangCar[g] = g_gangs[g].car ? FindDataModel(g_gangs[g].car) : -1;
	}
}

/*
 * models/gta3.dir: 32 byte records, "offset (sectors), size (sectors),
 * name[24]" (the format CStreaming::LoadCdDirectory reads).
 */
static void
ParseGta3Dir(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return;

	uint8_t record[32];
	while (fread(record, 1, 32, f) == 32 && g_numDirEntries < MAX_DIR_ENTRIES) {
		SDirEntry *e = &g_dir[g_numDirEntries];
		memcpy(&e->offset, record, 4);
		memcpy(&e->size, record + 4, 4);
		memcpy(e->name, record + 8, 24);
		e->name[24] = '\0';
		Lower(e->name);
		g_numDirEntries++;
	}
	fclose(f);
}

static const SDirEntry *
FindDirEntry(const char *name)
{
	for (int i = 0; i < g_numDirEntries; i++)
		if (strcmp(g_dir[i].name, name) == 0)
			return &g_dir[i];
	return NULL;
}

/* The class arrays of the shipped shape: the parsed rows, or the recorded
 * counts over synthetic ids. */
static void
BuildClassArrays(void)
{
	if (g_dataDerived) {
		for (int i = 0; i < g_numModels; i++) {
			int cls = g_models[i].classIndex;
			if (cls < 0 || cls >= NUM_CLASSES)
				continue;
			g_classModels[cls][g_classCounts[cls]++] = i;
		}
		return;
	}
	int id = 130;
	for (int c = 0; c < NUM_CLASSES; c++)
		for (int i = 0; i < g_recordedClassCounts[c]; i++)
			g_classModels[c][g_classCounts[c]++] = id++;
}

/* ------------------------------------------------------------------ *
 * A. the per-model rule
 * ------------------------------------------------------------------ */

static void
TestModelPredicate(void)
{
	printf("A. the per-model rule (model && txd && anim)\n");

	Check(SpawnReadyModelIsReady(true, 130, true, 3, true),
		"model, txd and animation block loaded is ready");
	Check(!SpawnReadyModelIsReady(false, 130, true, 3, true),
		"the model's own stream is not loaded");
	Check(!SpawnReadyModelIsReady(true, 130, false, 3, true),
		"the texture dictionary is not loaded");
	Check(!SpawnReadyModelIsReady(true, 130, true, 3, false),
		"the animation block is evicted while the model reads loaded (the ticket's case)");
	Check(SpawnReadyModelIsReady(true, 130, true, -1, false),
		"a model without an animation block needs none");
	Check(!SpawnReadyModelIsReady(true, -1, true, -1, false),
		"a model without a txd slot is never ready");
	Check(!SpawnReadyModelIsReady(true, -1, false, 3, true),
		"the txd slot and the load are both needed");
	Check(SpawnReadyModelIsReady(true, 0, true, 0, true),
		"the slot and block indices' low bound (0) is valid");

	printf("  the rule: the evicted animation block fails, the no-anim model passes\n\n");
}

/* ------------------------------------------------------------------ *
 * B. the whole-set predicate
 * ------------------------------------------------------------------ */

static void
TestWholeSetPredicate(void)
{
	printf("B. the whole-set predicate (the class arrays and the ped group)\n");

	/* The car class arrays of the shipped shape. */
	int32_t rows[NUM_CLASSES * MAX_CAR_MODELS_IN_ARRAY];
	int32_t counts[NUM_CLASSES];
	static bool readyByModel[MAX_DATA_MODELS];
	int total = 0;
	for (int c = 0; c < NUM_CLASSES; c++) {
		counts[c] = g_classCounts[c];
		for (int i = 0; i < counts[c]; i++) {
			rows[c * MAX_CAR_MODELS_IN_ARRAY + i] = g_classModels[c][i];
			readyByModel[g_classModels[c][i]] = true;
			total++;
		}
	}

	int32_t firstMissing = -1;
	Check(SpawnReadyClassArraysMissing(rows, MAX_CAR_MODELS_IN_ARRAY, counts, NUM_CLASSES, readyByModel, &firstMissing) == 0,
		"a healthy class set is ready");
	Check(firstMissing == -1, "a healthy class set names no missing model");

	/* One evicted animation block (or any not-ready model) fails the set
	 * and is named. */
	int victim = g_classModels[3][1];
	readyByModel[victim] = false;
	Check(SpawnReadyClassArraysMissing(rows, MAX_CAR_MODELS_IN_ARRAY, counts, NUM_CLASSES, readyByModel, &firstMissing) == 1,
		"one not-ready class model fails the whole set");
	Check(firstMissing == victim, "the failing class model is named");
	readyByModel[victim] = true;

	/* The class walk counts only the loaded rows: an empty row is skipped. */
	int32_t savedCount = counts[0];
	counts[0] = 0;
	readyByModel[g_classModels[0][0]] = false;
	Check(SpawnReadyClassArraysMissing(rows, MAX_CAR_MODELS_IN_ARRAY, counts, NUM_CLASSES, readyByModel, &firstMissing) == 0,
		"an empty rating row contributes nothing (the loaded-count walk)");
	readyByModel[g_classModels[0][0]] = true;
	counts[0] = savedCount;

	/* The pedestrian group: the -1 holes are skipped, a missing model fails
	 * and is named. */
	int32_t pedModels[NUMMODELSPERPEDGROUP];
	bool pedReady[NUMMODELSPERPEDGROUP];
	for (int i = 0; i < NUMMODELSPERPEDGROUP; i++) {
		pedModels[i] = i == 5 ? -1 : g_classModels[0][i % g_classCounts[0]];
		pedReady[i] = true;
	}
	Check(SpawnReadyListMissing(pedModels, pedReady, NUMMODELSPERPEDGROUP, &firstMissing) == 0,
		"a healthy pedestrian group is ready (the -1 hole skipped)");
	pedReady[2] = false;
	Check(SpawnReadyListMissing(pedModels, pedReady, NUMMODELSPERPEDGROUP, &firstMissing) == 1,
		"one not-ready group model fails the whole group");
	Check(firstMissing == pedModels[2], "the failing group model is named");
	pedReady[5] = false;	/* the hole is not ready either, and is skipped */
	Check(SpawnReadyListMissing(pedModels, pedReady, NUMMODELSPERPEDGROUP, &firstMissing) == 1,
		"the -1 hole is skipped whatever its readiness bit says");

	/* The whole-set verdict: an empty set is never ready, however clean its
	 * missing count is. */
	Check(!SpawnReadySetIsReady(0, 0), "an empty set is never ready");
	Check(SpawnReadySetIsReady(1, 0), "a non-empty set with nothing missing is ready");
	Check(!SpawnReadySetIsReady(5, 1), "a non-empty set with a missing model is not ready");
	Check(SpawnReadyListStored(pedModels, NUMMODELSPERPEDGROUP) == NUMMODELSPERPEDGROUP - 1,
		"the stored count skips the -1 holes");
	for (int i = 0; i < NUMMODELSPERPEDGROUP; i++)
		pedModels[i] = -1;
	Check(SpawnReadyListStored(pedModels, NUMMODELSPERPEDGROUP) == 0 &&
	      SpawnReadyListMissing(pedModels, pedReady, NUMMODELSPERPEDGROUP, &firstMissing) == 0 &&
	      !SpawnReadySetIsReady(0, 0),
		"an all-holes group stores nothing and is never ready");
	int32_t emptyRows[NUM_CLASSES * MAX_CAR_MODELS_IN_ARRAY] = {};
	int32_t emptyCounts[NUM_CLASSES] = {};
	Check(SpawnReadyClassArraysStored(emptyRows, MAX_CAR_MODELS_IN_ARRAY, emptyCounts, NUM_CLASSES) == 0 &&
	      SpawnReadyClassArraysMissing(emptyRows, MAX_CAR_MODELS_IN_ARRAY, emptyCounts, NUM_CLASSES, readyByModel, &firstMissing) == 0,
		"empty class arrays store nothing and miss nothing (the verdict's own rule refuses them)");
	Check(SpawnReadyClassArraysStored(rows, MAX_CAR_MODELS_IN_ARRAY, counts, NUM_CLASSES) == total,
		"the class arrays' stored count is the shipped total");

	/* The gang mask rule (CTheZones::GetZoneGangPedMask/GetZoneGangCarMask
	 * and the header's own copy): the first threshold that differs from the
	 * cops' opens the mask, every difference from its predecessor adds a
	 * gang. */
	uint16_t pedThresholds[9] = { 5, 5, 8, 8, 8, 12, 12, 12, 12 };
	Check(SpawnReadyGangMask(pedThresholds, (uint16_t)5, 9) == ((1 << 2) | (1 << 5)),
		"the pedestrian mask opens at the first differing threshold and adds the gang changes");
	int16_t carThresholds[9] = { 10, 20, 20, 20, 20, 20, 30, 30, 30 };
	Check(SpawnReadyGangMask(carThresholds, (int16_t)10, 9) == ((1 << 1) | (1 << 6)),
		"the car mask follows the same rule over the int16 thresholds");
	Check(SpawnReadyGangMask(pedThresholds, (uint16_t)5, 0) == 0, "no gangs read as an empty mask");

	printf("  the class set: %d models over %d ratings; the walk skips empty rows\n", total, NUM_CLASSES);
	printf("  the group: %d slots, the -1 holes skipped; the failing model is named\n", NUMMODELSPERPEDGROUP);
	printf("  the mask rule: the first difference from the cops opens it\n\n");
}

/* ------------------------------------------------------------------ *
 * C. the plan and the pin ledger
 * ------------------------------------------------------------------ */

static void
TestPlan(void)
{
	printf("C. the plan and the pin ledger\n");

	Check(SPAWN_READY_PIN_BUDGET == MAXVEHICLESLOADED - SPAWN_READY_PIN_RESERVE,
		"the budget is the tracker less the reserve");
	Check(SPAWN_READY_PIN_BUDGET < MAXVEHICLESLOADED, "the budget keeps the tracker's reserve free");
	Check(SPAWN_READY_MAX_PINS > SPAWN_READY_PIN_BUDGET, "the pin array holds the budget with the zone sets");
	Check(MAXVEHICLESLOADED <= VEHICLEMODELSIZE, "the tracker fits the vehicle model store");

	SSpawnReadyPlan plan;
	Check(plan.m_nPins == 0 && plan.m_nVehiclePins == 0 && plan.CarSetPending(),
		"a fresh plan holds nothing and wants the car set");
	Check(plan.ZoneSetPending(0, 0, 0), "a fresh plan wants a zone set");

	/* The global car set: every class model, once. */
	int carModels = 0;
	for (int c = 0; c < NUM_CLASSES; c++)
		for (int i = 0; i < g_classCounts[c]; i++) {
			Check(plan.PinFits(true), "a class model fits the budget");
			SSpawnReadyPin pin = {};
			pin.model = g_classModels[c][i];
			pin.txd = pin.model;
			pin.anim = -1;
			pin.owned = SPAWN_READY_OWN_MODEL | SPAWN_READY_OWN_TXD;
			pin.sets = SPAWN_READY_SET_CAR;
			plan.AddPin(pin, true);
			carModels++;
		}
	Check(plan.m_nPins == carModels && plan.m_nVehiclePins == carModels,
		"the class set is one pin per model");
	Check(plan.CarSetPending(), "the set is marked complete by the engine's caller, not by the pins alone");

	/* A zone model the car set already holds is one pin with both bits. */
	int shared = g_classModels[0][0];
	int32_t index = plan.FindPin(shared);
	Check(index >= 0, "the shared model is found");
	plan.m_aPins[index].sets |= SPAWN_READY_SET_ZONE;
	Check(plan.m_nPins == carModels, "the shared model is one pin, not two");

	/* A zone-only model is a new pin. */
	SSpawnReadyPin zonePin = {};
	zonePin.model = 900;
	zonePin.txd = 900;
	zonePin.anim = -1;
	zonePin.owned = SPAWN_READY_OWN_MODEL | SPAWN_READY_OWN_TXD;
	zonePin.sets = SPAWN_READY_SET_ZONE;
	plan.AddPin(zonePin, false);
	Check(plan.m_nPins == carModels + 1, "a zone-only model adds a pin");
	Check(plan.m_nVehiclePins == carModels, "a pedestrian pin is not a vehicle pin");

	/* The zone change: the zone-only pin goes, the shared model keeps its
	 * car bit and stays. The release is the plan's own ReleaseSet (the
	 * engine's ReleaseSetPins calls it and unpins what it hands back). */
	plan.StartZoneSet(1, 0, 0);
	SSpawnReadyPin released[SPAWN_READY_MAX_PINS];
	int32_t releasedCount = plan.ReleaseSet(SPAWN_READY_SET_ZONE, released, SPAWN_READY_MAX_PINS);
	Check(releasedCount == 1 && released[0].model == 900,
		"the zone change hands the zone-only pin back to the caller");
	Check(plan.m_nPins == carModels, "the zone change drops the zone-only pin");
	Check(plan.FindPin(shared) >= 0, "the shared model stays (the car set holds it)");
	Check(plan.ZoneSetPending(1, 0, 0), "the new zone set is pending until its models are pinned");
	Check(plan.ReleaseSet(SPAWN_READY_SET_ZONE, released, SPAWN_READY_MAX_PINS) == 0,
		"the same set released twice hands nothing back");

	/* The budget: fill the vehicle pins to the budget, then refuse. */
	while (plan.PinFits(true)) {
		SSpawnReadyPin pin = {};
		pin.model = 2000 + plan.m_nPins;
		pin.sets = SPAWN_READY_SET_ZONE;
		plan.AddPin(pin, true);
	}
	Check(plan.m_nVehiclePins == SPAWN_READY_PIN_BUDGET, "the vehicle pins stop at the budget");
	Check(!plan.PinFits(true), "the next vehicle pin is refused");
	Check(plan.PinFits(false), "a pedestrian pin still fits");
	Check(plan.m_nVehiclePins <= MAXVEHICLESLOADED - SPAWN_READY_PIN_RESERVE,
		"the tracker keeps its reserve of free slots");

	/* The deferral: a set bigger than the array counts its refused models.
	 * Fill the array first (pedestrian pins), then attempt ten more. */
	while (plan.PinFits(false)) {
		SSpawnReadyPin pin = {};
		pin.model = 3000 + plan.m_nPins;
		pin.sets = SPAWN_READY_SET_ZONE;
		plan.AddPin(pin, false);
	}
	int deferred = 0;
	for (int i = 0; i < 10; i++) {
		if (plan.PinFits(false)) {
			SSpawnReadyPin pin = {};
			pin.model = 4000 + i;
			pin.sets = SPAWN_READY_SET_ZONE;
			plan.AddPin(pin, false);
		} else {
			deferred++;
			plan.m_nRequestsDeferred++;
		}
	}
	Check(plan.m_nPins == SPAWN_READY_MAX_PINS, "the array is full at its own bound");
	Check(deferred == 10, "an over-full set counts every refused pin as deferred");
	Check(plan.m_nRequestsDeferred == (uint32_t)deferred, "the deferred counter holds the refusals");

	/* Reset: the pins go, the sets are pending again, the counters stay. */
	plan.m_nSetsResident = 7;
	plan.m_nFramesWaited = 42;
	plan.Reset();
	Check(plan.m_nPins == 0 && plan.m_nVehiclePins == 0, "Reset drops every pin");
	Check(plan.CarSetPending() && plan.m_nPedGroup == -1, "Reset forgets the sets");
	Check(plan.m_nSetsResident == 7 && plan.m_nFramesWaited == 42, "Reset keeps the counters (they are the evidence)");

	printf("  the budget: %d vehicle pins (the tracker %d less the reserve %d)\n",
	       SPAWN_READY_PIN_BUDGET, MAXVEHICLESLOADED, SPAWN_READY_PIN_RESERVE);
	printf("  the array: %d pins; the class set is %d pins, the shared zone model one more bit\n",
	       SPAWN_READY_MAX_PINS, carModels);
	printf("  the zone change: the zone-only pins drop, the car pins stay, the counters survive Reset\n\n");
}

/* ------------------------------------------------------------------ *
 * D. the shipped sets
 * ------------------------------------------------------------------ */

struct SFileSet
{
	const char *names[512];
	int count;
	uint32_t sectors;
	int missing;
};

static void
FileSetAdd(SFileSet *set, const char *name)
{
	for (int i = 0; i < set->count; i++)
		if (strcmp(set->names[i], name) == 0)
			return;
	if (set->count < 512)
		set->names[set->count++] = name;
}

static const char *
ModelFileName(const SDataModel *m, const char *ext)
{
	static char buffer[1024][32];
	static int next;
	if (next >= 1024)
		next = 0;
	char *s = buffer[next++];
	snprintf(s, 32, "%s.%s", m->name, ext);
	return s;
}

static const char *
AnimFileName(const SDataModel *m)
{
	static char buffer[1024][32];
	static int next;
	if (next >= 1024)
		next = 0;
	char *s = buffer[next++];
	snprintf(s, 32, "%s.ifp", m->anim);
	return s;
}

/* The files of one model: its clump, its txd and - when the model has one -
 * its animation block. The anim block counts only when the image holds the
 * .ifp: the engine's index resolves against the blocks
 * CStreaming::LoadCdDirectory registered from the image's .ifp entries, so
 * an .ide anim name with no file (the peds' "man") resolves to -1 and the
 * model has no block to load. */
static void
FileSetAddModel(SFileSet *set, int model)
{
	if (model < 0 || model >= g_numModels)
		return;
	const SDataModel *m = &g_models[model];
	FileSetAdd(set, ModelFileName(m, "dff"));
	FileSetAdd(set, ModelFileName(m, "txd"));
	if (strcmp(m->anim, "null") != 0 && m->anim[0]) {
		const char *animFile = AnimFileName(m);
		if (FindDirEntry(animFile))
			FileSetAdd(set, animFile);
	}
}

static void
FileSetAddModels(SFileSet *set, const int32_t *models, int count)
{
	for (int i = 0; i < count; i++)
		if (models[i] >= 0)
			FileSetAddModel(set, models[i]);
}

static void
FileSetResolve(SFileSet *set)
{
	set->sectors = 0;
	set->missing = 0;
	for (int i = 0; i < set->count; i++) {
		const SDirEntry *e = FindDirEntry(set->names[i]);
		if (e)
			set->sectors += e->size;
		else
			set->missing++;
	}
}

/* The representative zone set: the default pedestrian group and the first
 * three gangs (a typical zone; the real masks come from the script). */
static void
BuildZoneSet(SFileSet *set, const int32_t *pedGroup, int numGangs)
{
	for (int i = 0; i < NUMMODELSPERPEDGROUP; i++)
		if (pedGroup[i] >= 0)
			FileSetAddModel(set, pedGroup[i]);
	for (int g = 0; g < numGangs; g++) {
		if (g_gangPed[g][0] >= 0)
			FileSetAddModel(set, g_gangPed[g][0]);
		if (g_gangPed[g][1] >= 0)
			FileSetAddModel(set, g_gangPed[g][1]);
		if (g_gangCar[g] >= 0)
			FileSetAddModel(set, g_gangCar[g]);
	}
}

static void
TestShippedSets(void)
{
	printf("D. the shipped sets\n");

	int classModels = 0;
	for (int c = 0; c < NUM_CLASSES; c++)
		classModels += g_classCounts[c];
	printf("  the car class set: %d models over %d ratings (%s)\n",
	       classModels, NUM_CLASSES, g_dataDerived ? "derived from default.ide" : "the recorded counts");
	if (g_dataDerived)
		Check(classModels == RECORDED_CLASS_MODELS, "the shipped class rows are the recorded 56");
	else
		printf("  the installed data is not readable; pass --data <the game folder> for the derived numbers\n");

	/* The pins of the representative sets. The zone is the default
	 * pedestrian group and the first three gangs (a typical zone; the real
	 * masks come from the script). Without the data the composition is the
	 * recorded one: 16 group models, 6 gang peds and 3 gang cars. */
	const int32_t *pedGroup = g_numPedGroups > 0 ? g_pedGroups[0] : NULL;
	int32_t fallbackGroup[NUMMODELSPERPEDGROUP];
	if (pedGroup == NULL) {
		for (int i = 0; i < NUMMODELSPERPEDGROUP; i++)
			fallbackGroup[i] = 900 + i;
		pedGroup = fallbackGroup;
	}

	int32_t zonePeds[64], zoneCars[8];
	int numZonePeds = 0, numZoneCars = 0;
	for (int i = 0; i < NUMMODELSPERPEDGROUP; i++)
		if (pedGroup[i] >= 0)
			zonePeds[numZonePeds++] = pedGroup[i];
	int gangPedCount = 0, gangCarCount = 0;
	if (g_dataDerived) {
		for (int g = 0; g < 3; g++) {
			for (int p = 0; p < 2; p++)
				if (g_gangPed[g][p] >= 0) {
					zonePeds[numZonePeds++] = g_gangPed[g][p];
					gangPedCount++;
				}
			if (g_gangCar[g] >= 0) {
				zoneCars[numZoneCars++] = g_gangCar[g];
				gangCarCount++;
			}
		}
	} else {
		for (int i = 0; i < 6; i++)
			zonePeds[numZonePeds++] = 1000 + i;
		for (int i = 0; i < 3; i++)
			zoneCars[numZoneCars++] = 1100 + i;
		gangPedCount = 6;
		gangCarCount = 3;
	}

	/* The plan of the representative zone: the class set, the group and the
	 * three gangs, the shared models one pin. */
	SSpawnReadyPlan plan;
	int pins = 0, vehiclePins = 0;
	for (int c = 0; c < NUM_CLASSES; c++)
		for (int i = 0; i < g_classCounts[c]; i++) {
			SSpawnReadyPin pin = {};
			pin.model = g_classModels[c][i];
			pin.sets = SPAWN_READY_SET_CAR;
			plan.AddPin(pin, true);
			pins++;
			vehiclePins++;
		}
	for (int i = 0; i < numZonePeds; i++) {
		if (plan.FindPin(zonePeds[i]) >= 0)
			continue;
		SSpawnReadyPin pin = {};
		pin.model = zonePeds[i];
		pin.sets = SPAWN_READY_SET_ZONE;
		plan.AddPin(pin, false);
		pins++;
	}
	for (int i = 0; i < numZoneCars; i++) {
		if (plan.FindPin(zoneCars[i]) >= 0)
			continue;
		SSpawnReadyPin pin = {};
		pin.model = zoneCars[i];
		pin.sets = SPAWN_READY_SET_ZONE;
		plan.AddPin(pin, true);
		pins++;
		vehiclePins++;
	}

	Check(pins <= SPAWN_READY_MAX_PINS, "the representative sets fit the pin array");
	Check(vehiclePins <= SPAWN_READY_PIN_BUDGET, "the representative sets fit the vehicle budget");
	printf("  the representative zone: %d group models + %d gang peds + %d gang cars\n",
	       numZonePeds - gangPedCount, gangPedCount, gangCarCount);
	printf("  the pins: %d total, %d vehicle; the budget %d leaves %d vehicle pins and %d tracker slots free\n",
	       pins, vehiclePins, SPAWN_READY_PIN_BUDGET,
	       SPAWN_READY_PIN_BUDGET - vehiclePins, MAXVEHICLESLOADED - vehiclePins);

	/* The widest zone the data can ask for: every gang the script can set.
	 * The recorded composition: 8 gangs with peds, 5 with cars. */
	int widePeds = 0, wideCars = 0;
	if (g_dataDerived) {
		for (int g = 0; g < 9; g++) {
			for (int p = 0; p < 2; p++)
				if (g_gangPed[g][p] >= 0)
					widePeds++;
			if (g_gangCar[g] >= 0)
				wideCars++;
		}
	} else {
		widePeds = 16;
		wideCars = 5;
	}
	int widePins = classModels + (numZonePeds - gangPedCount) + widePeds + wideCars;
	int wideVehiclePins = classModels + wideCars;
	Check(widePins <= SPAWN_READY_MAX_PINS, "the widest zone fits the pin array");
	Check(wideVehiclePins <= SPAWN_READY_PIN_BUDGET, "the widest zone fits the vehicle budget");
	printf("  the widest zone: %d gang peds + %d gang cars, at most %d pins total, %d vehicle; the tracker keeps %d slots free\n",
	       widePeds, wideCars, widePins, wideVehiclePins, MAXVEHICLESLOADED - wideVehiclePins);

	/* The files and the bytes the prewarm reads. */
	SFileSet carSet = {}, zoneSet = {}, unionSet = {};
	for (int c = 0; c < NUM_CLASSES; c++)
		FileSetAddModels(&carSet, g_classModels[c], g_classCounts[c]);
	BuildZoneSet(&zoneSet, pedGroup, 3);
	FileSetResolve(&carSet);
	FileSetResolve(&zoneSet);
	for (int i = 0; i < carSet.count; i++)
		FileSetAdd(&unionSet, carSet.names[i]);
	for (int i = 0; i < zoneSet.count; i++)
		FileSetAdd(&unionSet, zoneSet.names[i]);
	FileSetResolve(&unionSet);

	printf("  the car set: %d files, %u sectors (%.2f MB)\n",
	       carSet.count, carSet.sectors, carSet.sectors * 2048.0 / (1024.0 * 1024.0));
	printf("  the zone set: %d files, %u sectors (%.2f MB)\n",
	       zoneSet.count, zoneSet.sectors, zoneSet.sectors * 2048.0 / (1024.0 * 1024.0));
	printf("  the union (the boot prewarm): %d files, %u sectors (%.2f MB)\n",
	       unionSet.count, unionSet.sectors, unionSet.sectors * 2048.0 / (1024.0 * 1024.0));
	if (g_dataDerived) {
		Check(carSet.count == RECORDED_CAR_FILES && carSet.sectors == RECORDED_CAR_SECTORS,
			"the car set's files and sectors are the recorded ones");
		Check(zoneSet.count == RECORDED_ZONE_FILES && zoneSet.sectors == RECORDED_ZONE_SECTORS,
			"the zone set's files and sectors are the recorded ones");
		Check(unionSet.count == RECORDED_UNION_FILES && unionSet.sectors == RECORDED_UNION_SECTORS,
			"the union's files and sectors are the recorded ones");
		Check(carSet.missing == 0 && zoneSet.missing == 0, "every shipped file is in the image directory");
	} else {
		printf("  (the recorded rows: the car set %d files/%d sectors, the zone set %d/%d, the union %d/%d)\n",
		       RECORDED_CAR_FILES, RECORDED_CAR_SECTORS, RECORDED_ZONE_FILES, RECORDED_ZONE_SECTORS,
		       RECORDED_UNION_FILES, RECORDED_UNION_SECTORS);
	}

	printf("\n");
}

/* ------------------------------------------------------------------ *
 * E. the stall
 * ------------------------------------------------------------------ */

/* Read every file of a set from the image once, at its own size: the I/O
 * shape of the blocking path (CdStreamRead of the file's sectors, the sync,
 * the next file). Returns the wall seconds; the bytes are accumulated in
 * *bytesRead. */
static double
MeasureSetRead(const SFileSet *set, uint64_t *bytesRead)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/models/gta3.img", g_dataDir);
	FILE *img = fopen(path, "rb");
	if (img == NULL)
		return -1.0;
	static uint8_t buffer[64 * 1024];
	uint64_t total = 0;
	double start = NowSeconds();
	for (int i = 0; i < set->count; i++) {
		const SDirEntry *e = FindDirEntry(set->names[i]);
		if (e == NULL)
			continue;
		uint64_t offset = (uint64_t)e->offset * 2048;
		uint64_t size = (uint64_t)e->size * 2048;
		if (fseek(img, (long)offset, SEEK_SET) != 0)
			continue;
		uint64_t left = size;
		while (left > 0) {
			size_t want = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);
			size_t got = fread(buffer, 1, want, img);
			if (got == 0)
				break;
			left -= got;
		}
		total += size;
		g_sink += buffer[0];
	}
	double elapsed = NowSeconds() - start;
	fclose(img);
	if (bytesRead)
		*bytesRead = total;
	return elapsed;
}

static void
TestStall(void)
{
	printf("E. the stall\n");

	/* The engine-free plan's own per-frame cost over the full pin set. */
	static int32_t rows[NUM_CLASSES * MAX_CAR_MODELS_IN_ARRAY];
	static int32_t counts[NUM_CLASSES];
	static bool readyByModel[MAX_DATA_MODELS];
	static int32_t groupModels[NUMMODELSPERPEDGROUP];
	static bool groupReady[NUMMODELSPERPEDGROUP];
	for (int c = 0; c < NUM_CLASSES; c++) {
		counts[c] = g_classCounts[c];
		for (int i = 0; i < counts[c]; i++) {
			rows[c * MAX_CAR_MODELS_IN_ARRAY + i] = g_classModels[c][i];
			readyByModel[g_classModels[c][i]] = true;
		}
	}
	for (int i = 0; i < NUMMODELSPERPEDGROUP; i++) {
		groupModels[i] = g_classModels[0][i % g_classCounts[0]];
		groupReady[i] = true;
	}

	const int iterations = 100000;
	int32_t firstMissing;
	double start = NowSeconds();
	for (int i = 0; i < iterations; i++)
		g_sink += SpawnReadyClassArraysMissing(rows, MAX_CAR_MODELS_IN_ARRAY, counts, NUM_CLASSES, readyByModel, &firstMissing);
	double classWalk = (NowSeconds() - start) / iterations * 1e9;
	start = NowSeconds();
	for (int i = 0; i < iterations; i++)
		g_sink += SpawnReadyListMissing(groupModels, groupReady, NUMMODELSPERPEDGROUP, &firstMissing);
	double groupWalk = (NowSeconds() - start) / iterations * 1e9;

	SSpawnReadyPlan plan;
	for (int c = 0; c < NUM_CLASSES; c++)
		for (int i = 0; i < g_classCounts[c]; i++) {
			SSpawnReadyPin pin = {};
			pin.model = g_classModels[c][i];
			plan.AddPin(pin, true);
		}
	start = NowSeconds();
	for (int i = 0; i < iterations; i++)
		g_sink += plan.FindPin(plan.m_aPins[i % plan.m_nPins].model);
	double findPin = (NowSeconds() - start) / iterations * 1e9;

	printf("  the plan's own cost over the %d pins: the class walk %.0f ns, the group walk %.0f ns, the pin find %.0f ns\n",
	       plan.m_nPins, classWalk, groupWalk, findPin);
	printf("  the engine's pump adds three flag reads a pin (AssertPins), no arithmetic of this scale\n");

	/* The I/O share of the blocking path, against the real image. */
	if (g_imageReadable) {
		SFileSet carSet = {}, zoneSet = {}, unionSet = {};
		for (int c = 0; c < NUM_CLASSES; c++)
			FileSetAddModels(&carSet, g_classModels[c], g_classCounts[c]);
		const int32_t *pedGroup = g_numPedGroups > 0 ? g_pedGroups[0] : NULL;
		int32_t fallback[NUMMODELSPERPEDGROUP];
		if (pedGroup == NULL) {
			for (int i = 0; i < NUMMODELSPERPEDGROUP; i++)
				fallback[i] = 900 + i;
			pedGroup = fallback;
		}
		BuildZoneSet(&zoneSet, pedGroup, 3);
		for (int i = 0; i < carSet.count; i++)
			FileSetAdd(&unionSet, carSet.names[i]);
		for (int i = 0; i < zoneSet.count; i++)
			FileSetAdd(&unionSet, zoneSet.names[i]);

		uint64_t bytes;
		/* Warm the cache with one untimed pass over the union, then measure
		 * the three sets: the numbers are the warm-cache read, the disk adds
		 * its own rate over the bytes. */
		MeasureSetRead(&unionSet, &bytes);
		double unionMs = MeasureSetRead(&unionSet, &bytes) * 1000.0;
		double unionBytes = (double)bytes;
		double zoneMs = MeasureSetRead(&zoneSet, &bytes) * 1000.0;
		double zoneBytes = (double)bytes;
		double carMs = MeasureSetRead(&carSet, &bytes) * 1000.0;
		double carBytes = (double)bytes;

		printf("  the measured read (warm cache, the image's own files):\n");
		printf("    the union    %6.1f MB in %7.1f ms (the boot prewarm)\n", unionBytes / (1024.0 * 1024.0), unionMs);
		printf("    the zone set %6.1f MB in %7.1f ms (a transition's own set)\n", zoneBytes / (1024.0 * 1024.0), zoneMs);
		printf("    the car set  %6.1f MB in %7.1f ms\n", carBytes / (1024.0 * 1024.0), carMs);
		printf("  the bound: the cold read is the disk's own rate over those bytes; the whole\n");
		printf("  stall (the file processing included) is the running game's ms_nLastStallMs\n");
	} else {
		printf("  the image is not readable; the I/O is not measured (pass --data <the game folder>)\n");
		printf("  the recorded rows: the boot union %d sectors (%.2f MB), a transition's zone %d sectors (%.2f MB)\n",
		       RECORDED_UNION_SECTORS, RECORDED_UNION_SECTORS * 2048.0 / (1024.0 * 1024.0),
		       RECORDED_ZONE_SECTORS, RECORDED_ZONE_SECTORS * 2048.0 / (1024.0 * 1024.0));
	}

	printf("\n");
}

/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
	const char *dataDir = DefaultDataDir();
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--data") == 0 && i + 1 < argc)
			dataDir = argv[++i];
		else if (strncmp(argv[i], "--data=", 7) == 0)
			dataDir = argv[i] + 7;
		else {
			printf("usage: %s [--data <the game folder>]\n", argv[0]);
			return 2;
		}
	}

	printf("the spawn readiness and prewarm (VICE-151)\n");
	printf("the budget: MAXVEHICLESLOADED %d, VEHICLEMODELSIZE %d, the pin budget %d (the reserve %d)\n",
	       MAXVEHICLESLOADED, VEHICLEMODELSIZE, SPAWN_READY_PIN_BUDGET, SPAWN_READY_PIN_RESERVE);
	printf("the arrays: %d pins, %d group slots, %d class ratings\n\n",
	       SPAWN_READY_MAX_PINS, NUMMODELSPERPEDGROUP, NUM_CLASSES);

	if (dataDir != NULL && dataDir[0]) {
		snprintf(g_dataDir, sizeof(g_dataDir), "%s", dataDir);
		char path[1024];
		snprintf(path, sizeof(path), "%s/data/default.ide", g_dataDir);
		ParseDefaultIde(path);
		snprintf(path, sizeof(path), "%s/data/pedgrp.dat", g_dataDir);
		ParsePedGroups(path);
		snprintf(path, sizeof(path), "%s/models/gta3.dir", g_dataDir);
		ParseGta3Dir(path);
		g_dataDerived = g_numModels > 0;
		if (g_dataDerived) {
			snprintf(path, sizeof(path), "%s/models/gta3.img", g_dataDir);
			FILE *img = fopen(path, "rb");
			if (img) {
				g_imageReadable = true;
				fclose(img);
			}
		}
	}
	BuildGangTable();
	BuildClassArrays();

	TestModelPredicate();
	TestWholeSetPredicate();
	TestPlan();
	TestShippedSets();
	TestStall();

	printf("%lld checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
