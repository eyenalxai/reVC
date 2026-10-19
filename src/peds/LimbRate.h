#ifndef __GTA_LIMBRATE_H__
#define __GTA_LIMBRATE_H__

// The limiter of the aim IK (CPedIK, PedIK.cpp): the machinery that walks a
// limb - the torso, the lean of a driver, the head, an arm - from where it is
// toward the angle the aim asks for, and the row of limits and rates that
// bounds it (the limb rate of GLOSSARY.md). The module is engine free - it
// includes no engine header - so the harness (utils/aim-ik-rate/rate.cpp)
// compiles it alone and holds the rows to their unit and their stock values
// (VICE-194).
//
// The rates are radians per second. The stock rows advanced a fixed angle per
// call, and one call is one frame of the engine's reference frame rate, 50
// frames per second, so every rate here is the stock per-call angle times 50
// and the limiter scales it by the frame step its caller hands in. A limb
// closes on its target in the same time at any frame rate.

// The angles one limb holds, in the body's own frame, radians.
struct LimbOrientation
{
	float yaw;
	float pitch;
};

// The limits and the rates of one limb. The limits are angles in radians, the
// rates radians per second; both are signed the way the limb is (max up,
// min down).
struct LimbMovementInfo {
	float maxYaw;
	float minYaw;
	float yawD;
	float maxPitch;
	float minPitch;
	float pitchD;
};

enum LimbMoveStatus {
	ANGLES_SET_TO_MAX, // because given angles were unreachable
	ONE_ANGLE_COULDNT_BE_SET_EXACTLY, // because it can't be reached in a jiffy
	ANGLES_SET_EXACTLY
};

// One step of the limiter: walk the limb toward the target by the row's rates
// over dt seconds, cap it to the row's limits and report how it ended. dt is
// the frame step in seconds (CTimer::GetTimeStepInSeconds at the call site),
// so the angular speed no longer follows the frame rate.
LimbMoveStatus MoveLimbStep(LimbOrientation &limb, float targetYaw, float targetPitch, const LimbMovementInfo &moveInfo, float dt);

// The rows, one per limb of the aim (LimbRate.cpp). ms_ is the module scope,
// the same prefix the rest of the tree uses for one.
extern LimbMovementInfo ms_torsoInfo;
extern LimbMovementInfo ms_seatTorsoInfo;
extern LimbMovementInfo ms_leanInfo;
extern LimbMovementInfo ms_headInfo;
extern LimbMovementInfo ms_headRestoreInfo;
extern LimbMovementInfo ms_upperArmInfo;
extern LimbMovementInfo ms_lowerArmInfo;

#endif // __GTA_LIMBRATE_H__
