#ifndef __GTA_PEDSITES_H__
#define __GTA_PEDSITES_H__

/*
 * The pedestrian standable cells (VICE-154): the validated ground the
 * off-network pedestrian spawns stand on.
 *
 * Why
 * ---
 * A pedestrian path node is ground a pedestrian can stand on, but the map
 * gives nodes only along its own paths: the beach, the airfield, the docks,
 * the parks and the golf course stayed empty while the node areas filled
 * (VICE-122). That ticket put a bounded fraction of the spawns off the network
 * with a per-attempt random search: every random point of every attempt was
 * validated from scratch against the world (two vertical casts, the water
 * line, a clearance sphere, a node search), most of the ring is not standable,
 * and an attempt that failed fell back to the node path. The search is the
 * cost and the gap: the same ground is re-rejected over and over, and a point
 * one metre outside the random sample is never found.
 *
 * The cells
 * ---------
 * The world is one grid of 16 m cells (250 x 250 over the 4 km of World.h -
 * the same grid the occupancy store uses, its own copy: the two are read for
 * different things and must be able to move apart). A cell is **unvalidated**
 * until its build: the build probes `PED_SITE_PROBES` points of the cell (16:
 * a 4 x 4 lattice, one probe every 4 m) against the world on the main thread,
 * runs the predicate below on each, and stores the probes that passed - up to
 * `PED_SITE_SPOTS` (4) of the most spread ones - as the cell's standable
 * points. A request maps the drawn position to its cell:
 *
 *	built, standable	the stored point nearest the draw answers (a
 *				position the build already validated)
 *	built, dead		the cell holds no standable point: the caller
 *				falls back to the node path, and this cell never
 *				costs another probe while it stays cached
 *	unvalidated		the cell enters the bounded build queue and the
 *				caller falls back to the node path now
 *	off the grid		no cell: the caller falls back
 *
 * The build is lazy (a cell is probed when a spawn asks for it, exactly where
 * the spawns go) and bounded (PED_SITE_CELLS_PER_FRAME cells a frame, the
 * AddModelsToRequestList shape: a per-frame cap, nearest-first). The worker
 * boundary of the design (section 9) is kept: every world query is here, on
 * the main thread, and the store the planner will read is plain arithmetic.
 *
 * The predicate
 * -------------
 * One probe is the world's answer at one point: the ground cast down from the
 * level of the request, the top surface over it (the cast from
 * PED_SITE_ROOF_CAST above), the water line of the block, the static
 * clearance sphere over the feet, and the ground's collision normal. The rules
 * run in this order, and every reject is counted under its own verdict:
 *
 *	1. ground		the cast found nothing: the void, the holes, the
 *				shores beyond the collision. Rejected
 *				(PED_SITE_NO_GROUND).
 *	2. water		the ground stands at or below the line of the
 *				block plus PED_SITE_WATER_MARGIN. The line is
 *				CWaterLevel::GetWaterLevelNoWaves, so the
 *				answer does not move with the wave phase; the
 *				stray cull reads the same line for the drown
 *				count. Rejected (PED_SITE_WATER).
 *	3. level		the ground is farther than PED_SITE_LEVEL_MARGIN
 *				(3 m, the node path's own tolerance) from the
 *				level of the request: a seabed, the street under
 *				a bridge, a hilltop above the player. Rejected
 *				(PED_SITE_LEVEL).
 *	4. roof			the top surface below the cast stands more than
 *				PED_SITE_STEP (0.5 m) above the level of the
 *				request: a roof, a canopy or a bridge deck over
 *				the point. This is the rule that keeps a
 *				pedestrian out of a building's inside and out of
 *				the air under a deck; it is the VICE-122 rule,
 *				unchanged. Rejected (PED_SITE_ROOF).
 *	5. clearance		the sphere (PED_SITE_CLEARANCE_RADIUS, raised
 *				PED_SITE_CLEARANCE_HEIGHT over the ground) into
 *				the static world: a spot inside a wall, a fence
 *				or a thin roof. The dynamic clearance (the
 *				vehicles and pedestrians a created spot must not
 *				overlap) stays where it was, in the creation
 *				loop of the population: that answer moves every
 *				frame and cannot be cached. Rejected
 *				(PED_SITE_CLEARANCE).
 *	6. slope		the ground's normal is past the walk limit:
 *				PedSiteGroundIsWalkable, the new predicate.
 *				Rejected (PED_SITE_SLOPE).
 *
 * The slope predicate
 * -------------------
 * There was no slope rule anywhere: the game marks its steep faces with the
 * SURFACE_STEEP_CLIFF surface type (the physics' own reaction, Ped.cpp
 * PushOffSteepSlope), and a vertical cast over a cliff face happily answers
 * with the face's height. PedSiteGroundIsWalkable is the new rule: the
 * ground's collision normal against `PED_SITE_SLOPE_COS_MIN` (cos 45). The
 * limit is a guess to tune like the ring values; it must accept the map's
 * walkable terrain (the beach slopes, the golf course, the park hills) and
 * reject the cliff faces, which stand far past it. A NaN normal (a degenerate
 * collision) fails the comparison and rejects. The predicate is a free
 * function of the normal alone, so the evidence program drives it over a
 * table of angles without a world.
 *
 * The spots
 * ---------
 * A cell keeps up to PED_SITE_SPOTS of its passing probes, chosen by a
 * farthest-point walk (start from the first pass, then keep adding the pass
 * farthest from the ones kept): a request snaps to the stored point nearest
 * its draw, so the cell's points must spread over the cell, not cluster in
 * one corner. A standable cell therefore answers from 1 to 4 validated
 * positions, each of which passed every rule of the build.
 *
 * The level of an entry
 * ---------------------
 * The rules 3 and 4 read the level of the request (the ring level at the
 * player), so a cell's answer belongs to a level. The queue and the cache
 * store the level the cell was requested at, and a request whose level is more
 * than PED_SITE_LEVEL_MARGIN away treats the entry as stale: the cell is
 * dropped and re-queued for the new level. The cells around a hill therefore
 * re-validate as the level moves, instead of answering with the ground of the
 * level below. The stored points carry their own z, and the caller still
 * refuses a point too far from its own ring level (the population's check):
 * both sides of the comparison share one margin.
 *
 * The queue and the cache
 * -----------------------
 * The queue holds up to PED_SITE_QUEUE (512) requested cells with the level
 * of the request that asked; the pump takes the entry nearest the anchor (the
 * player centre, set every frame) each time, so the near ring always builds
 * first. An admission to a full queue drops the farthest queued cell for the
 * new one (and counts it); a nearer cell is never dropped for a farther one.
 * A request for a cell that is already queued only moves its level when the
 * request's level moved.
 *
 * The cache holds up to PED_SITE_CACHE (1024) built cells - a 512 m square,
 * more than the whole creation ring of the capped ring table (about 470
 * cells). An insert into a full cache evicts the entry farthest from the
 * anchor; an evicted cell goes back to unvalidated and is re-queued by the
 * next request, so the cache is a cache, not a claim that the world cannot
 * change. A cell built as dead keeps an entry too (it holds its level), so a
 * dead cell is not re-probed while the player stays; the eviction takes the
 * dead entries like any other when the anchor moves away.
 *
 * Storage and memory
 * ------------------
 * The state of every cell is one uint16_t (0 unvalidated, 0xFFFF queued, else
 * the cache slot + 1): 62,500 cells, 125,000 B. One cache entry is 2 + 2 + 4
 * + 4 x 12 = 56 B, so the cache is 57,344 B; the queue is two arrays (512 x
 * 2 B + 512 x 4 B = 3,072 B); the stats are 15 uint32 fields, 60 B. The whole
 * store is about 181 KB as the evidence program prints it, every byte of it
 * static: no allocation, no growth.
 *
 * The counters
 * ------------
 * The store keeps SPedSiteStats: the cells built, queued (admissions), dropped
 * (queue overflow), evicted (cache overflow); the positions requested and how
 * they were answered (a spot, a dead cell, off the grid); the probes the
 * builds ran and their verdicts (the six rejects by their reason, the ticket's
 * telemetry). The engine side (PedSites.cpp) mirrors them into the plain
 * statics on CPedSites the live rig reads (utils/caps-audit/live-counters.py).
 *
 * What the checks cannot tell, and what covers it
 * -----------------------------------------------
 * A surface the casts find is a surface. A low platform whose top is within
 * the level margin of the request and whose surface cast agrees (a roof
 * garden, a hillside ledge) still passes, exactly as it did under VICE-122;
 * the reach to the network is not tested at all (see below), so the stray cull
 * and the pedestrian's own walk to the nearest node are what take such a
 * pedestrian, not the placement. And the old node reach bound (150 m, 5 m of
 * level) is deliberately not here: the design's cell checks are ground,
 * water, clearance and slope, only, and the level and roof rules take over the
 * bound's old job of rejecting roofs (a node far below the position meant a
 * roof; a ground off the ring level or a surface over it now means the same
 * thing directly). The walk to the network is the wander state's own (it
 * searches without a reach bound), the stray cull bounds the walk of the ones
 * that stall, and reachability is the service planner's contract (design
 * section 4), not the population's.
 *
 * The engine side (PedSites.cpp): the world queries of one probe (the two
 * casts, the water line, the clearance sphere), the pump (the frame's slice of
 * the build queue), and the population's entry point
 * (CPedSites::FindOffNetworkCoors: the draw, the band, the camera, the level).
 * The evidence program (utils/spawn-service/ped-sites.cpp) compiles this
 * header's implementing translation unit (PedSitesCore.cpp) alone, fills the
 * same SPedSiteProbe from a synthetic map and drives the same store: the
 * predicate, the lattice, the queue, the cache, the counters and the coverage
 * are checked there, and the world queries themselves are the game's (the
 * build and the live soak).
 */

#include <stdint.h>

/* --- the grid ------------------------------------------------------------ */

#define PED_SITE_CELL_SIZE	16.0f		/* metres */
#define PED_SITE_GRID_MIN_X	(-2400.0f)	/* WORLD_MIN_X */
#define PED_SITE_GRID_MIN_Y	(-2000.0f)	/* WORLD_MIN_Y */
#define PED_SITE_CELLS_X	250		/* WORLD_SIZE_X / CELL_SIZE */
#define PED_SITE_CELLS_Y	250		/* WORLD_SIZE_Y / CELL_SIZE */
#define PED_SITE_CELLS		(PED_SITE_CELLS_X * PED_SITE_CELLS_Y)

/* --- the probe and the build -------------------------------------------- */

#define PED_SITE_PROBES		16	/* the 4 x 4 lattice one build probes */
#define PED_SITE_SPOTS		4	/* the standable points a cell keeps */
#define PED_SITE_QUEUE		512	/* the bounded build queue */
#define PED_SITE_CACHE		1024	/* the bounded cache of built cells (a 512 m square) */
#define PED_SITE_CELLS_PER_FRAME 2	/* the build budget of one frame (main thread) */
#define PED_SITE_ATTEMPTS	5	/* the positions one spawn draws (the VICE-122 retries) */

/* --- the predicate's constants ------------------------------------------ */

#define PED_SITE_LEVEL_MARGIN	3.0f	/* the ground-to-request tolerance (the node path's own) */
#define PED_SITE_STEP		0.5f	/* the surface may stand this far above the request level */
#define PED_SITE_WATER_MARGIN	0.1f	/* the ground must stand this far above the water line */
#define PED_SITE_SLOPE_COS_MIN	0.70710678f /* cos 45: the steepest ground a pedestrian walks (a guess) */
#define PED_SITE_ROOF_CAST	50.0f	/* the surface cast starts this far above the request level */
#define PED_SITE_CLEARANCE_HEIGHT 0.9f	/* the clearance sphere sits this far above the ground */
#define PED_SITE_CLEARANCE_RADIUS 0.5f	/* and is this wide */

/* One standable point, metres. */
struct SPedSitePoint
{
	float x, y, z;
};

/* The world's answer at one probe point. The engine side (PedSites.cpp) fills
 * it with the world queries; the store's report runs the predicate. The z of
 * the water line is only read when the block has water, the surface's when
 * the cast found one, the normal's when the ground was found. */
struct SPedSiteProbe
{
	bool groundFound;	/* the cast down from the request level found a surface */
	float groundZ;		/* that surface's height (the ground the probe stands on) */
	float groundNormalZ;	/* its collision normal's z: 1 is flat, 0 is vertical */
	bool surfaceFound;	/* the cast from level + PED_SITE_ROOF_CAST found the top surface */
	float surfaceZ;		/* that surface's height (the one over the point) */
	bool waterFound;	/* the block under the point has a water line */
	float waterZ;		/* the line, without the waves */
	bool clearance;		/* the static world leaves the feet region empty */
};

/* What one probe earned. PED_SITE_PASS stores the point; every other verdict
 * is a reject, and the reason the counters count. */
enum EPedSiteVerdict
{
	PED_SITE_PASS = 0,	/* the ground the point stands on */
	PED_SITE_NO_GROUND,	/* the cast found nothing */
	PED_SITE_WATER,		/* the ground is at or below the water line */
	PED_SITE_LEVEL,		/* the ground is not at the level of the request */
	PED_SITE_ROOF,		/* a surface over the ground closes the point in */
	PED_SITE_CLEARANCE,	/* the static world fills the space over the feet */
	PED_SITE_SLOPE,		/* the ground is steeper than a pedestrian walks */
	PED_SITE_VERDICTS
};

/* What one request was answered with. */
enum EPedSiteAnswer
{
	PED_SITE_ANSWER_SPOT = 0,	/* a validated standable point (the store filled it) */
	PED_SITE_ANSWER_UNKNOWN,	/* the cell is not built yet (this call queued it) */
	PED_SITE_ANSWER_DEAD,		/* the cell is built and holds no standable point */
	PED_SITE_ANSWER_OFF_GRID,	/* the position has no cell (off the world) */
	PED_SITE_ANSWERS
};

/* The counters (the ticket's telemetry). The verdict array is indexed by
 * EPedSiteVerdict, PED_SITE_PASS included (the probes that passed). */
struct SPedSiteStats
{
	uint32_t cellsBuilt;	/* the builds the queue spent */
	uint32_t cellsQueued;	/* the admissions to the queue */
	uint32_t cellsDropped;	/* the cells the queue gave up for a nearer one (a displaced far cell, or a far admission) */
	uint32_t cellsEvicted;	/* cache entries dropped for a nearer build */
	uint32_t requests;	/* the positions requested */
	uint32_t answered;	/* of them, a validated spot answered */
	uint32_t deadAnswers;	/* of them, a built cell held no spot */
	uint32_t probes;	/* the probes the builds ran */
	uint32_t verdicts[PED_SITE_VERDICTS];
};

/* The new predicate, alone: the ground's collision normal against the walk
 * limit. A NaN normal fails the comparison and rejects. */
bool PedSiteGroundIsWalkable(float normalZ);

/* The water rule, alone: the ground stands above the line of the block by the
 * margin (the population's group placements and the stray cull read it too,
 * so the answer is one function, not three copies). */
bool PedSiteGroundIsClearOfWater(bool waterFound, float waterZ, float groundZ);

/* The whole predicate of one probe: the six rules of the header, in order.
 * PED_SITE_PASS writes the ground's height into *pStandZ when the pointer is
 * non-nil; any other verdict is the reason. */
EPedSiteVerdict PedSiteEvaluateProbe(float levelZ, const SPedSiteProbe &probe, float *pStandZ);

/*
 * The store: the grid, the queue, the cache and the counters. Engine-free
 * (plain arithmetic over its own arrays, no allocation anywhere), so the
 * evidence program instantiates it directly and drives the same code the game
 * runs. The engine's one instance lives in PedSites.cpp behind CPedSites.
 */
class CPedSiteCells
{
public:
	enum { CELL_UNVALIDATED = 0, CELL_QUEUED = 0xFFFF };	/* else: the cache slot + 1 */

	CPedSiteCells(void);
	void Reset(void);

	/* The frame's anchor: the queue and the cache keep the cells nearest it.
	 * The engine sets the player centre every frame, before the pump. */
	void SetAnchor(float x, float y);

	/* The pump's pop: the queued cell nearest the anchor, with the level of
	 * its request. False when the queue is empty. The cell's state stays
	 * queued until the report. */
	bool PopNearestQueued(uint16_t &cell, float &levelZ);

	/* The answer to "a validated standable point near (x, y) at this level".
	 * A cell that is not built yet is queued by this call (the lazy build's
	 * demand) and the answer is PED_SITE_ANSWER_UNKNOWN. A stale entry (the
	 * level moved past the margin) is dropped and re-queued. */
	EPedSiteAnswer RequestSpot(float x, float y, float levelZ, SPedSitePoint *pSpot);

	/* The report of one built cell: the engine's probes of every lattice
	 * point, in lattice order. The report evaluates the predicate, keeps the
	 * most spread passes, counts the verdicts and stores the entry (an entry
	 * with no spot is the cell's dead state). */
	void ReportCellProbed(uint16_t cell, float levelZ, const SPedSiteProbe *probes);

	/* The reads: the counters, the queue and cache occupancy, and the state
	 * of one cell (the harness's own checks). */
	const SPedSiteStats &GetStats(void) const { return m_stats; }
	int32_t GetNumQueued(void) const { return m_numQueued; }
	int32_t GetNumCached(void) const { return m_numCached; }
	float GetAnchorX(void) const { return m_anchorX; }
	float GetAnchorY(void) const { return m_anchorY; }
	bool CellIsQueued(uint16_t cell) const { return m_aCell[cell] == CELL_QUEUED; }
	bool CellIsBuilt(uint16_t cell) const { return m_aCell[cell] != CELL_UNVALIDATED && m_aCell[cell] != CELL_QUEUED; }
	bool CellIsStandable(uint16_t cell) const { return CellGetSpots(cell) > 0; }
	int32_t CellGetSpots(uint16_t cell) const;
	float CellGetLevel(uint16_t cell) const;
	const SPedSitePoint *CellGetSpot(uint16_t cell, int32_t i) const;

	/* The grid's own mapping, in the float domain (a NaN or a huge value is
	 * rejected before any cast, the occupancy store's pattern). */
	static bool CellAt(float x, float y, uint16_t *pCell);

	/* The lattice: the i-th probe of a cell, metres. The order is the
	 * lattice's (row by row), the probes are CELL_SIZE / 4 apart, and the
	 * evidence program's synthetic map reads the same points the game
	 * probes. */
	static void ProbePoint(uint16_t cell, int32_t i, float *px, float *py);

	/* The centre of a cell, metres: the queue's nearest-first key. */
	static void CellCentre(uint16_t cell, float *px, float *py);

private:
	/* The built cell: the level of its request, and the standable points.
	 * nSpots of 0 is the cell's dead state (built, nothing stands there). */
	struct SCellEntry
	{
		uint16_t cell;
		uint16_t nSpots;
		float levelZ;
		SPedSitePoint spots[PED_SITE_SPOTS];
	};

	int32_t FindCacheSlot(uint16_t cell) const { return m_aCell[cell] - 1; }
	int32_t QueueSlot(uint16_t cell) const;
	int32_t FarthestQueueSlot(float x, float y) const;
	int32_t FarthestCacheSlot(float x, float y) const;
	void QueueCell(uint16_t cell, float levelZ);
	void DropQueueSlot(int32_t slot);
	void EvictCacheSlot(int32_t slot);
	int32_t CacheSlotForNew(void);
	static bool LevelsMatch(float a, float b);

	/* The state of every cell: CELL_UNVALIDATED, CELL_QUEUED, or the cache
	 * slot + 1 of the built cell. */
	uint16_t m_aCell[PED_SITE_CELLS];

	/* The cache: the built cells, dense. */
	SCellEntry m_aCache[PED_SITE_CACHE];
	int32_t m_numCached;

	/* The queue: the requested cells and the level of their request, dense. */
	uint16_t m_aQueueCell[PED_SITE_QUEUE];
	float m_aQueueLevel[PED_SITE_QUEUE];
	int32_t m_numQueued;

	float m_anchorX, m_anchorY;
	SPedSiteStats m_stats;
};

/*
 * The engine side (PedSites.cpp): the world queries of one probe, the pump,
 * and the population's entry point. The pump spends
 * PED_SITE_CELLS_PER_FRAME builds a frame on the main thread (never the
 * worker: every world query stays with the game's own thread), and the entry
 * point draws up to PED_SITE_ATTEMPTS positions per spawn, asks the store for
 * each and returns the first that also passes the caller's own rules (the
 * creation band's visibility rule and the level of the ring).
 */
class CPedSites
{
public:
	/* The boot (and restart) reset: CPopulation::Initialise, with the stray
	 * table (the cells belong to the game that ends with the call). */
	static void Reset(void);

	/* The frame's slice of the lazy build: CPopulation::Update, before the
	 * spawn calls. Interiors are the caller's own gate (the population's
	 * spawn path does not ask for cells in an interior). */
	static void Pump(void);

	/* A free position for one off-network spawn. The caller passes the
	 * centre and the bands its node path uses (the creation ring and the
	 * off-screen band); the draw is the VICE-122 one. True with a validated
	 * position in *pPosition. */
	static bool FindOffNetworkCoors(const SPedSitePoint &centre, float minDist, float maxDist,
		float minDistOffScreen, float maxDistOffScreen, SPedSitePoint *pPosition);

	/* The store's stats, and the live rig's plain statics: the store holds
	 * the truth, PublishStats mirrors it after every pump and every request
	 * (the rig reads the game's memory by symbol name, so the counters it
	 * names are these; utils/caps-audit/live-counters.py). */
	static const SPedSiteStats &Stats(void);
	static uint32_t ms_nCellsBuilt;
	static uint32_t ms_nCellsQueued;
	static uint32_t ms_nCellsDropped;
	static uint32_t ms_nCellsEvicted;
	static uint32_t ms_nRequests;
	static uint32_t ms_nRequestsAnswered;
	static uint32_t ms_nRequestsDead;
	static uint32_t ms_nProbes;
	static uint32_t ms_nRejectsNoGround;
	static uint32_t ms_nRejectsWater;
	static uint32_t ms_nRejectsLevel;
	static uint32_t ms_nRejectsRoof;
	static uint32_t ms_nRejectsClearance;
	static uint32_t ms_nRejectsSlope;

private:
	static void ProbeCell(uint16_t cell, float levelZ);
	static void PublishStats(void);

	static CPedSiteCells ms_cells;
};

#endif // __GTA_PEDSITES_H__
