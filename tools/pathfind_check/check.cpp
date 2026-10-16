// Path-result checker and benchmark for VICE-130.
//
// Loads a path-graph dump (tools/pathfind_check/dump.cpp) into the real
// CPathFind object and drives the real path search code, so the results come
// from the code under test, not from a copy of it.
//
// Modes
//   run   <dump> <cases>       print the result of every stored case
//   lookup <dump> <cases>      print the closest-node result of every point,
//                              with the flag and limit combinations callers use
//   bench <dump> <spawn|chase> <frames> <scale>
//                              replay a call pattern, print the cost
//   scan  <dump> <iterations>  time the closest-node lookup alone
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
// Every line of the output (node counts, distances, node index lists, the
// closeness verdict, the closest node of a lookup) must be identical. The
// graph comes from a running game:
//
//   build/pathfind_dump <game pid> /tmp/pathgraph.bin
//   build/pathfind_check.miami run /tmp/pathgraph.bin cases.txt > base.txt
//   build/pathfind_check       run /tmp/pathgraph.bin cases.txt > new.txt
//   diff base.txt new.txt
//
// Rebuild both builds between runs if the dump is retaken (the game may have
// restarted on another build), and keep the dump with the runs it belongs to.

#include "common.h"
#include "PathFind.h"
#include "TrafficRings.h"

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

		bool close = false;
		if (c.type == PATH_CAR)
			// The game calls TestCoorsCloseness with PATH_CAR only; the ped
			// branch is dead code (and one no-callers path in it writes to a
			// zero-length node list, which is why this line guards the type).
			close = ThePaths.TestCoorsCloseness(c.target, c.type, c.start);
		printf("  closeness %d\n", close ? 1 : 0);
	}
}

// Pick road points for a frame: the target is the node a driving car is on,
// the starts are nodes in the same flood-fill group at about the ring
// distance, which is what a spawn placement is. The scan is outside the
// measured loop.
static void
PickFramePoints(int targetNode, float ringDist, int count, int *out)
{
	int carNodes = ThePaths.m_numCarPathNodes;
	int group = ThePaths.m_pathNodes[targetNode].group;
	CVector target = ThePaths.m_pathNodes[targetNode].GetPosition();
	int found = 0;
	for (int i = 0; i < carNodes && found < count; i++) {
		if (ThePaths.m_pathNodes[i].group != group)
			continue;
		float d = (ThePaths.m_pathNodes[i].GetPosition() - target).Magnitude2D();
		if (d < ringDist * 0.5f || d > ringDist * 1.5f)
			continue;
		out[found++] = i;
	}
	if (found == 0) {
		for (int i = 0; i < count; i++)
			out[i] = targetNode;
		return;
	}
	for (int i = found; i < count; i++)
		out[i] = out[i % found];
}

// The two patterns the ticket names.
//
// spawn: CCarCtrl::GenerateRandomCars calls GenerateOneRandomCar a few times
// per frame, and every call asks TestCoorsCloseness for the same target (the
// player centre) from its own placements. That is the repeated search.
//
// chase: CCarCtrl::PickNextNodeToChaseCar asks for a path to a moving car
// (the player's position). The target node changes as it moves, so this
// measures the cost when nothing repeats.
static void
ModeBench(const char *dump, const char *mode, int frames, float scale, uint32 seed)
{
	LoadGraph(dump);
	g_rngState = seed;
	extern STrafficRingInput g_ringInput;
	g_ringInput = STrafficRingInput{ 300.0f * scale, 1.0f, 2000.0f };

	int carNodes = ThePaths.m_numCarPathNodes;
	float ringDist = 250.0f * scale;
	int perFrame = strcmp(mode, "spawn") == 0 ? 24 : 8;
	std::vector<int> startNodes((size_t)frames * perFrame);
	for (int f = 0; f < frames; f++) {
		// The target node walks the car node list: that is a car driving the
		// graph, and it changes the target node the way driving does.
		int targetNode = (f * 7) % carNodes;
		PickFramePoints(targetNode, strcmp(mode, "spawn") == 0 ? ringDist : 120.0f,
			perFrame, &startNodes[(size_t)f * perFrame]);
	}

	CPathNode *nodes[32];
	int16 numNodes;
	float dist;
	long searches = 0, accepted = 0;
	double t0 = NowSeconds();

	if (strcmp(mode, "spawn") == 0) {
		// RANDOM_CAR_ATTEMPTS_PER_FRAME * the ring scale calls per frame,
		// CAR_SPAWN_ATTEMPTS placements each, one closeness test per placement.
		int callsPerFrame = (int)(3.0f * scale);
		for (int f = 0; f < frames; f++) {
			CVector target = ThePaths.m_pathNodes[(f * 7) % carNodes].GetPosition();
			for (int c = 0; c < callsPerFrame; c++)
				for (int k = 0; k < 4; k++) {
					searches++;
					if (ThePaths.TestCoorsCloseness(target, PATH_CAR,
						ThePaths.m_pathNodes[startNodes[(size_t)f * perFrame + (c * 4 + k) % perFrame]].GetPosition()))
						accepted++;
				}
		}
	} else {
		// One chase search per frame per car; the start is a road point near
		// the target, as a chasing car is.
		for (int f = 0; f < frames; f++) {
			CVector target = ThePaths.m_pathNodes[(f * 7) % carNodes].GetPosition();
			for (int c = 0; c < perFrame; c++) {
				searches++;
				ThePaths.DoPathSearch(PATH_CAR,
					ThePaths.m_pathNodes[startNodes[(size_t)f * perFrame + c]].GetPosition(),
					-1, target, nodes, &numNodes, 2, nil, &dist, 999999.9f, -1);
			}
		}
	}

	double t1 = NowSeconds();
	printf("bench %s scale %.1f: %d frames, %ld searches, %ld accepted\n", mode, scale, frames, searches, accepted);
	printf("bench %s scale %.1f: %.3f ms total, %.1f ns per search\n",
		mode, scale, (t1 - t0) * 1e3, (t1 - t0) * 1e9 / (searches ? searches : 1));
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

// The node lookup on its own, with every flag combination the callers use.
// The plain single-precision scan of the base revision and the grid of the
// change must return the same node for every query point.
static void
ModeLookup(const char *dump, const char *casefile)
{
	LoadGraph(dump);
	std::vector<Case> cases = LoadCases(casefile);
	for (size_t i = 0; i < cases.size(); i++) {
		CVector p = cases[i].target;
		printf("point %zu %.3f %.3f %.3f\n", i, p.x, p.y, p.z);
		int car = ThePaths.FindNodeClosestToCoors(p, PATH_CAR, 999999.88f);
		int carNear = ThePaths.FindNodeClosestToCoors(p, PATH_CAR, 60.0f);
		int carDisabled = ThePaths.FindNodeClosestToCoors(p, PATH_CAR, 999999.88f, true);
		int carBetween = ThePaths.FindNodeClosestToCoors(p, PATH_CAR, 999999.88f, false, true);
		int carWater = ThePaths.FindNodeClosestToCoors(p, PATH_CAR, 999999.88f, false, false, false, true);
		int ped = ThePaths.FindNodeClosestToCoors(p, PATH_PED, 999999.9f);
		int pedNear = ThePaths.FindNodeClosestToCoors(p, PATH_PED, 50.0f);
		int pedDisabled = ThePaths.FindNodeClosestToCoors(p, PATH_PED, 999999.9f, true, true);
		printf("  car %d near %d disabled %d between %d water %d ped %d pedNear %d pedFlags %d\n",
			car, carNear, carDisabled, carBetween, carWater, ped, pedNear, pedDisabled);
	}
}

// The node scan on its own: how much of a search is the closest-node lookup.
static void
ModeScan(const char *dump, int iters)
{
	LoadGraph(dump);
	int carNodes = ThePaths.m_numCarPathNodes;
	long found = 0;
	double t0 = NowSeconds();
	for (int i = 0; i < iters; i++) {
		CVector target = ThePaths.m_pathNodes[(i * 7) % carNodes].GetPosition();
		target.x += 3.5f;
		target.y -= 2.5f;
		int car = ThePaths.FindNodeClosestToCoors(target, PATH_CAR, 999999.88f);
		int ped = ThePaths.FindNodeClosestToCoors(target, PATH_PED, 50.0f);
		found += car + (ped >= 0);
	}
	double t1 = NowSeconds();
	printf("scan: %d iterations (%ld results), %.3f ms total, %.1f ns per iteration\n",
		iters, found, (t1 - t0) * 1e3, (t1 - t0) * 1e9 / iters);
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
		ModeBench(argv[2], argv[3], atoi(argv[4]), (float)atof(argv[5]), 0x1234);
	else if (strcmp(argv[1], "gencases") == 0 && argc == 5)
		ModeGenCases(argv[2], atoi(argv[3]), argv[4]);
	else if (strcmp(argv[1], "stat") == 0 && argc == 3)
		ModeStat(argv[2]);
	else if (strcmp(argv[1], "scan") == 0 && argc == 4)
		ModeScan(argv[2], atoi(argv[3]));
	else if (strcmp(argv[1], "lookup") == 0 && argc == 4)
		ModeLookup(argv[2], argv[3]);
	else {
		fprintf(stderr, "bad arguments\n");
		return 1;
	}
	return 0;
}
