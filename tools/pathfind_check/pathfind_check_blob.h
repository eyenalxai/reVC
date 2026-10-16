// Loader for the path-graph dump written by tools/pathfind_check/dump.cpp.
//
// Included by check.cpp after the game headers: it fills the real CPathFind
// object (ThePaths) field by field, so the checker can only see a graph the
// running game could have.

#ifndef PATHFIND_CHECK_BLOB_H
#define PATHFIND_CHECK_BLOB_H

struct PathDumpHeader
{
	char magic[8];
	uint32_t version;
	uint32_t numPathNodes;
	int32_t numCarPathNodes;
	int32_t numPedPathNodes;
	int32_t numConnections;
	int32_t numCarPathLinks;
	uint32_t sizes[4];
};

static void
ReadExact(FILE *f, void *dst, size_t size)
{
	if (fread(dst, 1, size, f) != size) {
		fprintf(stderr, "truncated dump\n");
		exit(1);
	}
}

static void
LoadGraph(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(1);
	}
	PathDumpHeader h;
	ReadExact(f, &h, sizeof(h));
	if (memcmp(h.magic, "RVCPATH1", 8) != 0) {
		fprintf(stderr, "bad dump magic\n");
		exit(1);
	}
	if (h.sizes[0] != sizeof(CPathNode) || h.sizes[1] != sizeof(CCarPathLink) ||
	    h.sizes[2] != NUM_PATHCONNECTIONS || h.sizes[3] != NUM_PATHNODES) {
		fprintf(stderr, "dump layout does not match the build (%u/%u/%u/%u)\n",
			h.sizes[0], h.sizes[1], h.sizes[2], h.sizes[3]);
		exit(1);
	}
	if (h.numPathNodes <= 0 || h.numPathNodes > NUM_PATHNODES ||
	    h.numConnections < 0 || h.numConnections > NUM_PATHCONNECTIONS) {
		fprintf(stderr, "implausible counts in dump\n");
		exit(1);
	}

	ReadExact(f, ThePaths.m_pathNodes, sizeof(CPathNode) * NUM_PATHNODES);
	ReadExact(f, ThePaths.m_connections, sizeof(uint16) * NUM_PATHCONNECTIONS);
	ReadExact(f, ThePaths.m_distances, sizeof(uint8) * NUM_PATHCONNECTIONS);
	ReadExact(f, ThePaths.m_carPathConnections, sizeof(int16) * NUM_PATHCONNECTIONS);
	ReadExact(f, ThePaths.m_carPathLinks, sizeof(CCarPathLink) * NUM_CARPATHLINKS);
	fclose(f);

	ThePaths.m_numPathNodes = h.numPathNodes;
	ThePaths.m_numCarPathNodes = h.numCarPathNodes;
	ThePaths.m_numPedPathNodes = h.numPedPathNodes;
	ThePaths.m_numConnections = h.numConnections;
	ThePaths.m_numCarPathLinks = h.numCarPathLinks;
	memset(ThePaths.m_mapObjects, 0, sizeof(ThePaths.m_mapObjects));
}

#endif
