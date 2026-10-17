/*
 * The evidence program for VICE-133, the far traffic layer's simulation on a
 * worker thread.
 *
 * It compiles and drives the game's own worker module
 * (src/control/FarTrafficWorker.cpp - the real simulation, the real cycle, the
 * real handshake) with a synthetic path graph; no engine header is included
 * anywhere (the module does not include one, and that is half of the proof).
 *
 *	A. The snapshot. The world the simulation reads is built here exactly as
 *	   CFarTraffic::BuildWorldSnapshot builds it from ThePaths (the field-by-
 *	   field extraction), and the checksum over the whole struct before and
 *	   after a batch of cycles says the worker writes nothing back into it.
 *
 *	B. The arithmetic. The VICE-133 change moved CCarCtrl's rail pose
 *	   (ComputeCarOnRailPose) and path pick (PickNextNodeRandomly) into the
 *	   shared core of CarOnRails.h, which the near traffic and the worker both
 *	   instantiate. This program carries a reference implementation of the
 *	   *original* code (transcribed from the pre-VICE-133 CarCtrl.cpp: the
 *	   pick and the pose, statement for statement) and drives the real step
 *	   and the reference from the same car state: the route state, the pose,
 *	   the height and the heading must agree bit for bit over a randomized
 *	   sweep (including the u-turn and the clamped-curve cases). The
 *	   reference reports the paths the original gets wrong (VICE-167: the
 *	   third round's 1000 sentinel, the give-up's index one past the node's
 *	   connections, a node with no connections, the flat curve, a zero curve
 *	   time, a progress outside the curve's span); where one fires equality
 *	   is not the claim - the original's value there is an out-of-range read
 *	   or a division by zero - and the case is held to the fix's promises
 *	   instead. The deterministic states of those guards are driven too (the
 *	   crash's own: the dead end's u-turn on the car's own link, the third
 *	   round's acceptance, the give-up's way back, the no-connection node,
 *	   the zero curve time, the out-of-span progress), every pose finite and
 *	   inside the world's sector grid.
 *
 *	C. The boundary. The worker's own list is kept beside a model of the
 *	   applicator's list (the engine's own swap-drop and its append), fed the
 *	   same ADD/DROP commands; every published pose must name the car that
 *	   list has at its index (the lockstep the poses' indices rest on), the
 *	   cycle's "lost" count must stay 0, the frame's share of the stagger must
 *	   be exact, and a cycle must allocate nothing (a global operator new that
 *	   counts while a cycle runs). The *order* of a frame is modelled too - a
 *	   cull hand-over's append, the apply, the prune's drop - and the model is
 *	   run twice: once in the order the applicator keeps (the VICE-133 review
 *	   fix) and once in the order the review found the bug in (the prune
 *	   before the apply), which the same checks must then fail on. The command
 *	   ring's bound and its report are exercised as well.
 *
 *	D. The measurement. The ticket asks for the split of the layer's cost: the
 *	   simulation the worker takes and the main thread's applicator plus
 *	   handshake. The real cycle and the real handshake are timed here at a
 *	   range of caps, in both modes (the worker thread, and the same cycle run
 *	   inline), and the crossover cap - where the simulation's saving passes
 *	   the handshake's cost - is the number the ticket's escape hatch turns on.
 *	   What this program cannot reach, stated plainly:
 *	   - the applicator itself (FarTraffic.cpp) includes the engine and is not
 *	     linked here, so its list and its frame order are a *model* of it, not
 *	     the code (the model is what the review's fix is tested against; the
 *	     applicator's own ordering is the fix's, by inspection);
 *	   - the applicator's catch-up (the command ring's water mark) is engine
 *	     code: the ring's bound and its report are exercised, the catch-up
 *	     itself is not;
 *	   - the engine calls of the applicator (the pose writes, the sector
 *	     re-insert, the RW-frame write, the alpha ramp) are the engine's; the
 *	     live rig and the owner's capture are their measurement, and the
 *	     VICE-128 harness's mirror bounds their arithmetic;
 *	   - the equivalence sweep below drives the real step and the reference
 *	     from the same car state *and the same draws* (the worker's own LCG):
 *	     it proves the branches and the arithmetic of the moved rules, not a
 *	     draw sequence. The far cars' stream is per car on purpose (VICE-133);
 *	     the near traffic's stream is untouched by inspection - the delegate
 *	     CCarCtrl::PickNextNodeRandomly is the route-seed reseed and one call
 *	     to the shared core.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 -pthread \
 *	    -o /tmp/far-traffic-worker src/control/FarTrafficWorker.cpp \
 *	    utils/far-traffic-worker/harness.cpp
 *	/tmp/far-traffic-worker
 *
 * The exit code is 0 when every check passes.
 */

#include "FarTrafficWorker.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The engine's own names for the fixed-width types (common.h), so the code
 * below reads like the code it mirrors; the module itself uses the _t names. */
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;

/* The world the sector grid covers (World.h): CWorld::RemoveAndAdd indexes a
 * sector from the position, and a position outside the grid walks a negative
 * index - the crash's own mechanism. The harness may use the numbers. */
#define WORLD_MIN_X (-2400.0f)
#define WORLD_MAX_X 1600.0f
#define WORLD_MIN_Y (-2000.0f)
#define WORLD_MAX_Y 2000.0f

/* ---------------------------------------------------------------------- *
 * the synthetic path graph
 * ---------------------------------------------------------------------- */

/* The graph the simulation runs on: GRID x GRID nodes on a torus, each with
 * two outgoing links (east and north), so a car can wander forever; a slice of
 * the nodes carries the flags the pick reacts to (dead ends, disabled,
 * between-levels), the links carry the lane counts of one-way and two-way
 * roads, and the z of the nodes varies so the far car's height has something to
 * interpolate. The real graph is a mesh of 3,351 car nodes (ADR 0024); the
 * simulation's cost per car does not depend on it. */
#define GRID 24
#define NUM_NODES (GRID*GRID)
#define LINKS_PER_NODE 2
#define NUM_CONNECTIONS (NUM_NODES*LINKS_PER_NODE)
#define NODE_SPACING 32.0f

static SFarTrafficWorld g_world;

static int
NodeAt(int x, int y)
{
	return ((y % GRID) + GRID) % GRID * GRID + (((x % GRID) + GRID) % GRID);
}

static uint32
HashWorld(const SFarTrafficWorld *pWorld)
{
	/* FNV-1a over everything the worker may read: a write anywhere in the
	 * snapshot shows up. */
	const unsigned char *p = (const unsigned char *)pWorld;
	uint32 hash = 2166136261u;
	for (size_t i = 0; i < sizeof(SFarTrafficWorld); i++) {
		hash ^= p[i];
		hash *= 16777619u;
	}
	return hash;
}

static void
BuildWorld(void)
{
	memset(&g_world, 0, sizeof(g_world));
	g_world.numNodes = NUM_NODES;
	g_world.numLinks = NUM_CONNECTIONS;
	g_world.numConnections = NUM_CONNECTIONS;
	for (int i = 0; i < NUM_NODES; i++) {
		int x = i / GRID;
		int y = i % GRID;
		g_world.nodes[i].x = x * NODE_SPACING;
		g_world.nodes[i].y = y * NODE_SPACING;
		g_world.nodes[i].z = (float)((i % 7) - 3) * 15.0f;
		g_world.nodes[i].firstLink = i * LINKS_PER_NODE;
		/* every 7th node is a dead end, every 11th is switched off, every
		 * 13th is a between-levels mark */
		g_world.nodes[i].numLinks = (i % 7 == 0) ? 1 : LINKS_PER_NODE;
		g_world.nodes[i].flags = 0;
		if (i % 7 == 0)
			g_world.nodes[i].flags |= FAR_SIM_NODE_DEADEND;
		if (i % 11 == 0)
			g_world.nodes[i].flags |= FAR_SIM_NODE_DISABLED;
		if (i % 13 == 0)
			g_world.nodes[i].flags |= FAR_SIM_NODE_BETWEENLEVELS;
	}
	for (int i = 0; i < NUM_CONNECTIONS; i++) {
		int node = i / LINKS_PER_NODE;
		int dir = i % LINKS_PER_NODE;
		int x = node / GRID;
		int y = node % GRID;
		int next = dir == 0 ? NodeAt(x + 1, y) : NodeAt(x, y + 1);
		float nx = g_world.nodes[next].x;
		float ny = g_world.nodes[next].y;
		float dx = nx - g_world.nodes[node].x;
		float dy = ny - g_world.nodes[node].y;
		float len = sqrtf(dx*dx + dy*dy);
		SFarSimLink *pLink = &g_world.links[i];
		g_world.connectionNode[i] = (uint16)next;
		g_world.connectionLink[i] = (int16)i;
		pLink->x = g_world.nodes[node].x + dx * 0.5f;
		pLink->y = g_world.nodes[node].y + dy * 0.5f;
		pLink->dirX = dx / len;
		pLink->dirY = dy / len;
		pLink->pathNodeIndex = (i % 3 == 0) ? (int16)node : (int16)next;
		pLink->numLeftLanes = (dir == 0) ? 2 : 1;
		pLink->numRightLanes = (i % 5 == 0) ? 0 : ((dir == 0) ? 0 : 2);
		pLink->laneOffset = pLink->numLeftLanes == 0 ? 0.5f - 0.5f * pLink->numRightLanes
			: (pLink->numRightLanes == 0 ? 0.5f - 0.5f * pLink->numLeftLanes
			: 0.5f + (float)((i % 3) + 1) / (2.0f * 8.0f * LANE_WIDTH));
	}
}

/*
 * The shapes the deterministic cases of the pick and the curve need, built
 * into the same world (the sweep and the cases read one graph): a node with no
 * connections at all, the dead end whose only connection is the two-way link
 * the car arrived on, the node whose first connection fails the third round
 * while its second is the way back, and the node whose connections all fail it
 * (the give-up), with their own links. The positions stay near (100, 100),
 * well inside the world's sector grid.
 */
#define ZERO_LINK_NODE 150		/* NodeAt(6, 6) */
#define DEADEND_NODE 501
#define DEADEND_PREV_NODE 500
#define DEADEND_LINK (DEADEND_NODE * LINKS_PER_NODE)		/* 1002 */
#define THIRD_NODE 503
#define THIRD_PREV_NODE 500
#define THIRD_DISABLED_NODE 495		/* the grid's own disabled node (495 % 11 == 0) */
#define THIRD_FAIL_LINK (THIRD_NODE * LINKS_PER_NODE)		/* 1006 */
#define THIRD_BACK_LINK (THIRD_NODE * LINKS_PER_NODE + 1)	/* 1007 */
#define GIVEUP_NODE 505
#define GIVEUP_PREV_NODE 506
#define GIVEUP_FAIL_LINK (GIVEUP_NODE * LINKS_PER_NODE)		/* 1010 */
#define GIVEUP_BACK_LINK (GIVEUP_NODE * LINKS_PER_NODE + 1)	/* 1011 */

static void
ShapeLink(int32 link, float x, float y, float dirX, float dirY, int32 pathNode, int8 leftLanes, int8 rightLanes)
{
	SFarSimLink *pLink = &g_world.links[link];

	pLink->x = x;
	pLink->y = y;
	pLink->dirX = dirX;
	pLink->dirY = dirY;
	pLink->laneOffset = 0.5f + 1.0f / (2.0f * 8.0f * LANE_WIDTH);
	pLink->pathNodeIndex = (int16)pathNode;
	pLink->numLeftLanes = leftLanes;
	pLink->numRightLanes = rightLanes;
}

static void
BuildCaseShapes(void)
{
	g_world.nodes[ZERO_LINK_NODE].numLinks = 0;

	/* (a) the dead end: one connection, the two-way link the car arrived on,
	 * back to the previous node. The two nodes sit 8 m apart, so the pick's
	 * lane draw cannot move the lane (the switch needs 14 m and a draw). */
	g_world.nodes[DEADEND_NODE].numLinks = 1;
	g_world.nodes[DEADEND_NODE].firstLink = DEADEND_LINK;
	g_world.nodes[DEADEND_NODE].flags |= FAR_SIM_NODE_DEADEND;
	g_world.nodes[DEADEND_PREV_NODE].x = 100.0f;
	g_world.nodes[DEADEND_PREV_NODE].y = 100.0f;
	g_world.nodes[DEADEND_NODE].x = 108.0f;
	g_world.nodes[DEADEND_NODE].y = 100.0f;
	g_world.connectionNode[DEADEND_LINK] = DEADEND_PREV_NODE;
	g_world.connectionLink[DEADEND_LINK] = DEADEND_LINK;
	ShapeLink(DEADEND_LINK, 104.0f, 100.0f, 1.0f, 0.0f, DEADEND_NODE, 1, 1);

	/* (b) the third round: connection 0 reaches a disabled node (the first
	 * two rounds' tests fail it), connection 1 is the two-way way back. */
	g_world.nodes[THIRD_NODE].numLinks = 2;
	g_world.nodes[THIRD_NODE].firstLink = THIRD_FAIL_LINK;
	g_world.nodes[THIRD_NODE].x = 120.0f;
	g_world.nodes[THIRD_NODE].y = 100.0f;
	g_world.connectionNode[THIRD_FAIL_LINK] = THIRD_DISABLED_NODE;
	g_world.connectionLink[THIRD_FAIL_LINK] = THIRD_FAIL_LINK;
	g_world.connectionNode[THIRD_BACK_LINK] = THIRD_PREV_NODE;
	g_world.connectionLink[THIRD_BACK_LINK] = THIRD_BACK_LINK;
	ShapeLink(THIRD_FAIL_LINK, 120.0f, 100.0f, 1.0f, 0.0f, THIRD_NODE, 1, 0);
	ShapeLink(THIRD_BACK_LINK, 122.0f, 103.0f, -1.0f, 0.0f, THIRD_NODE, 1, 1);

	/* (d) the give-up: every connection is one-way against the car, and
	 * connection 1 is the one that leads back to the previous node. */
	g_world.nodes[GIVEUP_NODE].numLinks = 2;
	g_world.nodes[GIVEUP_NODE].firstLink = GIVEUP_FAIL_LINK;
	g_world.nodes[GIVEUP_NODE].x = 130.0f;
	g_world.nodes[GIVEUP_NODE].y = 100.0f;
	g_world.nodes[GIVEUP_PREV_NODE].x = 138.0f;
	g_world.nodes[GIVEUP_PREV_NODE].y = 100.0f;
	g_world.connectionNode[GIVEUP_FAIL_LINK] = THIRD_DISABLED_NODE;
	g_world.connectionLink[GIVEUP_FAIL_LINK] = GIVEUP_FAIL_LINK;
	g_world.connectionNode[GIVEUP_BACK_LINK] = GIVEUP_PREV_NODE;
	g_world.connectionLink[GIVEUP_BACK_LINK] = GIVEUP_BACK_LINK;
	ShapeLink(GIVEUP_FAIL_LINK, 130.0f, 100.0f, 1.0f, 0.0f, GIVEUP_NODE, 1, 0);
	ShapeLink(GIVEUP_BACK_LINK, 132.0f, 103.0f, -1.0f, 0.0f, GIVEUP_NODE, 1, 0);
}

/* One valid car state at `slot`, wandering a random part of the graph. */
static void
MakeCar(SFarSimCar *pCar, uint16 slot, uint32 seed, int variant)
{
	memset(pCar, 0, sizeof(*pCar));
	uint32 s = seed * 1103515245u + 12345u;

	int node = s % NUM_NODES;
	int link = (s >> 8) % NUM_CONNECTIONS;
	int other = (s >> 16) % NUM_CONNECTIONS;
	/* The engine's placement never gives a car the same link for its current
	 * and next rail, but the pick itself can (the dead end that turns around
	 * on the road it arrived on), so the sweep keeps the general state here
	 * and adds the same-link states as its own slice below: the curve between
	 * coincident lane points is one of the paths the fix closes. */
	if (other == link)
		other = (link + 1) % NUM_CONNECTIONS;

	pCar->slot = slot;
	pCar->currentRouteNode = node;
	pCar->nextRouteNode = g_world.connectionNode[link];
	pCar->prevRouteNode = (s >> 24) % NUM_NODES;
	pCar->nextPathNodeInfo = link;
	pCar->currentPathNodeInfo = other;
	pCar->previousPathNodeInfo = (s >> 4) % NUM_CONNECTIONS;
	pCar->currentDirection = (s & 1) ? 1 : -1;
	pCar->nextDirection = (s & 2) ? 1 : -1;
	pCar->previousDirection = (s & 4) ? 1 : -1;
	pCar->currentLane = variant % 3;
	pCar->nextLane = (s >> 5) % 3;
	pCar->timeEnteredCurve = 1000 + (int32)(s % 5000);
	pCar->timeToSpendOnCurrentCurve = 600 + (int32)((s >> 12) % 4000);
	pCar->randomSeed = (uint16)(s >> 3);
	pCar->routeSeed = (int16)(s >> 7);
	pCar->bStayInCurrentLevel = (s & 8) ? 1 : 0;
	pCar->bStayInFastLane = (s & 16) ? 1 : 0;
	pCar->maxTrafficSpeed = 10.0f + (float)((s >> 20) % 21);
	pCar->heightAboveRoad = 0.5f + (float)((s >> 26) % 4) * 0.25f;
	pCar->forwardX = 1.0f;
	pCar->forwardY = 0.0f;
	pCar->rng = FarTrafficSimRngSeed(slot, pCar->randomSeed, pCar->routeSeed);
}

/* ---------------------------------------------------------------------- *
 * the reference implementation (the original CCarCtrl.cpp, pre-VICE-133)
 * ---------------------------------------------------------------------- */

/*
 * The reference's broken paths, flagged as it runs: the shipped code's own
 * states where its value is a sentinel index, an out-of-range read or a
 * division by zero. The sweep uses them to tell the cases it can compare with
 * the original from the cases it must check against the fix instead. One case
 * can carry more than one flag.
 */
enum {
	REF_BROKEN_PICK_SENTINEL = 1,	/* the third round's 1000 sentinel: the link read indexes connection[first + 1001] */
	REF_BROKEN_PICK_GIVEUP = 2,	/* the give-up exit: the read indexes connection[first + totalLinks] */
	REF_BROKEN_PICK_NO_LINKS = 4,	/* the node has no connections: Random() % 0 (the reference does not run it) */
	REF_BROKEN_CURVE_FLAT = 8,	/* t1 == t2 == 0: the curve divides 0/0 and takes a NaN cosine */
	REF_BROKEN_CURVE_ONE_ZERO = 16,	/* one projection is 0: a blend divides by 2 * 0 */
	REF_BROKEN_CURVE_TIME = 32,	/* the curve time is under the pick's 10 ms floor: the tangent divides by it */
	REF_BROKEN_CURVE_PROGRESS = 64,	/* the progress is outside [0, 1] (or NaN): the pose extrapolates */
};

static uint32 g_refBroken;

/* Transcribed from src/control/CarCtrl.cpp at miami (a5b24d8b, before this
 * ticket pulled the rules into CarOnRails.h): CCarCtrl::FindPathDirection,
 * CCurves (which the same ticket made delegate to CarOnRails.h, so the formulas
 * below are the ones it delegates to), CCarCtrl::ComputeCarOnRailPose and
 * CCarCtrl::PickNextNodeRandomly. The accessors are the graph arrays of the
 * snapshot; the random draws are the car's own stream, in the same order as the
 * worker's views draw them. */

#define REF_NONE 0
#define REF_STRAIGHT 1
#define REF_RIGHT 2
#define REF_LEFT 4
#define REF_ATTEMPTS 15

static float
RefCurveSpeedScaleFactor(float p1x, float p1y, float p2x, float p2y, float dir1X, float dir1Y, float dir2X, float dir2Y)
{
	float distance = sqrtf((p1x - p2x)*(p1x - p2x) + (p1y - p2y)*(p1y - p2y));
	float dp = dir1X*dir2X + dir1Y*dir2Y;
	if (dp > 0.9f)
		return distance + fabsf((p1x * dir1Y - p1y * dir1X) - (p2x * dir1Y - p2y * dir1X));
	else
		return ((1.0f - dp) * 0.25f + 1.0f) * distance;
}

static void
RefCurvePoint(float p1x, float p1y, float p2x, float p2y, float d1x, float d1y, float d2x, float d2y,
	float between, int32 timeOnCurve, float *pOutX, float *pOutY, float *pOutDirX, float *pOutDirY)
{
	float actualFactor = RefCurveSpeedScaleFactor(p1x, p1y, p2x, p2y, d1x, d1y, d2x, d2y);
	float dir1x = d1x * actualFactor;
	float dir1y = d1y * actualFactor;
	float dir2x = d2x * actualFactor;
	float dir2y = d2y * actualFactor;
	float t1 = fabsf((p2x - p1x) * dir1x + (p2y - p1y) * dir1y);
	float t2 = fabsf((p1x - p2x) * dir2x + (p1y - p2y) * dir2y);
	float curveCoef;
	if (t1 == 0.0f && t2 == 0.0f)
		g_refBroken |= REF_BROKEN_CURVE_FLAT;
	if (t1 > t2) {
		float coef = (t1 - t2) / (t1 + t2);
		if (between <= coef)
			curveCoef = 0.0f;
		else {
			if (t2 == 0.0f)
				g_refBroken |= REF_BROKEN_CURVE_ONE_ZERO;
			curveCoef = 0.5f - 0.5f * cosf(3.1415f * (between - coef) * (t1 + t2) / (2 * t2));
		}
	} else {
		float coef = 2 * t1 / (t1 + t2);
		if (coef <= between)
			curveCoef = 1.0f;
		else {
			if (t1 == 0.0f)
				g_refBroken |= REF_BROKEN_CURVE_ONE_ZERO;
			curveCoef = 0.5f - 0.5f * cosf(3.1415f * between * (t1 + t2) / (2 * t1));
		}
	}
	if (timeOnCurve < 10)
		g_refBroken |= REF_BROKEN_CURVE_TIME;
	*pOutX = (p1x + between * dir1x) * (1.0f - curveCoef) + (p2x - (1 - between) * dir2x) * curveCoef;
	*pOutY = (p1y + between * dir1y) * (1.0f - curveCoef) + (p2y - (1 - between) * dir2y) * curveCoef;
	*pOutDirX = (dir1x * (1.0f - curveCoef) + dir2x * curveCoef) / (timeOnCurve * 0.001f);
	*pOutDirY = (dir1y * (1.0f - curveCoef) + dir2y * curveCoef) / (timeOnCurve * 0.001f);
}

static uint32
RefRandom(SFarSimCar *pCar)
{
	pCar->rng = pCar->rng * 1103515245u + 12345u;
	return (pCar->rng >> 16) & 0x7FFF;
}

static bool
RefRandomTrueFalse(SFarSimCar *pCar)
{
	return RefRandom(pCar) < 0x7FFF / 2;
}

static uint8
RefPathDirection(int32 prevNode, int32 curNode, int32 nextNode)
{
	float curX = g_world.nodes[curNode].x;
	float curY = g_world.nodes[curNode].y;
	float prevToCurX = curX - g_world.nodes[prevNode].x;
	float prevToCurY = curY - g_world.nodes[prevNode].y;
	float curToNextX = g_world.nodes[nextNode].x - curX;
	float curToNextY = g_world.nodes[nextNode].y - curY;
	float distPrevToCur = sqrtf(prevToCurX*prevToCurX + prevToCurY*prevToCurY);
	if (distPrevToCur == 0.0f)
		return REF_NONE;
	float sin_a = prevToCurX / distPrevToCur;
	float cos_a = prevToCurY / distPrevToCur;
	float distCurToNext = sqrtf(curToNextX*curToNextX + curToNextY*curToNextY);
	if (distCurToNext == 0.0f)
		return REF_NONE;
	float sin_b = curToNextY / distCurToNext;
	float cos_b = curToNextX / distCurToNext;
	float sin_direction = sin_a * sin_b - cos_a * cos_b;
	if (sin_direction > 0.77f)
		return REF_LEFT;
	if (sin_direction < -0.77f)
		return REF_RIGHT;
	return REF_STRAIGHT;
}

static float
RefForwardX(int32 link) { return g_world.links[link].dirX; }
static float
RefForwardY(int32 link) { return g_world.links[link].dirY; }

static void
RefPose(SFarSimCar *pCar, uint32 now, float *pX, float *pY, float *pDirX, float *pDirY)
{
	int32 curLink = pCar->currentPathNodeInfo;
	int32 nextLink = pCar->nextPathNodeInfo;
	float currentPathLinkForwardX = RefForwardX(curLink) * pCar->currentDirection;
	float currentPathLinkForwardY = RefForwardY(curLink) * pCar->currentDirection;
	float nextPathLinkForwardX = RefForwardX(nextLink) * pCar->nextDirection;
	float nextPathLinkForwardY = RefForwardY(nextLink) * pCar->nextDirection;
	float posCurX = g_world.links[curLink].x + ((pCar->currentLane + g_world.links[curLink].laneOffset) * LANE_WIDTH) * currentPathLinkForwardY;
	float posCurY = g_world.links[curLink].y - ((pCar->currentLane + g_world.links[curLink].laneOffset) * LANE_WIDTH) * currentPathLinkForwardX;
	float posNextX = g_world.links[nextLink].x + ((pCar->nextLane + g_world.links[nextLink].laneOffset) * LANE_WIDTH) * nextPathLinkForwardY;
	float posNextY = g_world.links[nextLink].y - ((pCar->nextLane + g_world.links[nextLink].laneOffset) * LANE_WIDTH) * nextPathLinkForwardX;
	int32 seedCur = (curLink + pCar->randomSeed) & 7;
	int32 seedNext = (nextLink + pCar->randomSeed) & 7;
	float dirCurX = (seedCur - 3) * 0.009f + currentPathLinkForwardX;
	float dirCurY = ((seedCur >> 3) - 3) * 0.009f + currentPathLinkForwardY;
	float dirNextX = (seedNext - 3) * 0.009f + nextPathLinkForwardX;
	float dirNextY = ((seedNext >> 3) - 3) * 0.009f + nextPathLinkForwardY;
	float sq = dirCurX*dirCurX + dirCurY*dirCurY;
	if (sq > 0.0f) {
		float invsqrt = 1.0f / sqrtf(sq);
		dirCurX *= invsqrt; dirCurY *= invsqrt;
	} else {
		dirCurX = 1.0f;
	}
	sq = dirNextX*dirNextX + dirNextY*dirNextY;
	if (sq > 0.0f) {
		float invsqrt = 1.0f / sqrtf(sq);
		dirNextX *= invsqrt; dirNextY *= invsqrt;
	} else {
		dirNextX = 1.0f;
	}
	float between = (float)(now - (uint32)pCar->timeEnteredCurve) / pCar->timeToSpendOnCurrentCurve;
	if (!(between >= 0.0f && between <= 1.0f))
		g_refBroken |= REF_BROKEN_CURVE_PROGRESS;
	RefCurvePoint(posCurX, posCurY, posNextX, posNextY, dirCurX, dirCurY, dirNextX, dirNextY,
		between, pCar->timeToSpendOnCurrentCurve, pX, pY, pDirX, pDirY);
}

static void
RefPickNextNode(SFarSimCar *pCar)
{
	int32 prevNode = pCar->currentRouteNode;
	int32 curNode = pCar->nextRouteNode;
	uint8 totalLinks = g_world.nodes[curNode].numLinks;
	int32 curLink = pCar->nextPathNodeInfo;
	uint8 lanesOnCurrentPath;
	bool isOnOneWayRoad;
	if (totalLinks == 0) {
		/* The original draws Random() % totalLinks and traps (SIGFPE), so the
		 * reference cannot run that path: it flags it and keeps the route,
		 * which is what the fix does there too (plus the u-turn). */
		g_refBroken |= REF_BROKEN_PICK_NO_LINKS;
		return;
	}
	if (g_world.links[curLink].pathNodeIndex == curNode) {
		lanesOnCurrentPath = g_world.links[curLink].numLeftLanes;
		isOnOneWayRoad = g_world.links[curLink].numRightLanes == 0;
	} else {
		lanesOnCurrentPath = g_world.links[curLink].numRightLanes;
		isOnOneWayRoad = g_world.links[curLink].numLeftLanes == 0;
	}
	uint8 allowedDirections = REF_NONE;
	uint8 nextLane = pCar->nextLane;
	if (nextLane == 0)
		allowedDirections |= REF_LEFT;
	if (nextLane == lanesOnCurrentPath - 1)
		allowedDirections |= REF_RIGHT;
	if (lanesOnCurrentPath < 3 || allowedDirections == REF_NONE)
		allowedDirections |= REF_STRAIGHT;
	int attempt;
	pCar->prevRouteNode = pCar->currentRouteNode;
	pCar->currentRouteNode = pCar->nextRouteNode;
	int32 prevPathNode = prevNode;
	int32 curPathNode = curNode;
	int16 nextLink;
	int32 nextPathLink;
	bool goingAgainstOneWayRoad = false;
	bool nextNodeIsOneWayRoad = false;
	uint8 direction = REF_NONE;
	for (attempt = 0; attempt < REF_ATTEMPTS; attempt++) {
		if (attempt != 0) {
			if (pCar->nextRouteNode != prevNode) {
				if (direction & allowedDirections) {
					int32 candidate = pCar->nextRouteNode;
					if ((!((g_world.nodes[candidate].flags & FAR_SIM_NODE_DEADEND) != 0) || ((g_world.nodes[prevPathNode].flags & FAR_SIM_NODE_DEADEND) != 0)) &&
						(!((g_world.nodes[candidate].flags & FAR_SIM_NODE_DISABLED) != 0) || ((g_world.nodes[prevPathNode].flags & FAR_SIM_NODE_DISABLED) != 0)) &&
						(!((g_world.nodes[candidate].flags & FAR_SIM_NODE_BETWEENLEVELS) != 0) || ((g_world.nodes[prevPathNode].flags & FAR_SIM_NODE_BETWEENLEVELS) != 0) || !pCar->bStayInCurrentLevel) &&
						!goingAgainstOneWayRoad && (!isOnOneWayRoad || !nextNodeIsOneWayRoad))
						break;
				}
			}
		}
		nextLink = RefRandom(pCar) % totalLinks;
		pCar->nextRouteNode = g_world.connectionNode[nextLink + g_world.nodes[curPathNode].firstLink];
		direction = RefPathDirection(prevNode, curNode, pCar->nextRouteNode);
		nextPathLink = g_world.connectionLink[nextLink + g_world.nodes[curPathNode].firstLink];
		goingAgainstOneWayRoad = g_world.links[nextPathLink].pathNodeIndex == curNode ? g_world.links[nextPathLink].numRightLanes == 0 : g_world.links[nextPathLink].numLeftLanes == 0;
		nextNodeIsOneWayRoad = g_world.links[nextPathLink].pathNodeIndex == curNode ? g_world.links[nextPathLink].numLeftLanes == 0 : g_world.links[nextPathLink].numRightLanes == 0;
	}
	if (attempt >= REF_ATTEMPTS) {
		for (attempt = 0; attempt < REF_ATTEMPTS; attempt++) {
			if (attempt != 0) {
				if (pCar->nextRouteNode != prevNode) {
					int32 candidate = pCar->nextRouteNode;
					if ((!((g_world.nodes[candidate].flags & FAR_SIM_NODE_DISABLED) != 0) || ((g_world.nodes[prevPathNode].flags & FAR_SIM_NODE_DISABLED) != 0)) &&
						(!((g_world.nodes[candidate].flags & FAR_SIM_NODE_BETWEENLEVELS) != 0) || ((g_world.nodes[prevPathNode].flags & FAR_SIM_NODE_BETWEENLEVELS) != 0) || !pCar->bStayInCurrentLevel) &&
						!goingAgainstOneWayRoad)
						break;
				}
			}
			nextLink = RefRandom(pCar) % totalLinks;
			pCar->nextRouteNode = g_world.connectionNode[nextLink + g_world.nodes[curPathNode].firstLink];
			nextPathLink = g_world.connectionLink[nextLink + g_world.nodes[curPathNode].firstLink];
			goingAgainstOneWayRoad = g_world.links[nextPathLink].pathNodeIndex == curNode ? g_world.links[nextPathLink].numRightLanes == 0 : g_world.links[nextPathLink].numLeftLanes == 0;
		}
	}
	if (attempt >= REF_ATTEMPTS) {
		for (nextLink = 0; nextLink < totalLinks; nextLink++) {
			pCar->nextRouteNode = g_world.connectionNode[nextLink + g_world.nodes[curPathNode].firstLink];
			nextPathLink = g_world.connectionLink[nextLink + g_world.nodes[curPathNode].firstLink];
			goingAgainstOneWayRoad = g_world.links[nextPathLink].pathNodeIndex == curNode ? g_world.links[nextPathLink].numRightLanes == 0 : g_world.links[nextPathLink].numLeftLanes == 0;
			if (!goingAgainstOneWayRoad) {
				int32 candidate = pCar->nextRouteNode;
				if ((!((g_world.nodes[candidate].flags & FAR_SIM_NODE_DISABLED) != 0) || ((g_world.nodes[prevPathNode].flags & FAR_SIM_NODE_DISABLED) != 0)) &&
					(!((g_world.nodes[candidate].flags & FAR_SIM_NODE_BETWEENLEVELS) != 0) || ((g_world.nodes[prevPathNode].flags & FAR_SIM_NODE_BETWEENLEVELS) != 0) || !pCar->bStayInCurrentLevel)) {
					/* the shipped sentinel: the read below indexes first + 1001 */
					g_refBroken |= REF_BROKEN_PICK_SENTINEL;
					nextLink = 1000;
				}
			}
		}
		if (nextLink < 999) {
			/* the give-up: the read below indexes first + totalLinks */
			g_refBroken |= REF_BROKEN_PICK_GIVEUP;
			pCar->nextRouteNode = prevNode;
		}
	}
	nextPathLink = g_world.connectionLink[nextLink + g_world.nodes[curPathNode].firstLink];
	if (prevNode == pCar->nextRouteNode)
		pCar->bUTurn = 1;
	pCar->timeEnteredCurve += pCar->timeToSpendOnCurrentCurve;
	pCar->previousPathNodeInfo = pCar->currentPathNodeInfo;
	pCar->currentPathNodeInfo = pCar->nextPathNodeInfo;
	pCar->previousDirection = pCar->currentDirection;
	pCar->currentDirection = pCar->nextDirection;
	pCar->currentLane = pCar->nextLane;
	pCar->nextPathNodeInfo = g_world.connectionLink[nextLink + g_world.nodes[curPathNode].firstLink];
	int8 lanesOnNextNode;
	if (curNode >= pCar->nextRouteNode) {
		pCar->nextDirection = 1;
		lanesOnNextNode = g_world.links[nextPathLink].numLeftLanes;
	} else {
		pCar->nextDirection = -1;
		lanesOnNextNode = g_world.links[nextPathLink].numRightLanes;
	}
	float currentPathLinkForwardX = pCar->currentDirection * RefForwardX(curLink);
	float nextPathLinkForwardX = pCar->nextDirection * RefForwardX(nextPathLink);
	float currentPathLinkForwardY = pCar->currentDirection * RefForwardY(curLink);
	float nextPathLinkForwardY = pCar->nextDirection * RefForwardY(nextPathLink);
	if (lanesOnNextNode >= 0) {
		if ((RefRandom(pCar) & 0x600) == 0) {
			float distX = g_world.nodes[pCar->nextRouteNode].x - g_world.nodes[curNode].x;
			float distY = g_world.nodes[pCar->nextRouteNode].y - g_world.nodes[curNode].y;
			if (distX*distX + distY*distY >= 14.0f*14.0f) {
				if (RefRandomTrueFalse(pCar))
					pCar->nextLane += 1;
				else
					pCar->nextLane -= 1;
			}
		}
		pCar->nextLane = (lanesOnNextNode - 1) < pCar->nextLane ? (lanesOnNextNode - 1) : pCar->nextLane;
		pCar->nextLane = 0 > pCar->nextLane ? 0 : pCar->nextLane;
	} else {
		pCar->nextLane = pCar->currentLane;
	}
	if (pCar->bStayInFastLane)
		pCar->nextLane = 0;
	float positionOnCurrentLinkX = g_world.links[curLink].x + ((pCar->currentLane + g_world.links[curLink].laneOffset) * LANE_WIDTH) * currentPathLinkForwardY;
	float positionOnCurrentLinkY = g_world.links[curLink].y - ((pCar->currentLane + g_world.links[curLink].laneOffset) * LANE_WIDTH) * currentPathLinkForwardX;
	float positionOnNextLinkX = g_world.links[nextPathLink].x + ((pCar->nextLane + g_world.links[nextPathLink].laneOffset) * LANE_WIDTH) * nextPathLinkForwardY;
	float positionOnNextLinkY = g_world.links[nextPathLink].y - ((pCar->nextLane + g_world.links[nextPathLink].laneOffset) * LANE_WIDTH) * nextPathLinkForwardX;
	float directionCurrentLinkX = RefForwardX(curLink) * pCar->currentDirection;
	float directionCurrentLinkY = RefForwardY(curLink) * pCar->currentDirection;
	float directionNextLinkX = RefForwardX(nextPathLink) * pCar->nextDirection;
	float directionNextLinkY = RefForwardY(nextPathLink) * pCar->nextDirection;
	int32 timeOnCurve = (int32)(RefCurveSpeedScaleFactor(
		positionOnCurrentLinkX, positionOnCurrentLinkY,
		positionOnNextLinkX, positionOnNextLinkY,
		directionCurrentLinkX, directionCurrentLinkY,
		directionNextLinkX, directionNextLinkY
	) * (1000.0f / pCar->maxTrafficSpeed));
	int32 clamped = 0;
	if (timeOnCurve < 10)
		clamped = 1;
	pCar->timeToSpendOnCurrentCurve = 10 > timeOnCurve ? 10 : timeOnCurve;
	(void)clamped;
}

/* The reference step: the original FarTrafficUpdateCar's simulation half, with
 * the pick and the pose above. */
static void
RefUpdateCar(SFarSimCar *pCar, uint32 now, SFarSimPose *pPose)
{
	pCar->bUTurn = 0;
	if ((uint32)(pCar->timeEnteredCurve + pCar->timeToSpendOnCurrentCurve) <= now)
		RefPickNextNode(pCar);
	float x, y, dirX, dirY;
	RefPose(pCar, now, &x, &y, &dirX, &dirY);
	float between = (float)(now - (uint32)pCar->timeEnteredCurve) / pCar->timeToSpendOnCurrentCurve;
	float z = g_world.nodes[pCar->currentRouteNode].z * (1.0f - between) + g_world.nodes[pCar->nextRouteNode].z * between;
	z += pCar->heightAboveRoad;
	float forwardX = dirX;
	float forwardY = dirY;
	forwardX = dirX; forwardY = dirY;
	float sq = forwardX*forwardX + forwardY*forwardY;
	if (sq > 0.0001f) {
		float invsqrt = 1.0f / sqrtf(sq);
		forwardX *= invsqrt; forwardY *= invsqrt;
	} else {
		forwardX = pCar->forwardX; forwardY = pCar->forwardY;
	}
	pCar->forwardX = forwardX;
	pCar->forwardY = forwardY;
	pPose->slot = pCar->slot;
	pPose->bUTurn = pCar->bUTurn;
	pPose->x = x; pPose->y = y; pPose->z = z;
	pPose->dirX = dirX; pPose->dirY = dirY;
	pPose->forwardX = forwardX; pPose->forwardY = forwardY;
}

/* ---------------------------------------------------------------------- *
 * the checks
 * ---------------------------------------------------------------------- */

static int g_failures;
static int g_checks;

static void
Check(bool ok, const char *what)
{
	g_checks++;
	if (!ok) {
		g_failures++;
		printf("  FAIL: %s\n", what);
	}
}

static uint64
NowUs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* The process's own CPU time, every thread of it included: the check that the
 * simulation the worker takes off the frame thread is run somewhere, not that
 * it disappears. (The worker is a thread of the same process.) */
/* A derived number that cannot be negative (the differences below are smaller
 * than the baseline's own run-to-run noise when the machine is busy): reported
 * as zero, and said so. */
static double
NonNeg(double x)
{
	return x > 0.0 ? x : 0.0;
}

static uint64
ProcessCpuUs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return (uint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* The route state both implementations write (everything the pick touches).
 * bUTurn, the heading and the car's own stream are separate: the pose and a
 * draw can move them without a route change. */
static bool
SameRouteState(const SFarSimCar &a, const SFarSimCar &b)
{
	return a.currentRouteNode == b.currentRouteNode &&
		a.nextRouteNode == b.nextRouteNode &&
		a.prevRouteNode == b.prevRouteNode &&
		a.nextPathNodeInfo == b.nextPathNodeInfo &&
		a.currentPathNodeInfo == b.currentPathNodeInfo &&
		a.previousPathNodeInfo == b.previousPathNodeInfo &&
		a.currentDirection == b.currentDirection &&
		a.nextDirection == b.nextDirection &&
		a.previousDirection == b.previousDirection &&
		a.currentLane == b.currentLane &&
		a.nextLane == b.nextLane &&
		a.timeEnteredCurve == b.timeEnteredCurve &&
		a.timeToSpendOnCurrentCurve == b.timeToSpendOnCurrentCurve;
}

/* True when `link` is one of `node`'s own connections. The fix promises this
 * for every stored link; the original reads one past them instead. */
static bool
IsConnectionLinkOf(int32 node, int32 link)
{
	int32 first = g_world.nodes[node].firstLink;
	int32 total = g_world.nodes[node].numLinks;

	for (int32 i = 0; i < total; i++)
		if (g_world.connectionLink[first + i] == link)
			return true;
	return false;
}

/* The fix's promises for the cases the original gets wrong: the link the pick
 * stored is one of the node's own connections and the u-turn flag agrees with
 * the route; a node with no connections keeps the route and the link it had
 * (and flags the turn). */
static bool
FixedRouteClaims(const SFarSimCar &car, const SFarSimCar &before, uint32 flags)
{
	if (flags & REF_BROKEN_PICK_NO_LINKS)
		return SameRouteState(car, before) && car.bUTurn == 1;
	if (!IsConnectionLinkOf(car.currentRouteNode, car.nextPathNodeInfo))
		return false;
	return car.bUTurn == (car.nextRouteNode == car.prevRouteNode);
}

/* The crash's own claim: a pose outside the world's sector grid walks a
 * negative sector index in CPhysical::RemoveAndAdd, and a NaN pose is the
 * crash's other form. Every pose must be finite, its position inside the
 * grid. */
static bool
PoseIsFinite(const SFarSimPose &pose)
{
	return isfinite(pose.x) && isfinite(pose.y) && isfinite(pose.z) &&
		isfinite(pose.dirX) && isfinite(pose.dirY) &&
		isfinite(pose.forwardX) && isfinite(pose.forwardY);
}

static bool
PoseIsInWorld(const SFarSimPose &pose)
{
	return pose.x >= WORLD_MIN_X && pose.x < WORLD_MAX_X &&
		pose.y >= WORLD_MIN_Y && pose.y < WORLD_MAX_Y;
}

static bool
PoseIsFiniteAndInWorld(const SFarSimPose &pose)
{
	return PoseIsFinite(pose) && PoseIsInWorld(pose);
}

/* The same claim, one Check per half so a failure says which one broke. */
static void
CheckPoseClaim(const SFarSimPose *pPose, const char *what)
{
	char text[160];

	snprintf(text, sizeof(text), "%s: every value finite", what);
	Check(PoseIsFinite(*pPose), text);
	snprintf(text, sizeof(text), "%s: the position inside the world's sector grid", what);
	Check(PoseIsInWorld(*pPose), text);
}

static void
PrintCase(int i, const SFarSimCar &sim, const SFarSimCar &ref, const SFarSimPose &simPose, const SFarSimPose &refPose)
{
	printf("  case %d:\n", i);
	printf("    sim route %d/%d/%d links %d/%d/%d dirs %d/%d/%d lanes %d/%d t %d/%d u%d\n",
		sim.currentRouteNode, sim.nextRouteNode, sim.prevRouteNode,
		sim.currentPathNodeInfo, sim.nextPathNodeInfo, sim.previousPathNodeInfo,
		sim.currentDirection, sim.nextDirection, sim.previousDirection,
		sim.currentLane, sim.nextLane, sim.timeEnteredCurve, sim.timeToSpendOnCurrentCurve, sim.bUTurn);
	printf("    ref route %d/%d/%d links %d/%d/%d dirs %d/%d/%d lanes %d/%d t %d/%d u%d\n",
		ref.currentRouteNode, ref.nextRouteNode, ref.prevRouteNode,
		ref.currentPathNodeInfo, ref.nextPathNodeInfo, ref.previousPathNodeInfo,
		ref.currentDirection, ref.nextDirection, ref.previousDirection,
		ref.currentLane, ref.nextLane, ref.timeEnteredCurve, ref.timeToSpendOnCurrentCurve, ref.bUTurn);
	printf("    sim pose %.6f %.6f %.6f | ref %.6f %.6f %.6f\n",
		simPose.x, simPose.y, simPose.z, refPose.x, refPose.y, refPose.z);
	printf("    sim dir %.6f %.6f fwd %.6f %.6f | ref dir %.6f %.6f fwd %.6f %.6f\n",
		simPose.dirX, simPose.dirY, simPose.forwardX, simPose.forwardY,
		refPose.dirX, refPose.dirY, refPose.forwardX, refPose.forwardY);
}

/*
 * B. the moved arithmetic against the original code, bit for bit - wherever the
 * original is correct. The reference carries the original's own broken paths
 * as flags (the 1000 sentinel, the give-up's index one past the connections, a
 * node with no connections, the flat curve, a curve time under the pick's
 * floor, a progress outside the span). Where one fires, equality is not the
 * claim - the original's value there is a sentinel index, an out-of-range read
 * or a division by zero - so the case is checked against what the fix promises
 * instead. The counts printed below say exactly which cases are compared and
 * which are held to the fix, and why.
 */
static void
CheckEquivalence(void)
{
	int cases = 20000;
	int mismatches = 0, compared = 0, poseCompared = 0, uTurns = 0, clamped = 0, picks = 0;
	int fixedRouteBad = 0, fixedPoseBad = 0;
	int sentinel = 0, giveUp = 0, noLinks = 0, flat = 0, oneZero = 0, shortTime = 0, progress = 0;

	for (int i = 0; i < cases; i++) {
		SFarSimCar sim, before, ref;
		SFarSimPose simPose, refPose;
		uint32 now = 2000 + (uint32)(i * 37 % 8000);

		MakeCar(&sim, (uint16)i, (uint32)i, i);
		/* a quarter of the cases has the curve spent (the pick runs), and a
		 * tenth is fast enough for the clamped curve */
		if (i % 10 == 0)
			sim.maxTrafficSpeed = 5000.0f;	/* the curve time clamps to 10 ms */
		if (i % 4 != 0)
			sim.timeEnteredCurve = now - (int32)(i % 500);
		else
			sim.timeEnteredCurve = now - sim.timeToSpendOnCurrentCurve - 1;

		/* The original's own broken states, one slice each, so the classes
		 * below are exact (a case is never two slices at once). */
		if (i % 71 == 0) {
			/* the dead end: the only connection is the two-way link back,
			 * so the third round accepts it and stores the car's own link */
			sim.currentRouteNode = DEADEND_PREV_NODE;
			sim.nextRouteNode = DEADEND_NODE;
			sim.prevRouteNode = DEADEND_PREV_NODE;
			sim.nextPathNodeInfo = DEADEND_LINK;
			sim.timeEnteredCurve = now - sim.timeToSpendOnCurrentCurve - 1;
		} else if (i % 73 == 0) {
			/* the give-up: every connection is one-way against the car */
			sim.currentRouteNode = GIVEUP_PREV_NODE;
			sim.nextRouteNode = GIVEUP_NODE;
			sim.prevRouteNode = GIVEUP_PREV_NODE;
			sim.nextPathNodeInfo = GIVEUP_BACK_LINK;
			sim.timeEnteredCurve = now - sim.timeToSpendOnCurrentCurve - 1;
		} else if (i % 97 == 0) {
			/* the dead-end store the pick itself can make: the same link,
			 * lane and direction for current and next (the two lane points
			 * coincide: the flat curve) */
			sim.nextPathNodeInfo = sim.currentPathNodeInfo;
			sim.nextLane = sim.currentLane;
			sim.nextDirection = sim.currentDirection;
			sim.timeEnteredCurve = now - (int32)(i % 500);
		} else if (i % 53 == 0) {
			/* a curve time of 0, with the clock one millisecond past it (the
			 * pick must not run: its own floor never stores 0) */
			sim.timeToSpendOnCurrentCurve = 0;
			sim.timeEnteredCurve = (int32)now + 1;
		} else if (i % 59 == 0) {
			/* the clock ahead of a live curve: a progress outside the span */
			sim.timeToSpendOnCurrentCurve = 1000;
			sim.timeEnteredCurve = (int32)now + 500;
		} else if (i % 61 == 0) {
			/* a node with no connections at all: the pick must run */
			sim.nextRouteNode = ZERO_LINK_NODE;
			sim.timeEnteredCurve = now - sim.timeToSpendOnCurrentCurve - 1;
		}

		before = sim;
		ref = sim;
		ref.rng = FarTrafficSimRngSeed(ref.slot, ref.randomSeed, ref.routeSeed);
		sim.rng = ref.rng;
		bool pickDue = (uint32)(sim.timeEnteredCurve + sim.timeToSpendOnCurrentCurve) <= now;

		g_refBroken = 0;
		memset(&simPose, 0, sizeof(simPose));
		memset(&refPose, 0, sizeof(refPose));
		FarTrafficSimUpdateCar(&g_world, &sim, now, &simPose);
		RefUpdateCar(&ref, now, &refPose);
		uint32 flags = g_refBroken;

		bool pickBroken = (flags & (REF_BROKEN_PICK_SENTINEL | REF_BROKEN_PICK_GIVEUP | REF_BROKEN_PICK_NO_LINKS)) != 0;
		bool curveBroken = (flags & (REF_BROKEN_CURVE_FLAT | REF_BROKEN_CURVE_ONE_ZERO | REF_BROKEN_CURVE_TIME | REF_BROKEN_CURVE_PROGRESS)) != 0;

		if (flags & REF_BROKEN_PICK_SENTINEL) sentinel++;
		if (flags & REF_BROKEN_PICK_GIVEUP) giveUp++;
		if (flags & REF_BROKEN_PICK_NO_LINKS) noLinks++;
		if (flags & REF_BROKEN_CURVE_FLAT) flat++;
		if (flags & REF_BROKEN_CURVE_ONE_ZERO) oneZero++;
		if (flags & REF_BROKEN_CURVE_TIME) shortTime++;
		if (flags & REF_BROKEN_CURVE_PROGRESS) progress++;

		if (!pickBroken) {
			/* the original's own correct path: the route and the u-turn
			 * must agree bit for bit */
			bool sameRoute = SameRouteState(sim, ref) && sim.bUTurn == ref.bUTurn;
			if (!sameRoute && mismatches < 3)
				PrintCase(i, sim, ref, simPose, refPose);
			if (!sameRoute)
				mismatches++;
			compared++;
		} else if (!FixedRouteClaims(sim, before, flags)) {
			if (fixedRouteBad < 3)
				printf("  case %d: the fix's route claims fail (flags %x)\n", i, flags);
			fixedRouteBad++;
		}

		if (!pickBroken && !curveBroken) {
			bool samePose =
				simPose.x == refPose.x && simPose.y == refPose.y && simPose.z == refPose.z &&
				simPose.dirX == refPose.dirX && simPose.dirY == refPose.dirY &&
				simPose.forwardX == refPose.forwardX && simPose.forwardY == refPose.forwardY &&
				simPose.bUTurn == refPose.bUTurn;
			if (!samePose && mismatches < 3)
				PrintCase(i, sim, ref, simPose, refPose);
			if (!samePose)
				mismatches++;
			poseCompared++;
		} else if (!PoseIsFiniteAndInWorld(simPose)) {
			if (fixedPoseBad < 3)
				printf("  case %d: the fix's pose is not finite/in-world (flags %x) %.3f %.3f %.3f\n",
					i, flags, simPose.x, simPose.y, simPose.z);
			fixedPoseBad++;
		}

		if (sim.bUTurn)
			uTurns++;
		if (simPose.bShortCurve)
			clamped++;
		if (pickDue)
			picks++;
	}

	Check(mismatches == 0, "the real step and the original code agree on every case the original is correct on, bit for bit");
	Check(fixedRouteBad == 0, "every case on the original's broken paths stores one of the node's own connections (or keeps the route) and agrees on the u-turn");
	Check(fixedPoseBad == 0, "every case on the original's broken paths has a finite pose inside the world");
	printf("  %d cases (%d with the path pick, %d u-turns, %d clamped curves):\n", cases, picks, uTurns, clamped);
	printf("    %d ran on a path the original gets right: the route state agrees bit for bit for\n", compared);
	printf("    all of them, and the pose for the %d of those whose curve is also on a correct\n", poseCompared);
	printf("    path.\n");
	printf("    The other %d took a path the original gets wrong, where its value is a sentinel\n", cases - compared);
	printf("    index, an out-of-range read or a division by zero, so equality is not the claim\n");
	printf("    there; they are held to the fix instead (the stored link is one of the node's\n");
	printf("    own connections, the u-turn agrees with the route, the pose is finite and\n");
	printf("    inside the world). The broken paths the sweep saw:\n");
	printf("      %d the third round's 1000 sentinel (the shipped read indexes first + 1001),\n", sentinel);
	printf("      %d the give-up exit (the shipped read indexes first + totalLinks),\n", giveUp);
	printf("      %d a node with no connections (Random() %% 0: the reference cannot run it),\n", noLinks);
	printf("      %d the flat curve (t1 == t2 == 0: the shipped pose is NaN),\n", flat);
	printf("      %d a blend against a zero projection, %d a curve time under the pick's floor,\n", oneZero, shortTime);
	printf("      %d a progress outside the curve's span; a case can carry more than one class\n", progress);
	Check(picks > 0 && uTurns > 0, "the sweep exercised the pick and the u-turn");
	Check(clamped > 0, "the sweep exercised the clamped curve");
	Check(sentinel > 0 && giveUp > 0 && noLinks > 0 && flat > 0 && progress > 0,
		"the sweep exercised every broken path the fix closes");
}

/*
 * The deterministic states of the pick's and the curve's guards, driven
 * straight through the real simulation (no reference): the dead end that turns
 * around on the link it arrived on, the third round's acceptance, the give-up's
 * way back, a node with no connections, a curve time of 0 and a progress the
 * curve does not have. These are the states the crash came from; every pose
 * must be finite and inside the world, and the pick's route claims must hold.
 */
static void
InitCaseCar(SFarSimCar *pCar, uint16 slot, int32 currentRouteNode, int32 nextRouteNode)
{
	memset(pCar, 0, sizeof(*pCar));
	pCar->slot = slot;
	pCar->currentRouteNode = currentRouteNode;
	pCar->nextRouteNode = nextRouteNode;
	pCar->prevRouteNode = currentRouteNode;
	pCar->currentDirection = 1;
	pCar->nextDirection = 1;
	pCar->previousDirection = 1;
	pCar->currentLane = 0;
	pCar->nextLane = 0;
	pCar->randomSeed = (uint16)(1000 + slot);
	pCar->routeSeed = (int16)(7 + slot);
	pCar->maxTrafficSpeed = 20.0f;
	pCar->heightAboveRoad = 0.75f;
	pCar->forwardX = 1.0f;
	pCar->forwardY = 0.0f;
	pCar->timeToSpendOnCurrentCurve = 1000;
	pCar->rng = FarTrafficSimRngSeed(pCar->slot, pCar->randomSeed, pCar->routeSeed);
}

static void
CheckPickAndCurveGuards(void)
{
	SFarSimCar car, before;
	SFarSimPose pose;
	uint32 now = 50000;

	/* (a) the dead end: its only connection is the two-way link the car
	 * arrived on, so the pick decides the u-turn and stores that link as the
	 * car's current and next rail. The two lane points coincide: the curve is
	 * the flat one, and its pose is the car's own lane point. */
	InitCaseCar(&car, 0, DEADEND_PREV_NODE, DEADEND_NODE);
	car.nextPathNodeInfo = DEADEND_LINK;
	car.currentPathNodeInfo = DEADEND_LINK;
	car.previousPathNodeInfo = DEADEND_LINK;
	car.timeEnteredCurve = (int32)now - car.timeToSpendOnCurrentCurve;
	FarTrafficSimUpdateCar(&g_world, &car, now, &pose);
	Check(car.nextRouteNode == car.prevRouteNode, "the dead end's pick turns the car around");
	Check(car.bUTurn == 1, "the dead end's u-turn is flagged for the applicator");
	Check(car.currentPathNodeInfo == DEADEND_LINK && car.nextPathNodeInfo == DEADEND_LINK,
		"the dead end's pick stores the car's own link as its current and next rail");
	CheckPoseClaim(&pose, "the dead end's pose");
	{
		const SFarSimLink &link = g_world.links[DEADEND_LINK];
		float laneX = link.x + ((car.currentLane + link.laneOffset) * LANE_WIDTH) * (link.dirY * car.currentDirection);
		float laneY = link.y - ((car.currentLane + link.laneOffset) * LANE_WIDTH) * (link.dirX * car.currentDirection);
		Check(pose.x == laneX && pose.y == laneY, "the dead end's pose is the car's own lane point");
	}

	/* (b) the third round: both connections fail the first two rounds (the
	 * first reaches a disabled node, the second is the way back, which those
	 * rounds skip), and the third round accepts the way back. */
	InitCaseCar(&car, 1, THIRD_PREV_NODE, THIRD_NODE);
	car.nextPathNodeInfo = DEADEND_LINK;
	car.currentPathNodeInfo = DEADEND_LINK;
	car.previousPathNodeInfo = DEADEND_LINK;
	car.timeEnteredCurve = (int32)now - car.timeToSpendOnCurrentCurve;
	FarTrafficSimUpdateCar(&g_world, &car, now, &pose);
	Check(car.nextRouteNode == car.prevRouteNode, "the third round's way back turns the car around");
	Check(car.bUTurn == 1, "the third round's u-turn is flagged for the applicator");
	Check(car.nextPathNodeInfo == THIRD_BACK_LINK, "the third round stores the connection it accepted");
	Check(IsConnectionLinkOf(THIRD_NODE, car.nextPathNodeInfo),
		"the third round's link is one of the node's own connections");
	CheckPoseClaim(&pose, "the third round's pose");

	/* (d) the give-up: every connection is one-way against the car, so the
	 * third round accepts nothing; the turn's link is the connection that
	 * leads back to the previous node. */
	InitCaseCar(&car, 2, GIVEUP_PREV_NODE, GIVEUP_NODE);
	car.nextPathNodeInfo = DEADEND_LINK;
	car.currentPathNodeInfo = DEADEND_LINK;
	car.previousPathNodeInfo = DEADEND_LINK;
	car.timeEnteredCurve = (int32)now - car.timeToSpendOnCurrentCurve;
	FarTrafficSimUpdateCar(&g_world, &car, now, &pose);
	Check(car.nextRouteNode == car.prevRouteNode, "the give-up turns the car around");
	Check(car.bUTurn == 1, "the give-up's u-turn is flagged for the applicator");
	Check(car.nextPathNodeInfo == GIVEUP_BACK_LINK, "the give-up's link is the connection back to the previous node");
	Check(IsConnectionLinkOf(GIVEUP_NODE, car.nextPathNodeInfo),
		"the give-up's link is one of the node's own connections");
	CheckPoseClaim(&pose, "the give-up's pose");

	/* (e) a node with no connections at all (the original divides by zero
	 * there): the pick keeps the route and the link and flags the turn. */
	InitCaseCar(&car, 3, DEADEND_PREV_NODE, ZERO_LINK_NODE);
	car.nextPathNodeInfo = DEADEND_LINK;
	car.currentPathNodeInfo = DEADEND_LINK;
	car.previousPathNodeInfo = DEADEND_LINK;
	car.timeEnteredCurve = (int32)now - car.timeToSpendOnCurrentCurve;
	before = car;
	FarTrafficSimUpdateCar(&g_world, &car, now, &pose);
	Check(SameRouteState(car, before), "the no-connection node keeps the car's route and its link");
	Check(car.bUTurn == 1, "the no-connection node flags the u-turn");
	CheckPoseClaim(&pose, "the no-connection pose");

	/* (c) a curve time of 0 (a placement's seed carries one: the pick's own
	 * floor never stores it) with the clock past it, and a clock ahead of a
	 * live curve: the progress has no scale at all, or is outside the span. */
	MakeCar(&car, 4, 11, 3);
	car.timeToSpendOnCurrentCurve = 0;
	car.timeEnteredCurve = (int32)now + 1;
	FarTrafficSimUpdateCar(&g_world, &car, now, &pose);
	CheckPoseClaim(&pose, "the zero curve time's pose");

	MakeCar(&car, 5, 13, 5);
	car.timeToSpendOnCurrentCurve = 1000;
	car.timeEnteredCurve = (int32)now + 500;
	FarTrafficSimUpdateCar(&g_world, &car, now, &pose);
	CheckPoseClaim(&pose, "the out-of-span progress's pose");

	printf("  the guards, on the crash's own states: the dead end's u-turn on the car's own\n");
	printf("  link (its pose the lane point), the third round's acceptance, the give-up's\n");
	printf("  way back, a node with no connections, a zero curve time and an out-of-span\n");
	printf("  progress: every pose finite and inside the world\n");
}

/* C. the boundary: the lockstep, the share, the allocation. */
static uint64 g_allocCount;
static bool g_countAllocs;

/* The global new, patched so the checked region can prove a cycle does not
 * allocate. (The thread and the conditions are created before the region; a
 * cycle itself must walk arrays only.) */
void *
operator new(size_t size)
{
	void *p = malloc(size);
	if (g_countAllocs)
		g_allocCount++;
	return p;
}

void *
operator new[](size_t size)
{
	void *p = malloc(size);
	if (g_countAllocs)
		g_allocCount++;
	return p;
}

void
operator delete(void *p) noexcept
{
	free(p);
}

void
operator delete[](void *p) noexcept
{
	free(p);
}

void
operator delete(void *p, size_t) noexcept
{
	free(p);
}

void
operator delete[](void *p, size_t) noexcept
{
	free(p);
}

static void CheckQueueBound(void);

/*
 * The applicator's list, as FarTraffic.cpp keeps it: an index-based array whose
 * drop puts the last entry in the dropped one's place, with a "dead" mark for a
 * car the world took (a cutscene's cleanup, the pool's valve, a delete). This is
 * a model of the applicator, not the applicator itself (FarTraffic.cpp includes
 * the engine and is not linked here); what it carries is the *order* of a frame,
 * which is what the worker's published poses rest on:
 *
 *	the cull		a hand-over appends one entry (moves no index)
 *	the far hook		the apply (the poses name an entry index and a slot)
 *				then the prune (the stale entries drop, swapping)
 *				then the gate pass and the creations
 *	the end of the frame	the cycle is handed over
 */
struct SFarSimAppList
{
	uint16 slots[FAR_TRAFFIC_MAX];
	bool dead[FAR_TRAFFIC_MAX];
	int32 num;
};

static void
AppAdd(SFarSimAppList *pList, uint16 slot)
{
	if (pList->num >= FAR_TRAFFIC_MAX)
		return;
	pList->slots[pList->num] = slot;
	pList->dead[pList->num] = false;
	pList->num++;
}

/* The engine's own drop (FarTrafficDropEntry): the last entry takes the place. */
static void
AppDropAt(SFarSimAppList *pList, int32 index)
{
	pList->slots[index] = pList->slots[pList->num - 1];
	pList->dead[index] = pList->dead[pList->num - 1];
	pList->num--;
}

/*
 * One run of the applicator's frame order, over real cycles of the module. The
 * poses of the previous cycle are checked the way FarTrafficApplyPose checks
 * them (the entry index must hold the pose's slot). `bPreFixOrder` replays the
 * order VICE-133's review found the bug in - the stale entries dropped *before*
 * the apply, so the drop's swap moves an entry under the poses - which the
 * checks below must then see as mismatches: that is what makes them a test of
 * the fix and not of the model.
 */
static int
RunApplicatorOrder(bool bPreFixOrder, int frames, int *pPosesChecked)
{
	static SFarSimAppList app;
	static uint32 nextSlot = 0;
	int mismatches = 0, posesChecked = 0, lost = 0, cycles = 0;
	uint32 now = 40000;
	uint32 appliedCycle = 0;	/* the applicator's own marker: a block is
					 * applied once (CFarTraffic::Process) */

	app.num = 0;
	CFarTrafficWorker::SetThreaded(false);
	CFarTrafficWorker::SetWorld(&g_world);
	CFarTrafficWorker::ResetCars();

	/* the previous cycle's output is published before the first apply */
	CFarTrafficWorker::BeginCycle(now, 0);
	CFarTrafficWorker::EndCycle();
	CFarTrafficWorker::Run();
	CFarTrafficWorker::Join();
	const SFarTrafficOutput *pOutput = CFarTrafficWorker::Published();

	for (int frame = 0; frame < frames; frame++) {
		uint32 r = frame * 2654435761u + 7;
		now += 33;
		bool preOrder = bPreFixOrder && (frame % 2 == 0);

		/* the cull, before the far hook: a hand-over appends one entry (an
		 * append moves no index, in either order) */
		if (frame % 9 == 0 && app.num < FAR_TRAFFIC_MAX) {
			SFarSimCar car;
			MakeCar(&car, (uint16)nextSlot++, r, frame);
			CFarTrafficWorker::AddCar(car);
			AppAdd(&app, car.slot);
		}

		/* the far hook: the frame's cycle is stamped and the events queue */
		CFarTrafficWorker::BeginCycle(now, (uint32)(frame + 1));

		/* The prune and the gate pass: two entries go, the engine's own
		 * swap-drop, one command each. The pre-fix order ran this part inside
		 * the cull, i.e. before the apply below; the fix runs it after. */
		if (bPreFixOrder && preOrder) {
			if (frame % 11 == 3 && app.num > 1) {
				int32 at = (r >> 8) % app.num;		/* the world took this car */
				CFarTrafficWorker::DropCar(app.slots[at]);
				AppDropAt(&app, at);
			}
			if (frame % 7 == 1 && app.num > 1) {
				int32 at = (r >> 16) % app.num;		/* the gate pass, a switch or a removal */
				CFarTrafficWorker::DropCar(app.slots[at]);
				AppDropAt(&app, at);
			}
		}

		/* the apply: the previous cycle's poses, against the list as it is
		 * exactly now (the order the fix keeps has nothing moved since the
		 * cycle). A block is applied once, by its cycle number, exactly as
		 * FarTrafficApplyOutput does: a frame whose cycle did nothing leaves
		 * the block in place and it is not read again. */
		if (pOutput->cycle != appliedCycle) {
			appliedCycle = pOutput->cycle;
			for (int32 i = 0; i < pOutput->numPoses; i++) {
				const SFarSimPose *pPose = &pOutput->poses[i];
				if (pPose->index >= (uint16)app.num || app.slots[pPose->index] != pPose->slot)
					mismatches++;
				posesChecked++;
			}
		}
		lost += pOutput->numLost;

		/* the same prune and gate pass, in the fix's place (and in the
		 * pre-fix run's odd frames, which is the half of a frame the bug hid
		 * in) */
		if (!bPreFixOrder || !preOrder) {
			if (frame % 11 == 3 && app.num > 1) {
				int32 at = (r >> 8) % app.num;
				CFarTrafficWorker::DropCar(app.slots[at]);
				AppDropAt(&app, at);
			}
			if (frame % 7 == 1 && app.num > 1) {
				int32 at = (r >> 16) % app.num;
				CFarTrafficWorker::DropCar(app.slots[at]);
				AppDropAt(&app, at);
			}
		}

		/* the creations queue their appends too */
		if (frame % 2 == 1 && app.num < FAR_TRAFFIC_MAX) {
			SFarSimCar car;
			MakeCar(&car, (uint16)nextSlot++, r, frame);
			CFarTrafficWorker::AddCar(car);
			AppAdd(&app, car.slot);
		}
		CFarTrafficWorker::EndCycle();

		/* the end of the frame: the cycle runs */
		if (CFarTrafficWorker::Run())
			cycles++;
		CFarTrafficWorker::Join();
		pOutput = CFarTrafficWorker::Published();
		if (pOutput->numCars != app.num) {
			/* the two lists disagree: that is the failure the checks below
			 * count, and the run stops here */
			mismatches += 1000;
			break;
		}
	}

	*pPosesChecked = posesChecked;
	Check(lost == 0, "no cycle in the order test lost a command");
	Check(cycles > 0, "the order test ran cycles");
	return mismatches;
}

static void
CheckCycleAndLockstep(void)
{
	uint32 hashBefore = HashWorld(&g_world);
	int frames = 600;
	int adds = 0, drops = 0, poseCount = 0, lost = 0, badShare = 0, badIndex = 0;
	uint32 now = 10000;
	static SFarSimAppList app;	/* the applicator's own list, in its order */

	app.num = 0;
	CFarTrafficWorker::SetThreaded(false);	/* the lockstep is the cycle's, not the thread's */
	CFarTrafficWorker::SetWorld(&g_world);
	CFarTrafficWorker::ResetCars();

	/* The commands the frames queue must not allocate either, so the counting
	 * starts before the first cycle. */
	g_allocCount = 0;
	g_countAllocs = true;

	for (int frame = 0; frame < frames; frame++) {
		uint32 r = frame * 2654435761u + 1;
		now += 33;

		CFarTrafficWorker::BeginCycle(now, (uint32)frame);
		/* a fifth of the frames adds a car (the engine's append), a seventh
		 * drops one (the engine's swap-drop by index) */
		if (frame % 5 == 0 && app.num < FAR_TRAFFIC_MAX) {
			SFarSimCar car;
			MakeCar(&car, (uint16)(1000 + frame), r, frame);
			CFarTrafficWorker::AddCar(car);
			AppAdd(&app, car.slot);
			adds++;
		}
		if (frame % 7 == 3 && app.num > 0) {
			int32 at = (r >> 8) % app.num;
			uint16 slot = app.slots[at];
			CFarTrafficWorker::DropCar(slot);
			AppDropAt(&app, at);
			drops++;
		}
		CFarTrafficWorker::EndCycle();
		bool ran = CFarTrafficWorker::Run();
		CFarTrafficWorker::Join();
		const SFarTrafficOutput *pOutput = CFarTrafficWorker::Published();

		if (ran) {
			Check(pOutput->numCars == app.num, "the cycle's count matches the applicator's");
			if (pOutput->numCars != app.num)
				break;
			for (int32 i = 0; i < pOutput->numPoses; i++) {
				const SFarSimPose *pPose = &pOutput->poses[i];
				if (pPose->index >= (uint16)app.num || app.slots[pPose->index] != pPose->slot)
					badIndex++;
				if (FarTrafficUpdatesThisFrame((uint32)frame, pPose->index) == false)
					badShare++;
				poseCount++;
			}
			lost += pOutput->numLost;
		}
	}

	g_countAllocs = false;
	Check(badIndex == 0, "every published pose named the applicator's car at its index (the lockstep)");
	Check(badShare == 0, "the cycle moved exactly the frame's share of the stagger");
	Check(lost == 0, "the cycle lost no command");
	Check(g_allocCount == 0, "no cycle allocated");
	Check(HashWorld(&g_world) == hashBefore, "the worker wrote nothing back into the snapshot");
	/* the last cycle's list is the applicator's list: walk it once more with
	 * one more frame that only steps */
	CFarTrafficWorker::BeginCycle(now + 33, (uint32)frames);
	CFarTrafficWorker::EndCycle();
	CFarTrafficWorker::Run();
	CFarTrafficWorker::Join();
	Check(CFarTrafficWorker::Published()->numCars == app.num,
		"the worker's list still matches the applicator's at the end");
	Check(!CFarTrafficWorker::TakeOverflow(), "the ring never overflowed in the lockstep run");
	printf("  %d frames, %d adds, %d drops, %d poses applied in lockstep, "
		"%d allocations in the cycles\n", frames, adds, drops, poseCount, (int)g_allocCount);

	/* The frame's own order, with a cull hand-over and a dead car: the poses
	 * must stay on their cars, and the harness must be able to see the order
	 * the fix removes (otherwise it proves nothing). */
	int posesChecked = 0, bugPosesChecked = 0;
	int mismatches = RunApplicatorOrder(false, 400, &posesChecked);
	Check(mismatches == 0, "the applicator's frame order (hand-over append, apply, prune) keeps every pose on its car");
	int bugMismatches = RunApplicatorOrder(true, 400, &bugPosesChecked);
	Check(bugMismatches > 0, "the harness sees the pre-fix order's mismatches (the order test can fail)");
	printf("  the order test (a cull hand-over and a dead car every frame): %d poses checked\n",
		posesChecked);
	printf("  with the fix, %d of them off their car; with the pre-fix order (the prune before\n",
		mismatches);
	printf("  the apply), %d of them off their car: the test sees the bug it guards\n",
		bugMismatches);

	/* The ring's bound: a mutation past it is reported, never silent. */
	CheckQueueBound();
}

/* The command ring at its bound: the module never says "dropped" by silence. */
static void
CheckQueueBound(void)
{
	int32 added = 0;

	CFarTrafficWorker::SetThreaded(false);
	CFarTrafficWorker::SetWorld(&g_world);
	CFarTrafficWorker::ResetCars();
	Check(CFarTrafficWorker::CommandsPending() == 0, "a reset layer holds no commands");
	Check(!CFarTrafficWorker::TakeOverflow(), "a reset layer reports no overflow");

	CFarTrafficWorker::BeginCycle(1000, 0);
	/* fill the layer: the ring is far wider than the cars the cycle holds */
	for (int i = 0; i < FAR_TRAFFIC_SIM_COMMANDS + 8; i++) {
		SFarSimCar car;
		MakeCar(&car, (uint16)i, (uint32)i, i);
		CFarTrafficWorker::AddCar(car);
		added++;
	}
	Check(CFarTrafficWorker::CommandsPending() == FAR_TRAFFIC_SIM_COMMANDS,
		"the ring holds exactly FAR_TRAFFIC_SIM_COMMANDS commands");
	Check(added == FAR_TRAFFIC_SIM_COMMANDS + 8, "the extra commands were offered");
	Check(CFarTrafficWorker::TakeOverflow(),
		"a command past the ring is reported, never silently dropped");
	Check(!CFarTrafficWorker::TakeOverflow(), "the report is taken once");
	/* the commands that fit are applied: the layer is its own cap, the refused
	 * cars are the ones past it (the cycle counts them) */
	CFarTrafficWorker::EndCycle();
	CFarTrafficWorker::Run();
	CFarTrafficWorker::Join();
	Check(CFarTrafficWorker::Published()->numCars == FAR_TRAFFIC_MAX,
		"the cars past the layer's own cap are the cycle's own refusal");
	printf("  the ring: %d commands held, %d offered, the overflow reported once\n",
		FAR_TRAFFIC_SIM_COMMANDS, added);
}

/* D. the measurement. */

static uint64
TimeCycleInline(int cap, int frames, uint32 *pPosTimes)
{
	static SFarSimCar cars[FAR_TRAFFIC_MAX];
	static SFarTrafficInput input;
	static SFarTrafficOutput output;
	int32 numCars = cap;
	uint32 now = 1000;

	for (int i = 0; i < cap; i++) {
		MakeCar(&cars[i], (uint16)i, (uint32)i * 7 + 1, i);
		cars[i].rng = FarTrafficSimRngSeed(cars[i].slot, cars[i].randomSeed, cars[i].routeSeed);
	}
	uint64 start = NowUs();
	uint64 cycles = 0;
	for (int f = 0; f < frames; f++) {
		now += 33;
		input.cycle = (uint32)f;
		input.now = now;
		input.frameCounter = (uint32)f;
		input.numCommands = 0;
		FarTrafficSimRunCycle(&g_world, cars, &numCars, &input, &output);
		cycles += output.numPoses;
		asm volatile("" : : "g"(&output) : "memory");
	}
	uint64 end = NowUs();
	if (pPosTimes)
		*pPosTimes = (uint32)cycles;
	return end - start;
}

static uint64
TimeUpdateNoPick(int cars, int iterations, uint32 *pMoved)
{
	static SFarSimCar a[FAR_TRAFFIC_MAX];
	static SFarSimPose pose;
	uint32 now = 10000;
	uint32 moved = 0;

	for (int i = 0; i < cars; i++) {
		MakeCar(&a[i], (uint16)i, (uint32)i * 13 + 3, i);
		/* the curve never spends: the pose path only */
		a[i].timeToSpendOnCurrentCurve = 100000;
		a[i].timeEnteredCurve = 0;
	}
	uint64 start = NowUs();
	for (int it = 0; it < iterations; it++) {
		now += 33;
		for (int i = 0; i < cars; i++) {
			FarTrafficSimUpdateCar(&g_world, &a[i], now, &pose);
			asm volatile("" : : "g"(&pose) : "memory");
		}
		moved += cars;
	}
	uint64 end = NowUs();
	if (pMoved)
		*pMoved = moved;
	return end - start;
}

static uint64
TimeUpdateWithPick(int cars, int iterations, uint32 *pMoved)
{
	static SFarSimCar a[FAR_TRAFFIC_MAX];
	static SFarSimPose pose;
	uint32 now = 10000;
	uint32 moved = 0;

	for (int i = 0; i < cars; i++) {
		MakeCar(&a[i], (uint16)i, (uint32)i * 17 + 5, i);
		a[i].timeEnteredCurve = 0;
		a[i].timeToSpendOnCurrentCurve = 1000;
	}
	uint64 start = NowUs();
	for (int it = 0; it < iterations; it++) {
		now += 33;
		for (int i = 0; i < cars; i++) {
			/* the curve is always spent: the pick runs on every step */
			a[i].timeEnteredCurve = now - a[i].timeToSpendOnCurrentCurve - 1;
			FarTrafficSimUpdateCar(&g_world, &a[i], now, &pose);
			asm volatile("" : : "g"(&pose) : "memory");
		}
		moved += cars;
	}
	uint64 end = NowUs();
	if (pMoved)
		*pMoved = moved;
	return end - start;
}

/* The real handshake: the module's own API, a real thread, the real cycle. The
 * schedule is the game's - the cycle handed over at the end of frame N, the
 * whole of frame N+1's other work, the join at the top of frame N+1 - so the
 * worker has the frame's own time to finish and the frame thread pays the
 * hand-over, not a round trip. The frame's other work is a busy loop, timed on
 * its own so the report can subtract it. Every number below is the best of
 * several runs (microbenchmarks on a shared machine only get smaller). */
static float g_busySink;

static void
BusyWork(int iterations)
{
	float x = g_busySink;
	for (int i = 0; i < iterations; i++)
		x = x * 1.0000001f + 0.5f;
	asm volatile("" : : "g"(x) : "memory");
	g_busySink = x;
}

static double
TimeBusy(int iterations, int frames)
{
	double best = 1e30;
	for (int run = 0; run < 3; run++) {
		uint64 start = NowUs();
		for (int f = 0; f < frames; f++)
			BusyWork(iterations);
		double us = (double)(NowUs() - start) / frames;
		if (us < best)
			best = us;
	}
	return best;
}

/* One run of the schedule; returns the frame thread's wall time a frame. */
static double
TimeHandshake(bool threaded, int cap, int frames, int busy, uint32 *pCycles,
	double *pRunUs, double *pJoinUs, double *pCpuUs = 0)
{
	uint32 now = 1000;
	uint32 cycles = 0;
	uint64 runUs = 0, joinUs = 0;

	CFarTrafficWorker::SetThreaded(threaded);
	CFarTrafficWorker::SetWorld(&g_world);
	CFarTrafficWorker::ResetCars();
	CFarTrafficWorker::Start();
	for (int i = 0; i < cap; i++) {
		SFarSimCar car;
		MakeCar(&car, (uint16)i, (uint32)i * 19 + 7, i);
		CFarTrafficWorker::AddCar(car);
	}
	/* the initial fill rides the first cycle */
	CFarTrafficWorker::BeginCycle(now, 0);
	CFarTrafficWorker::EndCycle();
	CFarTrafficWorker::Run();
	CFarTrafficWorker::Join();

	uint64 start = NowUs();
	uint64 cpuStart = ProcessCpuUs();
	for (int f = 0; f < frames; f++) {
		now += 33;
		CFarTrafficWorker::BeginCycle(now, (uint32)(f + 1));
		CFarTrafficWorker::EndCycle();
		uint64 t0 = NowUs();
		if (CFarTrafficWorker::Run())
			cycles++;		/* the end of the frame */
		uint64 t1 = NowUs();
		runUs += t1 - t0;
		BusyWork(busy);			/* the frame's other work */
		t0 = NowUs();
		CFarTrafficWorker::Join();	/* the top of the next frame */
		t1 = NowUs();
		joinUs += t1 - t0;
	}
	double total = (double)(NowUs() - start) / frames;
	double cpuUs = (double)(ProcessCpuUs() - cpuStart) / frames;
	CFarTrafficWorker::Join();	/* the cycle of the last frame */
	CFarTrafficWorker::Stop();
	if (pCycles)
		*pCycles = cycles;
	if (pRunUs)
		*pRunUs = (double)runUs / frames;
	if (pJoinUs)
		*pJoinUs = (double)joinUs / frames;
	if (pCpuUs)
		*pCpuUs = cpuUs;
	return total;
}

/* The hand-over's own cost with the worker parked, which is the game's case:
 * between two signals a whole frame passes (tens of milliseconds), so the
 * worker has long finished and sleeps on its condition variable when the next
 * signal arrives. A long gap makes sure of it here. */
static double
TimeHandoverParked(int cap, int frames, int busyLong, double *pJoinUs)
{
	uint32 now = 1000;
	uint64 runUs = 0, joinUs = 0;

	CFarTrafficWorker::SetThreaded(true);
	CFarTrafficWorker::SetWorld(&g_world);
	CFarTrafficWorker::ResetCars();
	CFarTrafficWorker::Start();
	for (int i = 0; i < cap; i++) {
		SFarSimCar car;
		MakeCar(&car, (uint16)i, (uint32)i * 23 + 11, i);
		CFarTrafficWorker::AddCar(car);
	}
	CFarTrafficWorker::BeginCycle(now, 0);
	CFarTrafficWorker::EndCycle();
	CFarTrafficWorker::Run();
	CFarTrafficWorker::Join();

	for (int f = 0; f < frames; f++) {
		now += 33;
		CFarTrafficWorker::BeginCycle(now, (uint32)(f + 1));
		CFarTrafficWorker::EndCycle();
		uint64 t0 = NowUs();
		CFarTrafficWorker::Run();
		uint64 t1 = NowUs();
		runUs += t1 - t0;
		BusyWork(busyLong);		/* a frame's worth of gap */
		t0 = NowUs();
		CFarTrafficWorker::Join();
		t1 = NowUs();
		joinUs += t1 - t0;
	}
	CFarTrafficWorker::Join();
	CFarTrafficWorker::Stop();
	*pJoinUs = (double)joinUs / frames;
	return (double)runUs / frames;
}

/* The best of three runs of the schedule, with the busy work subtracted. */
static double
BestHandshake(bool threaded, int cap, int frames, int busy, double busyPerFrame,
	uint32 *pCycles, double *pRunUs, double *pJoinUs, double *pCpuUs = 0)
{
	double best = 1e30, bestRun = 0, bestJoin = 0, bestCpu = 0;
	uint32 bestCycles = 0;
	for (int run = 0; run < 3; run++) {
		uint32 cycles;
		double runUs, joinUs, cpuUs;
		double us = TimeHandshake(threaded, cap, frames, busy, &cycles, &runUs, &joinUs, &cpuUs);
		if (us < best) {
			best = us;
			bestRun = runUs;
			bestJoin = joinUs;
			bestCpu = cpuUs;
			bestCycles = cycles;
		}
	}
	if (pCycles)
		*pCycles = bestCycles;
	if (pRunUs)
		*pRunUs = bestRun;
	if (pJoinUs)
		*pJoinUs = bestJoin;
	if (pCpuUs)
		*pCpuUs = bestCpu;
	return best - busyPerFrame;
}

static double
BestInlineCycle(int cap, int frames, uint32 *pPoses)
{
	double best = 1e30;
	uint32 bestPoses = 0;
	for (int run = 0; run < 3; run++) {
		uint32 poses = 0;
		uint64 us = TimeCycleInline(cap, frames, &poses);
		double perFrame = (double)us / frames;
		if (perFrame < best) {
			best = perFrame;
			bestPoses = poses;
		}
	}
	if (pPoses)
		*pPoses = bestPoses;
	return best;
}

static void
Measure(void)
{
	uint32 moved, poses, cycles;
	double runUs, joinUs, cpuUs = 0;

	/* warm the caches the same way for every number below */
	TimeCycleInline(FAR_TRAFFIC_MAX, 200, &moved);

	uint64 usPose = TimeUpdateNoPick(FAR_TRAFFIC_MAX, 2000, &moved);
	uint64 usPick = TimeUpdateWithPick(FAR_TRAFFIC_MAX, 2000, &moved);
	printf("  one car's update, the pose only:            %.1f ns\n",
		(double)usPose * 1000.0 / ((double)FAR_TRAFFIC_MAX * 2000.0));
	printf("  one car's update, with the path pick:       %.1f ns\n",
		(double)usPick * 1000.0 / ((double)FAR_TRAFFIC_MAX * 2000.0));
	printf("  (a car's curve spends about once a second, so the pick runs for a few\n");
	printf("   cars a frame: the pose path is the bulk of the simulation)\n\n");

	/* the frame's other work, the stand-in for CWorld::Process and the render
	 * between two hand-overs: about 0.3 ms a frame (a fast frame's world) */
	int busy = 4000;
	while (TimeBusy(busy, 500) < 300.0)
		busy *= 2;
	double busyPerFrame = TimeBusy(busy, 2000);
	printf("  the cycle of one frame, the real code, at a range of caps (the frame\n");
	printf("  thread's own time; the same cycle, run inline and handed to the worker):\n");
	printf("  (the frame's other work in the schedule below: %.0f us a frame)\n", busyPerFrame);

	int caps[] = { FAR_TRAFFIC_MAX / 4, FAR_TRAFFIC_MAX / 2, FAR_TRAFFIC_MAX };
	for (int i = 0; i < 3; i++) {
		int cap = caps[i];
		int frames = 1200;
		double inlinePerFrame = BestInlineCycle(cap, frames, &poses);
		double workerPerFrame = BestHandshake(true, cap, frames, busy, busyPerFrame,
			&cycles, &runUs, &joinUs, &cpuUs);
		printf("    cap %3d | inline %6.2f us a frame (%5.1f ns a pose, %.1f poses)"
			" | worker %6.2f us a frame\n",
			cap, inlinePerFrame, inlinePerFrame * 1000.0 / ((double)poses / frames),
			(double)poses / frames, workerPerFrame);
		printf("           | of the worker's number: the hand-over %.2f us, the join %.2f us; the\n",
			runUs, joinUs);
		printf("           | process's own CPU %.2f us a frame, %.2f of it outside the frame's\n",
			cpuUs, NonNeg(cpuUs - busyPerFrame));
		printf("           | other work: the layer's own work, on the worker thread or inline (the\n");
		printf("           | simulation moved threads, it did not disappear)\n");
	}
	printf("\n");

	/* The two ends of the split, at the cap: what the simulation costs the
	 * frame thread (run inline) and what the boundary's handshake costs it
	 * (the hand-over with the worker parked, the game's own case, and the
	 * join). The crossover: the cap at which the simulation the worker takes
	 * off the frame thread passes the handshake. */
	double inlineCap = BestInlineCycle(FAR_TRAFFIC_MAX, 3000, &poses);
	double posesPerFrame = (double)poses / 3000.0;
	double simPerCar = inlineCap / FAR_TRAFFIC_MAX;
	double joinParked = 0;
	double runParked = TimeHandoverParked(FAR_TRAFFIC_MAX, 300, busy * 8, &joinParked);
	double handshake = runParked + joinParked;
	double crossover = handshake / simPerCar;
	double idleThreaded = BestHandshake(true, 0, 3000, busy, busyPerFrame, &cycles, &runUs, &joinUs);
	double idleInline = BestHandshake(false, 0, 3000, busy, busyPerFrame, &cycles, &runUs, &joinUs);

	printf("  the split at the cap (%d cars, %.1f poses a frame):\n", FAR_TRAFFIC_MAX, posesPerFrame);
	printf("    the simulation:            %.2f us a frame (%.1f ns a pose) on the frame thread,\n"
		"                               the worker's when the boundary is on\n", inlineCap, inlineCap * 1000.0 / posesPerFrame);
	printf("    the boundary's handshake:  %.2f us a frame (the compiled cycle handed over and\n",
		handshake);
	printf("                               joined; measured with the worker parked, the game's\n");
	printf("                               own case of a whole frame between two signals)\n");
	printf("    the frame thread's total:  %.2f us with the worker, %.2f us inline\n",
		handshake, inlineCap);
	if (handshake < inlineCap)
		printf("    the boundary wins at the cap by %.2f us a frame; it wins from about\n"
			"    %.0f cars up (the crossover), and the layer's cap is %d\n",
			inlineCap - handshake, crossover, FAR_TRAFFIC_MAX);
	else
		printf("    the boundary does not win at the cap (%.2f us more a frame): the ticket's\n"
			"    escape hatch applies, and the boundary is the deliverable either way; the\n"
			"    crossover cap is about %.0f cars\n", handshake - inlineCap, crossover);
	printf("  a frame with no layer work: the schedule's own noise floor (within %.2f / %.2f us,\n",
		fabs(idleThreaded), fabs(idleInline));
	printf("  subtraction's noise): the worker is not woken when the layer would do nothing\n");
	printf("  (the stock draw distance's case), and the cycle is not even offered to it\n");


}

int
main(void)
{
	printf("== the far traffic layer's worker (VICE-133)\n\n");

	BuildWorld();
	BuildCaseShapes();
	printf("== 1. the snapshot\n");
	printf("  the synthetic graph: %d car nodes, %d connections, %d links, a %dx%d grid\n",
		g_world.numNodes, g_world.numConnections, g_world.numLinks, GRID, GRID);
	printf("  the snapshot hashes to %08x before and after the cycles below\n", HashWorld(&g_world));

	printf("\n== 2. the moved arithmetic against the original code\n");
	CheckEquivalence();
	CheckPickAndCurveGuards();

	printf("\n== 3. the boundary: the lockstep, the share, the allocations\n");
	CheckCycleAndLockstep();

	printf("\n== 4. the split of the layer's cost\n");
	Measure();

	printf("\nRESULT: %s (%d checks, %d failures)\n",
		g_failures == 0 ? "every check passed" : "FAILURES", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
