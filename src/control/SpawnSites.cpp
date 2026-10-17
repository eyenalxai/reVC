#include "common.h"

#include "PathFind.h"
#include "SpawnSites.h"
#include "Zones.h"

/*
 * The engine side of the spawn site index (VICE-148). The index itself, its
 * model and its rules are in SpawnSites.h/SpawnSitesIndex.cpp, which are
 * engine-free; this file is the snapshot out of ThePaths, the info-zone
 * indices, the build at load and the per-frame flags refresh.
 *
 * The snapshot is its own, not FarTrafficWorker.h's SFarTrafficWorld: the far
 * world carries no water or small-boat flag, no spawn rate, no link width and
 * no zone, and its link direction is a float vector with the lane offset
 * precomputed for the rail arithmetic (SpawnSites.h, "why they are not reused").
 */

/* The graph's plain view: the car block of the path arrays, field by field.
 * The node block is sized to the graph at Build (the car nodes only: the index
 * reads no ped node), the link and connection blocks to the graph's own
 * capacities (an id the graph can name must resolve). The node flags are the
 * only field that moves after the build, and RefreshFlags re-reads them;
 * nothing else is written here. */
static SSpawnSiteNode *ms_pSiteNodes;
static int32 ms_nSiteNodes;			/* the entries ms_pSiteNodes holds */
static SSpawnSiteLink ms_aSiteLinks[NUM_CARPATHLINKS];
static uint16 ms_aSiteConnNode[NUM_PATHCONNECTIONS];
static int16 ms_aSiteConnLink[NUM_PATHCONNECTIONS];
static int16 ms_aSiteConnZone[NUM_PATHCONNECTIONS];

/* The graph flags the index holds: the FarTrafficRefreshWorldFlags pattern's
 * own version (FarTraffic.cpp). */
static uint32 ms_nSiteFlagsVersion = ~0u;

CSpawnSiteIndex CSpawnSites::ms_siteIndex;

/* The node block, one allocation at the first build of the process (the graph
 * never grows after PreparePathData; a later load finds the block big enough).
 * The block is sized to the car nodes, not to NUM_PATHNODES: the shipped graph
 * holds 3,351 car nodes against the 9,650 node capacity, so the car block sizes
 * the snapshot honestly (20 B a node: 67 KB against 193 KB). */
static void
SpawnSiteNodesEnsure(int32 numNodes)
{
	if (ms_nSiteNodes >= numNodes)
		return;
	delete[] ms_pSiteNodes;
	ms_nSiteNodes = numNodes;
	ms_pSiteNodes = numNodes > 0 ? new SSpawnSiteNode[numNodes] : nil;
}

/* CPathNode -> the site snapshot's flag bits; the five bits the sites read. */
static uint8
SpawnSiteNodeFlags(const CPathNode *pNode)
{
	uint8 flags = 0;
	if (pNode->bWaterPath)
		flags |= SPAWN_SITE_NODE_WATER;
	if (pNode->bOnlySmallBoats)
		flags |= SPAWN_SITE_NODE_SMALLBOATS;
	if (pNode->bDeadEnd)
		flags |= SPAWN_SITE_NODE_DEADEND;
	if (pNode->bDisabled)
		flags |= SPAWN_SITE_NODE_DISABLED;
	if (pNode->bBetweenLevels)
		flags |= SPAWN_SITE_NODE_BETWEENLEVELS;
	return flags;
}

void
CSpawnSites::Build(void)
{
	int32 i, j;

	SpawnSiteNodesEnsure(ThePaths.m_numCarPathNodes);

	for (i = 0; i < ThePaths.m_numCarPathNodes; i++) {
		CPathNode *pNode = &ThePaths.m_pathNodes[i];
		ms_pSiteNodes[i].x = pNode->GetX();
		ms_pSiteNodes[i].y = pNode->GetY();
		ms_pSiteNodes[i].z = pNode->GetZ();
		ms_pSiteNodes[i].firstLink = pNode->firstLink;
		ms_pSiteNodes[i].numLinks = pNode->numLinks;
		ms_pSiteNodes[i].spawnRate = pNode->spawnRate;
		ms_pSiteNodes[i].flags = SpawnSiteNodeFlags(pNode);
	}
	for (i = 0; i < ThePaths.m_numCarPathLinks; i++) {
		CCarPathLink *pLink = &ThePaths.m_carPathLinks[i];
		ms_aSiteLinks[i].dirX = pLink->dirX;
		ms_aSiteLinks[i].dirY = pLink->dirY;
		ms_aSiteLinks[i].numLeftLanes = pLink->numLeftLanes;
		ms_aSiteLinks[i].numRightLanes = pLink->numRightLanes;
		ms_aSiteLinks[i].pathNodeIndex = pLink->pathNodeIndex;
		ms_aSiteLinks[i].width = pLink->width;
	}
	for (i = 0; i < ThePaths.m_numConnections; i++) {
		ms_aSiteConnNode[i] = ThePaths.ConnectedNode(i);
		ms_aSiteConnLink[i] = ThePaths.m_carPathConnections[i];
		ms_aSiteConnZone[i] = -1;
	}

	/* The info-zone index of every connection's midpoint. The zone *records*
	 * are never copied: the script fills them after boot (SetZoneCarInfo and
	 * its relatives) and a save load replaces them, so the service resolves
	 * the info live through this index. The zone boundaries do not move after
	 * PostZoneCreation, which ran before this build (CGame::Initialise), and a
	 * save load rewrites the same rectangles in place.
	 *
	 * The pass is one CTheZones::FindInformationZoneForPosition per directed
	 * connection of the car block - every car connection, the dropped ones
	 * included (the loop below is the graph walk, the drops are the index's) -
	 * at the shipped scale about 2 x NUM_CARPATHLINKS of them, and each lookup
	 * is a linear test of the information zones' rectangles (up to NUMINFOZONES
	 * = 169; the shipped info.zon creates about 150, the root zone first). So
	 * the shipped map pays around a million rectangle tests once, under the
	 * loading screen; a map at the connection capacity pays two to three. The
	 * offline harness cannot reach CTheZones, so its mirror of the pass (a
	 * linear scan of the same shape) is what prices it (utils/spawn-service). */
	for (i = 0; i < ThePaths.m_numCarPathNodes; i++) {
		CPathNode *pNode = &ThePaths.m_pathNodes[i];
		for (j = 0; j < pNode->numLinks; j++) {
			int32 conn = pNode->firstLink + j;
			int32 node2;
			CVector mid;
			if (conn < 0 || conn >= ThePaths.m_numConnections)
				continue;
			node2 = ThePaths.ConnectedNode(conn);
			if (node2 < 0 || node2 >= ThePaths.m_numCarPathNodes)
				continue;
			mid.x = (pNode->GetX() + ThePaths.m_pathNodes[node2].GetX()) * 0.5f;
			mid.y = (pNode->GetY() + ThePaths.m_pathNodes[node2].GetY()) * 0.5f;
			mid.z = (pNode->GetZ() + ThePaths.m_pathNodes[node2].GetZ()) * 0.5f;
			ms_aSiteConnZone[conn] = CTheZones::GetIndexForInfoZonePointer(
				CTheZones::FindInformationZoneForPosition(&mid));
		}
	}

	SSpawnSiteGraph graph;
	graph.numNodes = ThePaths.m_numCarPathNodes;
	graph.numLinks = ThePaths.m_numCarPathLinks;
	graph.numConnections = ThePaths.m_numConnections;
	graph.nodes = ms_pSiteNodes;
	graph.links = ms_aSiteLinks;
	graph.connectionNode = ms_aSiteConnNode;
	graph.connectionLink = ms_aSiteConnLink;
	graph.connectionZone = ms_aSiteConnZone;
	ms_siteIndex.Build(graph);

	/* The index now holds the flags of the graph's own version. */
	ms_nSiteFlagsVersion = ThePaths.m_nodeFlagsVersion;

#ifdef DEBUG
	/* The debug build's read of what was built (the guard is the build's own
	 * DEBUG/NDEBUG define, CMakeLists.txt): a boot log line under the loading
	 * screen, nothing in a release build. */
	CSpawnSites::PrintStats();
#endif
}

void
CSpawnSites::PrintStats(void)
{
	const SSpawnSiteBuildStats &stats = ms_siteIndex.GetStats();
	uint snapshotBytes = ms_nSiteNodes * sizeof(SSpawnSiteNode) +
		NUM_CARPATHLINKS * sizeof(SSpawnSiteLink) +
		NUM_PATHCONNECTIONS * (2 + 2 + 2);
	uint indexBytes = sizeof(CSpawnSiteIndex);

	printf("Spawn sites: %d of the %d directed connections usable (%d boats); dropped %d no lanes, %d mixed, %d unresolved; the hash %s, %d cells\n",
		stats.sites, stats.connections, stats.boatSites, stats.droppedNoLanes,
		stats.droppedMixed, stats.droppedUnresolved,
		ms_siteIndex.IsHashValid() ? "built" : "not built (the plain scan)", stats.hashCells);
	printf("Spawn sites: the store %u B (the index %u B: %d sites x %u B, the hash %u B; the snapshot %u B: %d nodes x %u B, %d links x %u B, %d connections x %u B)\n",
		indexBytes + snapshotBytes, indexBytes, NUM_PATHCONNECTIONS, (uint)sizeof(SSpawnSite),
		(SPAWN_SITE_GRID_CELLS + 1) * 2 + NUM_PATHCONNECTIONS * 2, snapshotBytes,
		ms_nSiteNodes, (uint)sizeof(SSpawnSiteNode),
		NUM_CARPATHLINKS, (uint)sizeof(SSpawnSiteLink),
		NUM_PATHCONNECTIONS, (uint)(2 + 2 + 2));
}

void
CSpawnSites::RefreshFlags(void)
{
	/* The FarTrafficRefreshWorldFlags pattern (FarTraffic.cpp): the graph's
	 * own version gates the whole refresh, so a frame with no script road
	 * toggle (no SwitchOffNodeAndNeighbours, no between-levels mark, no save
	 * load) pays one comparison. When it moved, the node flags are re-read
	 * and the sites re-derive their own flags in place - never a rebuild: the
	 * site array, its order and every geometry field stand. */
	if (ms_nSiteFlagsVersion == ThePaths.m_nodeFlagsVersion)
		return;
	ms_nSiteFlagsVersion = ThePaths.m_nodeFlagsVersion;

	for (int32 i = 0; i < ThePaths.m_numCarPathNodes; i++)
		ms_pSiteNodes[i].flags = SpawnSiteNodeFlags(&ThePaths.m_pathNodes[i]);
	ms_siteIndex.RefreshFlags(ms_pSiteNodes);
}
