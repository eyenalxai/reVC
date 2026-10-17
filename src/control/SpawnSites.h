#ifndef __GTA_SPAWNSITES_H__
#define __GTA_SPAWNSITES_H__

/*
 * The spawn sites (VICE-148): the index of the places a vehicle may be created
 * on the car path graph - one record per **directed** connection.
 *
 * The directed record
 * -------------------
 *
 * A car path link record (CCarPathLink) is one road segment, and the two travel
 * directions share it. The direction, the lane side and the one-way-ness of a
 * placement are derived from the placement's node pair against the link's own
 * `pathNodeIndex` (CCarCtrl::CreateCarFromPlacement, CarCtrl.cpp): the lanes of
 * the travelled side are `numLeftLanes` when the target node is the link's own
 * node and `numRightLanes` otherwise, and the travel direction is the link's
 * direction with the sign of the node pair. A site is therefore a directed
 * connection - the (entry node -> ConnectedNode) pair of one connection record
 * - and every travelled-side fact lives per orientation, not per link.
 *
 * One site per directed connection, up to NUM_PATHCONNECTIONS (20,400): the
 * graph never moves after CPathFind::PreparePathData, so a site's geometry,
 * its lanes and its rarity are read once. The flags are the one thing that
 * moves with the graph (a script's road toggle, a save load): the refresh
 * below re-derives them, and nothing else in a site changes.
 *
 * The site record
 * ---------------
 *
 *	x, y, z		the connection midpoint: the node pair's own midpoint,
 *			never the link record's position (the external links'
 *			positions are the synthetic midpoints of the merged
 *			external nodes, PathFind.cpp PreparePathDataForType)
 *	connection	the directed connection's id (the m_connections
 *			index): the key the creation primitive repeats its own
 *			lane lookup with
 *	link		the connection's car path link (m_carPathConnections)
 *	dirX, dirY	the travel direction, hundredths: the link's own
 *			direction with the primitive's sign rule
 *	lanes		the lane count of the travelled side (the primitive's
 *			own rule); a site is never stored with 0
 *	width		the link's width
 *	spawnRate	the rarity weight: the *smaller* of the two nodes'
 *			rates, which is the acceptance threshold the primitive's
 *			own dice read (CarCtrl.cpp: Min(node1, node2))
 *	zoneIndex	the info-zone index of the midpoint (CTheZones'
 *			information zone array)
 *	flags		the SPAWN_SITE_* bits below
 *
 * What a site never caches. The **ground**: the collision world is not
 * resident at load, so a site starts unvalidated and the ground is validated
 * lazily per site, on the main thread. The **zone info**: the record holds only
 * the index into CTheZones' information zones - the script fills the zone
 * records after boot (SetZoneCarInfo and its relatives) and a save load
 * replaces the array, so a cached CZoneInfo would be stale and the service
 * resolves the info live, index in hand, with the day/night blend. (The
 * save-load replacement keeps the array's own order, and the saved zones are
 * the same map's rectangles, so the stored index stays the site's zone.)
 *
 * The drops
 * ---------
 *
 * A directed connection is a site only when the travelled side has lanes: a
 * zero count is the wrong direction of a one-way street (the primitive refuses
 * every placement there) and the pair is dropped, so `lanes` is always >= 1 on
 * a stored site. The index drops `lanes <= 0`, the primitive only `== 0`: after
 * PreparePathData the counts are never negative (the lane fixups fall back to
 * 0, PathFind.cpp), so the two agree on every shipped graph, and the index's
 * stricter test keeps a corrupt snapshot out of `GetRandomNumber() % lanes`.
 * A pair whose ends disagree about water is dropped early too: boat sites are
 * directed water-water pairs, and a road site is a land-land pair, so the
 * SPAWN_SITE_BOAT bit is the whole of the split.
 *
 * The flags, and what may change after the build
 * ----------------------------------------------
 *
 * The dead-end, disabled and between-level bits are the union of the two
 * endpoints: a site whose either end is off, between levels or a dead end is
 * not a clean site, and the planner reads the flags to filter. The per-endpoint
 * bits (water, small boats) name the end.
 *
 * The script's road toggles (CPathFind::SwitchOffNodeAndNeighbours, the
 * between-levels marks, CPathFind::Load) and nothing else move the graph after
 * PreparePathData, and they bump CPathFind::m_nodeFlagsVersion. The index never
 * rebuilds for that: CSpawnSites::RefreshFlags re-reads the nodes' flag bits and
 * re-derives the sites' own flags - the FarTrafficRefreshWorldFlags pattern
 * (FarTraffic.cpp), one comparison a frame. Positions, node pairs, lanes,
 * directions, widths, rarities, zones and the site order never change.
 *
 * Storage and memory
 * ------------------
 *
 * Flat arrays plus a 32 m spatial hash, in the style of the path node grid
 * (CPathFind::BuildNodeGrid, ADR 0024): the hash's origin is the bounding box's
 * corner less one cell of margin, its cells are a counting sort of the site
 * array in site order, and CollectSitesNear walks rings of cells around the
 * point. A site outside the grid leaves the hash unbuilt and the query keeps
 * the plain scan, exactly as the node grid does.
 *
 * The store is static, sized by the graph's capacities: a site record is 28
 * bytes and the index holds at most NUM_PATHCONNECTIONS of them (571,200 B),
 * beside the hash's cell offsets (16,641 + 1 entries of 2 B = 33,284 B) and its
 * site list (NUM_PATHCONNECTIONS of 2 B = 40,800 B), so a CSpawnSiteIndex is
 * 645,336 B (630 KiB) as the harness prints it. The engine side's snapshot
 * (SpawnSites.cpp) adds the car node block (sized to the graph at load: the
 * shipped 3,351 nodes of 20 B = 67,020 B), the link block (NUM_CARPATHLINKS of
 * 8 B = 28,000 B) and the three connection arrays (node, link, zone:
 * NUM_PATHCONNECTIONS of 6 B = 122,400 B) - 217,420 B (212 KiB) at the shipped
 * counts, and 862,756 B (843 KiB) for the whole store. utils/spawn-service
 * prints both totals.
 *
 * Two neighbours, and why they are not reused. The snapshot is its own, not
 * FarTrafficWorker.h's SFarTrafficWorld: the far world carries no water and
 * small-boat flags, no spawn rate, no link width and no zone at all (a far car
 * does not need them), and its link direction is a float vector with the lane
 * offset precomputed for the rail arithmetic. And SpawnSitesIndex.cpp's cell
 * arithmetic is a deliberate copy of PathFind.cpp's PathGridCell: this header
 * is engine-free by design, and the two copies must agree bit for bit - the
 * cell of a site at build time and the cell of a query point at request time.
 *
 * The engine side resolves the sites' info-zone indices in one pass: one
 * CTheZones::FindInformationZoneForPosition per directed connection of the car
 * block (every connection, dropped or not), each a linear test of the
 * information zones' rectangles (up to NUMINFOZONES = 169, of which the shipped
 * info.zon creates about 150). That is about a million rectangle tests at the
 * shipped connection count (roughly 2 x NUM_CARPATHLINKS), paid once at load
 * under the loading screen.
 *
 * The module is engine-free: the index and its build read the plain snapshot
 * (SSpawnSiteGraph) and never touch CPathFind, CTheZones or the collision
 * world. SpawnSites.cpp is the engine side that fills the snapshot; the
 * evidence program (utils/spawn-service) drives the index over a synthetic
 * graph of the shipped node counts and, given one, over a real graph dump
 * (tools/pathfind_check's format).
 */

#include <stdint.h>

#include "config.h"	/* NUM_PATHNODES, NUM_CARPATHLINKS, NUM_PATHCONNECTIONS */

/* The path node's own bits a site reads (CPathNode): bWaterPath,
 * bOnlySmallBoats, bDeadEnd, bDisabled, bBetweenLevels. */
enum {
	SPAWN_SITE_NODE_WATER         = 1 << 0,
	SPAWN_SITE_NODE_SMALLBOATS    = 1 << 1,
	SPAWN_SITE_NODE_DEADEND       = 1 << 2,
	SPAWN_SITE_NODE_DISABLED      = 1 << 3,
	SPAWN_SITE_NODE_BETWEENLEVELS = 1 << 4,
};

/* One path node of the graph the index is built over: the fields the sites
 * read out of CPathNode. The position is metres (the graph stores eighths). */
struct SSpawnSiteNode
{
	float x, y, z;
	int16_t firstLink;	/* the node's connections start here */
	uint8_t numLinks;
	uint8_t flags;		/* SPAWN_SITE_NODE_* */
	uint8_t spawnRate;	/* CPathNode::spawnRate: the road's rarity */
};

/* One car path link of the graph: the fields the sites read out of
 * CCarPathLink. The direction is hundredths (the graph stores int8 * 100). The
 * link record's own position is deliberately not part of the view: a site's
 * midpoint is its node pair's own (the external links' positions are the
 * synthetic midpoints of the merged external nodes), so nothing here can read
 * the wrong one. */
struct SSpawnSiteLink
{
	int8_t dirX, dirY;	/* the link's own direction (*100) */
	int8_t numLeftLanes, numRightLanes;
	int16_t pathNodeIndex;	/* the node the link record was made from */
	uint8_t width;
};

/* The graph the index is built over: the car block of CPathFind's arrays, field
 * by field (the shape FarTrafficWorker.h's snapshot has). The connections are
 * laid out per node - node i owns [firstLink, firstLink + numLinks) - and the
 * three connection arrays are indexed by the connection id: connectionNode is
 * ConnectedNode (masked), connectionLink is m_carPathConnections, and
 * connectionZone is the info-zone index of the connection's midpoint (or nil,
 * which stores -1: the zones are the engine side's business). Only the first
 * numNodes (the car nodes) are read. */
struct SSpawnSiteGraph
{
	int32_t numNodes;
	int32_t numLinks;
	int32_t numConnections;
	const SSpawnSiteNode *nodes;
	const SSpawnSiteLink *links;
	const uint16_t *connectionNode;
	const int16_t *connectionLink;
	const int16_t *connectionZone;
};

/*
 * One spawn site: a directed connection with every fact a creation reads.
 * The comments in the file header name each field; the flags are:
 *
 *	WATER1/WATER2		the entry / target node is a water path
 *	SMALLBOATS1/SMALLBOATS2	the entry / target node takes small boats only
 *	DEADEND			either endpoint is a dead end
 *	DISABLED		either endpoint is off (a script's road toggle)
 *	BETWEENLEVELS		either endpoint changes level
 *	BOAT			both endpoints are water: a boat site
 */
struct SSpawnSite
{
	float x, y, z;		/* the connection midpoint (metres) */
	uint16_t connection;
	int16_t node1, node2;	/* the node pair in travel order */
	int16_t link;		/* the connection's car path link */
	int16_t zoneIndex;	/* the info-zone index of the midpoint (-1: none) */
	int8_t dirX, dirY;	/* the travel direction (*100) */
	uint8_t lanes;		/* the travelled side's lanes (never 0) */
	uint8_t width;
	uint8_t spawnRate;	/* the rarity weight: min of the two nodes */
	uint8_t flags;
};

enum {
	SPAWN_SITE_WATER1         = 1 << 0,
	SPAWN_SITE_WATER2         = 1 << 1,
	SPAWN_SITE_SMALLBOATS1    = 1 << 2,
	SPAWN_SITE_SMALLBOATS2    = 1 << 3,
	SPAWN_SITE_DEADEND        = 1 << 4,
	SPAWN_SITE_DISABLED       = 1 << 5,
	SPAWN_SITE_BETWEENLEVELS  = 1 << 6,
	SPAWN_SITE_BOAT           = 1 << 7,
};

/* What one build found: the ticket's counts. `connections` is every directed
 * connection of the car block; a connection is either a site or one of the
 * drops, so usable + the three drop counters = connections. */
struct SSpawnSiteBuildStats
{
	int32_t connections;	/* the directed connections examined */
	int32_t sites;		/* the usable sites stored (never 0 lanes) */
	int32_t boatSites;	/* of them, the directed water-water pairs */
	int32_t droppedNoLanes;	/* dropped: the travelled side has no lanes (a one-way's wrong direction) */
	int32_t droppedMixed;	/* dropped: one endpoint is water and the other is not */
	int32_t droppedUnresolved;	/* dropped: the connection or its link does not resolve in the snapshot */
	int32_t hashCells;	/* the cells the spatial hash holds sites in (0 when the hash is not built) */
};

#define SPAWN_SITE_GRID_CELL 32		/* metres per hash cell (the node grid's cell) */
#define SPAWN_SITE_GRID_DIM 129		/* cells per axis, 4 km: the map is 3.6 km wide */
#define SPAWN_SITE_GRID_CELLS (SPAWN_SITE_GRID_DIM * SPAWN_SITE_GRID_DIM)

/*
 * The index: the sites, the stats, and the spatial hash. Built once per load
 * (the graph never moves), queried by position, never rebuilt for a flag move
 * (RefreshFlags only).
 */
class CSpawnSiteIndex
{
public:
	/* The whole build: the sites over the graph's car nodes, then the
	 * spatial hash over the sites. */
	void Build(const SSpawnSiteGraph &graph);

	/* The flags refresh: re-derive the sites' own flags from the nodes'
	 * (the caller passes the array the index was built from, with fresh
	 * flags). Nothing else moves - no site is added, dropped or reordered.
	 * Returns what the re-derived flags are for the harness's counters:
	 * the number of sites whose flags changed. */
	int32_t RefreshFlags(const SSpawnSiteNode *nodes);

	/* The sites whose midpoint is within `radius` of (x, y) - the hash's own
	 * map-plane query (the path graph is a network in the map plane; a site's
	 * z is its road's height). A negative radius is read as 0. Writes at most
	 * `max` site ids into `out` and returns how many were written; sites are
	 * XY-only, and a site exactly at the radius is included (<=). The order of
	 * a truncated answer is the walk's own: the hash answers cell by cell
	 * (ring order), the fallback in site order - a caller that needs a
	 * deterministic order sorts, a caller that needs them all passes a
	 * `max` no smaller than GetNumSites(). */
	int32_t CollectSitesNear(float x, float y, float radius, int32_t *out, int32_t max) const;

	bool IsHashValid(void) const { return m_hashValid; }
	int32_t GetNumSites(void) const { return m_numSites; }
	const SSpawnSite *GetSites(void) const { return m_sites; }
	const SSpawnSiteBuildStats &GetStats(void) const { return m_stats; }
	int32_t GetNumBuilds(void) const { return m_numBuilds; }
	int32_t GetNumFlagRefreshes(void) const { return m_numFlagRefreshes; }

private:
	SSpawnSiteBuildStats m_stats;
	int32_t m_numSites;
	int32_t m_numBuilds;
	int32_t m_numFlagRefreshes;	/* the refresh path's own count (a full build is never one) */
	int32_t m_gridOriginX;
	int32_t m_gridOriginY;
	bool m_hashValid;		/* false: a site outside the grid, or no site at all */
	uint16_t m_hashCellStart[SPAWN_SITE_GRID_CELLS + 1];
	int16_t m_hashSites[NUM_PATHCONNECTIONS];
	SSpawnSite m_sites[NUM_PATHCONNECTIONS];
};

/*
 * The engine side (SpawnSites.cpp): the snapshot of ThePaths, the build at
 * load, and the per-frame flags refresh. Nothing consumes the index yet
 * (VICE-148); the spawn service and the planner are the next tickets, and the
 * harness (utils/spawn-service) is the index's reader today.
 */
class CSpawnSites
{
public:
	/* The build, right after CPathFind::PreparePathData (and BuildNodeGrid,
	 * CFarTraffic::BuildWorldSnapshot) in CGame::Initialise, with the zones
	 * created above it and the collision world not resident yet. */
	static void Build(void);

	/* The per-frame flags refresh: the FarTrafficRefreshWorldFlags pattern.
	 * A frame whose graph flags did not move reads one integer; the sites are
	 * only re-derived when CPathFind::m_nodeFlagsVersion moved. */
	static void RefreshFlags(void);

	/* A debug build's read of the index, right at the end of Build and usable
	 * anywhere after it (CGame::Initialise's loading screen prints it): the
	 * counts of the ticket's stats and the hash's state, plus the store's
	 * bytes. The call is compiled out of a release build. */
	static void PrintStats(void);

	static const CSpawnSiteIndex &Index(void) { return ms_siteIndex; }
	static int32_t GetNumSites(void) { return ms_siteIndex.GetNumSites(); }
	static const SSpawnSiteBuildStats &Stats(void) { return ms_siteIndex.GetStats(); }

private:
	static CSpawnSiteIndex ms_siteIndex;
};

#endif // __GTA_SPAWNSITES_H__
