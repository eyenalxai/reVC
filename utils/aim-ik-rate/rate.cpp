/*
 * The evidence program for VICE-194: the aim IK's limiter runs on the frame
 * step, and the reference frame rate does not change.
 *
 * It compiles the game's own limiter module (src/peds/LimbRate.cpp: the seven
 * rows and MoveLimbStep, engine free) and drives it beside the limiter the
 * ticket replaces, transcribed here from CPedIK::MoveLimb at 68118cae (a fixed
 * yawD/pitchD per call, so the angular speed followed the frame rate):
 *
 *	A. The rows. Every row of the module carries the stock limits and the
 *	   stock per-call angle times the reference frame rate, 50 frames per
 *	   second. The stock table is transcribed here in degrees per call, the
 *	   way PedIK.cpp spelled it.
 *
 *	B. The step at the reference rate. The module's limiter at dt = 1/50 s
 *	   and the transcribed stock limiter are driven over a randomized target
 *	   stream over all seven rows, from random starting orientations: 420,000
 *	   steps. The angles must agree to within a float ulp of a step, and no
 *	   step may end in a different state. The engine's fiftieth is not a
 *	   binary fraction - the frame step is 0.0199999996 s, a touch under 1/50
 *	   - so a per-second rate over the engine's step sits a fraction of an
 *	   ulp under the stock per-call angle, and the stock limiter can land on
 *	   a target one frame before the per-second one. That is the only
 *	   difference the sweep may find, it is counted, and the settle check at
 *	   the end of the sweep proves the two converge on the same angle.
 *
 *	C. The frame rate. The module's limiter closes a gap in the same time at
 *	   25, 50, 100 and 200 frames per second: its close time stays within one
 *	   frame of the exact gap over the rate, at every rate. The stock limiter
 *	   at 100 fps closes the same gap in half the time, which is the defect.
 *
 *	D. The limits. No dt walks a limb outside its row's limits, and a target
 *	   beyond a limit ends at ANGLES_SET_TO_MAX - every step but a zero one,
 *	   which moves nothing at all (E).
 *
 *	E. The zero step. A frame of zero seconds - a paused frame - moves no
 *	   limb, where the stock call moved a fixed angle whatever the clock said.
 *
 *	F. The frame's calls. A limb moved twice in one frame takes two steps, the
 *	   same as the stock limiter took two calls: the frame's step scales the
 *	   call, it does not cap the frame.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -O2 -o /tmp/opencode/aim-ik-rate src/peds/LimbRate.cpp utils/aim-ik-rate/rate.cpp
 *	/tmp/opencode/aim-ik-rate
 *
 * The module includes no engine header, so that line is also the structural
 * check: the rows and the limiter carry no clock, no ped and no animation, and
 * the frame step arrives as the step's own argument.
 *
 * The exit code is 0 when every check passes, 1 otherwise.
 */

#include "../../src/peds/LimbRate.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int g_checks = 0;
static int g_failures = 0;
static char g_where[64] = "";

static void
Check(bool ok, const char *what)
{
	g_checks++;
	if (!ok) {
		g_failures++;
		printf("FAIL: %s%s\n", g_where, what);
	}
}

// The engine's DEGTORAD (common.h), as the module spells it.
static float
DegToRad(float degrees)
{
	return degrees * 3.14159265358979323846f / 180.0f;
}

// The reference frame rate of the module's rows.
#define STOCK_HZ	50.0f

// The stock rows (PedIK.cpp at 68118cae), in degrees: maxYaw, minYaw, yaw per
// call, maxPitch, minPitch, pitch per call.
static const float aStockDegrees[7][6] = {
	{ 50.0f, -50.0f, 8.0f, 45.0f, -45.0f, 5.0f },	// torso
	{ 90.0f, -90.0f, 8.0f, 45.0f, -45.0f, 5.0f },	// seated torso
	{ 20.0f, -20.0f, 3.0f, 0.0f, 0.0f, 1.0f },	// driver lean
	{ 90.0f, -90.0f, 15.0f, 45.0f, -45.0f, 8.0f },	// head
	{ 90.0f, -90.0f, 10.0f, 45.0f, -45.0f, 5.0f },	// head restore
	{ 5.0f, -120.0f, 20.0f, 70.0f, -70.0f, 20.0f },	// upper arm
	{ 60.0f, 0.0f, 15.0f, 90.0f, -90.0f, 10.0f },	// lower arm
};

static const char *aLimbNames[7] = {
	"torso", "seat torso", "lean", "head", "head restore", "upper arm", "lower arm"
};

// Name the row the checks below run on, so a failure says which limb.
static void
AtLimb(int limb)
{
	snprintf(g_where, sizeof(g_where), "[%s] ", aLimbNames[limb]);
}

static LimbMovementInfo *apRows[7] = {
	&ms_torsoInfo, &ms_seatTorsoInfo, &ms_leanInfo, &ms_headInfo,
	&ms_headRestoreInfo, &ms_upperArmInfo, &ms_lowerArmInfo
};

// The stock row for limb i: the limits and the per-call angles in radians,
// converted the way the stock table converted them.
static LimbMovementInfo
StockRow(int limb)
{
	const float *d = aStockDegrees[limb];
	LimbMovementInfo row;
	row.maxYaw = DegToRad(d[0]);
	row.minYaw = DegToRad(d[1]);
	row.yawD = DegToRad(d[2]);
	row.maxPitch = DegToRad(d[3]);
	row.minPitch = DegToRad(d[4]);
	row.pitchD = DegToRad(d[5]);
	return row;
}

// The pre-ticket limiter (CPedIK::MoveLimb at 68118cae): a fixed angle per
// call, whatever the frame did.
static LimbMoveStatus
StockMoveLimb(LimbOrientation &limb, float targetYaw, float targetPitch, const LimbMovementInfo &moveInfo)
{
	LimbMoveStatus result = ONE_ANGLE_COULDNT_BE_SET_EXACTLY;

	// yaw

	if(fabsf(limb.yaw - targetYaw) < moveInfo.yawD){
		limb.yaw = targetYaw;
		result = ANGLES_SET_EXACTLY;
	}else{
		if (limb.yaw > targetYaw) {
			limb.yaw -= moveInfo.yawD;
		} else if (limb.yaw < targetYaw) {
			limb.yaw += moveInfo.yawD;
		}
	}

	if (limb.yaw > moveInfo.maxYaw || limb.yaw < moveInfo.minYaw) {
		limb.yaw = limb.yaw < moveInfo.minYaw ? moveInfo.minYaw : limb.yaw > moveInfo.maxYaw ? moveInfo.maxYaw : limb.yaw;
		result = ANGLES_SET_TO_MAX;
	}

	// pitch

	if (fabsf(limb.pitch - targetPitch) < moveInfo.pitchD){
		limb.pitch = targetPitch;
	}else{
		if (limb.pitch > targetPitch) {
			limb.pitch -= moveInfo.pitchD;
		} else if (limb.pitch < targetPitch) {
			limb.pitch += moveInfo.pitchD;
		}
		result = ONE_ANGLE_COULDNT_BE_SET_EXACTLY;
	}

	if (limb.pitch > moveInfo.maxPitch || limb.pitch < moveInfo.minPitch) {
		limb.pitch = limb.pitch < moveInfo.minPitch ? moveInfo.minPitch : limb.pitch > moveInfo.maxPitch ? moveInfo.maxPitch : limb.pitch;
		result = ANGLES_SET_TO_MAX;
	}
	return result;
}

/* A deterministic stream: the same stream every run. */
static uint32_t g_random = 0x194169u;

static uint32_t
NextRandom(void)
{
	g_random = g_random * 1664525u + 1013904223u;
	return g_random >> 8;
}

static float
RandomRange(float low, float high)
{
	return low + (high - low) * (float)(NextRandom() & 0xFFFF) / 65535.0f;
}

// How a limb ends a walk toward one target at one frame rate: the time in
// seconds the close took, and the status of the step that landed it.
static float
CloseTime(const LimbMovementInfo &row, float target, float fps, bool stock, int *landedExactly)
{
	LimbOrientation limb = { 0.0f, 0.0f };
	float dt = 1.0f / fps;
	int frames = 0;

	while (frames < 1000000) {
		LimbMoveStatus status = stock
			? StockMoveLimb(limb, target, 0.0f, row)
			: MoveLimbStep(limb, target, 0.0f, row, dt);
		frames++;
		if (limb.yaw == target) {
			if (landedExactly)
				*landedExactly = status == ANGLES_SET_EXACTLY;
			return frames / fps;
		}
	}
	return -1.0f;
}

int
main(void)
{
	int limb, i;

	/* A. The rows */

	int rowsWithExactRate = 0;
	float maxRateDeviation = 0.0f;
	for (limb = 0; limb < 7; limb++) {
		LimbMovementInfo stock = StockRow(limb);
		LimbMovementInfo *row = apRows[limb];
		AtLimb(limb);

		Check(row->maxYaw == stock.maxYaw && row->minYaw == stock.minYaw &&
			row->maxPitch == stock.maxPitch && row->minPitch == stock.minPitch,
			"a row keeps the stock limits");

		// The rate is the stock per-call angle times the reference frame
		// rate. The module spells it DegToRad(perCall) * 50; a spelling that
		// rounds differently is reported by the deviation below, and only a
		// per-call value left behind (a factor of 50) is a failure.
		float expectedYaw = DegToRad(aStockDegrees[limb][2]) * STOCK_HZ;
		float expectedPitch = DegToRad(aStockDegrees[limb][5]) * STOCK_HZ;
		float yawDeviation = fabsf(row->yawD - expectedYaw);
		float pitchDeviation = fabsf(row->pitchD - expectedPitch);
		if (yawDeviation > maxRateDeviation) maxRateDeviation = yawDeviation;
		if (pitchDeviation > maxRateDeviation) maxRateDeviation = pitchDeviation;
		if (row->yawD == expectedYaw && row->pitchD == expectedPitch)
			rowsWithExactRate++;

		Check(fabsf(row->yawD - expectedYaw) <= 1e-6f * expectedYaw,
			"a row's yaw rate is the stock per-call angle times 50");
		Check(fabsf(row->pitchD - expectedPitch) <= 1e-6f * expectedPitch,
			"a row's pitch rate is the stock per-call angle times 50");
	}
	Check(rowsWithExactRate == 7, "every row spells the rate as the stock angle times the reference rate");
	printf("A. rows: 7 stock rows, %d spelled exactly, largest rate deviation %.3g rad/s\n",
		rowsWithExactRate, maxRateDeviation);

	/* B. The parity at the reference rate */

	float referenceDt = 1.0f / STOCK_HZ;
	float maxAngleDeviation = 0.0f;
	long identicalSteps = 0;
	long comparedSteps = 0;
	long statusDifferences = 0;
	long resolvedNextFrame = 0;
	long unresolvedStatusDifferences = 0;
	long settleDifferences = 0;

	for (limb = 0; limb < 7; limb++) {
		LimbMovementInfo stock = StockRow(limb);
		LimbMovementInfo *row = apRows[limb];
		AtLimb(limb);

		LimbOrientation lastModule = { 0.0f, 0.0f };
		LimbOrientation lastStock = { 0.0f, 0.0f };

		for (i = 0; i < 4; i++) {
			LimbOrientation moduleLimb = { RandomRange(stock.minYaw, stock.maxYaw), RandomRange(stock.minPitch, stock.maxPitch) };
			LimbOrientation stockLimb = moduleLimb;

			for (long step = 0; step < 15000; step++) {
				float targetYaw = RandomRange(-1.3f, 1.3f);
				float targetPitch = RandomRange(-1.3f, 1.3f);

				LimbMoveStatus moduleStatus = MoveLimbStep(moduleLimb, targetYaw, targetPitch, *row, referenceDt);
				LimbMoveStatus stockStatus = StockMoveLimb(stockLimb, targetYaw, targetPitch, stock);

				if (moduleStatus != stockStatus) {
					// The step of the per-second limiter is a float ulp under
					// the stock per-call angle (the engine's fiftieth is not a
					// binary fraction), so the frame that lands on a target or
					// reaches a limit can be one frame later. The claim checked
					// here is that it is exactly one frame: the same target one
					// step later reports the stock state again.
					LimbOrientation moduleNext = moduleLimb;
					LimbOrientation stockNext = stockLimb;
					LimbMoveStatus moduleNextStatus = MoveLimbStep(moduleNext, targetYaw, targetPitch, *row, referenceDt);
					LimbMoveStatus stockNextStatus = StockMoveLimb(stockNext, targetYaw, targetPitch, stock);
					statusDifferences++;
					if (moduleNextStatus == stockNextStatus && moduleNext.yaw == stockNext.yaw && moduleNext.pitch == stockNext.pitch)
						resolvedNextFrame++;
					else
						unresolvedStatusDifferences++;
				}

				float yawDeviation = fabsf(moduleLimb.yaw - stockLimb.yaw);
				float pitchDeviation = fabsf(moduleLimb.pitch - stockLimb.pitch);
				if (yawDeviation > maxAngleDeviation) maxAngleDeviation = yawDeviation;
				if (pitchDeviation > maxAngleDeviation) maxAngleDeviation = pitchDeviation;
				if (moduleLimb.yaw == stockLimb.yaw && moduleLimb.pitch == stockLimb.pitch)
					identicalSteps++;
				comparedSteps++;
			}
			lastModule = moduleLimb;
			lastStock = stockLimb;
		}

		// The settle: a fixed target for fifty steps. Whatever the last frame
		// of the stream left behind, both limiters must end on the target
		// itself, bit for bit.
		for (int step = 0; step < 50; step++) {
			MoveLimbStep(lastModule, 0.0f, 0.0f, *row, referenceDt);
			StockMoveLimb(lastStock, 0.0f, 0.0f, stock);
		}
		if (lastModule.yaw != lastStock.yaw || lastModule.pitch != lastStock.pitch)
			settleDifferences++;
	}

	Check(unresolvedStatusDifferences == 0, "every status difference of the reference stream is the stock state one frame later");
	Check(maxAngleDeviation <= 1e-6f, "every step of the reference stream is within a float ulp of the stock angle");
	Check(settleDifferences == 0, "the two limiters settle on the same angle, bit for bit");
	printf("B. reference rate (dt = 1/50 s): %ld steps, %ld bit-identical, %ld status differences (%ld resolved one frame later, %ld not), largest angle deviation %.3g rad\n",
		comparedSteps, identicalSteps, statusDifferences, resolvedNextFrame, unresolvedStatusDifferences, maxAngleDeviation);

	/* C. The frame rate */

	LimbMovementInfo *head = &ms_headInfo;
	LimbMovementInfo stockHead = StockRow(3);
	float target = 1.5f;
	float exactTime = target / (DegToRad(aStockDegrees[3][2]) * STOCK_HZ);
	float aRates[4] = { 25.0f, 50.0f, 100.0f, 200.0f };
	float stockTime50 = -1.0f;

	printf("C. one gap of %.3g rad, the head's yaw at %.0f deg/s:\n", target, aStockDegrees[3][2] * STOCK_HZ);
	for (i = 0; i < 4; i++) {
		float fps = aRates[i];
		int moduleLandedExactly = 0, stockLandedExactly = 0;
		float moduleTime = CloseTime(*head, target, fps, false, &moduleLandedExactly);
		float stockTime = CloseTime(stockHead, target, fps, true, &stockLandedExactly);

		// The exact close is gap / rate; a frame lands on the target at the
		// end of the frame that covers it, so the time sits within one frame.
		Check(moduleTime >= exactTime - 1e-6f && moduleTime <= exactTime + 1.0f/fps + 1e-6f,
			"the module closes the gap within one frame of the exact time");
		Check(moduleLandedExactly, "the module lands on the target with ANGLES_SET_EXACTLY");
		Check(stockLandedExactly, "the stock limiter lands on the target with ANGLES_SET_EXACTLY");
		printf("   %6.0f fps: module %8.6f s, stock %8.6f s\n", fps, moduleTime, stockTime);
		if (fps == 50.0f)
			stockTime50 = stockTime;
		else if (fps == 100.0f) {
			Check(fabsf(stockTime - stockTime50 * 0.5f) <= 1.0f/fps + exactTime * 1e-4f,
				"the stock limiter takes half the time at 100 fps (the defect)");
		}
	}

	/* D. The limits */

	float aSteps[5] = { 0.0f, 1.0f/200.0f, referenceDt, 1.0f/10.0f, 0.1f };
	int limitSteps = 0;
	for (limb = 0; limb < 7; limb++) {
		LimbMovementInfo *row = apRows[limb];
		AtLimb(limb);
		for (i = 0; i < 5; i++) {
			LimbOrientation limbState = { 0.0f, 0.0f };
			LimbMoveStatus status = ONE_ANGLE_COULDNT_BE_SET_EXACTLY;
			for (int step = 0; step < 500; step++)
				status = MoveLimbStep(limbState, row->maxYaw * 4.0f, row->maxPitch * 4.0f, *row, aSteps[i]);
			Check(limbState.yaw <= row->maxYaw && limbState.yaw >= row->minYaw &&
				limbState.pitch <= row->maxPitch && limbState.pitch >= row->minPitch,
				"a limb past its limit stays inside the row's limits");
			if (aSteps[i] > 0.0f) {
				Check(status == ANGLES_SET_TO_MAX, "a target beyond the limit ends at ANGLES_SET_TO_MAX");
				limitSteps++;
			}
		}
	}
	printf("D. limits: %d walks past a limit, none left it\n", limitSteps);

	/* E. The zero step */

	{
		LimbOrientation limbState = { 0.1f, 0.05f };
		LimbOrientation before = limbState;
		LimbMoveStatus status = MoveLimbStep(limbState, 1.0f, 0.5f, ms_headInfo, 0.0f);
		Check(limbState.yaw == before.yaw && limbState.pitch == before.pitch,
			"a frame of zero seconds moves no limb");
		Check(status == ONE_ANGLE_COULDNT_BE_SET_EXACTLY,
			"a frame of zero seconds reports neither an exact angle nor a limit");

		// A tenth of a reference step moves about a tenth of the angle.
		LimbOrientation small = { 0.0f, 0.0f };
		LimbOrientation full = { 0.0f, 0.0f };
		MoveLimbStep(small, 1.0f, 0.0f, ms_headInfo, referenceDt * 0.1f);
		MoveLimbStep(full, 1.0f, 0.0f, ms_headInfo, referenceDt);
		Check(fabsf(small.yaw / full.yaw - 0.1f) < 1e-5f,
			"a tenth of the frame step moves a tenth of the angle");
		printf("E. zero step: no move, no exact angle\n");
	}

	/* F. The frame's calls */

	{
		LimbOrientation moduleLimb = { 0.0f, 0.0f };
		LimbOrientation stockLimb = { 0.0f, 0.0f };
		MoveLimbStep(moduleLimb, 1.0f, 0.0f, ms_headInfo, referenceDt);
		MoveLimbStep(moduleLimb, 1.0f, 0.0f, ms_headInfo, referenceDt);
		StockMoveLimb(stockLimb, 1.0f, 0.0f, stockHead);
		StockMoveLimb(stockLimb, 1.0f, 0.0f, stockHead);
		Check(fabsf(moduleLimb.yaw - stockLimb.yaw) <= 1e-6f,
			"two calls in one frame are two stock steps");
		printf("F. two calls in one frame: module %.6f rad, stock %.6f rad\n", moduleLimb.yaw, stockLimb.yaw);
	}

	printf("\nRESULT: %s (%d checks, %d failures)\n",
		g_failures == 0 ? "every check passed" : "FAILED", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
