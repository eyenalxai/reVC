#ifndef __GTA_TEMPCOLMODELS_H__
#define __GTA_TEMPCOLMODELS_H__

#include "ColModel.h"

class CTempColModels
{
public:
	static CColModel ms_colModelPed1;
	// The per-stance ped sets (VICE-178): selected by CPed::GetCollisionColModel.
	// Both live at LEVEL_GENERIC with ownsCollisionVolumes = false, exactly like
	// ms_colModelPed1, so CColStore::RemoveCol can never free them.
	static CColModel ms_colModelPedCrouch;
	static CColModel ms_colModelPedProne;
	static CColModel ms_colModelPed2;
	static CColModel ms_colModelBBox;
	static CColModel ms_colModelBumper1;
	static CColModel ms_colModelWheel1;	
	static CColModel ms_colModelPanel1;
	static CColModel ms_colModelBodyPart2;
	static CColModel ms_colModelBodyPart1;
	static CColModel ms_colModelCutObj[5];
	static CColModel ms_colModelPedGroundHit;
	static CColModel ms_colModelBoot1;
	static CColModel ms_colModelDoor1;
	static CColModel ms_colModelBonnet1;
	static CColModel ms_colModelWeapon;

	static void Initialise(void);
};

#endif // __GTA_TEMPCOLMODELS_H__
