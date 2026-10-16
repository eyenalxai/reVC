// Path-result checker and benchmark for VICE-130.
//
// Loads a path-graph dump (tools/pathfind_check/dump.cpp) into the real
// CPathFind object and drives the real path search code, so the results come
// from the code under test, not from a copy of it.
//
// Modes
//   run   <dump> <cases>       print the result of every stored case
//   bench <dump> <iters> <scale> <seed>
//                              replay the car-spawn call pattern, print the cost
//   gencases <dump> <count> <out>
//                              write deterministic cases taken from the real graph
//   stat  <dump>               print graph statistics
//
// The case file has one query per line:
//   <type> <startX> <startY> <startZ> <targetX> <targetY> <targetZ>
// type 0 is PATH_CAR, 1 is PATH_PED.
//
// The equality check is: build this tool once from the base revision and once
// from the change, run both over the same dump and cases, and diff the output.
// Every line of the output (node counts, distances, node index lists) must be
// identical.

#include "common.h"
#include "PathFind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------- rng
// Deterministic stand-in for CGeneral::GetRandomNumber, so the old and the
// new build of the checker draw the same sequence.

static uint32 g_rngState = 0x9e3779b9u;

static uint32
NextRandom(void)
{
	g_rngState ^= g_rngState << 13;
	g_rngState ^= g_rngState >> 17;
	g_rngState ^= g_rngState << 5;
	return g_rngState;
}

// ---------------------------------------------------------------- blob load

#include "pathfind_check_blob.h"

// ---------------------------------------------------------------- timing

static double
NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------- cases

struct Case
{
	int type;
	CVector start;
	CVector target;
};

static std::vector<Case>
LoadCases(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		exit(1);
	}
	std::vector<Case> cases;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		Case c;
		float sx, sy, sz, tx, ty, tz;
		if (sscanf(line, "%d %f %f %f %f %f %f", &c.type, &sx, &sy, &sz, &tx, &ty, &tz) != 7)
			continue;
		c.start = CVector(sx, sy, sz);
		c.target = CVector(tx, ty, tz);
		cases.push_back(c);
	}
	fclose(f);
	return cases;
}

// ---------------------------------------------------------------- modes

static void
ModeRun(const char *dump, const char *casefile)
{
	LoadGraph(dump);
	std::vector<Case> cases = LoadCases(casefile);

	CPathNode *nodes[32];
	int16 numNodes;
	float dist;

	for (size_t i = 0; i < cases.size(); i++) {
		Case &c = cases[i];
		int targetNode = ThePaths.FindNodeClosestToCoors(c.target, c.type, 999999.88f);
		int startNode = ThePaths.FindNodeClosestToCoors(c.start, c.type, 999999.88f);
		printf("case %zu type %d targetNode %d startNode %d\n", i, c.type, targetNode, startNode);

		memset(nodes, 0, sizeof(nodes));
		numNodes = -1;
		dist = -1.0f;
		ThePaths.DoPathSearch(c.type, c.start, -1, c.target, nodes, &numNodes, 32, nil, &dist, 999999.88f, -1);
		printf("  search nodes %d dist %.6f list", numNodes, dist);
		for (int j = 0; j < numNodes && j < 32; j++)
			printf(" %d", (int)(nodes[j] ? nodes[j] - ThePaths.m_pathNodes : -1));
		printf("\n");

		bool close = ThePaths.TestCoorsCloseness(c.target, c.type, c.start);
		printf("  closeness %d\n", close ? 1 : 0);
	}
}

// The spawn path of CCarCtrl::CreateCarFromPlacement: the target is the
// player centre, the starts are the placements the spawn scan picked. One
// target is queried from several starts, and the same target comes back on
// the next call while the player stands still: this is the repeated work
// VICE-130 measures.
static void
ModeBench(const char *dump, int iters, float scale, uint32 seed)
{
	LoadGraph(dump);
	g_rngState = seed;
	extern STrafficRingInput g_ringInput;
	g_ringInput = STrafficRingInput{ 300.0f * scale, 1.0f, 2000.0f };

	const int placementsPerCall = 4;
	CCarCreationPlacement placements[placementsPerCall];
	long searched = 0, accepted = 0, placementTotal = 0;
	double t0 = NowSeconds();
	for (int i = 0; i < iters; i++) {
		// A player centre that moves slowly, like a driving player. The
		// target node changes only when the point crosses a node boundary.
		float px = -300.0f + (float)(i % 100) * 3.0f;
		float py = 400.0f - (float)(i % 50) * 2.0f;
		CVector target(px, py, 10.0f);
		float frontX = 1.0f, frontY = 0.0f;
		int32 numPlacements = ThePaths.CollectCarCreationCoors(px, py, frontX, frontY,
			250.0f * scale, 0.707f, true, false, placements, placementsPerCall);
		placementTotal += numPlacements;
		for (int32 j = 0; j < numPlacements; j++) {
			searched++;
			if (ThePaths.TestCoorsCloseness(target, PATH_CAR, placements[j].position))
				accepted++;
		}
	}
	double t1 = NowSeconds();
	printf("bench: %d calls, %ld placements, %ld searches, %ld accepted\n",
		iters, (long)placementTotal, searched, accepted);
	printf("bench: %.3f ms total, %.1f ns per call, %.1f ns per search\n",
		(t1 - t0) * 1e3, (t1 - t0) * 1e9 / iters, (t1 - t0) * 1e9 / (searched ? searched : 1));
}

static void
ModeGenCases(const char *dump, int count, const char *out)
{
	LoadGraph(dump);
	FILE *f = fopen(out, "w");
	if (!f) {
		perror(out);
		exit(1);
	}
	fprintf(f, "# VICE-130 stored path-search cases, taken from the real path graph.\n");
	fprintf(f, "# type startX startY startZ targetX targetY targetZ\n");
	g_rngState = 0x517ce130u;
	int carNodes = ThePaths.m_numCarPathNodes;
	int total = ThePaths.m_numPathNodes;
	for (int i = 0; i < count; i++) {
		int type = (i % 4 == 3) ? PATH_PED : PATH_CAR;
		int first = type == PATH_CAR ? 0 : carNodes;
		int last = type == PATH_CAR ? carNodes : total;
		int si = first + NextRandom() % (last - first);
		int ti = first + NextRandom() % (last - first);
		CVector s = ThePaths.m_pathNodes[si].GetPosition();
		CVector t = ThePaths.m_pathNodes[ti].GetPosition();
		// Offsets make the queries land between nodes, like the game's
		// positions do; the magnitude stays small so the pair still resolves.
		s.x += ((int)(NextRandom() % 401) - 200) / 8.0f;
		s.y += ((int)(NextRandom() % 401) - 200) / 8.0f;
		t.x += ((int)(NextRandom() % 401) - 200) / 8.0f;
		t.y += ((int)(NextRandom() % 401) - 200) / 8.0f;
		fprintf(f, "%d %.3f %.3f %.3f %.3f %.3f %.3f\n", type, s.x, s.y, s.z, t.x, t.y, t.z);
	}
	fclose(f);
	printf("wrote %d cases to %s\n", count, out);
}

static void
ModeStat(const char *dump)
{
	LoadGraph(dump);
	int carNodes = ThePaths.m_numCarPathNodes;
	int pedNodes = ThePaths.m_numPedPathNodes;
	int water = 0, disabled = 0, between = 0;
	long links = 0;
	for (int i = 0; i < ThePaths.m_numPathNodes; i++) {
		links += ThePaths.m_pathNodes[i].numLinks;
		water += ThePaths.m_pathNodes[i].bWaterPath;
		disabled += ThePaths.m_pathNodes[i].bDisabled;
		between += ThePaths.m_pathNodes[i].bBetweenLevels;
	}
	printf("graph: %d nodes (%d car, %d ped), %ld links, %d connections, %d car path links\n",
		ThePaths.m_numPathNodes, carNodes, pedNodes, links, ThePaths.m_numConnections, ThePaths.m_numCarPathLinks);
	printf("flags: %d water, %d disabled, %d between levels\n", water, disabled, between);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: pathfind_check run|bench|gencases|stat ...\n");
		return 1;
	}
	if (strcmp(argv[1], "run") == 0 && argc == 4)
		ModeRun(argv[2], argv[3]);
	else if (strcmp(argv[1], "bench") == 0 && argc == 6)
		ModeBench(argv[2], atoi(argv[3]), (float)atof(argv[4]), (uint32)strtoul(argv[5], nil, 0));
	else if (strcmp(argv[1], "gencases") == 0 && argc == 5)
		ModeGenCases(argv[2], atoi(argv[3]), argv[4]);
	else if (strcmp(argv[1], "stat") == 0 && argc == 3)
		ModeStat(argv[2]);
	else {
		fprintf(stderr, "bad arguments\n");
		return 1;
	}
	return 0;
}
