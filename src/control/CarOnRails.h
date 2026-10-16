#ifndef __GTA_CARONRAILS_H__
#define __GTA_CARONRAILS_H__

/*
 * The car-on-rails arithmetic (VICE-133).
 *
 * The near traffic's on-rails car (CCarCtrl::UpdateCarOnRails, the "simple"
 * physics most of the spawner's cars get) and the far traffic layer (VICE-128,
 * whose simulation VICE-133 moved onto a worker thread) drive the same way:
 * the lane offsets of the two path links, the curve between them, the heading,
 * and the pick of the next node when the curve is spent. That arithmetic used
 * to live in CCarCtrl alone. The worker cannot call CCarCtrl - it must not
 * touch the world, the pools, the path graph or the shared random stream - so
 * the rules live here once and both sides instantiate them:
 *
 *	the near side	CCarCtrl::ComputeCarOnRailPose and
 *			CCarCtrl::PickNextNodeRandomly read the live car and
 *			the live path graph through the engine views
 *			(CCarCtrl.cpp);
 *	the far side	the worker's simulation (FarTrafficWorker.h/.cpp)
 *			reads its own per-car state and the FarTrafficWorld
 *			snapshot through the sim views.
 *
 * The two sides are told apart by a pair of small *views* (the template
 * parameters G and C): a graph view (the nodes, the links, the connections) and
 * a car view (the route state of one car, its random draw, and the two
 * decisions that leave the arithmetic: the u-turn and the short curve). Each
 * side defines its own two views next to its data. The rules below are the only
 * copy, so the near traffic and the far traffic cannot drift apart.
 *
 * The view interface - what this header calls:
 *
 *	G::NodeX(node), NodeY(node)			metres (the engine stores 8ths)
 *	G::NumLinks(node)
 *	G::NodeFirstLink(node)
 *	G::NodeDeadEnd(node), NodeDisabled(node), NodeBetweenLevels(node)
 *	G::ConnectedNode(connection)			the node a connection reaches
 *	G::LinkOfConnection(connection)			the car link of a connection
 *	G::LinkPathNode(link)				the node a link belongs to
 *	G::LinkX(link), LinkY(link)
 *	G::LinkDirX(link), LinkDirY(link)		the packed direction / 100
 *	G::LinkLeftLanes(link), LinkRightLanes(link)
 *	G::LinkLaneOffset(link)				CCarPathLink::OneWayLaneOffset
 *
 *	C::CurrentRouteNode() / SetCurrentRouteNode(v)
 *	C::NextRouteNode() / SetNextRouteNode(v)
 *	C::PrevRouteNode() / SetPrevRouteNode(v)
 *	C::TimeEnteredCurve() / SetTimeEnteredCurve(v)
 *	C::TimeOnCurve() / SetTimeOnCurve(v)
 *	C::CurrentPathNodeInfo() / SetCurrentPathNodeInfo(v)
 *	C::NextPathNodeInfo() / SetNextPathNodeInfo(v)
 *	C::PreviousPathNodeInfo() / SetPreviousPathNodeInfo(v)
 *	C::CurrentDirection() / SetCurrentDirection(v)
 *	C::NextDirection() / SetNextDirection(v)
 *	C::PreviousDirection() / SetPreviousDirection(v)
 *	C::CurrentLane() / SetCurrentLane(v)
 *	C::NextLane() / SetNextLane(v)
 *	C::RandomSeed()					CEntity::m_randomSeed
 *	C::MaxTrafficSpeed()				AutoPilot::m_fMaxTrafficSpeed
 *	C::StayInCurrentLevel(), StayInFastLane()
 *	C::CurveProgress()				the place on the curve, 0..1
 *	C::Random()					a 15 bit draw (MYRAND_MAX)
 *	C::RandomTrueFalse()				a coin from the same stream
 *	C::UTurn()					the pick gave up: turn around
 *	C::ShortCurve()					the curve time came out < 10 ms
 *
 * Nothing here includes an engine header: the header compiles on its own (the
 * far layer's evidence program, utils/far-traffic-worker, compiles it without
 * the game).
 *
 * The arithmetic is the one CCarCtrl shipped with (FIX_BUGS branches and all,
 * which config.h always defines in this tree); it was moved, not rewritten.
 */

#include <math.h>
#include <stdint.h>

/* FIX_BUGS (the shipped arithmetic of the two conditional branches below) and
 * the pool sizes come from the game's configuration; the header is otherwise
 * engine-free. */
#include "config.h"

/* The lane spacing of the path links (was PathFind.h's; the pick and the pose
 * of both the near rails and the far layer measure the lanes with it). */
#define LANE_WIDTH 5.0f

/* The numbers of the path pick (were CCarCtrl.cpp's file-locals, moved with
 * the pick). */
#define PATH_DIRECTION_NONE (0)
#define PATH_DIRECTION_STRAIGHT (1)
#define PATH_DIRECTION_RIGHT (2)
#define PATH_DIRECTION_LEFT (4)
#define ATTEMPTS_TO_FIND_NEXT_NODE (15)

/*
 * The curve between two path links (CCurves, which delegates here).
 */

/* The scale factor of a curve: how much longer the road between two links is
 * than the straight line, from the angle between the two directions. */
static inline float
CarRailsCurveSpeedScaleFactor(float p1x, float p1y, float p2x, float p2y,
	float dir1X, float dir1Y, float dir2X, float dir2Y)
{
	float dx = p1x - p2x;
	float dy = p1y - p2y;
	float distance = sqrtf(dx*dx + dy*dy);
	float dp = dir1X*dir2X + dir1Y*dir2Y;
	if (dp > 0.9f)
		return distance + fabsf((p1x * dir1Y - p1y * dir1X) - (p2x * dir1Y - p2y * dir1X));
	else
		return ((1.0f - dp) * 0.25f + 1.0f) * distance;
}

/* The point of the curve between two lane positions and its tangent. The
 * tangent is per millisecond of the curve's time. */
static inline void
CarRailsCurvePoint(float p1x, float p1y, float p2x, float p2y,
	float dir1X, float dir1Y, float dir2X, float dir2Y,
	float between, int32_t timeOnCurve,
	float *pOutX, float *pOutY, float *pOutDirX, float *pOutDirY)
{
	float actualFactor = CarRailsCurveSpeedScaleFactor(p1x, p1y, p2x, p2y, dir1X, dir1Y, dir2X, dir2Y);
	float d1x = dir1X * actualFactor;
	float d1y = dir1Y * actualFactor;
	float d2x = dir2X * actualFactor;
	float d2y = dir2Y * actualFactor;
	float t1 = fabsf((p2x - p1x) * d1x + (p2y - p1y) * d1y);
	float t2 = fabsf((p1x - p2x) * d2x + (p1y - p2y) * d2y);
	float curveCoef;
	if (t1 > t2) {
		float coef = (t1 - t2) / (t1 + t2);
#ifdef FIX_BUGS
		if (between <= coef)
#else
		if (between < coef)
#endif
			curveCoef = 0.0f;
		else
			curveCoef = 0.5f - 0.5f * cosf(3.1415f * (between - coef) * (t1 + t2) / (2 * t2));
	}
	else {
		float coef = 2 * t1 / (t1 + t2);
#ifdef FIX_BUGS
		if (coef <= between)
#else
		if (coef < between)
#endif
			curveCoef = 1.0f;
		else
			curveCoef = 0.5f - 0.5f * cosf(3.1415f * between * (t1 + t2) / (2 * t1));
	}
	*pOutX = (p1x + between * d1x) * (1.0f - curveCoef) + (p2x - (1 - between) * d2x) * curveCoef;
	*pOutY = (p1y + between * d1y) * (1.0f - curveCoef) + (p2y - (1 - between) * d2y) * curveCoef;
	*pOutDirX = (d1x * (1.0f - curveCoef) + d2x * curveCoef) / (timeOnCurve * 0.001f);
	*pOutDirY = (d1y * (1.0f - curveCoef) + d2y * curveCoef) / (timeOnCurve * 0.001f);
}

/*
 * The direction of a turn: whether the way from prev to cur to next turns
 * left, right or goes straight (CCarCtrl::FindPathDirection).
 */
static inline uint8_t
CarRailsPathDirection(float prevX, float prevY, float curX, float curY, float nextX, float nextY)
{
	float prevToCurX = curX - prevX;
	float prevToCurY = curY - prevY;
	float distPrevToCur = sqrtf(prevToCurX*prevToCurX + prevToCurY*prevToCurY);
	if (distPrevToCur == 0.0f)
		return PATH_DIRECTION_NONE;
	/* We are trying to determine angle between prevToCur and curToNext. */
	/* To find it, we consider a to be an angle between y axis and prevToCur */
	/* and b an angle between x axis and curToNext */
	/* Then the angle we are looking for is (pi/2 + a + b). */
	float sin_a = prevToCurX / distPrevToCur;
	float cos_a = prevToCurY / distPrevToCur;
	float curToNextX = nextX - curX;
	float curToNextY = nextY - curY;
	float distCurToNext = sqrtf(curToNextX*curToNextX + curToNextY*curToNextY);
	if (distCurToNext == 0.0f)
		return PATH_DIRECTION_NONE;
	float sin_b = curToNextY / distCurToNext;
	float cos_b = curToNextX / distCurToNext;
	/* sin(a) * sin(b) - cos(a) * cos(b) = -cos(a+b) = sin(pi/2+a+b) */
	float sin_direction = sin_a * sin_b - cos_a * cos_b;
	if (sin_direction > 0.77f) /* Roughly between -50 and -130 degrees */
		return PATH_DIRECTION_LEFT;
	if (sin_direction < -0.77f) /* Roughly between 50 and 130 degrees */
		return PATH_DIRECTION_RIGHT;
	return PATH_DIRECTION_STRAIGHT;
}

/*
 * The pose of a car on the rail of its two path links
 * (CCarCtrl::ComputeCarOnRailPose): the lane-including positions of the two
 * links, the car's own offset on them, and the curve point between them. The
 * height is the caller's (the near rails drag the car to the ground, the far
 * layer takes the path's own z).
 */
template<class G, class C>
static inline void
CarRailsPose(G &g, C &c, float *pOutX, float *pOutY, float *pOutDirX, float *pOutDirY)
{
	int32_t curLink = c.CurrentPathNodeInfo();
	int32_t nextLink = c.NextPathNodeInfo();
	float currentPathLinkForwardX = g.LinkDirX(curLink) * c.CurrentDirection();
	float currentPathLinkForwardY = g.LinkDirY(curLink) * c.CurrentDirection();
	float nextPathLinkForwardX = g.LinkDirX(nextLink) * c.NextDirection();
	float nextPathLinkForwardY = g.LinkDirY(nextLink) * c.NextDirection();
	float posCurX = g.LinkX(curLink) + ((c.CurrentLane() + g.LinkLaneOffset(curLink)) * LANE_WIDTH) * currentPathLinkForwardY;
	float posCurY = g.LinkY(curLink) - ((c.CurrentLane() + g.LinkLaneOffset(curLink)) * LANE_WIDTH) * currentPathLinkForwardX;
	float posNextX = g.LinkX(nextLink) + ((c.NextLane() + g.LinkLaneOffset(nextLink)) * LANE_WIDTH) * nextPathLinkForwardY;
	float posNextY = g.LinkY(nextLink) - ((c.NextLane() + g.LinkLaneOffset(nextLink)) * LANE_WIDTH) * nextPathLinkForwardX;
	/* The car's own offset on the link (GetRandomOffsetForVehicle): the low
	 * three bits of the link index mixed with the car's random seed. The y
	 * term shifts by three again, which is the shipped arithmetic (the seed
	 * is masked to three bits first, so the term is a constant -3). */
	int32_t seedCurrent = (curLink + c.RandomSeed()) & 7;
	int32_t seedNext = (nextLink + c.RandomSeed()) & 7;
	float dirCurX = (seedCurrent - 3) * 0.009f + currentPathLinkForwardX;
	float dirCurY = ((seedCurrent >> 3) - 3) * 0.009f + currentPathLinkForwardY;
	float dirNextX = (seedNext - 3) * 0.009f + nextPathLinkForwardX;
	float dirNextY = ((seedNext >> 3) - 3) * 0.009f + nextPathLinkForwardY;
	/* CVector::Normalise */
	float sq = dirCurX*dirCurX + dirCurY*dirCurY;
	if (sq > 0.0f) {
		float invsqrt = 1.0f / sqrtf(sq);
		dirCurX *= invsqrt;
		dirCurY *= invsqrt;
	} else {
		dirCurX = 1.0f;
	}
	sq = dirNextX*dirNextX + dirNextY*dirNextY;
	if (sq > 0.0f) {
		float invsqrt = 1.0f / sqrtf(sq);
		dirNextX *= invsqrt;
		dirNextY *= invsqrt;
	} else {
		dirNextX = 1.0f;
	}
	CarRailsCurvePoint(
		posCurX, posCurY, posNextX, posNextY, dirCurX, dirCurY, dirNextX, dirNextY,
		c.CurveProgress(), c.TimeOnCurve(),
		pOutX, pOutY, pOutDirX, pOutDirY
	);
}

/*
 * The pick of the next node of an on-rails car (CCarCtrl::PickNextNodeRandomly):
 * walk the links of the current node and take the first candidate the
 * strategy's restrictions allow, three rounds of relaxing them, and then the
 * lane and the curve time of the chosen link. `totalLinks` is the number of
 * the current node's links.
 */
template<class G, class C>
static inline void
CarRailsPickNextNode(G &g, C &c)
{
	int32_t prevNode = c.CurrentRouteNode();
	int32_t curNode = c.NextRouteNode();
	uint8_t totalLinks = (uint8_t)g.NumLinks(curNode);
	int32_t curLink = c.NextPathNodeInfo();
	uint8_t lanesOnCurrentPath;
	bool isOnOneWayRoad;
	if (g.LinkPathNode(curLink) == curNode) {
		lanesOnCurrentPath = (uint8_t)g.LinkLeftLanes(curLink);
		isOnOneWayRoad = g.LinkRightLanes(curLink) == 0;
	}
	else {
		lanesOnCurrentPath = (uint8_t)g.LinkRightLanes(curLink);
		isOnOneWayRoad = g.LinkLeftLanes(curLink) == 0;
	}
	uint8_t allowedDirections = PATH_DIRECTION_NONE;
	uint8_t nextLane = (uint8_t)c.NextLane();
	if (nextLane == 0)
		/* We are always allowed to turn left from  leftmost lane */
		allowedDirections |= PATH_DIRECTION_LEFT;
	if (nextLane == lanesOnCurrentPath - 1)
		/* We are always allowed to turn right from rightmost lane */
		allowedDirections |= PATH_DIRECTION_RIGHT;
	if (lanesOnCurrentPath < 3 || allowedDirections == PATH_DIRECTION_NONE)
		/* We are always allowed to go straight on one/two-laned road */
		/* or if we are in one of middle lanes of the road */
		allowedDirections |= PATH_DIRECTION_STRAIGHT;
	int attempt;
	c.SetPrevRouteNode(c.CurrentRouteNode());
	c.SetCurrentRouteNode(c.NextRouteNode());
	int32_t nextLink;
	bool goingAgainstOneWayRoad;
	bool nextNodeIsOneWayRoad;
	uint8_t direction;
	for (attempt = 0; attempt < ATTEMPTS_TO_FIND_NEXT_NODE; attempt++) {
		if (attempt != 0) {
			if (c.NextRouteNode() != prevNode) {
				if (direction & allowedDirections) {
					int32_t candidate = c.NextRouteNode();
					if ((!g.NodeDeadEnd(candidate) || g.NodeDeadEnd(prevNode)) &&
						(!g.NodeDisabled(candidate) || g.NodeDisabled(prevNode)) &&
						(!g.NodeBetweenLevels(candidate) || g.NodeBetweenLevels(prevNode) || !c.StayInCurrentLevel()) &&
						!goingAgainstOneWayRoad && (!isOnOneWayRoad || !nextNodeIsOneWayRoad))
						break;
				}
			}
		}
		nextLink = c.Random() % totalLinks;
		c.SetNextRouteNode(g.ConnectedNode(nextLink + g.NodeFirstLink(curNode)));
		direction = CarRailsPathDirection(
			g.NodeX(prevNode), g.NodeY(prevNode), g.NodeX(curNode), g.NodeY(curNode),
			g.NodeX(c.NextRouteNode()), g.NodeY(c.NextRouteNode()));
		int32_t candidateLink = g.LinkOfConnection(nextLink + g.NodeFirstLink(curNode));
		goingAgainstOneWayRoad = g.LinkPathNode(candidateLink) == curNode ? g.LinkRightLanes(candidateLink) == 0 : g.LinkLeftLanes(candidateLink) == 0;
		nextNodeIsOneWayRoad = g.LinkPathNode(candidateLink) == curNode ? g.LinkLeftLanes(candidateLink) == 0 : g.LinkRightLanes(candidateLink) == 0;
	}
	if (attempt >= ATTEMPTS_TO_FIND_NEXT_NODE) {
		/* If we failed 15 times, then remove dead end, one way road and current lane limitations */
		for (attempt = 0; attempt < ATTEMPTS_TO_FIND_NEXT_NODE; attempt++) {
			if (attempt != 0) {
				if (c.NextRouteNode() != prevNode) {
					int32_t candidate = c.NextRouteNode();
					if ((!g.NodeDisabled(candidate) || g.NodeDisabled(prevNode)) &&
						(!g.NodeBetweenLevels(candidate) || g.NodeBetweenLevels(prevNode) || !c.StayInCurrentLevel()) &&
						!goingAgainstOneWayRoad)
						break;
				}
			}
			nextLink = c.Random() % totalLinks;
			c.SetNextRouteNode(g.ConnectedNode(nextLink + g.NodeFirstLink(curNode)));
			int32_t candidateLink = g.LinkOfConnection(nextLink + g.NodeFirstLink(curNode));
			goingAgainstOneWayRoad = g.LinkPathNode(candidateLink) == curNode ? g.LinkRightLanes(candidateLink) == 0 : g.LinkLeftLanes(candidateLink) == 0;
		}
	}
	if (attempt >= ATTEMPTS_TO_FIND_NEXT_NODE) {
		/* If we failed again, remove no U-turn limitation and remove randomness */
		for (nextLink = 0; nextLink < totalLinks; nextLink++) {
			c.SetNextRouteNode(g.ConnectedNode(nextLink + g.NodeFirstLink(curNode)));
			int32_t candidateLink = g.LinkOfConnection(nextLink + g.NodeFirstLink(curNode));
			goingAgainstOneWayRoad = g.LinkPathNode(candidateLink) == curNode ? g.LinkRightLanes(candidateLink) == 0 : g.LinkLeftLanes(candidateLink) == 0;
			if (!goingAgainstOneWayRoad) {
				int32_t candidate = c.NextRouteNode();
				if ((!g.NodeDisabled(candidate) || g.NodeDisabled(prevNode)) &&
					(!g.NodeBetweenLevels(candidate) || g.NodeBetweenLevels(prevNode) || !c.StayInCurrentLevel()))
					/* Nice way to exit loop but this will fail because this is used for indexing! */
					nextLink = 1000;
			}
		}
		if (nextLink < 999)
			/* If everything else failed, turn vehicle around */
			c.SetNextRouteNode(prevNode);
	}
	int32_t nextRouteNode = c.NextRouteNode();
	int32_t nextLinkOfChoice = g.LinkOfConnection(nextLink + g.NodeFirstLink(curNode));
	if (prevNode == nextRouteNode)
		c.UTurn();
	c.SetTimeEnteredCurve(c.TimeEnteredCurve() + c.TimeOnCurve());
	c.SetPreviousPathNodeInfo(c.CurrentPathNodeInfo());
	c.SetCurrentPathNodeInfo(c.NextPathNodeInfo());
	c.SetPreviousDirection(c.CurrentDirection());
	c.SetCurrentDirection(c.NextDirection());
	c.SetCurrentLane(c.NextLane());
	c.SetNextPathNodeInfo(nextLinkOfChoice);
	int8_t lanesOnNextNode;
	if (curNode >= nextRouteNode) {
		c.SetNextDirection(1);
		lanesOnNextNode = (int8_t)g.LinkLeftLanes(nextLinkOfChoice);
	} else {
		c.SetNextDirection(-1);
		lanesOnNextNode = (int8_t)g.LinkRightLanes(nextLinkOfChoice);
	}
	float currentPathLinkForwardX = c.CurrentDirection() * g.LinkDirX(curLink);
	float nextPathLinkForwardX = c.NextDirection() * g.LinkDirX(nextLinkOfChoice);
#ifdef FIX_BUGS
	float currentPathLinkForwardY = c.CurrentDirection() * g.LinkDirY(curLink);
	float nextPathLinkForwardY = c.NextDirection() * g.LinkDirY(nextLinkOfChoice);
#endif
	if (lanesOnNextNode >= 0) {
		if ((c.Random() & 0x600) == 0) {
			/* 25% chance vehicle will try to switch lane */
			float distX = g.NodeX(nextRouteNode) - g.NodeX(curNode);
			float distY = g.NodeY(nextRouteNode) - g.NodeY(curNode);
			/* SQR(14.0f): the shipped distance test, inlined (the header has
			 * no common.h) */
			if (distX*distX + distY*distY >= 14.0f*14.0f) {
				if (c.RandomTrueFalse())
					c.SetNextLane(c.NextLane() + 1);
				else
					c.SetNextLane(c.NextLane() - 1);
			}
		}
		c.SetNextLane(c.NextLane() < lanesOnNextNode - 1 ? c.NextLane() : lanesOnNextNode - 1);
		c.SetNextLane(c.NextLane() > 0 ? c.NextLane() : 0);
	} else {
		c.SetNextLane(c.CurrentLane());
	}
	if (c.StayInFastLane())
		c.SetNextLane(0);
	float positionOnCurrentLinkX = g.LinkX(curLink) + ((c.CurrentLane() + g.LinkLaneOffset(curLink)) * LANE_WIDTH)
#ifdef FIX_BUGS
		* currentPathLinkForwardY
#endif
		;
	float positionOnCurrentLinkY = g.LinkY(curLink) - ((c.CurrentLane() + g.LinkLaneOffset(curLink)) * LANE_WIDTH) * currentPathLinkForwardX;
	float positionOnNextLinkX = g.LinkX(nextLinkOfChoice) + ((c.NextLane() + g.LinkLaneOffset(nextLinkOfChoice)) * LANE_WIDTH)
#ifdef FIX_BUGS
		* nextPathLinkForwardY
#endif
		;
	float positionOnNextLinkY = g.LinkY(nextLinkOfChoice) - ((c.NextLane() + g.LinkLaneOffset(nextLinkOfChoice)) * LANE_WIDTH) * nextPathLinkForwardX;
	float directionCurrentLinkX = g.LinkDirX(curLink) * c.CurrentDirection();
	float directionCurrentLinkY = g.LinkDirY(curLink) * c.CurrentDirection();
	float directionNextLinkX = g.LinkDirX(nextLinkOfChoice) * c.NextDirection();
	float directionNextLinkY = g.LinkDirY(nextLinkOfChoice) * c.NextDirection();
	/* We want to make a path between two links that may not have the same forward directions a curve. */
	int32_t timeOnCurve = (int32_t)(CarRailsCurveSpeedScaleFactor(
		positionOnCurrentLinkX, positionOnCurrentLinkY,
		positionOnNextLinkX, positionOnNextLinkY,
		directionCurrentLinkX, directionCurrentLinkY,
		directionNextLinkX, directionNextLinkY
	) * (1000.0f / c.MaxTrafficSpeed()));
	if (timeOnCurve < 10)
		c.ShortCurve();
	c.SetTimeOnCurve(timeOnCurve > 10 ? timeOnCurve : 10);
}

#endif // __GTA_CARONRAILS_H__
