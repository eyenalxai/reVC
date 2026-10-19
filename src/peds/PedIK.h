#ifndef __GTA_PEDIK_H__
#define __GTA_PEDIK_H__
#include "common.h"
#include "AnimBlendClumpData.h"
// The limb orientations, the limiter rows and the limiter step itself; the
// engine free module (LimbRate.cpp) so the harness can drive it (VICE-194).
#include "LimbRate.h"

class CPed;

class CPedIK
{
public:
	enum {
		GUN_POINTED_SUCCESSFULLY = 1,
		LOOKAROUND_HEAD_ONLY = 2,
		AIMS_WITH_ARM = 4,
	};

	CPed *Const m_ped;
	LimbOrientation m_headOrient;
	LimbOrientation m_torsoOrient;
	LimbOrientation m_upperArmOrient;
	LimbOrientation m_lowerArmOrient;
	// Sideways lean of the upper body out of the car; yaw is the lean angle
	LimbOrientation m_leanOrient;
	int32 m_flags;

	CPedIK(CPed *ped);
	bool PointGunInDirection(float targetYaw, float targetPitch);
	bool PointGunInDirectionUsingArm(float targetYaw, float targetPitch);
	bool PointGunAtPosition(CVector const& position);
	void GetComponentPosition(RwV3d &pos, uint32 node);
	void RotateTorso(AnimBlendFrameData* animBlend, LimbOrientation* limb, bool changeRoll);
	LimbMovementInfo &TorsoInfo(void);
	bool SeatedInCar(void);
	void GetTorsoBendAxis(RwV3d &axis);
	void RotateLean(RtQuat *q, const RwV3d &bendAxis);
	void ExtractYawAndPitchLocal(RwMatrix *mat, float *yaw, float *pitch);
	void ExtractYawAndPitchLocalSkinned(AnimBlendFrameData *node, float *yaw, float *pitch);
	void ExtractYawAndPitchWorld(RwMatrix *mat, float *yaw, float *pitch);
	LimbMoveStatus MoveLimb(LimbOrientation &limb, float targetYaw, float targetPitch, LimbMovementInfo &moveInfo);
	bool RestoreGunPosn(void);
	bool LookInDirection(float targetYaw, float targetPitch);
	bool LookAtPosition(CVector const& pos);
	bool RestoreLookAt(void);
};

VALIDATE_SIZE(CPedIK, 0x30);

#endif // __GTA_PEDIK_H__
