#include "LimbRate.h"

#include <math.h>

// The aim IK's limiter (VICE-194). The stock game advanced every limb by a
// fixed angle per call of MoveLimb, so the IK's angular speed followed the
// frame rate: at 100 fps a limb closed twice as fast as at 50, and it reached
// its limits sooner. The rates are per second now - the stock per-call angle
// times the reference frame rate, 50 frames per second, which is the engine's
// own time base (CTimer::GetTimeStep() is 1.0 there) - and MoveLimbStep walks
// the limb by the rate times the frame step. At the reference rate one call is
// worth exactly the stock angle, so a 50 fps frame aims as it always did, and
// at any other rate the limb closes on its target in the same time instead of
// at a speed that follows the display.
//
// The limits are the stock ones, unchanged: the frame rate cannot move a limit.

// The stock per-call angle, converted to a rate:
//
//	row		stock per call		rate now
//	torso yaw	8 deg per call		400 deg per second
//	torso pitch	5 deg per call		250 deg per second
//	seat yaw	8 deg per call		400 deg per second
//	seat pitch	5 deg per call		250 deg per second
//	lean yaw	3 deg per call		150 deg per second
//	lean pitch	1 deg per call		50 deg per second
//	head yaw	15 deg per call		750 deg per second
//	head pitch	8 deg per call		400 deg per second
//	restore yaw	10 deg per call		500 deg per second
//	restore pitch	5 deg per call		250 deg per second
//	upper arm yaw	20 deg per call		1000 deg per second
//	upper arm pitch	20 deg per call		1000 deg per second
//	lower arm yaw	15 deg per call		750 deg per second
//	lower arm pitch	10 deg per call		500 deg per second
//
// The two seat rows share the stock values; the lean's targets are pinned to
// the seat, so its rates are the stock ones too.

// 50 frames per second: the rate the stock per-call angles are stated at.
#define LIMB_RATE_REFERENCE_HZ	50.0f

// The engine's DEGTORAD (common.h), in the module's own domain: the rows below
// carry the stock angles in the stock bits.
static float
DegToRad(float degrees)
{
	return degrees * 3.14159265358979323846f / 180.0f;
}

// The engine's fabsf (maths.h Abs) and Clamp (common.h), so the module stays
// engine free.
static float
Clamp(float value, float low, float high)
{
	return value < low ? low : value > high ? high : value;
}

const LimbMovementInfo ms_torsoInfo = { DegToRad(50.0f), DegToRad(-50.0f), DegToRad(8.0f) * LIMB_RATE_REFERENCE_HZ, DegToRad(45.0f), DegToRad(-45.0f), DegToRad(5.0f) * LIMB_RATE_REFERENCE_HZ };
// The seated player turns the torso further: the body heading is pinned to the seat
const LimbMovementInfo ms_seatTorsoInfo = { DegToRad(90.0f), DegToRad(-90.0f), DegToRad(8.0f) * LIMB_RATE_REFERENCE_HZ, DegToRad(45.0f), DegToRad(-45.0f), DegToRad(5.0f) * LIMB_RATE_REFERENCE_HZ };
const LimbMovementInfo ms_leanInfo = { DegToRad(20.0f), DegToRad(-20.0f), DegToRad(3.0f) * LIMB_RATE_REFERENCE_HZ, 0.0f, 0.0f, DegToRad(1.0f) * LIMB_RATE_REFERENCE_HZ };
const LimbMovementInfo ms_headInfo = { DegToRad(90.0f), DegToRad(-90.0f), DegToRad(15.0f) * LIMB_RATE_REFERENCE_HZ, DegToRad(45.0f), DegToRad(-45.0f), DegToRad(8.0f) * LIMB_RATE_REFERENCE_HZ };
const LimbMovementInfo ms_headRestoreInfo = { DegToRad(90.0f), DegToRad(-90.0f), DegToRad(10.0f) * LIMB_RATE_REFERENCE_HZ, DegToRad(45.0f), DegToRad(-45.0f), DegToRad(5.0f) * LIMB_RATE_REFERENCE_HZ };
const LimbMovementInfo ms_upperArmInfo = { DegToRad(5.0f), DegToRad(-120.0f), DegToRad(20.0f) * LIMB_RATE_REFERENCE_HZ, DegToRad(70.0f), DegToRad(-70.0f), DegToRad(20.0f) * LIMB_RATE_REFERENCE_HZ };
const LimbMovementInfo ms_lowerArmInfo = { DegToRad(60.0f), DegToRad(0.0f), DegToRad(15.0f) * LIMB_RATE_REFERENCE_HZ, DegToRad(90.0f), DegToRad(-90.0f), DegToRad(10.0f) * LIMB_RATE_REFERENCE_HZ };

// The step of one frame: the rate over dt seconds. A zero step (a paused
// frame) moves nothing, where the stock call moved a fixed angle whatever the
// clock said.
LimbMoveStatus
MoveLimbStep(LimbOrientation &limb, float targetYaw, float targetPitch, const LimbMovementInfo &moveInfo, float dt)
{
	LimbMoveStatus result = ONE_ANGLE_COULDNT_BE_SET_EXACTLY;

	float yawStep = moveInfo.yawD * dt;
	float pitchStep = moveInfo.pitchD * dt;

	// yaw

	if(fabsf(limb.yaw - targetYaw) < yawStep){
		limb.yaw = targetYaw;
		result = ANGLES_SET_EXACTLY;
	}else{
		if (limb.yaw > targetYaw) {
			limb.yaw -= yawStep;
		} else if (limb.yaw < targetYaw) {
			limb.yaw += yawStep;
		}
	}

	if (limb.yaw > moveInfo.maxYaw || limb.yaw < moveInfo.minYaw) {
		limb.yaw = Clamp(limb.yaw, moveInfo.minYaw, moveInfo.maxYaw);
		result = ANGLES_SET_TO_MAX;
	}

	// pitch

	if (fabsf(limb.pitch - targetPitch) < pitchStep){
		limb.pitch = targetPitch;
	}else{
		if (limb.pitch > targetPitch) {
			limb.pitch -= pitchStep;
		} else if (limb.pitch < targetPitch) {
			limb.pitch += pitchStep;
		}
		result = ONE_ANGLE_COULDNT_BE_SET_EXACTLY;
	}

	if (limb.pitch > moveInfo.maxPitch || limb.pitch < moveInfo.minPitch) {
		limb.pitch = Clamp(limb.pitch, moveInfo.minPitch, moveInfo.maxPitch);
		result = ANGLES_SET_TO_MAX;
	}
	return result;
}
