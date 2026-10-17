/*
 * The evidence program for the occupancy store (ticket VICE-149).
 *
 * This program reads src/control/SpawnOccupancy.h on its own (no engine
 * include beyond config.h) and drives the store - the engine-free
 * SSpawnOccupancyStore, the code the game's CSpawnOccupancy wraps - through
 * synthetic streams:
 *
 *	A. The grid. The cell mapping over the corners, the cell edges and the
 *	   full width: every position lands in its own 16 m cell, the grid's
 *	   edge is the world's, and a position off the grid (a NaN and a huge
 *	   value included) has no cell.
 *
 *	B. The lifecycle streams. The constructor's registration (a registered
 *	   handle counts nothing until its first placement, so the constructor
 *	   never reads the unplaced matrix), then creates, moves and removes
 *	   over a seeded stream, with the counts checked against a reference
 *	   the program keeps itself (from the entities' positions, never from
 *	   the store's records): after every update the totals and the touched
 *	   cells, and every two thousand updates the whole grid. Then a
 *	   world-clear-style bulk removal (the pool swept by handle, no
 *	   positions in hand), a pool-valve-style churn (the oldest out, a new
 *	   one in, over the pool's own slot reuse) and a drift removal
 *	   (entities moved without the store watching, then deleted).
 *
 *	C. The grid edge. An entity crosses the west edge and comes back: the
 *	   total drops and returns, the cell it left stays clean, and the
 *	   removal leaves every cell zero.
 *
 *	D. The guards. Bad handles, unknown classes, NaN and huge positions,
 *	   the readers and the mutation guard: an invalid update moves no
 *	   count, an unknown class reads zero, a move off the grid drops the
 *	   count, a battery of reads writes not one byte (the whole store is
 *	   byte-compared around it), and an injected one-count fault is caught
 *	   by the reference comparison and reads clean again once undone (the
 *	   guard that the checks are not vacuous).
 *
 *	E. Bounded movement. A same-cell move writes nothing; a crossing move
 *	   touches exactly the two cells and nothing else; the totals never
 *	   move. Checked by diffing the store's own arrays.
 *
 *	F. Reservations. The reserve, the second request refused, the expiry,
 *	   the re-take after expiry, the release, the out-of-range ids, the
 *	   game timer's wrap, a clock that moved backwards and an entry older
 *	   than the wrap half.
 *
 *	G. The cost. The per-update price of a fresh create, a create+remove
 *	   churn, a same-cell move and a crossing move, over precomputed
 *	   positions (the RNG is not inside the timed loops).
 *
 * What this program cannot reach: the engine wrapper CSpawnOccupancy and the
 * hooks themselves (the pools, the constructors, CWorld::Add, the aircraft,
 * Resync) are the game's, checked by the build and by inspection; this
 * harness drives the store they call.
 *
 * Build and run:
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 -o /tmp/spawn-occupancy-harness utils/spawn-service/occupancy.cpp
 *	/tmp/spawn-occupancy-harness
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "SpawnOccupancy.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

static int g_failures = 0;
static long long g_checks = 0;

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

static double
NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ the rng */

static uint32_t g_seed = 20261017u;

static uint32_t
Rand(void)
{
	g_seed = g_seed * 1664525u + 1013904223u;
	return g_seed;
}

static int32_t
RandInt(int32_t n)
{
	return (int32_t)(Rand() % (uint32_t)n);
}

static float
RandFloat(float lo, float hi)
{
	return lo + (hi - lo) * (float)((Rand() >> 8) * (1.0 / 16777216.0));
}

static float
Clamp(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

/* ------------------------------------------------------- the reference model */

/* The store under test: the same struct the game instantiates. */
static SSpawnOccupancyStore ms_aStore;

/*
 * The reference counts live beside the store: one count per class per cell,
 * updated from the entities' positions (a floor() mapping, written here, not
 * the store's cast), plus the entity table. It is the independent copy the
 * acceptance compares against; the store's own records are not read for it.
 */
#define REF_MAX 4096

struct SRefEntity
{
	int32_t handle;
	int32_t cls;
	float x, y;
};

static SRefEntity ms_aRef[REF_MAX];
static int32_t ms_nRef;
static int32_t ms_aRefFree[REF_MAX];
static int32_t ms_nRefFree;
static int16_t ms_aRefVehicle[NUMVEHICLES];	/* handle -> entity index, -1 free */
static int16_t ms_aRefPed[NUMPEDS];
static int32_t ms_aRefCount[SPAWN_OCCUPANCY_CLASSES][SPAWN_OCCUPANCY_CELLS];
static int32_t ms_aRefTotal[SPAWN_OCCUPANCY_CLASSES];
static int32_t ms_aLive[REF_MAX];		/* live entity indices */
static int32_t ms_aLivePos[REF_MAX];
static int32_t ms_nLive;

static bool
RefCell(float x, float y, int32_t &cell)
{
	int32_t cx = (int32_t)floorf((x - SPAWN_OCCUPANCY_GRID_MIN_X) / SPAWN_OCCUPANCY_CELL_SIZE);
	int32_t cy = (int32_t)floorf((y - SPAWN_OCCUPANCY_GRID_MIN_Y) / SPAWN_OCCUPANCY_CELL_SIZE);
	if (cx < 0 || cx >= SPAWN_OCCUPANCY_CELLS_X || cy < 0 || cy >= SPAWN_OCCUPANCY_CELLS_Y)
		return false;
	cell = cy * SPAWN_OCCUPANCY_CELLS_X + cx;
	return true;
}

static int16_t *
RefSlot(int32_t cls, int32_t handle)
{
	if (cls == SPAWN_OCCUPANCY_VEHICLE && handle >= 0 && handle < NUMVEHICLES)
		return &ms_aRefVehicle[handle];
	if (cls == SPAWN_OCCUPANCY_PED && handle >= 0 && handle < NUMPEDS)
		return &ms_aRefPed[handle];
	return 0;
}

static void
RefReset(void)
{
	for (int32_t i = 0; i < NUMVEHICLES; i++)
		ms_aRefVehicle[i] = -1;
	for (int32_t i = 0; i < NUMPEDS; i++)
		ms_aRefPed[i] = -1;
	for (int32_t cls = 0; cls < SPAWN_OCCUPANCY_CLASSES; cls++) {
		for (int32_t cell = 0; cell < SPAWN_OCCUPANCY_CELLS; cell++)
			ms_aRefCount[cls][cell] = 0;
		ms_aRefTotal[cls] = 0;
	}
	ms_nRef = 0;
	ms_nRefFree = 0;
	ms_nLive = 0;
}

/* A creation (the position must land on the grid; the streams keep it there). */
static void
RefAdd(int32_t cls, int32_t handle, float x, float y)
{
	/* The entity records are reused like the pool slots are: a removal
	 * hands its record back, the next creation takes it. */
	int32_t idx;
	if (ms_nRefFree > 0)
		idx = ms_aRefFree[--ms_nRefFree];
	else
		idx = ms_nRef++;
	int16_t *pSlot = RefSlot(cls, handle);
	SRefEntity &e = ms_aRef[idx];
	e.handle = handle;
	e.cls = cls;
	e.x = x;
	e.y = y;
	*pSlot = (int16_t)idx;
	ms_aLivePos[idx] = ms_nLive;
	ms_aLive[ms_nLive++] = idx;
	int32_t cell;
	if (RefCell(x, y, cell)) {
		ms_aRefCount[cls][cell]++;
		ms_aRefTotal[cls]++;
	}
}

static void
RefRemove(int32_t cls, int32_t handle)
{
	int16_t *pSlot = RefSlot(cls, handle);
	int32_t idx = *pSlot;
	SRefEntity &e = ms_aRef[idx];
	int32_t cell;
	if (RefCell(e.x, e.y, cell)) {
		ms_aRefCount[cls][cell]--;
		ms_aRefTotal[cls]--;
	}
	ms_aRefFree[ms_nRefFree++] = idx;
	*pSlot = -1;
	int32_t pos = ms_aLivePos[idx];
	ms_aLive[pos] = ms_aLive[--ms_nLive];
	ms_aLivePos[ms_aLive[pos]] = pos;
}

static void
RefMove(int32_t cls, int32_t handle, float x, float y)
{
	int16_t *pSlot = RefSlot(cls, handle);
	SRefEntity &e = ms_aRef[*pSlot];
	int32_t oldCell = 0, newCell = 0;
	bool oldOn = RefCell(e.x, e.y, oldCell);
	bool newOn = RefCell(x, y, newCell);
	if (oldOn)
		ms_aRefCount[cls][oldCell]--;
	if (newOn)
		ms_aRefCount[cls][newCell]++;
	if (oldOn && !newOn)
		ms_aRefTotal[cls]--;
	if (!oldOn && newOn)
		ms_aRefTotal[cls]++;
	e.x = x;
	e.y = y;
}

/* The reference checks: the totals after every update, the touched cells, and
 * the whole grid periodically. */
static void
RefCheckTotals(const char *what)
{
	for (int32_t cls = 0; cls < SPAWN_OCCUPANCY_CLASSES; cls++) {
		g_checks++;
		if (ms_aStore.TotalCount((ESpawnOccupancyClass)cls) != ms_aRefTotal[cls])
			Fail(what);
	}
}

static void
RefCheckCell(int32_t cls, int32_t cell)
{
	if (cell < 0)
		return;
	g_checks++;
	if (ms_aStore.CountAtCell((ESpawnOccupancyClass)cls, cell % SPAWN_OCCUPANCY_CELLS_X, cell / SPAWN_OCCUPANCY_CELLS_X) != ms_aRefCount[cls][cell])
		Fail("a touched cell's count must match the reference");
}

static void
RefCheckFull(const char *what)
{
	for (int32_t cls = 0; cls < SPAWN_OCCUPANCY_CLASSES; cls++) {
		int32_t total = 0;
		for (int32_t cell = 0; cell < SPAWN_OCCUPANCY_CELLS; cell++) {
			g_checks++;
			if (ms_aStore.CountAtCell((ESpawnOccupancyClass)cls, cell % SPAWN_OCCUPANCY_CELLS_X, cell / SPAWN_OCCUPANCY_CELLS_X) != ms_aRefCount[cls][cell]) {
				Fail(what);
				return;
			}
			total += ms_aRefCount[cls][cell];
		}
		g_checks++;
		if (ms_aStore.TotalCount((ESpawnOccupancyClass)cls) != total || total != ms_aRefTotal[cls]) {
			Fail(what);
			return;
		}
	}
}

/* The mutation guard's silent read: the number of cells (and totals) where
 * the store and the reference disagree. The checks above report their
 * failures; this one tells the guard whether the machinery would have seen
 * an injected fault, without printing anything. */
static int32_t
RefMismatches(void)
{
	int32_t n = 0;
	for (int32_t cls = 0; cls < SPAWN_OCCUPANCY_CLASSES; cls++) {
		for (int32_t cell = 0; cell < SPAWN_OCCUPANCY_CELLS; cell++) {
			if (ms_aStore.CountAtCell((ESpawnOccupancyClass)cls, cell % SPAWN_OCCUPANCY_CELLS_X, cell / SPAWN_OCCUPANCY_CELLS_X) != ms_aRefCount[cls][cell])
				n++;
		}
		if (ms_aStore.TotalCount((ESpawnOccupancyClass)cls) != ms_aRefTotal[cls])
			n++;
	}
	return n;
}

/* ------------------------------------------------------------------ A. the grid */

static void
SectionGrid(void)
{
	int32_t cell;

	printf("A. the grid (16 m cells; %d x %d over the world)\n",
		SPAWN_OCCUPANCY_CELLS_X, SPAWN_OCCUPANCY_CELLS_Y);

	/* The corner, the first edge and the two sides of a cell boundary. */
	Check(SSpawnOccupancyStore::CellAt(SPAWN_OCCUPANCY_GRID_MIN_X, SPAWN_OCCUPANCY_GRID_MIN_Y, cell) && cell == 0,
		"the world's corner must be cell 0");
	Check(SSpawnOccupancyStore::CellAt(SPAWN_OCCUPANCY_GRID_MIN_X + SPAWN_OCCUPANCY_CELL_SIZE - 0.01f,
		SPAWN_OCCUPANCY_GRID_MIN_Y + SPAWN_OCCUPANCY_CELL_SIZE - 0.01f, cell) && cell == 0,
		"the first cell must end at its edge");
	Check(SSpawnOccupancyStore::CellAt(SPAWN_OCCUPANCY_GRID_MIN_X + SPAWN_OCCUPANCY_CELL_SIZE,
		SPAWN_OCCUPANCY_GRID_MIN_Y + SPAWN_OCCUPANCY_CELL_SIZE, cell) && cell == SPAWN_OCCUPANCY_CELLS_X + 1,
		"the cell after the first edge must be (1, 1)");
	Check(!SSpawnOccupancyStore::CellAt(SPAWN_OCCUPANCY_GRID_MIN_X - 0.5f, 0.0f, cell),
		"a position past the west edge must have no cell");
	Check(!SSpawnOccupancyStore::CellAt(0.0f, SPAWN_OCCUPANCY_GRID_MIN_Y - 0.5f, cell),
		"a position past the south edge must have no cell");
	Check(!SSpawnOccupancyStore::CellAt(SPAWN_OCCUPANCY_GRID_MIN_X + SPAWN_OCCUPANCY_CELLS_X * SPAWN_OCCUPANCY_CELL_SIZE, 0.0f, cell),
		"a position at the east edge must have no cell");
	Check(!SSpawnOccupancyStore::CellAt(0.0f, SPAWN_OCCUPANCY_GRID_MIN_Y + SPAWN_OCCUPANCY_CELLS_Y * SPAWN_OCCUPANCY_CELL_SIZE, cell),
		"a position at the north edge must have no cell");

	/* Every column and every row of the map: the midpoint of a cell must map
	 * back to it, over the whole grid. */
	int32_t walked = 0;
	for (int32_t cx = 0; cx < SPAWN_OCCUPANCY_CELLS_X; cx++) {
		float x = SPAWN_OCCUPANCY_GRID_MIN_X + (cx + 0.5f) * SPAWN_OCCUPANCY_CELL_SIZE;
		g_checks++;
		if (!SSpawnOccupancyStore::CellAt(x, SPAWN_OCCUPANCY_GRID_MIN_Y + 0.5f * SPAWN_OCCUPANCY_CELL_SIZE, cell) || cell != cx)
			Fail("the column sweep must land in its own cell");
		walked++;
	}
	for (int32_t cy = 0; cy < SPAWN_OCCUPANCY_CELLS_Y; cy++) {
		float y = SPAWN_OCCUPANCY_GRID_MIN_Y + (cy + 0.5f) * SPAWN_OCCUPANCY_CELL_SIZE;
		g_checks++;
		if (!SSpawnOccupancyStore::CellAt(SPAWN_OCCUPANCY_GRID_MIN_X + 0.5f * SPAWN_OCCUPANCY_CELL_SIZE, y, cell) || cell != cy * SPAWN_OCCUPANCY_CELLS_X)
			Fail("the row sweep must land in its own cell");
		walked++;
	}
	Check(walked == SPAWN_OCCUPANCY_CELLS_X + SPAWN_OCCUPANCY_CELLS_Y,
		"the sweeps must cover every column and row");
	printf("  the corner, the edges and every column and row: %d checks\n", walked + 7);
}

/* ------------------------------------------------- B. the lifecycle streams */

/*
 * The constructor's registration: the handle is marked, nothing is read and
 * nothing is counted until the first placement - the game's shape is
 * register (the constructor), place (the world add), move (the physics
 * step), remove (the destructor).
 */
static void
SectionRegister(void)
{
	printf("B. the constructor's registration (no position read)\n");
	RefReset();
	ms_aStore.Reset();

	/* A registered handle holds no count until its first placement. */
	ms_aStore.Register(SPAWN_OCCUPANCY_VEHICLE, 7);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE) == 0,
		"a registered handle must not be counted");
	Check(ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, 0.0f, 0.0f) == 0,
		"a registered handle must count nowhere");

	ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, 7, 100.0f, 100.0f);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE) == 1 &&
	      ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, 100.0f, 100.0f) == 1,
		"the first placement must count the registered handle");
	ms_aStore.Remove(SPAWN_OCCUPANCY_VEHICLE, 7);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE) == 0,
		"a placed handle's removal must take the count back");

	/* A handle created and destroyed before any placement leaves nothing. */
	ms_aStore.Register(SPAWN_OCCUPANCY_PED, 3);
	ms_aStore.Remove(SPAWN_OCCUPANCY_PED, 3);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_PED) == 0,
		"an unplaced handle's removal must leave nothing");

	/* The game's own walk, against the reference. */
	for (int32_t i = 0; i < 400; i++) {
		ms_aStore.Register(SPAWN_OCCUPANCY_VEHICLE, i);
		float x = RandFloat(-1300.0f, 1300.0f);
		float y = RandFloat(-1600.0f, 1600.0f);
		ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
		RefAdd(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
	}
	RefCheckTotals("the register-place walk must count like the reference");
	RefCheckFull("the whole grid must match after the register-place walk");
	for (int32_t i = 0; i < 400; i++) {
		ms_aStore.Remove(SPAWN_OCCUPANCY_VEHICLE, i);
		RefRemove(SPAWN_OCCUPANCY_VEHICLE, i);
	}
	RefReset();
	RefCheckFull("the register-place walk must empty cleanly");
	printf("  register -> place -> move -> remove: the counts follow the placement\n");
}

/* A seeded create walk: hVehicles vehicles and hPeds pedestrians, each at a
 * random position well inside the map. */
static void
StreamCreate(int32_t nVehicles, int32_t nPeds)
{
	for (int32_t i = 0; i < nVehicles; i++) {
		float x = RandFloat(-1300.0f, 1300.0f);
		float y = RandFloat(-1600.0f, 1600.0f);
		ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
		RefAdd(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
	}
	for (int32_t i = 0; i < nPeds; i++) {
		float x = RandFloat(-1300.0f, 1300.0f);
		float y = RandFloat(-1600.0f, 1600.0f);
		ms_aStore.Create(SPAWN_OCCUPANCY_PED, i, x, y);
		RefAdd(SPAWN_OCCUPANCY_PED, i, x, y);
	}
	RefCheckTotals("the totals must match the reference after the creates");
}

/* A random move stream: the live entities walk around the city (a step below
 * the map's edge keeps them on it), and after every move the totals and the
 * two cells the walk could have touched are compared. */
static void
StreamMove(int32_t nMoves)
{
	for (int32_t i = 0; i < nMoves; i++) {
		int32_t idx = ms_aLive[RandInt(ms_nLive)];
		SRefEntity &e = ms_aRef[idx];
		float nx = Clamp(e.x + RandFloat(-24.0f, 24.0f), -1300.0f, 1300.0f);
		float ny = Clamp(e.y + RandFloat(-24.0f, 24.0f), -1600.0f, 1600.0f);
		int32_t oldCell = 0, newCell = 0;
		bool oldOn = RefCell(e.x, e.y, oldCell);
		bool newOn = RefCell(nx, ny, newCell);
		ms_aStore.Move((ESpawnOccupancyClass)e.cls, e.handle, nx, ny);
		RefMove(e.cls, e.handle, nx, ny);
		RefCheckTotals("the totals must not move with a move");
		RefCheckCell(e.cls, oldOn ? oldCell : -1);
		RefCheckCell(e.cls, newOn ? newCell : -1);
		if (i % 2000 == 0)
			RefCheckFull("the whole grid must match the reference");
	}
}

/* A random removal stream: half of the live entities die in random order. */
static void
StreamRemove(int32_t nRemovals)
{
	for (int32_t i = 0; i < nRemovals && ms_nLive > 0; i++) {
		int32_t idx = ms_aLive[RandInt(ms_nLive)];
		SRefEntity &e = ms_aRef[idx];
		int32_t cls = e.cls;
		int32_t handle = e.handle;
		int32_t cell = 0;
		bool on = RefCell(e.x, e.y, cell);
		ms_aStore.Remove((ESpawnOccupancyClass)cls, handle);
		RefRemove(cls, handle);
		RefCheckTotals("the totals must match the reference after a removal");
		RefCheckCell(cls, on ? cell : -1);
	}
	RefCheckFull("the whole grid must match the reference after the removals");
}

static void
SectionStreams(void)
{
	printf("B. the streams (a seeded create/move/remove walk)\n");
	RefReset();
	ms_aStore.Reset();

	StreamCreate(800, 1200);
	printf("  created 800 vehicles and 1200 pedestrians; totals: %d / %d\n",
		ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE), ms_aStore.TotalCount(SPAWN_OCCUPANCY_PED));

	StreamMove(20000);
	printf("  moved 20000 times; totals still: %d / %d\n",
		ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE), ms_aStore.TotalCount(SPAWN_OCCUPANCY_PED));

	StreamRemove(1000);
	printf("  removed 1000; live %d, totals: %d / %d\n",
		ms_nLive, ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE), ms_aStore.TotalCount(SPAWN_OCCUPANCY_PED));

	/* Pool handle reuse: the dead handles come back with new cars. */
	int32_t reused = 0;
	for (int32_t i = 0; i < NUMVEHICLES && reused < 300; i++) {
		if (ms_aRefVehicle[i] == -1) {
			float x = RandFloat(-1300.0f, 1300.0f);
			float y = RandFloat(-1600.0f, 1600.0f);
			ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
			RefAdd(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
			reused++;
		}
	}
	RefCheckTotals("handle reuse must count fresh");
	RefCheckFull("the whole grid must match after handle reuse");

	/* The rest dies; the grid must come back empty. */
	StreamRemove(ms_nLive);
	for (int32_t cls = 0; cls < SPAWN_OCCUPANCY_CLASSES; cls++)
		Check(ms_aStore.TotalCount((ESpawnOccupancyClass)cls) == 0, "an empty population must leave a zero total");
	for (int32_t cell = 0; cell < SPAWN_OCCUPANCY_CELLS; cell++) {
		g_checks++;
		if (ms_aStore.CountAtCell(SPAWN_OCCUPANCY_VEHICLE, cell % SPAWN_OCCUPANCY_CELLS_X, cell / SPAWN_OCCUPANCY_CELLS_X) != 0 ||
		    ms_aStore.CountAtCell(SPAWN_OCCUPANCY_PED, cell % SPAWN_OCCUPANCY_CELLS_X, cell / SPAWN_OCCUPANCY_CELLS_X) != 0)
			Fail("an empty population must leave every cell zero");
	}
	printf("  the handles were reused (300), the population emptied; every cell zero\n");
}

/* A world clear: the pool is swept by handle, positions not in hand. */
static void
SectionWorldClear(void)
{
	printf("B. a world-clear-style bulk removal\n");
	RefReset();
	ms_aStore.Reset();
	StreamCreate(700, 900);

	/* The clear walks the pools: every handle in turn, no positions. */
	for (int32_t i = 0; i < NUMVEHICLES; i++) {
		if (ms_aRefVehicle[i] != -1)
			ms_aStore.Remove(SPAWN_OCCUPANCY_VEHICLE, i);
	}
	for (int32_t i = 0; i < NUMPEDS; i++) {
		if (ms_aRefPed[i] != -1)
			ms_aStore.Remove(SPAWN_OCCUPANCY_PED, i);
	}
	for (int32_t cls = 0; cls < SPAWN_OCCUPANCY_CLASSES; cls++)
		Check(ms_aStore.TotalCount((ESpawnOccupancyClass)cls) == 0, "the clear must leave a zero total");
	RefReset();
	RefCheckFull("the clear must leave every cell zero");
	printf("  swept the two pools by handle; every cell zero\n");
}

/* The pool valve: the pool is full, the oldest car dies, a new one arrives;
 * the handle it takes is the freed one (the pool's own reuse). */
static void
SectionValve(void)
{
#define VALVE_SLOTS 300
	printf("B. a pool-valve-style churn (%d live slots)\n", VALVE_SLOTS);
	RefReset();
	ms_aStore.Reset();

	static int32_t aSlotHandle[VALVE_SLOTS];
	for (int32_t i = 0; i < VALVE_SLOTS; i++) {
		float x = RandFloat(-1300.0f, 1300.0f);
		float y = RandFloat(-1600.0f, 1600.0f);
		aSlotHandle[i] = i;
		ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
		RefAdd(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
	}

	int32_t swaps = 100000;
	int32_t victim = 0;
	for (int32_t i = 0; i < swaps; i++) {
		/* The valve deletes the oldest car; the pool frees its slot and
		 * the creation that follows takes the freed handle. */
		int32_t oldHandle = aSlotHandle[victim];
		ms_aStore.Remove(SPAWN_OCCUPANCY_VEHICLE, oldHandle);
		RefRemove(SPAWN_OCCUPANCY_VEHICLE, oldHandle);

		float x = RandFloat(-1300.0f, 1300.0f);
		float y = RandFloat(-1600.0f, 1600.0f);
		ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, oldHandle, x, y);
		RefAdd(SPAWN_OCCUPANCY_VEHICLE, oldHandle, x, y);
		victim = (victim + 1) % VALVE_SLOTS;

		RefCheckTotals("the churn must keep the totals exact");
		if (i % 25000 == 0)
			RefCheckFull("the whole grid must match the reference through the churn");
	}
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE) == VALVE_SLOTS,
		"the churn must leave the slot count");
	printf("  %d swaps (the oldest out, a new one in, the freed handle); totals exact\n", swaps);

	/* Empty it again. */
	for (int32_t i = 0; i < VALVE_SLOTS; i++) {
		ms_aStore.Remove(SPAWN_OCCUPANCY_VEHICLE, aSlotHandle[i]);
		RefRemove(SPAWN_OCCUPANCY_VEHICLE, aSlotHandle[i]);
	}
	RefReset();
	RefCheckFull("the churn must empty cleanly");
}

/*
 * The drift removal: entities the store never saw move, then deleted. The
 * removal subtracts the recorded cell, so the sums stay exact even though the
 * cells the entities stand in have no idea they were ever counted.
 */
static void
SectionDrift(void)
{
	printf("B. a drift removal (moves the store never saw, then delete)\n");
	RefReset();
	ms_aStore.Reset();
	StreamCreate(400, 600);

	/* The store is not told about these moves; only the reference table is. */
	for (int32_t i = 0; i < ms_nLive; i++) {
		SRefEntity &e = ms_aRef[ms_aLive[i]];
		e.x = Clamp(e.x + 400.0f, -1300.0f, 1300.0f);
		e.y = Clamp(e.y + 400.0f, -1600.0f, 1600.0f);
	}
	RefCheckTotals("a drift must not move the totals");

	/* Now a world clear of the drifted population, by handle only. */
	for (int32_t i = 0; i < NUMVEHICLES; i++) {
		if (ms_aRefVehicle[i] != -1)
			ms_aStore.Remove(SPAWN_OCCUPANCY_VEHICLE, i);
	}
	for (int32_t i = 0; i < NUMPEDS; i++) {
		if (ms_aRefPed[i] != -1)
			ms_aStore.Remove(SPAWN_OCCUPANCY_PED, i);
	}
	for (int32_t cls = 0; cls < SPAWN_OCCUPANCY_CLASSES; cls++)
		Check(ms_aStore.TotalCount((ESpawnOccupancyClass)cls) == 0, "the drifted removal must leave a zero total");
	/* The reference still believed the drifted cells held the counts; zero it
	 * and check the store's own sums: 800 creates 800 removals, no leak. */
	RefReset();
	RefCheckFull("the drifted removal must leave every cell zero");
	printf("  the moves were never seen; the deletes still zeroed every cell\n");
}

/* ------------------------------------------------------------ C. the grid edge */

/*
 * An entity against the west edge crosses out of the world and comes back:
 * the total drops when it leaves and returns when it re-enters, the cell it
 * left stays clean, and the removal at the end leaves every cell zero.
 */
static void
SectionOffGrid(void)
{
	printf("C. the grid edge (a crossing, out and back)\n");
	RefReset();
	ms_aStore.Reset();

	float inX = SPAWN_OCCUPANCY_GRID_MIN_X + 4.0f;
	float inY = 0.0f;
	float outX = SPAWN_OCCUPANCY_GRID_MIN_X - 100.0f;
	int32_t cell;

	Check(!SSpawnOccupancyStore::CellAt(outX, inY, cell), "a position past the west edge has no cell");

	ms_aStore.Create(SPAWN_OCCUPANCY_PED, 0, inX, inY);
	RefAdd(SPAWN_OCCUPANCY_PED, 0, inX, inY);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_PED) == 1, "the entity starts counted");
	Check(ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_PED, inX, inY) == 1, "counted in its cell");

	ms_aStore.Move(SPAWN_OCCUPANCY_PED, 0, outX, inY);
	RefMove(SPAWN_OCCUPANCY_PED, 0, outX, inY);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_PED) == 0, "the total drops when the entity leaves the grid");
	Check(ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_PED, inX, inY) == 0, "the cell it left is clean");
	RefCheckFull("the whole grid must match the reference off the grid");

	ms_aStore.Move(SPAWN_OCCUPANCY_PED, 0, inX, inY);
	RefMove(SPAWN_OCCUPANCY_PED, 0, inX, inY);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_PED) == 1, "the total returns when the entity comes back");
	Check(ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_PED, inX, inY) == 1, "counted again where it stands");
	RefCheckFull("the whole grid must match the reference back on the grid");

	ms_aStore.Remove(SPAWN_OCCUPANCY_PED, 0);
	RefRemove(SPAWN_OCCUPANCY_PED, 0);
	RefReset();
	RefCheckFull("the removal must leave every cell zero");
	printf("  out: 1 -> 0, the left cell clean; back: 0 -> 1; remove: all zero\n");
}

/* ------------------------------------------------------------------ D. the guards */

/*
 * Bad input is inert, the readers write nothing, and the checks themselves
 * are guarded: an injected one-count fault must be caught.
 */
static void
SectionGuards(void)
{
	printf("D. the guards (invalid input, the readers, the mutation guard)\n");
	RefReset();
	ms_aStore.Reset();

	/* Bad handles and unknown classes move no count. */
	ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, 0, 100.0f, 100.0f);
	ms_aStore.Register(SPAWN_OCCUPANCY_VEHICLE, -1);
	ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, NUMVEHICLES, 200.0f, 200.0f);
	ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, NUMPEDS, 200.0f, 200.0f);
	ms_aStore.Remove(SPAWN_OCCUPANCY_VEHICLE, NUMVEHICLES);
	ms_aStore.Create(SPAWN_OCCUPANCY_CLASSES, 0, 200.0f, 200.0f);
	ms_aStore.Move(SPAWN_OCCUPANCY_CLASSES, 0, 300.0f, 300.0f);
	ms_aStore.Remove(SPAWN_OCCUPANCY_CLASSES, 0);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE) == 1 && ms_aStore.TotalCount(SPAWN_OCCUPANCY_PED) == 0,
		"a bad handle or class must move no count");
	Check(ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, 100.0f, 100.0f) == 1,
		"the one real entity must stand");

	/* Unknown classes read zero, by position, by cell and in the totals. */
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_CLASSES) == 0, "an unknown class reads a zero total");
	Check(ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_CLASSES, 100.0f, 100.0f) == 0, "an unknown class reads no cell");
	Check(ms_aStore.CountAtCell(SPAWN_OCCUPANCY_CLASSES, 0, 0) == 0, "an unknown class reads no cell by index");

	/* The float-domain guard: off the grid, NaN and huge values. */
	int32_t cell;
	Check(!SSpawnOccupancyStore::CellAt(SPAWN_OCCUPANCY_GRID_MIN_X - 1.0f, 0.0f, cell), "west of the grid");
	Check(!SSpawnOccupancyStore::CellAt(0.0f, SPAWN_OCCUPANCY_GRID_MIN_Y - 1.0f, cell), "south of the grid");
	Check(!SSpawnOccupancyStore::CellAt(SPAWN_OCCUPANCY_GRID_MIN_X + SPAWN_OCCUPANCY_CELLS_X * SPAWN_OCCUPANCY_CELL_SIZE, 0.0f, cell), "the east edge");
	Check(!SSpawnOccupancyStore::CellAt(0.0f, SPAWN_OCCUPANCY_GRID_MIN_Y + SPAWN_OCCUPANCY_CELLS_Y * SPAWN_OCCUPANCY_CELL_SIZE, cell), "the north edge");
	Check(!SSpawnOccupancyStore::CellAt(NAN, 0.0f, cell), "a NaN x has no cell");
	Check(!SSpawnOccupancyStore::CellAt(0.0f, NAN, cell), "a NaN y has no cell");
	Check(!SSpawnOccupancyStore::CellAt(1e30f, 0.0f, cell), "a huge x has no cell");
	Check(!SSpawnOccupancyStore::CellAt(0.0f, -1e30f, cell), "a huge negative y has no cell");

	/* A move to a rejected position drops the count, like any move off the
	 * grid; the removal of the dropped handle is clean. */
	ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, 0, NAN, 100.0f);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE) == 0, "a move to a rejected position drops the count");
	ms_aStore.Remove(SPAWN_OCCUPANCY_VEHICLE, 0);
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE) == 0, "the removal of a dropped handle is clean");

	/* The readers write nothing: the whole store is byte-compared around a
	 * battery of reads over a populated store. */
	static unsigned char aSnapshot[sizeof(SSpawnOccupancyStore)];
	float savedX = 0.0f, savedY = 0.0f;
	ms_aStore.Reset();
	RefReset();
	for (int32_t i = 0; i < 100; i++) {
		float x = RandFloat(-1300.0f, 1300.0f);
		float y = RandFloat(-1600.0f, 1600.0f);
		ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
		RefAdd(SPAWN_OCCUPANCY_VEHICLE, i, x, y);
		if (i == 0) {
			savedX = x;
			savedY = y;
		}
	}
	memcpy(aSnapshot, &ms_aStore, sizeof(aSnapshot));
	int32_t sink = 0;
	for (int32_t i = 0; i < 200; i++) {
		int32_t readCell;
		sink += SSpawnOccupancyStore::CellAt(savedX + i, savedY, readCell) ? readCell : 0;
		sink += ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, savedX, savedY);
		sink += ms_aStore.CountAtCell(SPAWN_OCCUPANCY_VEHICLE, i % SPAWN_OCCUPANCY_CELLS_X, 0);
		sink += ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE);
		sink += ms_aStore.TotalCount(SPAWN_OCCUPANCY_CLASSES);
		sink += ms_aStore.SiteReserved(i % SPAWN_OCCUPANCY_SITES, (uint32_t)i);
	}
	Check(memcmp(aSnapshot, &ms_aStore, sizeof(aSnapshot)) == 0, "a battery of reads must write nothing");
	(void)sink;

	/* The mutation guard: the reference comparison is not vacuous. */
	Check(RefMismatches() == 0, "the populated store must read clean");
	ms_aStore.m_aVehicleCount[100] += 1;
	Check(RefMismatches() > 0, "an injected count fault must be caught");
	ms_aStore.m_aVehicleCount[100] -= 1;
	ms_aStore.m_nVehicleTotal += 1;
	Check(RefMismatches() > 0, "an injected total fault must be caught");
	ms_aStore.m_nVehicleTotal -= 1;
	Check(RefMismatches() == 0, "the undone store must read clean");
	printf("  invalid updates inert; the reads byte-clean; the injected faults caught\n");
}

/* --------------------------------------------------------- E. bounded movement */

/* The count of cells whose value differs between two snapshots. */
static int32_t
CountDiffCells(const uint16_t *pa, const uint16_t *pb)
{
	int32_t n = 0;
	for (int32_t cell = 0; cell < SPAWN_OCCUPANCY_CELLS; cell++)
		if (pa[cell] != pb[cell])
			n++;
	return n;
}

static void
SectionMovementBound(void)
{
	printf("E. bounded movement (what one update writes)\n");
	RefReset();
	ms_aStore.Reset();

	static uint16_t aBefore[SPAWN_OCCUPANCY_CELLS];
	static uint16_t aAfter[SPAWN_OCCUPANCY_CELLS];

	/* The entity sits at the middle of a cell; a step of 4 m cannot leave it. */
	ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, 0, 16.0f, 16.0f);
	int32_t sameWritten = 0, crossWritten = 0;

	for (int32_t i = 0; i < 100; i++) {
		memcpy(aBefore, ms_aStore.m_aVehicleCount, sizeof(aBefore));
		ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, 0, 16.0f + (i % 4), 16.0f);
		memcpy(aAfter, ms_aStore.m_aVehicleCount, sizeof(aAfter));
		sameWritten += CountDiffCells(aBefore, aAfter);
	}
	Check(sameWritten == 0, "a move inside the cell must write nothing");

	/* The entity is placed at a cell's middle, then moved 32 m east: one
	 * crossing, so exactly two cells change and the total stands. */
	ms_aStore.Reset();
	ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, 0, 16.0f, 16.0f);
	for (int32_t i = 0; i < 100; i++) {
		float x = (i % 2) == 0 ? 48.0f : 16.0f;
		memcpy(aBefore, ms_aStore.m_aVehicleCount, sizeof(aBefore));
		ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, 0, x, 16.0f);
		memcpy(aAfter, ms_aStore.m_aVehicleCount, sizeof(aAfter));
		crossWritten += CountDiffCells(aBefore, aAfter);
	}
	Check(crossWritten == 200, "a crossing move must touch exactly the two cells");
	Check(ms_aStore.TotalCount(SPAWN_OCCUPANCY_VEHICLE) == 1, "a move must never change the total");
	Check(ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, 16.0f, 16.0f) == 1 &&
	      ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, 48.0f, 16.0f) == 0,
		"the crossing must leave the count in the destination cell");
	printf("  100 same-cell moves wrote 0 cells; 100 crossing moves wrote %d (2 each)\n", crossWritten);
}

/* ---------------------------------------------------------- F. the reservations */

static void
SectionReservations(void)
{
	printf("F. the reservations\n");
	ms_aStore.Reset();

	int32_t site = 777;
	uint32_t now = 1000000u;

	Check(!ms_aStore.SiteReserved(site, now), "a site starts free");
	Check(ms_aStore.ReserveSite(site, now), "the first request takes the site");
	Check(ms_aStore.SiteReserved(site, now), "the site reads reserved");
	Check(!ms_aStore.ReserveSite(site, now + 1), "a second request is refused while held");
	Check(ms_aStore.SiteReserved(site, now + SPAWN_OCCUPANCY_RESERVE_MS - 1),
		"the site is held until the expiry");
	Check(!ms_aStore.SiteReserved(site, now + SPAWN_OCCUPANCY_RESERVE_MS),
		"the site is free at the expiry");
	Check(ms_aStore.ReserveSite(site, now + SPAWN_OCCUPANCY_RESERVE_MS),
		"an expired site can be taken again");
	ms_aStore.ReleaseSite(site);
	Check(!ms_aStore.SiteReserved(site, now + SPAWN_OCCUPANCY_RESERVE_MS + 1),
		"a released site is free at once");
	Check(ms_aStore.ReserveSite(0, now) && ms_aStore.SiteReserved(0, now), "the first site id works");
	Check(ms_aStore.ReserveSite(SPAWN_OCCUPANCY_SITES - 1, now), "the last site id works");
	Check(!ms_aStore.ReserveSite(-1, now) && !ms_aStore.SiteReserved(-1, now), "a negative id is refused");
	Check(!ms_aStore.ReserveSite(SPAWN_OCCUPANCY_SITES, now) && !ms_aStore.SiteReserved(SPAWN_OCCUPANCY_SITES, now),
		"an id past the table is refused");

	/* The game's millisecond timer wraps; the signed difference must survive
	 * it (a reservation crossing the wrap is held for its own 2 s). */
	ms_aStore.Reset();
	uint32_t nearWrap = 0xFFFFFF00u;	/* 256 ms before the wrap */
	Check(ms_aStore.ReserveSite(42, nearWrap), "a reservation just before the wrap");
	Check(ms_aStore.SiteReserved(42, nearWrap + 100), "the reservation holds across the wrap");
	Check(ms_aStore.SiteReserved(42, nearWrap + SPAWN_OCCUPANCY_RESERVE_MS - 1), "it holds until its expiry");
	Check(!ms_aStore.SiteReserved(42, nearWrap + SPAWN_OCCUPANCY_RESERVE_MS), "and frees after it");

	/* A load can move the clock backwards; a stale entry (the timer past the
	 * wrap half) must not read held either. */
	ms_aStore.Reset();
	Check(ms_aStore.ReserveSite(5, now), "reserve at t");
	Check(!ms_aStore.SiteReserved(5, now - 1), "a clock that moved backwards frees the site");
	Check(ms_aStore.SiteReserved(5, now), "the site is held again at t");
	uint32_t staleNow = now + SPAWN_OCCUPANCY_RESERVE_MS + 0x80000000u + 1000u;
	Check(!ms_aStore.SiteReserved(5, staleNow), "an entry older than the wrap half cannot read held");
	Check(ms_aStore.ReserveSite(5, staleNow), "and it can be taken again");
	printf("  reserve, refusal, expiry, re-take, release, bounds, wrap, backwards and stale: ok\n");
}

/* ------------------------------------------------------------------ G. the cost */

#define COST_N 1000000

/* The positions of the timed streams are precomputed: the RNG never runs
 * inside a timed loop. They also keep the loops alive - the compiler cannot
 * know what an array holds, so it can neither fold the cell's branch away nor
 * turn the walks into one update. */
static float ms_aCostX[COST_N];
static float ms_aCostY[COST_N];
static float ms_aSameX[COST_N];		/* always the one same-cell walk */
static float ms_aSameY[COST_N];
static float ms_aCrossX[COST_N];	/* the one other-cell walk */
static float ms_aCrossY[COST_N];

static volatile int g_sink;

static float
CellCentreX(int32_t cx)
{
	return SPAWN_OCCUPANCY_GRID_MIN_X + (cx + 0.5f) * SPAWN_OCCUPANCY_CELL_SIZE;
}

static float
CellCentreY(int32_t cy)
{
	return SPAWN_OCCUPANCY_GRID_MIN_Y + (cy + 0.5f) * SPAWN_OCCUPANCY_CELL_SIZE;
}

static void
CostSetup(void)
{
	/* The same-cell pair: one cell's centre, swung 7 m either way inside
	 * its own cell, so every step of the walk stays inside that cell. */
	float centreX = CellCentreX(91);
	float centreY = CellCentreY(77);
	for (int32_t i = 0; i < COST_N; i++)
		ms_aSameX[i] = centreX + RandFloat(-7.0f, 7.0f), ms_aSameY[i] = centreY + RandFloat(-7.0f, 7.0f);

	/* The crossing pair: a cell centre and the centre 32 m east (two cells
	 * away), each swung 5 m either way inside its own cell, so every step
	 * crosses. */
	float crossX = centreX + 32.0f;
	for (int32_t i = 0; i < COST_N; i++)
		ms_aCrossX[i] = crossX + RandFloat(-5.0f, 5.0f), ms_aCrossY[i] = centreY + RandFloat(-5.0f, 5.0f);

	for (int32_t i = 0; i < COST_N; i++)
		ms_aCostX[i] = RandFloat(-1300.0f, 1300.0f), ms_aCostY[i] = RandFloat(-1600.0f, 1600.0f);
}

static void
SectionCost(void)
{
	printf("G. the cost (per update; %d updates per stream)\n", COST_N);
	CostSetup();
	ms_aStore.Reset();
	double t0, t1;

	/* A fresh create: the pool is empty, so every create counts anew. */
	int32_t fresh = NUMVEHICLES + NUMPEDS;
	t0 = NowSeconds();
	for (int32_t i = 0; i < NUMVEHICLES; i++)
		ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, i, ms_aCostX[i], ms_aCostY[i]);
	for (int32_t i = 0; i < NUMPEDS; i++)
		ms_aStore.Create(SPAWN_OCCUPANCY_PED, i, ms_aCostX[i], ms_aCostY[i]);
	t1 = NowSeconds();
	printf("  a fresh create:                  %6.1f ns (%d creates)\n", (t1 - t0) / fresh * 1e9, fresh);

	/* The valve's own churn: a car in, a car out, on reused handles. */
	ms_aStore.Reset();
	t0 = NowSeconds();
	for (int32_t i = 0; i < COST_N; i++) {
		int32_t h = i % 512;
		ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, h, ms_aCostX[i], ms_aCostY[i]);
		ms_aStore.Remove(SPAWN_OCCUPANCY_VEHICLE, h);
	}
	t1 = NowSeconds();
	printf("  a create+remove pair:            %6.1f ns\n", (t1 - t0) / COST_N * 1e9);

	/* A move that stays in its cell: the early out, no count writes. */
	ms_aStore.Reset();
	ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, 0, ms_aSameX[0], ms_aSameY[0]);
	t0 = NowSeconds();
	for (int32_t i = 0; i < COST_N; i++)
		ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, 0, ms_aSameX[i], ms_aSameY[i]);
	t1 = NowSeconds();
	g_sink += ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, ms_aSameX[0], ms_aSameY[0]);
	printf("  a move inside the cell:          %6.1f ns\n", (t1 - t0) / COST_N * 1e9);

	/* A move across a cell edge: two count writes, every step (the walk
	 * alternates between the same-cell pair and the two-cells-east pair,
	 * so every move crosses). */
	t0 = NowSeconds();
	for (int32_t i = 0; i < COST_N / 2; i++) {
		ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, 0, ms_aSameX[i], ms_aSameY[i]);
		ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, 0, ms_aCrossX[i], ms_aCrossY[i]);
	}
	t1 = NowSeconds();
	g_sink += ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, ms_aCrossX[0], ms_aCrossY[0]);
	printf("  a move across a cell edge:       %6.1f ns\n", (t1 - t0) / COST_N * 1e9);

	/* A move stream at driving size: 2 m east and 3 m south a step, so
	 * about one step in three crosses a cell edge. */
	ms_aStore.Reset();
	ms_aStore.Create(SPAWN_OCCUPANCY_VEHICLE, 0, ms_aCostX[0], ms_aCostY[0]);
	t0 = NowSeconds();
	float x = ms_aCostX[0], y = ms_aCostY[0];
	for (int32_t i = 0; i < COST_N; i++) {
		x += 2.0f;
		y += 3.0f;
		if (x > 1300.0f)
			x = -1300.0f;
		if (y > 1600.0f)
			y = -1600.0f;
		ms_aStore.Move(SPAWN_OCCUPANCY_VEHICLE, 0, x, y);
	}
	t1 = NowSeconds();
	g_sink += ms_aStore.CountAtPosition(SPAWN_OCCUPANCY_VEHICLE, x, y);
	printf("  a driving move (2 m east, 3 m south):%6.1f ns\n", (t1 - t0) / COST_N * 1e9);

	printf("  sizeof(SSpawnOccupancyStore):    %6.1f KB\n", sizeof(SSpawnOccupancyStore) / 1024.0);
}

/* ------------------------------------------------------------------ main */

int
main(void)
{
	printf("The occupancy store (VICE-149): the grid, the streams, the movement, the reservations.\n\n");
	SectionGrid();
	printf("\n");
	SectionRegister();
	printf("\n");
	SectionStreams();
	printf("\n");
	SectionWorldClear();
	printf("\n");
	SectionValve();
	printf("\n");
	SectionDrift();
	printf("\n");
	SectionOffGrid();
	printf("\n");
	SectionGuards();
	printf("\n");
	SectionMovementBound();
	printf("\n");
	SectionReservations();
	printf("\n");
	SectionCost();

	printf("\nchecks: %lld\n", g_checks);
	if (g_failures == 0)
		printf("RESULT: every check passed.\n");
	else
		printf("RESULT: %d check(s) failed.\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
