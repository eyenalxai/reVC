#ifndef __GTA_SPAWN_OCCUPANCY_H__
#define __GTA_SPAWN_OCCUPANCY_H__

/*
 * The occupancy store (VICE-149): the spawn service's presence grid and its
 * site reservations.
 *
 * The service needs three questions answered cheaply: is the street around
 * this position already crowded, which cells hold the traffic, and has this
 * site just been promised to another request. This module is that memory and
 * nothing else: no placement, no decisions, and no consumer yet - the planner
 * and the director of the later tickets read it.
 *
 * The presence grid
 * -----------------
 *
 * The world is covered by 16 m cells (250 x 250 over World.h's 4000 x 4000 m
 * from (-2400, -2000)); a cell holds the count of the vehicles and of the
 * pedestrians seen in it. The cell is coarse on purpose. It is a filter for
 * the planner's candidate scan and a density read for the director, and it
 * can express neither lanes nor two-way occupancy nor the ground under a
 * position, so the final collision query at creation stays the true guard: a
 * cell below its neighbours is a hint, never a promise.
 *
 * The store is fed from the entity lifecycle, never from spawn calls: the
 * vehicle and pedestrian constructors and destructors (the shape the
 * accounting counters already have - CCarCtrl::UpdateCarCount at
 * Vehicle.cpp:92/212 and CPopulation::UpdatePedCount at Ped.cpp:368/404) and
 * one move/teleport hook. The reason is that the entities leave and enter
 * outside any creation call - a world clear, the pool valve, a distance
 * cull, a replay, a save load - and a store fed from creation calls alone
 * keeps the phantom cars those paths remove. The hooks are:
 *
 *	the constructors and destructors	CVehicle (CAutomobile/CBike/CBoat)
 *						and CPed register and unregister;
 *	CWorld::Add				the placement of a fresh entity, a
 *						teleport (Remove -> SetPosition ->
 *						Add) and the save load's restore
 *						all pass through it, and the handle
 *						re-homes to the position it just
 *						entered the world at;
 *	CPhysical::ApplyMoveSpeed		the physics step: an entity whose
 *						move crossed a cell edge re-homes, and
 *						one that stayed in its cell keeps it;
 *	the far layer's own re-pose		its cars are static and off the
 *						moving list, so their per-update
 *						pose write re-homes them itself.
 *
 * Every hook is O(1) and does bookkeeping only; a move costs one cell
 * comparison and two count updates only when an edge was actually crossed.
 * The bookkeeping is exact by construction: a creation adds one count, a
 * removal subtracts one, and a move transfers one between two cells. The
 * handle remembers the cell it is counted in, so the removal subtracts the
 * recorded cell whatever the entity's current position is; the count is
 * accounting, and the cell it names can lag, but the sum can never drift.
 * Position writes that pass none of the hooks (an in-car pedestrian's seat
 * matrix, a script's direct matrix write) lag until the next hooked event;
 * the lag is bounded by the cell size and is inside the accuracy budget the
 * worker copy below states. The store is main-thread only; nothing on the
 * far worker touches it.
 *
 * The reservations
 * ----------------
 *
 * The planner promises a site before it creates the actor there, so two
 * requests cannot plan the same connection in the same frame. A reservation
 * is per site id (the site index's directed connection id, VICE-148) and
 * expires after SPAWN_OCCUPANCY_RESERVE_MS: short enough that an abandoned
 * plan frees the street for the next request, long enough to cover a
 * planning batch. The test is on the signed difference of the game's
 * millisecond timer, so a wrap of the timer never lengthens or cuts an
 * expiry.
 *
 * The worker copy (a later ticket)
 * --------------------------------
 *
 * The planner worker of the design's section 11 does not read this store; it
 * reads a copy, double-buffered and merged on the main thread. The merge
 * takes the whole store with the worker joined (the far worker's own window)
 * and runs at most once per planning batch and at most every 250 ms (fifteen
 * frames at 60 Hz). The copy is exact at the merge - it is this store - and
 * between merges it lags by the updates of the window; the accuracy target is
 * that a cell never reads more than two cars away from this store. The lag is
 * bounded by one cell crossing per tracked entity per window (30 m/s is 7.5 m
 * in 250 ms, inside a 16 m cell), and the planner treats the copy as a
 * filter: the final collision query at creation stays the true guard, so a
 * stale count can refuse or admit a candidate, never create an overlap. No
 * worker is added here.
 *
 * No engine header is included; the evidence program in utils/spawn-service
 * drives the store on its own.
 */

#include "config.h"		/* NUMPEDS, NUMVEHICLES, NUM_PATHCONNECTIONS */
#include <stdint.h>
#include <string.h>

class CVehicle;
class CPed;
class CEntity;

/* The presence grid: 16 m cells over the world (World.h: 80 sectors of 50 m,
 * WORLD_MIN_X/Y (-2400, -2000), WORLD_SIZE_X/Y 4000). The SpawnOccupancy.cpp
 * side static-asserts the fit; a position off the grid is simply not counted. */
#define SPAWN_OCCUPANCY_CELL_SIZE	16.0f		/* metres */
#define SPAWN_OCCUPANCY_GRID_MIN_X	(-2400.0f)	/* WORLD_MIN_X */
#define SPAWN_OCCUPANCY_GRID_MIN_Y	(-2000.0f)	/* WORLD_MIN_Y */
#define SPAWN_OCCUPANCY_CELLS_X		250		/* WORLD_SIZE_X / CELL_SIZE */
#define SPAWN_OCCUPANCY_CELLS_Y		250		/* WORLD_SIZE_Y / CELL_SIZE */
#define SPAWN_OCCUPANCY_CELLS		(SPAWN_OCCUPANCY_CELLS_X * SPAWN_OCCUPANCY_CELLS_Y)

/* The short expiry of a site reservation. Two seconds cover a planning batch
 * and a stalled request; an abandoned plan frees the site right after. */
#define SPAWN_OCCUPANCY_RESERVE_MS	2000

/* The reservation table: one expiry per site id. The ids are the site index's
 * directed connections (VICE-148), 0..NUM_PATHCONNECTIONS-1. */
#define SPAWN_OCCUPANCY_SITES		NUM_PATHCONNECTIONS

/* What a cell counts. Vehicles and pedestrians have their own cells, their
 * own totals and their own handle spaces (the two pools). */
enum ESpawnOccupancyClass
{
	SPAWN_OCCUPANCY_VEHICLE = 0,
	SPAWN_OCCUPANCY_PED,
	SPAWN_OCCUPANCY_CLASSES
};

/*
 * The store: the grid, the handle records and the reservations. It is
 * engine-free (plain arithmetic over its own arrays; no allocation anywhere),
 * so the evidence program instantiates it directly and drives the same code
 * the game runs. The engine's one instance lives in SpawnOccupancy.cpp behind
 * CSpawnOccupancy.
 */
struct SSpawnOccupancyStore
{
	/* The per-cell counts: the planner's capacity read and the director's
	 * density read. uint16 is far above any cell's reachable population
	 * (the pools cap at 2560 vehicles and 8192 pedestrians). */
	uint16_t m_aVehicleCount[SPAWN_OCCUPANCY_CELLS];
	uint16_t m_aPedCount[SPAWN_OCCUPANCY_CELLS];

	/* The cell each tracked handle is counted in, -1 when the handle is off
	 * the map or gone. The removal subtracts the recorded cell, never the
	 * current position's, so the counts stay exact however the entity moved
	 * without the store watching. */
	int32_t m_aVehicleCell[NUMVEHICLES];
	int32_t m_aPedCell[NUMPEDS];

	/* The live totals (the sum over the cells, kept O(1)). */
	int32_t m_nVehicleTotal;
	int32_t m_nPedTotal;

	/* The reservations: the expiry, in game ms, of the site; 0 is free. */
	uint32_t m_aSiteExpiry[SPAWN_OCCUPANCY_SITES];

	SSpawnOccupancyStore(void) { Reset(); }

	void
	Reset(void)
	{
		memset(m_aVehicleCount, 0, sizeof(m_aVehicleCount));
		memset(m_aPedCount, 0, sizeof(m_aPedCount));
		for (int32_t i = 0; i < NUMVEHICLES; i++)
			m_aVehicleCell[i] = -1;
		for (int32_t i = 0; i < NUMPEDS; i++)
			m_aPedCell[i] = -1;
		m_nVehicleTotal = 0;
		m_nPedTotal = 0;
		memset(m_aSiteExpiry, 0, sizeof(m_aSiteExpiry));
	}

	/*
	 * The presence updates. A create and a move carry the same statement -
	 * the handle is here now - so both go through Place; the two names
	 * exist for their call sites (the constructor and the move hook) and
	 * for the evidence stream.
	 */
	void
	Create(ESpawnOccupancyClass type, int32_t handle, float x, float y)
	{
		Place(type, handle, x, y);
	}

	void
	Move(ESpawnOccupancyClass type, int32_t handle, float x, float y)
	{
		Place(type, handle, x, y);
	}

	/* A destruction (or a delete outside any spawn call: a world clear, the
	 * pool valve, a cull, a replay, a save load). */
	void
	Remove(ESpawnOccupancyClass type, int32_t handle)
	{
		int32_t *pRecord = CellRecord(type, handle);
		if (pRecord)
			Untrack(type, pRecord);
	}

	/*
	 * The reads. CellAt is the grid's own mapping (a position off the grid
	 * has no cell); the counts are what the planner checks for capacity and
	 * the director reads for density.
	 */
	static bool
	CellAt(float x, float y, int32_t &cell)
	{
		if (x < SPAWN_OCCUPANCY_GRID_MIN_X || y < SPAWN_OCCUPANCY_GRID_MIN_Y)
			return false;
		int32_t cx = (int32_t)((x - SPAWN_OCCUPANCY_GRID_MIN_X) / SPAWN_OCCUPANCY_CELL_SIZE);
		int32_t cy = (int32_t)((y - SPAWN_OCCUPANCY_GRID_MIN_Y) / SPAWN_OCCUPANCY_CELL_SIZE);
		if (cx < 0 || cx >= SPAWN_OCCUPANCY_CELLS_X || cy < 0 || cy >= SPAWN_OCCUPANCY_CELLS_Y)
			return false;
		cell = cy * SPAWN_OCCUPANCY_CELLS_X + cx;
		return true;
	}

	int32_t
	CountAt(ESpawnOccupancyClass type, int32_t cell) const
	{
		return type == SPAWN_OCCUPANCY_VEHICLE ? m_aVehicleCount[cell] : m_aPedCount[cell];
	}

	int32_t
	CountAtPosition(ESpawnOccupancyClass type, float x, float y) const
	{
		int32_t cell;
		if (!CellAt(x, y, cell))
			return 0;
		return CountAt(type, cell);
	}

	int32_t
	CountAtCell(ESpawnOccupancyClass type, int32_t cellX, int32_t cellY) const
	{
		if (cellX < 0 || cellX >= SPAWN_OCCUPANCY_CELLS_X ||
		    cellY < 0 || cellY >= SPAWN_OCCUPANCY_CELLS_Y)
			return 0;
		return CountAt(type, cellY * SPAWN_OCCUPANCY_CELLS_X + cellX);
	}

	int32_t
	TotalCount(ESpawnOccupancyClass type) const
	{
		return type == SPAWN_OCCUPANCY_VEHICLE ? m_nVehicleTotal : m_nPedTotal;
	}

	/*
	 * The reservations. ReserveSite refuses while another request holds the
	 * site; SiteReserved is the read; ReleaseSite frees it early (a failed
	 * creation); the expiry frees it by itself. The comparisons are on the
	 * signed difference, so the timer's wrap is harmless.
	 */
	bool
	ReserveSite(int32_t site, uint32_t now)
	{
		if (site < 0 || site >= SPAWN_OCCUPANCY_SITES)
			return false;
		uint32_t &expiry = m_aSiteExpiry[site];
		if (expiry != 0 && (int32_t)(expiry - now) > 0)
			return false;
		expiry = now + SPAWN_OCCUPANCY_RESERVE_MS;
		return true;
	}

	bool
	SiteReserved(int32_t site, uint32_t now) const
	{
		if (site < 0 || site >= SPAWN_OCCUPANCY_SITES)
			return false;
		uint32_t expiry = m_aSiteExpiry[site];
		return expiry != 0 && (int32_t)(expiry - now) > 0;
	}

	void
	ReleaseSite(int32_t site)
	{
		if (site >= 0 && site < SPAWN_OCCUPANCY_SITES)
			m_aSiteExpiry[site] = 0;
	}

private:
	/* The presence update behind Create and Move: the handle is counted in
	 * the cell of its position and re-homed from whatever cell it was
	 * counted in; off the grid the count is dropped, so the grid never
	 * holds an entity that left the map. */
	void
	Place(ESpawnOccupancyClass type, int32_t handle, float x, float y)
	{
		int32_t *pRecord = CellRecord(type, handle);
		int32_t cell;
		if (pRecord == 0)
			return;
		if (!CellAt(x, y, cell)) {
			Untrack(type, pRecord);
			return;
		}
		if (*pRecord < 0) {
			CountRef(type, cell)++;
			TotalRef(type)++;
			*pRecord = cell;
		} else if (*pRecord != cell) {
			CountRef(type, *pRecord)--;
			CountRef(type, cell)++;
			*pRecord = cell;
		}
	}

	int32_t *
	CellRecord(ESpawnOccupancyClass type, int32_t handle)
	{
		if (type == SPAWN_OCCUPANCY_VEHICLE && handle >= 0 && handle < NUMVEHICLES)
			return &m_aVehicleCell[handle];
		if (type == SPAWN_OCCUPANCY_PED && handle >= 0 && handle < NUMPEDS)
			return &m_aPedCell[handle];
		return 0;
	}

	uint16_t &
	CountRef(ESpawnOccupancyClass type, int32_t cell)
	{
		return type == SPAWN_OCCUPANCY_VEHICLE ? m_aVehicleCount[cell] : m_aPedCount[cell];
	}

	int32_t &
	TotalRef(ESpawnOccupancyClass type)
	{
		return type == SPAWN_OCCUPANCY_VEHICLE ? m_nVehicleTotal : m_nPedTotal;
	}

	/* The handle's count leaves the grid. */
	void
	Untrack(ESpawnOccupancyClass type, int32_t *pRecord)
	{
		if (*pRecord < 0)
			return;
		CountRef(type, *pRecord)--;
		TotalRef(type)--;
		*pRecord = -1;
	}
};

/*
 * The engine's one store and the hooks the lifecycle calls; every method
 * below is defined in SpawnOccupancy.cpp (the engine types live there). The
 * reads are the planner's and the director's API.
 */
class CSpawnOccupancy
{
public:
	/* The planner (capacity at a candidate's position) and the director
	 * (density per cell). CellAt gives the cell of a position, false when
	 * it is off the grid. */
	static bool CellAt(float x, float y, int32_t &cellX, int32_t &cellY);
	static int32_t CountAtPosition(ESpawnOccupancyClass type, float x, float y);
	static int32_t CountAtCell(ESpawnOccupancyClass type, int32_t cellX, int32_t cellY);
	static int32_t TotalCount(ESpawnOccupancyClass type);

	/* The short promise on a site (the planner's reservations). */
	static bool ReserveSite(int32_t site, uint32_t now);
	static bool SiteReserved(int32_t site, uint32_t now);
	static void ReleaseSite(int32_t site);

	/* The hooks. The constructors and destructors of CVehicle and CPed
	 * (next to the accounting counters), and the move/teleport hook the
	 * world add and the physics step call. O(1), bookkeeping only. */
	static void VehicleCreated(CVehicle *pVehicle);
	static void VehicleRemoved(CVehicle *pVehicle);
	static void PedCreated(CPed *pPed);
	static void PedRemoved(CPed *pPed);
	static void EntityMoved(CEntity *pEntity);

	/* The evidence program and a new game's boot. */
	static void Reset(void);

private:
	static SSpawnOccupancyStore ms_Store;
};

#endif // __GTA_SPAWN_OCCUPANCY_H__
