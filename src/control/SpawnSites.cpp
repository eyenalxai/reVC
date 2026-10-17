#include "common.h"

#include "PathFind.h"
#include "SpawnSites.h"
#include "Zones.h"

/*
 * The engine side of the spawn site index (VICE-148). The index itself, its
 * model and its rules are in SpawnSites.h/SpawnSitesIndex.cpp, which are
 * engine-free; this file is the snapshot out of ThePaths, the info-zone
 * indices, the build at load and the per-frame flags refresh.
 */

/* The graph's plain view: the car block of the path arrays, field by field
 * (the shape FarTrafficWorker.h's snapshot has). The node flags are the only
 * field that moves after the build, and RefreshFlags re-reads them; nothing
 * else is written here. */
static SSpawnSiteNode ms_aSiteNodes[NUM_PATHNODES];
static SSpawnSiteLink ms_aSiteLinks[NUM_CARPATHLINKS];
static uint16 ms_aSiteConnNode[NUM_PATHCONNECTIONS];
static int16 ms_aSiteConnLink[NUM_PATHCONNECTIONS];
static int16 ms_aSiteConnZone[NUM_PATHCONNECTIONS];

/* The graph flags the index holds: the FarTrafficRefreshWorldFlags pattern's
 * own version (FarTraffic.cpp). */
static uint32 ms_nSiteFlagsVersion = ~0u;

CSpawnSiteIndex CSpawnSites::ms_siteIndex;

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

	for (i = 0; i < ThePaths.m_numCarPathNodes; i++) {
		CPathNode *pNode = &ThePaths.m_pathNodes[i];
		ms_aSiteNodes[i].x = pNode->GetX();
		ms_aSiteNodes[i].y = pNode->GetY();
		ms_aSiteNodes[i].z = pNode->GetZ();
		ms_aSiteNodes[i].firstLink = pNode->firstLink;
		ms_aSiteNodes[i].numLinks = pNode->numLinks;
		ms_aSiteNodes[i].spawnRate = pNode->spawnRate;
		ms_aSiteNodes[i].flags = SpawnSiteNodeFlags(pNode);
	}
	for (i = 0; i < ThePaths.m_numCarPathLinks; i++) {
		CCarPathLink *pLink = &ThePaths.m_carPathLinks[i];
		ms_aSiteLinks[i].x = pLink->GetX();
		ms_aSiteLinks[i].y = pLink->GetY();
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
	 * save load rewrites the same rectangles in place. The walk is one scan of
	 * the information zones per connection (CTheZones::FindInformationZoneForPosition
	 * is a linear test of up to NUMINFOZONES rectangles): a few milliseconds
	 * at the shipped data, paid once, under the loading screen. */
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
	graph.nodes = ms_aSiteNodes;
	graph.links = ms_aSiteLinks;
	graph.connectionNode = ms_aSiteConnNode;
	graph.connectionLink = ms_aSiteConnLink;
	graph.connectionZone = ms_aSiteConnZone;
	ms_siteIndex.Build(graph);

	/* The index now holds the flags of the graph's own version. */
	ms_nSiteFlagsVersion = ThePaths.m_nodeFlagsVersion;
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
		ms_aSiteNodes[i].flags = SpawnSiteNodeFlags(&ThePaths.m_pathNodes[i]);
	ms_siteIndex.RefreshFlags(ms_aSiteNodes);
}
