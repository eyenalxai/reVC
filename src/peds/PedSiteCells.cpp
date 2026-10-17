/*
 * The pedestrian standable cells (VICE-154): the lattice, the predicate, the
 * queue and the cache. The model and the design decisions are in PedSites.h.
 *
 * Nothing here includes an engine header, and nothing here calls the game: the
 * predicate reads the plain SPedSiteProbe the engine side fills, the store
 * moves plain arithmetic, and the evidence program (utils/spawn-service)
 * drives this translation unit over a synthetic map. The engine side
 * (PedSites.cpp) is the world queries, the pump and the population's entry
 * point.
 */

#include <string.h>

#include "PedSites.h"

/* The squared plane distance, the queue's and the cache's one key. */
static inline float
PedSiteDistSq(float ax, float ay, float bx, float by)
{
	return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

bool
PedSiteGroundIsWalkable(float normalZ)
{
	/* The ground's collision normal against the walk limit. A NaN normal (a
	 * degenerate collision) fails the comparison and rejects, and a ground
	 * without a normal (every flag the engine side could not fill) reads
	 * 0.0 and rejects too: the safe direction. */
	return normalZ >= PED_SITE_SLOPE_COS_MIN;
}

bool
PedSiteGroundIsClearOfWater(bool waterFound, float waterZ, float groundZ)
{
	/* A block without water has no line to stand above; a block with one
	 * reads the same comparison the VICE-122 placement read. The margin
	 * keeps a wading pedestrian out: the ground must stand clear of the
	 * line, not at it. */
	if (!waterFound)
		return true;
	return groundZ > waterZ + PED_SITE_WATER_MARGIN;
}

EPedSiteVerdict
PedSiteEvaluateProbe(float levelZ, const SPedSiteProbe &probe, float *pStandZ)
{
	/* 1. The ground: nothing under the probe is the void - the holes of the
	 * collision world and the shores past its end. */
	if (!probe.groundFound)
		return PED_SITE_NO_GROUND;
	/* 2. The water: the ground must stand above the line of the block. The
	 * line is the level without the waves, so the shoreline does not move
	 * with the wave phase (the stray cull reads the same line). */
	if (!PedSiteGroundIsClearOfWater(probe.waterFound, probe.waterZ, probe.groundZ))
		return PED_SITE_WATER;
	/* 3. The level: the ground stands within the margin of the level of the
	 * request (the node path's own 3 m). A seabed, a hilltop above the
	 * player and the street under a bridge read here. */
	if (probe.groundZ - levelZ > PED_SITE_LEVEL_MARGIN || levelZ - probe.groundZ > PED_SITE_LEVEL_MARGIN)
		return PED_SITE_LEVEL;
	/* 4. The surface over the ground: the top surface below the cast stands
	 * no more than a step above the ground the probe stands on. A roof, a
	 * canopy or a bridge deck stands higher, and a point under one is a
	 * point inside the world - the VICE-122 rule exactly (its cast started
	 * at the ground and its comparison read the ground, because it set the
	 * position to the ground before the test). */
	if (!probe.surfaceFound || probe.surfaceZ > probe.groundZ + PED_SITE_STEP)
		return PED_SITE_ROOF;
	/* 5. The clearance over the feet: the static world leaves the sphere
	 * empty. The dynamic clearance (vehicles, pedestrians) is the creation
	 * loop's own check, which cannot be cached. */
	if (!probe.clearance)
		return PED_SITE_CLEARANCE;
	/* 6. The slope: the ground is not steeper than a pedestrian walks. */
	if (!PedSiteGroundIsWalkable(probe.groundNormalZ))
		return PED_SITE_SLOPE;
	if (pStandZ)
		*pStandZ = probe.groundZ;
	return PED_SITE_PASS;
}

/* The cell of a coordinate: the float domain first (a NaN or a huge value
 * fails the range test before any cast), then the cell's own index. The same
 * mapping the occupancy store uses; the two grids are separate on purpose
 * (the occupancy store is read for the counts, this one for the ground). */
bool
CPedSiteCells::CellAt(float x, float y, uint16_t *pCell)
{
	float fx = (x - PED_SITE_GRID_MIN_X) / PED_SITE_CELL_SIZE;
	float fy = (y - PED_SITE_GRID_MIN_Y) / PED_SITE_CELL_SIZE;
	if (!(fx >= 0.0f && fx < PED_SITE_CELLS_X && fy >= 0.0f && fy < PED_SITE_CELLS_Y))
		return false;
	*pCell = (uint16_t)((int32_t)fy * PED_SITE_CELLS_X + (int32_t)fx);
	return true;
}

void
CPedSiteCells::ProbePoint(uint16_t cell, int32_t i, float *px, float *py)
{
	/* The 4 x 4 lattice, row by row: one probe every CELL_SIZE / 4 metres,
	 * the first at the cell's edge plus half a step. */
	*px = PED_SITE_GRID_MIN_X + ((cell % PED_SITE_CELLS_X) + ((i & 3) + 0.5f) / 4.0f) * PED_SITE_CELL_SIZE;
	*py = PED_SITE_GRID_MIN_Y + ((cell / PED_SITE_CELLS_X) + ((i >> 2) + 0.5f) / 4.0f) * PED_SITE_CELL_SIZE;
}

void
CPedSiteCells::CellCentre(uint16_t cell, float *px, float *py)
{
	*px = PED_SITE_GRID_MIN_X + ((cell % PED_SITE_CELLS_X) + 0.5f) * PED_SITE_CELL_SIZE;
	*py = PED_SITE_GRID_MIN_Y + ((cell / PED_SITE_CELLS_X) + 0.5f) * PED_SITE_CELL_SIZE;
}

bool
CPedSiteCells::LevelsMatch(float a, float b)
{
	/* The signed differences, so a NaN level fails both comparisons and the
	 * entry is stale (a re-queue, never an answer). */
	return a - b <= PED_SITE_LEVEL_MARGIN && b - a <= PED_SITE_LEVEL_MARGIN;
}

CPedSiteCells::CPedSiteCells(void)
{
	Reset();
}

void
CPedSiteCells::Reset(void)
{
	/* The cells, the queue, the cache and the counters start empty. The
	 * store owns no pointer, so a reset drops nothing else. */
	memset(m_aCell, 0, sizeof(m_aCell));
	m_numCached = 0;
	m_numQueued = 0;
	m_anchorX = 0.0f;
	m_anchorY = 0.0f;
	memset(&m_stats, 0, sizeof(m_stats));
}

void
CPedSiteCells::SetAnchor(float x, float y)
{
	m_anchorX = x;
	m_anchorY = y;
}

int32_t
CPedSiteCells::QueueSlot(uint16_t cell) const
{
	for (int32_t i = 0; i < m_numQueued; i++)
		if (m_aQueueCell[i] == cell)
			return i;
	return -1;
}

int32_t
CPedSiteCells::FarthestQueueSlot(float x, float y) const
{
	int32_t far = 0;
	float farD = 0.0f;
	for (int32_t i = 0; i < m_numQueued; i++) {
		float cx, cy;
		CellCentre(m_aQueueCell[i], &cx, &cy);
		float d = PedSiteDistSq(cx, cy, x, y);
		if (i == 0 || d > farD) {
			far = i;
			farD = d;
		}
	}
	return far;
}

int32_t
CPedSiteCells::FarthestCacheSlot(float x, float y) const
{
	int32_t far = 0;
	float farD = 0.0f;
	for (int32_t i = 0; i < m_numCached; i++) {
		float cx, cy;
		CellCentre(m_aCache[i].cell, &cx, &cy);
		float d = PedSiteDistSq(cx, cy, x, y);
		if (i == 0 || d > farD) {
			far = i;
			farD = d;
		}
	}
	return far;
}

void
CPedSiteCells::DropQueueSlot(int32_t slot)
{
	/* The cell leaves the queue: its state goes back to unvalidated (a
	 * popped cell is re-queued by the next request if the report is not
	 * there yet, and an evicted admission is simply gone). */
	m_aCell[m_aQueueCell[slot]] = CELL_UNVALIDATED;
	m_numQueued--;
	if (slot != m_numQueued) {
		m_aQueueCell[slot] = m_aQueueCell[m_numQueued];
		m_aQueueLevel[slot] = m_aQueueLevel[m_numQueued];
	}
}

void
CPedSiteCells::QueueCell(uint16_t cell, float levelZ)
{
	if (m_numQueued >= PED_SITE_QUEUE) {
		/* The queue is full: the admission takes the slot of the farthest
		 * queued cell, and a cell farther than that one is not admitted at
		 * all. The build is nearest-first (the AddModelsToRequestList
		 * shape), so a far cell must never push a near one out. Either way
		 * the cell that gives way counts as dropped. */
		int32_t far = FarthestQueueSlot(m_anchorX, m_anchorY);
		float cx, cy, fx, fy;
		CellCentre(cell, &cx, &cy);
		CellCentre(m_aQueueCell[far], &fx, &fy);
		if (PedSiteDistSq(cx, cy, m_anchorX, m_anchorY) >= PedSiteDistSq(fx, fy, m_anchorX, m_anchorY)) {
			m_stats.cellsDropped++;
			return;
		}
		DropQueueSlot(far);
		m_stats.cellsDropped++;
	}
	m_aQueueCell[m_numQueued] = cell;
	m_aQueueLevel[m_numQueued] = levelZ;
	m_numQueued++;
	m_aCell[cell] = CELL_QUEUED;
	m_stats.cellsQueued++;
}

bool
CPedSiteCells::PopNearestQueued(uint16_t &cell, float &levelZ)
{
	int32_t best = -1;
	float bestD = 0.0f;
	for (int32_t i = 0; i < m_numQueued; i++) {
		float cx, cy;
		CellCentre(m_aQueueCell[i], &cx, &cy);
		float d = PedSiteDistSq(cx, cy, m_anchorX, m_anchorY);
		if (best < 0 || d < bestD) {
			best = i;
			bestD = d;
		}
	}
	if (best < 0)
		return false;
	cell = m_aQueueCell[best];
	levelZ = m_aQueueLevel[best];
	DropQueueSlot(best);
	return true;
}

void
CPedSiteCells::EvictCacheSlot(int32_t slot)
{
	uint16_t cell = m_aCache[slot].cell;
	m_aCell[cell] = CELL_UNVALIDATED;
	m_stats.cellsEvicted++;
	m_numCached--;
	if (slot != m_numCached) {
		m_aCache[slot] = m_aCache[m_numCached];
		m_aCell[m_aCache[slot].cell] = (uint16_t)(slot + 1);
	}
}

int32_t
CPedSiteCells::CacheSlotForNew(void)
{
	if (m_numCached < PED_SITE_CACHE)
		return m_numCached++;
	/* The cache is full: the entry farthest from the anchor goes. A cell
	 * evicted here is unvalidated again and the next request for it queues a
	 * fresh probe - the cache is a cache. */
	int32_t slot = FarthestCacheSlot(m_anchorX, m_anchorY);
	EvictCacheSlot(slot);
	return slot;
}

EPedSiteAnswer
CPedSiteCells::RequestSpot(float x, float y, float levelZ, SPedSitePoint *pSpot)
{
	uint16_t cell;
	m_stats.requests++;
	if (!CellAt(x, y, &cell))
		return PED_SITE_ANSWER_OFF_GRID;

	uint16_t state = m_aCell[cell];
	if (state == CELL_UNVALIDATED) {
		QueueCell(cell, levelZ);
		return PED_SITE_ANSWER_UNKNOWN;
	}
	if (state == CELL_QUEUED) {
		/* The cell is waiting for its build: the last request's level wins
		 * (the level rules are the request's own, and the request is what
		 * the build answers). */
		int32_t slot = QueueSlot(cell);
		if (slot >= 0 && !LevelsMatch(m_aQueueLevel[slot], levelZ))
			m_aQueueLevel[slot] = levelZ;
		return PED_SITE_ANSWER_UNKNOWN;
	}

	int32_t slot = state - 1;
	SCellEntry *pEntry = &m_aCache[slot];
	if (!LevelsMatch(pEntry->levelZ, levelZ)) {
		/* The rules 3 and 4 read the level, so the answer belongs to it:
		 * the entry is stale, the cell is dropped and re-queued for this
		 * level. */
		EvictCacheSlot(slot);
		QueueCell(cell, levelZ);
		return PED_SITE_ANSWER_UNKNOWN;
	}
	if (pEntry->nSpots == 0) {
		m_stats.deadAnswers++;
		return PED_SITE_ANSWER_DEAD;
	}

	/* The standable point nearest the drawn position: the draw is where the
	 * caller asked, the stored points are the validated standpoints of the
	 * cell, and the nearest one moves the pedestrian as little as the cell's
	 * ground allows. */
	int32_t best = 0;
	float bestD = PedSiteDistSq(pEntry->spots[0].x, pEntry->spots[0].y, x, y);
	for (int32_t i = 1; i < pEntry->nSpots; i++) {
		float d = PedSiteDistSq(pEntry->spots[i].x, pEntry->spots[i].y, x, y);
		if (d < bestD) {
			best = i;
			bestD = d;
		}
	}
	*pSpot = pEntry->spots[best];
	m_stats.answered++;
	return PED_SITE_ANSWER_SPOT;
}

void
CPedSiteCells::ReportCellProbed(uint16_t cell, float levelZ, const SPedSiteProbe *probes)
{
	SPedSitePoint aPass[PED_SITE_PROBES];
	bool aChosen[PED_SITE_PROBES];
	SPedSitePoint aKept[PED_SITE_SPOTS];
	int32_t nPass = 0;
	int32_t nKept = 0;
	int32_t i;

	/* The predicate over the lattice, in order: every probe counts under its
	 * verdict and the passes are kept as candidates. */
	for (i = 0; i < PED_SITE_PROBES; i++) {
		float standZ;
		EPedSiteVerdict verdict = PedSiteEvaluateProbe(levelZ, probes[i], &standZ);
		m_stats.probes++;
		m_stats.verdicts[verdict]++;
		if (verdict != PED_SITE_PASS)
			continue;
		ProbePoint(cell, i, &aPass[nPass].x, &aPass[nPass].y);
		aPass[nPass].z = standZ;
		nPass++;
	}

	/* The most spread passes, up to PED_SITE_SPOTS (the farthest-point
	 * walk): a request snaps to the stored point nearest its draw, so the
	 * cell's points must cover the cell. The walk starts from the first
	 * pass, which keeps the report deterministic. */
	memset(aChosen, 0, sizeof(aChosen));
	if (nPass > 0) {
		aKept[0] = aPass[0];
		aChosen[0] = true;
		nKept = 1;
		while (nKept < PED_SITE_SPOTS && nKept < nPass) {
			int32_t best = -1;
			float bestD = -1.0f;
			for (i = 0; i < nPass; i++) {
				float d = 1.0e9f;
				if (aChosen[i])
					continue;
				for (int32_t k = 0; k < nKept; k++) {
					float d2 = PedSiteDistSq(aPass[i].x, aPass[i].y, aKept[k].x, aKept[k].y);
					if (d2 < d)
						d = d2;
				}
				if (d > bestD) {
					bestD = d;
					best = i;
				}
			}
			aKept[nKept] = aPass[best];
			aChosen[best] = true;
			nKept++;
		}
	}

	/* The entry: a re-report lands in the entry the cell already has, a new
	 * build takes a free or evicted slot. */
	int32_t slot;
	if (m_aCell[cell] != CELL_UNVALIDATED && m_aCell[cell] != CELL_QUEUED)
		slot = m_aCell[cell] - 1;
	else
		slot = CacheSlotForNew();
	SCellEntry *pEntry = &m_aCache[slot];
	pEntry->cell = cell;
	pEntry->nSpots = (uint16_t)nKept;
	pEntry->levelZ = levelZ;
	for (i = 0; i < nKept; i++)
		pEntry->spots[i] = aKept[i];
	m_aCell[cell] = (uint16_t)(slot + 1);
	m_stats.cellsBuilt++;
}

int32_t
CPedSiteCells::CellGetSpots(uint16_t cell) const
{
	if (!CellIsBuilt(cell))
		return 0;
	return m_aCache[FindCacheSlot(cell)].nSpots;
}

float
CPedSiteCells::CellGetLevel(uint16_t cell) const
{
	if (!CellIsBuilt(cell))
		return 0.0f;
	return m_aCache[FindCacheSlot(cell)].levelZ;
}

const SPedSitePoint *
CPedSiteCells::CellGetSpot(uint16_t cell, int32_t i) const
{
	if (!CellIsBuilt(cell))
		return NULL;
	const SCellEntry *pEntry = &m_aCache[FindCacheSlot(cell)];
	if (i < 0 || i >= pEntry->nSpots)
		return NULL;
	return &pEntry->spots[i];
}
