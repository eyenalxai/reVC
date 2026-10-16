#include "common.h"

#include "Curves.h"
#include "CarOnRails.h"

/*
 * The curve arithmetic of a car on its two path links. The formulas live in
 * CarOnRails.h (VICE-133): the far traffic layer's simulation runs on a worker
 * thread and cannot call these wrappers (or anything else of the engine), so
 * the one copy of the arithmetic sits in the engine-free header and both the
 * near rails and the worker instantiate it. These two calls keep the CCurves
 * API the rest of the engine reads (AutoPilot.cpp and the pick sites of
 * CCarCtrl.cpp use them too).
 */

float CCurves::CalcSpeedScaleFactor(CVector* pPoint1, CVector* pPoint2, float dir1X, float dir1Y, float dir2X, float dir2Y)
{
	return CarRailsCurveSpeedScaleFactor(pPoint1->x, pPoint1->y, pPoint2->x, pPoint2->y,
		dir1X, dir1Y, dir2X, dir2Y);
}

void CCurves::CalcCurvePoint(CVector* pPos1, CVector* pPos2, CVector* pDir1, CVector* pDir2, float between, int32 timeOnCurve, CVector* pOutPos, CVector* pOutDir)
{
	CarRailsCurvePoint(pPos1->x, pPos1->y, pPos2->x, pPos2->y,
		pDir1->x, pDir1->y, pDir2->x, pDir2->y,
		between, timeOnCurve,
		&pOutPos->x, &pOutPos->y, &pOutDir->x, &pOutDir->y);
	/* The curve point lies on the road plane: the callers read z as zero (the
	 * near rails set it from the ground, the far layer from the path nodes). */
	pOutPos->z = 0.0f;
	pOutDir->z = 0.0f;
}
