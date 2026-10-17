#ifndef __GTA_SPAWN_OCCUPANCY_H__
#define __GTA_SPAWN_OCCUPANCY_H__

/*
 * The occupancy store (VICE-149, the design's section 2): the spawn service's
 * presence grid and its site reservations.
 *
 * The service needs three questions answered cheaply: is the street around
 * this position already crowded, which cells hold the traffic, and has this
 * site just been promised to another request. This module is that memory and
 * nothing else: no placement, no decisions. The spawn service's planner
 * reads it since VICE-150 (the cell counts for the capacity filter and the
 * site reservations); the director is a later reader.
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
 * cell below its neighbours is a hint, never a promise. The grid's own read,
 * SSpawnOccupancyStore::CellAt, returns the linear cell index; the engine
 * class's CSpawnOccupancy::CellAt returns the column and row pair.
 *
 * The store is fed from the entity lifecycle, never from spawn calls: the
 * vehicle and pedestrian constructors and destructors (the shape the
 * accounting counters have - CCarCtrl::UpdateCarCount in Vehicle.cpp and
 * CPopulation::UpdatePedCount in Ped.cpp, next to these hooks) and the
 * movement hooks. The reason is that the entities leave, enter and move
 * outside any creation call - a world clear, the pool valve, a distance cull,
 * a replay restore, a save load - and a store fed from creation calls alone
 * keeps the phantom cars those paths remove or move. The hooks are:
 *
 *	the constructors			CVehicle (CAutomobile/CBike/CBoat)
 *						and CPed register the handle and
 *						nothing else: the position is not
 *						read (the matrix is unplaced and
 *						the pool memory is stale), and the
 *						first placement counts the handle;
 *	the destructors				the handle's count is taken from
 *						the cell it was counted in;
 *	CWorld::Add				the placement of a fresh entity, a
 *						teleport (Remove -> SetPosition ->
 *						Add) and a save load's restore
 *						all pass through it, and the handle
 *						re-homes to the position it just
 *						entered the world at;
 *	CPhysical::ApplyMoveSpeed		the physics step: an entity whose
 *						move crossed a cell edge re-homes, and
 *						one that stayed in its cell keeps it;
 *	the aircraft's own movement		CHeli::ProcessControl,
 *						CPlane::ProcessControl and
 *						CCarCtrl::SteerAIHeliTowardsTargetCoors
 *						write the matrix directly (no
 *						physics step while flying);
 *	the far layer's own re-pose		its cars are static and off the
 *						moving list, so their per-update
 *						pose write re-homes them itself;
 *	the crane and the respray		their own placement of a car
 *						(CCrane::Update's carried car, the
 *						garage's centring);
 *	CPed::SetPedPositionInCar		the seat matrix of a carried
 *						pedestrian;
 *	CSpawnOccupancy::Resync			the one true rebuild: the records
 *						are cleared and every live pool slot
 *						is counted at its current position.
 *						The replay restore (CReplay::
 *						RestoreStuffFromMem brings the pool
 *						images back by memcpy, so no
 *						constructor runs) and the boot
 *						call it.
 *
 * Every hook is O(1) and does bookkeeping only; a move costs one cell
 * comparison and two count updates only when an edge was actually crossed.
 * The bookkeeping is exact by construction: a creation adds one count, a
 * removal subtracts one, and a move transfers one between two cells. The
 * handle remembers the cell it is counted in, so the removal subtracts the
 * recorded cell whatever the entity's current position is; the count is
 * accounting, and the cell it names can lag, but the sum can never drift.
 * Position writes the hooks do not carry (a script's direct SetPosition, a
 * cutscene's matrix) lag until the next hooked event, and no longer. The store
 * is always live: every hook runs in every mode and no gate stands in front of
 * it; a later ticket adds one when the consumers exist. The store is
 * main-thread only; nothing on the far worker touches it.
 *
 * The reservations
 * ----------------
 *
 * The planner promises a site before it creates the actor there, so two
 * requests cannot plan the same connection in the same frame. A reservation
 * is per site id (the site index's directed connection id, VICE-148) and
 * expires after SPAWN_OCCUPANCY_RESERVE_MS: short enough that an abandoned
 * plan frees the street for the next request, long enough to cover a
 * planning batch. An entry carries an in-use flag, and a held entry is one
 * whose remaining lifetime is inside (0, SPAWN_OCCUPANCY_RESERVE_MS]: the
 * signed difference survives the timer's wrap, a clock that jumped backwards
 * (a load) reads the reservation free, and an entry older than the wrap half
 * cannot read held. A rebuild (Resync) drops the reservations with the
 * records: a load or a replay plans afresh.
 *
 * The worker copy (a later ticket)
 * --------------------------------
 *
 * The design's section 2 wants the planner worker to read a copy, merged at a
 * stated cadence; the worker itself is the design's section 11. It does not
 * read this store: it reads a copy of the counts only, double-buffered and
 * merged on the main thread. The merge runs with the worker joined (the far
 * worker's own window) and at most once per planning batch and at most every
 * 250 ms (fifteen frames at 60 Hz). The copy is exact at the merge - it is
 * these arrays, and the worker touches no store - and between merges it lags
 * by the updates of the window. The accuracy target is per entity, not per
 * cell: a tracked entity's cell is stale by at most its own unwatched
 * movement, one cell crossing per window (30 m/s is 7.5 m in 250 ms, inside a
 * 16 m cell), and the store's own sums never drift. The planner treats the
 * copy as a filter: the final collision query at creation stays the true
 * guard, so a stale count can refuse or admit a candidate, never create an
 * overlap. No worker is added here.
 *
 * No engine header beyond config.h is included; the evidence program in
 * utils/spawn-service drives the store on its own.
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

/* The reservation table: one entry per site id. The ids are the site index's
 * directed connections (VICE-148), 0..NUM_PATHCONNECTIONS-1. */
#define SPAWN_OCCUPANCY_SITES		NUM_PATHCONNECTIONS

/* A handle record holds the cell the handle is counted in, or one of the two
 * markers: NONE after a removal (or a rebuild), PENDING between a
 * constructor's registration and the handle's first placement. */
#define SPAWN_OCCUPANCY_CELL_NONE	(-1)
#define SPAWN_OCCUPANCY_CELL_PENDING	(-2)

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

	/* The cell each tracked handle is counted in, a marker above when it is
	 * not counted. The removal subtracts the recorded cell, never the
	 * current position's, so the counts stay exact however the entity moved
	 * without the store watching. */
	int32_t m_aVehicleCell[NUMVEHICLES];
	int32_t m_aPedCell[NUMPEDS];

	/* The live totals (the sum over the cells, kept O(1)). */
	int32_t m_nVehicleTotal;
	int32_t m_nPedTotal;

	/* The reservations: the expiry, in game ms, of a held site. The flag
	 * and the remaining-lifetime test make the sentinel unambiguous and
	 * survive a wrap of the timer. */
	uint32_t m_aSiteExpiry[SPAWN_OCCUPANCY_SITES];
	bool m_aSiteInUse[SPAWN_OCCUPANCY_SITES];

	SSpawnOccupancyStore(void) { Reset(); }

	void
	Reset(void)
	{
		memset(m_aVehicleCount, 0, sizeof(m_aVehicleCount));
		memset(m_aPedCount, 0, sizeof(m_aPedCount));
		for (int32_t i = 0; i < NUMVEHICLES; i++)
			m_aVehicleCell[i] = SPAWN_OCCUPANCY_CELL_NONE;
		for (int32_t i = 0; i < NUMPEDS; i++)
			m_aPedCell[i] = SPAWN_OCCUPANCY_CELL_NONE;
		m_nVehicleTotal = 0;
		m_nPedTotal = 0;
		memset(m_aSiteExpiry, 0, sizeof(m_aSiteExpiry));
		memset(m_aSiteInUse, 0, sizeof(m_aSiteInUse));
	}

	/*
	 * A construction: the handle is marked, nothing more. The constructor
	 * must not read the position (the matrix is unplaced and the pool
	 * memory is stale); the first placement - the world add or a move -
	 * counts the handle. A stale count under the handle is dropped first,
	 * so a slot reused without its removal cannot leak one.
	 */
	void
	Register(ESpawnOccupancyClass type, int32_t handle)
	{
		int32_t *pRecord = CellRecord(type, handle);
		if (pRecord == 0)
			return;
		Untrack(type, pRecord);
		*pRecord = SPAWN_OCCUPANCY_CELL_PENDING;
	}

	/*
	 * The presence updates. A create and a move carry the same statement -
	 * the handle is here now - so both go through Place; the two names
	 * exist for their call sites (an explicit platform and the move hook)
	 * and for the evidence stream.
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
	 * The reads. CellAt is the grid's own mapping, in the float domain (a
	 * NaN or a huge value is rejected before any cast), and returns the
	 * linear cell index; the counts are what the planner checks for
	 * capacity and the director reads for density.
	 */
	static bool
	CellAt(float x, float y, int32_t &cell)
	{
		float fx = (x - SPAWN_OCCUPANCY_GRID_MIN_X) / SPAWN_OCCUPANCY_CELL_SIZE;
		float fy = (y - SPAWN_OCCUPANCY_GRID_MIN_Y) / SPAWN_OCCUPANCY_CELL_SIZE;
		if (!(fx >= 0.0f && fx < SPAWN_OCCUPANCY_CELLS_X && fy >= 0.0f && fy < SPAWN_OCCUPANCY_CELLS_Y))
			return false;
		cell = (int32_t)fy * SPAWN_OCCUPANCY_CELLS_X + (int32_t)fx;
		return true;
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
		if (type == SPAWN_OCCUPANCY_VEHICLE)
			return m_nVehicleTotal;
		if (type == SPAWN_OCCUPANCY_PED)
			return m_nPedTotal;
		return 0;
	}

	/*
	 * The reservations. ReserveSite refuses while another request holds the
	 * site; SiteReserved is the read; ReleaseSite frees it early (a failed
	 * creation); the expiry frees it by itself. A held entry is one whose
	 * remaining lifetime is inside (0, SPAWN_OCCUPANCY_RESERVE_MS]: the
	 * signed difference survives the timer's wrap, a clock that moved
	 * backwards reads the site free, and a stale entry cannot read held.
	 */
	bool
	ReserveSite(int32_t site, uint32_t now)
	{
		if (site < 0 || site >= SPAWN_OCCUPANCY_SITES)
			return false;
		if (SiteReserved(site, now))
			return false;
		m_aSiteInUse[site] = true;
		m_aSiteExpiry[site] = now + SPAWN_OCCUPANCY_RESERVE_MS;
		return true;
	}

	bool
	SiteReserved(int32_t site, uint32_t now) const
	{
		if (site < 0 || site >= SPAWN_OCCUPANCY_SITES || !m_aSiteInUse[site])
			return false;
		int32_t remaining = (int32_t)(m_aSiteExpiry[site] - now);
		return remaining > 0 && remaining <= SPAWN_OCCUPANCY_RESERVE_MS;
	}

	void
	ReleaseSite(int32_t site)
	{
		if (site >= 0 && site < SPAWN_OCCUPANCY_SITES)
			m_aSiteInUse[site] = false;
	}

private:
	/* The presence update behind Create and Move: the handle is counted in
	 * the cell of its position and re-homed from whatever cell it was
	 * counted in; off the grid the count is dropped, so the grid never
	 * holds an entity that left the map. A marker record (none or pending)
	 * counts at the cell: the first placement promises the handle. */
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

	int32_t
	CountAt(ESpawnOccupancyClass type, int32_t cell) const
	{
		if (cell < 0 || cell >= SPAWN_OCCUPANCY_CELLS)
			return 0;
		if (type == SPAWN_OCCUPANCY_VEHICLE)
			return m_aVehicleCount[cell];
		if (type == SPAWN_OCCUPANCY_PED)
			return m_aPedCount[cell];
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
		*pRecord = SPAWN_OCCUPANCY_CELL_NONE;
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
	 * (density per cell). CellAt gives the column and row pair of a
	 * position (the store's own CellAt gives the linear cell index), false
	 * when it is off the grid. */
	static bool CellAt(float x, float y, int32_t &cellX, int32_t &cellY);
	static int32_t CountAtPosition(ESpawnOccupancyClass type, float x, float y);
	static int32_t CountAtCell(ESpawnOccupancyClass type, int32_t cellX, int32_t cellY);
	static int32_t TotalCount(ESpawnOccupancyClass type);

	/* The planner's own handle on the store: the capacity reads and the
	 * reservations run in the engine-free core (SpawnService.h), which takes
	 * the store itself. */
	static SSpawnOccupancyStore &Store(void);

	/* The short promise on a site (the planner's reservations). */
	static bool ReserveSite(int32_t site, uint32_t now);
	static bool SiteReserved(int32_t site, uint32_t now);
	static void ReleaseSite(int32_t site);

	/* The hooks. The constructors of CVehicle and CPed register the handle
	 * (they must not read the position); the destructors take the count
	 * back; EntityMoved is the move/teleport hook (the world add, the
	 * physics step, the aircraft, the far layer, the crane, the respray,
	 * the seat matrix). O(1), bookkeeping only. */
	static void VehicleRegistered(CVehicle *pVehicle);
	static void VehicleRemoved(CVehicle *pVehicle);
	static void PedRegistered(CPed *pPed);
	static void PedRemoved(CPed *pPed);
	static void EntityMoved(CEntity *pEntity);

	/* The one true rebuild: clear the records and count every live pool
	 * slot at its current position. Called by the replay restore, which
	 * brings the pool images back without constructors, and by the boot. */
	static void Resync(void);

	/* The restart: the records and the reservations go. Called by
	 * CWorld::ClearForRestart. */
	static void Reset(void);

private:
	static SSpawnOccupancyStore ms_Store;
};

#endif // __GTA_SPAWN_OCCUPANCY_H__
