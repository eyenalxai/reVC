/*
 * The spawn site index (VICE-148): the build over the directed car path
 * connections, the flags refresh and the spatial hash query. The model, the
 * fields and the design decisions are in SpawnSites.h.
 *
 * Nothing here includes an engine header, and nothing here calls the game: the
 * build reads the plain SSpawnSiteGraph snapshot, the refresh reads the node
 * flags, the query walks the sites. The engine side (SpawnSites.cpp) fills the
 * snapshot from ThePaths; the evidence program (utils/spawn-service) builds the
 * same graph over synthetic data and drives this translation unit.
 */

#include <string.h>

#include "SpawnSites.h"

/* The sites' flags, from the two nodes' bits: the per-endpoint bits name the
 * end, the three tripwires are the union of the two ends (a site whose either
 * endpoint is off, between levels or a dead end is not a clean site), and the
 * boat bit is the water-water pair the drops left. */
static inline uint8_t
SpawnSiteFlags(const SSpawnSiteNode &node1, const SSpawnSiteNode &node2)
{
	uint8_t flags = 0;
	uint8_t both = node1.flags | node2.flags;

	if (node1.flags & SPAWN_SITE_NODE_WATER)
		flags |= SPAWN_SITE_WATER1;
	if (node2.flags & SPAWN_SITE_NODE_WATER)
		flags |= SPAWN_SITE_WATER2;
	if ((node1.flags & node2.flags) & SPAWN_SITE_NODE_WATER)
		flags |= SPAWN_SITE_BOAT;
	if (node1.flags & SPAWN_SITE_NODE_SMALLBOATS)
		flags |= SPAWN_SITE_SMALLBOATS1;
	if (node2.flags & SPAWN_SITE_NODE_SMALLBOATS)
		flags |= SPAWN_SITE_SMALLBOATS2;
	if (both & SPAWN_SITE_NODE_DEADEND)
		flags |= SPAWN_SITE_DEADEND;
	if (both & SPAWN_SITE_NODE_DISABLED)
		flags |= SPAWN_SITE_DISABLED;
	if (both & SPAWN_SITE_NODE_BETWEENLEVELS)
		flags |= SPAWN_SITE_BETWEENLEVELS;
	return flags;
}

/* The cell of a coordinate, the node grid's own arithmetic (PathFind.cpp
 * PathGridCell): the cast truncates towards zero, the cell index floors. */
static inline int
SpawnSiteGridCell(float coord, float origin, float invCell)
{
	float f = (coord - origin) * invCell;
	int cell = (int)f;
	if (f < 0.0f && (float)cell != f)
		cell--;
	return cell;
}

void
CSpawnSiteIndex::Build(const SSpawnSiteGraph &graph)
{
	int32_t i, j;
	int32_t node1, node2, link;

	m_numBuilds++;
	m_numSites = 0;
	m_hashValid = false;
	memset(&m_stats, 0, sizeof(m_stats));

	/* The sites: every directed connection of the car nodes, in node order
	 * (so the site order is the graph's own - the build is deterministic and
	 * a query returns the sites in it). The drops of the ticket: no lanes on
	 * the travelled side, a mixed land/water pair, a connection or link that
	 * does not resolve in the snapshot. */
	for (node1 = 0; node1 < graph.numNodes; node1++) {
		const SSpawnSiteNode &n1 = graph.nodes[node1];
		for (j = 0; j < n1.numLinks; j++) {
			const SSpawnSiteLink *pLink;
			int32_t conn = n1.firstLink + j;
			int32_t lanes, sign;
			SSpawnSite *pSite;

			m_stats.connections++;
			if (conn < 0 || conn >= graph.numConnections) {
				m_stats.droppedUnresolved++;
				continue;
			}
			node2 = graph.connectionNode[conn];
			if (node2 < 0 || node2 >= graph.numNodes) {
				m_stats.droppedUnresolved++;
				continue;
			}
			link = graph.connectionLink[conn];
			if (link < 0 || link >= graph.numLinks) {
				m_stats.droppedUnresolved++;
				continue;
			}

			/* The boat rule: both ends water, or neither. A mixed pair is
			 * rejected here, before anything else is read. */
			if ((n1.flags ^ graph.nodes[node2].flags) & SPAWN_SITE_NODE_WATER) {
				m_stats.droppedMixed++;
				continue;
			}

			/* The lanes of the travelled side, the primitive's own rule
			 * (CarCtrl.cpp CreateCarFromPlacement): the link's own node is
			 * the target's node for one direction and the entry's for the
			 * other. A zero (or a corrupt negative) count is the wrong
			 * direction of a one-way street: the pair is dropped. */
			pLink = &graph.links[link];
			lanes = (pLink->pathNodeIndex == node2) ? pLink->numLeftLanes : pLink->numRightLanes;
			if (lanes <= 0) {
				m_stats.droppedNoLanes++;
				continue;
			}

			pSite = &m_sites[m_numSites++];
			pSite->x = (n1.x + graph.nodes[node2].x) * 0.5f;
			pSite->y = (n1.y + graph.nodes[node2].y) * 0.5f;
			pSite->z = (n1.z + graph.nodes[node2].z) * 0.5f;
			pSite->connection = (uint16_t)conn;
			pSite->node1 = (int16_t)node1;
			pSite->node2 = (int16_t)node2;
			pSite->link = (int16_t)link;
			pSite->zoneIndex = graph.connectionZone ? graph.connectionZone[conn] : -1;
			/* The primitive's own sign rule: the link's direction points
			 * from the higher node index to the lower one, so the travel
			 * direction is the link's when the entry node is the higher of
			 * the pair (CarCtrl.cpp: node1 >= node2 ? 1 : -1). */
			sign = (node1 >= node2) ? 1 : -1;
			pSite->dirX = (int8_t)(sign * pLink->dirX);
			pSite->dirY = (int8_t)(sign * pLink->dirY);
			pSite->lanes = (uint8_t)lanes;
			pSite->width = pLink->width;
			pSite->spawnRate = n1.spawnRate < graph.nodes[node2].spawnRate ?
				n1.spawnRate : graph.nodes[node2].spawnRate;
			pSite->flags = SpawnSiteFlags(n1, graph.nodes[node2]);
			if (pSite->flags & SPAWN_SITE_BOAT)
				m_stats.boatSites++;
		}
	}
	m_stats.sites = m_numSites;

	/* The spatial hash, the node grid's style (BuildNodeGrid): the origin is
	 * the bounding box's corner less one cell of margin, the cells' ranges
	 * are a counting sort of the site array (site order kept inside a cell),
	 * and a site outside the grid leaves the hash unbuilt so the query keeps
	 * the plain scan. */
	if (m_numSites == 0)
		return;

	float minX = 1.0e9f, minY = 1.0e9f;
	int32_t numCells = 0;
	for (i = 0; i < m_numSites; i++) {
		if (m_sites[i].x < minX) minX = m_sites[i].x;
		if (m_sites[i].y < minY) minY = m_sites[i].y;
	}
	m_gridOriginX = (int32_t)minX - SPAWN_SITE_GRID_CELL;
	m_gridOriginY = (int32_t)minY - SPAWN_SITE_GRID_CELL;

	memset(m_hashCellStart, 0, sizeof(m_hashCellStart));
	for (i = 0; i < m_numSites; i++) {
		int gx = SpawnSiteGridCell(m_sites[i].x, (float)m_gridOriginX, 1.0f / SPAWN_SITE_GRID_CELL);
		int gy = SpawnSiteGridCell(m_sites[i].y, (float)m_gridOriginY, 1.0f / SPAWN_SITE_GRID_CELL);
		if (gx < 0 || gx >= SPAWN_SITE_GRID_DIM || gy < 0 || gy >= SPAWN_SITE_GRID_DIM)
			return;	// a site outside the grid: keep the plain scan
		m_hashCellStart[gx * SPAWN_SITE_GRID_DIM + gy + 1]++;
	}
	for (i = 1; i <= SPAWN_SITE_GRID_CELLS; i++)
		m_hashCellStart[i] += m_hashCellStart[i - 1];
	for (i = 0; i < SPAWN_SITE_GRID_CELLS; i++)
		if (m_hashCellStart[i] != m_hashCellStart[i + 1])
			numCells++;

	/* The placement walks the sites backwards and fills each cell from its
	 * end (the prefix sums above are the cursors), so the counting sort needs
	 * no second array. The walk leaves every range starting one entry later
	 * than the query wants; one shift left puts the ranges back and the total
	 * (the end of the last cell) goes to the last entry. The fill leaves a
	 * cell's sites in site order. */
	for (i = m_numSites - 1; i >= 0; i--) {
		int gx = SpawnSiteGridCell(m_sites[i].x, (float)m_gridOriginX, 1.0f / SPAWN_SITE_GRID_CELL);
		int gy = SpawnSiteGridCell(m_sites[i].y, (float)m_gridOriginY, 1.0f / SPAWN_SITE_GRID_CELL);
		m_hashSites[--m_hashCellStart[gx * SPAWN_SITE_GRID_DIM + gy + 1]] = (int16_t)i;
	}
	memmove(m_hashCellStart, m_hashCellStart + 1, sizeof(uint16_t) * SPAWN_SITE_GRID_CELLS);
	m_hashCellStart[SPAWN_SITE_GRID_CELLS] = (uint16_t)m_numSites;

	m_hashValid = true;
	m_stats.hashCells = numCells;
}

int32_t
CSpawnSiteIndex::RefreshFlags(const SSpawnSiteNode *nodes)
{
	int32_t i;
	int32_t changed = 0;

	m_numFlagRefreshes++;
	/* Only the flags move: the node pair, the midpoint, the lanes, the
	 * direction, the rarity, the zone index and the site order are the
	 * build's, and nothing in the graph moves after PreparePathData except
	 * the node flags (CPathFind::m_nodeFlagsVersion). */
	for (i = 0; i < m_numSites; i++) {
		SSpawnSite *pSite = &m_sites[i];
		uint8_t flags = SpawnSiteFlags(nodes[pSite->node1], nodes[pSite->node2]);
		if (flags != pSite->flags) {
			pSite->flags = flags;
			changed++;
		}
	}
	return changed;
}

int32_t
CSpawnSiteIndex::CollectSitesNear(float x, float y, float radius, int32_t *out, int32_t max) const
{
	int32_t found = 0;
	int32_t i;
	const float radiusSq = radius * radius;

	if (m_hashValid) {
		/* The rings of cells around the point, the node grid's own walk
		 * (FindNodeClosestToCoors): a site in ring r is at least (r-1) cells
		 * away, so the walk stops at radius/cellSize + 1 - the division
		 * truncates, and the +1 keeps the ring whose (r-1)-cell edge can
		 * still be inside the radius. Only a ring's own cells are visited
		 * (an inner cell belongs to an earlier ring), and a ring clipped by
		 * the grid visits its part inside it: a cell outside the grid holds
		 * no site when the hash is valid. */
		const float cellSize = (float)SPAWN_SITE_GRID_CELL;
		int cx = SpawnSiteGridCell(x, (float)m_gridOriginX, 1.0f / cellSize);
		int cy = SpawnSiteGridCell(y, (float)m_gridOriginY, 1.0f / cellSize);
		int rMax = radius > 0.0f ? (int)(radius / cellSize) + 1 : 0;
		int r;
		for (r = 0; r <= rMax; r++) {
			int x0 = cx - r, x1 = cx + r;
			int y0 = cy - r, y1 = cy + r;
			int gx0, gx1, gy0, gy1;
			int gx, gy;
			if (x1 < 0 || x0 >= SPAWN_SITE_GRID_DIM || y1 < 0 || y0 >= SPAWN_SITE_GRID_DIM)
				continue;
			gx0 = x0 < 0 ? 0 : x0;
			gx1 = x1 >= SPAWN_SITE_GRID_DIM ? SPAWN_SITE_GRID_DIM - 1 : x1;
			gy0 = y0 < 0 ? 0 : y0;
			gy1 = y1 >= SPAWN_SITE_GRID_DIM ? SPAWN_SITE_GRID_DIM - 1 : y1;
			for (gx = gx0; gx <= gx1; gx++) {
				for (gy = gy0; gy <= gy1; gy++) {
					int cell;
					int k;
					if (r > 0 && gx != x0 && gx != x1 && gy != y0 && gy != y1)
						continue;
					cell = gx * SPAWN_SITE_GRID_DIM + gy;
					for (k = m_hashCellStart[cell]; k < m_hashCellStart[cell + 1]; k++) {
						const SSpawnSite &site = m_sites[m_hashSites[k]];
						float dx = site.x - x, dy = site.y - y;
						if (dx * dx + dy * dy > radiusSq)
							continue;
						if (found >= max)
							return found;
						out[found++] = m_hashSites[k];
					}
				}
			}
		}
		return found;
	}

	/* No hash (a site outside the grid, or an empty index): the plain scan,
	 * the site order kept. */
	for (i = 0; i < m_numSites; i++) {
		float dx = m_sites[i].x - x, dy = m_sites[i].y - y;
		if (dx * dx + dy * dy > radiusSq)
			continue;
		if (found >= max)
			return found;
		out[found++] = i;
	}
	return found;
}
