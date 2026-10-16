#ifndef __GTA_OBJECT_H__
#define __GTA_OBJECT_H__

#include "Physical.h"

enum {
	UNKNOWN_OBJECT = 0,
	GAME_OBJECT = 1,
	MISSION_OBJECT = 2,
	TEMP_OBJECT = 3,
	CUTSCENE_OBJECT = 4,
	CONTROLLED_SUB_OBJECT = 5,
};

enum CollisionSpecialResponseCase
{
	COLLRESPONSE_NONE,
	COLLRESPONSE_LAMPOST,
	COLLRESPONSE_SMALLBOX,
	COLLRESPONSE_BIGBOX,
	COLLRESPONSE_FENCEPART,
	COLLRESPONSE_UNKNOWN5
};

enum CollisionDamageEffect
{
	DAMAGE_EFFECT_NONE,
	DAMAGE_EFFECT_CHANGE_MODEL,
	DAMAGE_EFFECT_SPLIT_MODEL,
	DAMAGE_EFFECT_SMASH_AND_DAMAGE_TRAFFICLIGHTS,

	DAMAGE_EFFECT_SMASH_COMPLETELY = 20,
	DAMAGE_EFFECT_CHANGE_THEN_SMASH,

	DAMAGE_EFFECT_SMASH_CARDBOARD_COMPLETELY = 50,
	DAMAGE_EFFECT_SMASH_YELLOW_TARGET_COMPLETELY = 51,

	DAMAGE_EFFECT_SMASH_WOODENBOX_COMPLETELY = 60,
	DAMAGE_EFFECT_SMASH_TRAFFICCONE_COMPLETELY = 70,
	DAMAGE_EFFECT_SMASH_BARPOST_COMPLETELY = 80,

	DAMAGE_EFFECT_SMASH_NEWSTANDNEW1 = 91,
	DAMAGE_EFFECT_SMASH_NEWSTANDNEW2 = 92,
	DAMAGE_EFFECT_SMASH_NEWSTANDNEW3 = 93,
	DAMAGE_EFFECT_SMASH_NEWSTANDNEW4 = 94,
	DAMAGE_EFFECT_SMASH_NEWSTANDNEW5 = 95,

	DAMAGE_EFFECT_SMASH_BLACKBAG = 100,
	DAMAGE_EFFECT_SMASH_VEGPALM = 110,
	DAMAGE_EFFECT_BURST_BEACHBALL = 120,
	DAMAGE_EFFECT_SMASH_BEACHLOUNGE_WOOD = 131,
	DAMAGE_EFFECT_SMASH_BEACHLOUNGE_TOWEL = 132,
};

class CVehicle;
class CDummyObject;

class CObject : public CPhysical
{
public:
	CMatrix m_objectMatrix;
	float m_fUprootLimit;
	int8 ObjectCreatedBy;
	uint8 bIsPickup : 1;
	uint8 bAmmoCollected : 1;
	uint8 bPickupObjWithMessage : 1;
	uint8 bOutOfStock : 1;
	uint8 bGlassCracked : 1;
	uint8 bGlassBroken : 1;
	uint8 bHasBeenDamaged : 1;
	uint8 bUseVehicleColours : 1;
	uint8 bIsWeapon : 1;
	uint8 bIsStreetLight : 1;
	uint8 bIsFelled : 1;
	int8 m_nBonusValue;
	uint16 m_nCostValue;
	float m_fCollisionDamageMultiplier;
	uint8 m_nCollisionDamageEffect;
	uint8 m_nSpecialCollisionResponseCases;
	bool m_bCameraToAvoidThisObject;
	uint8 m_nBeachballBounces;
	uint32 m_obj_unused1;
	uint32 m_nEndOfLifeTime;
	int16 m_nRefModelIndex;
	CEntity *m_pCurSurface;
	CEntity *m_pCollidingEntity;
	int8 m_colour1, m_colour2;

	static int16 nNoTempObjects;
	static float fDistToNearestTree;

	static void *operator new(size_t) throw();
	static void *operator new(size_t, int) throw();
	static void operator delete(void*, size_t) throw();
	static void operator delete(void*, int) throw();

	CObject(void);
	CObject(int32, bool);
	CObject(CDummyObject*);
	~CObject(void);

	void ProcessControl(void);
	void Teleport(CVector vecPos);
	void Render(void);
	bool SetupLighting(void);
	void RemoveLighting(bool reset);

	void ObjectDamage(float amount);
	void RefModelInfo(int32 modelId);
	void Init(void);
	bool CanBeDeleted(void);

	static void DeleteAllMissionObjects();
	static void DeleteAllTempObjects();
	static void DeleteAllTempObjectsInArea(CVector point, float fRadius);
};

// Impulse that breaks vegetation (a palm object or a vegetation building),
// much lower than the 150 threshold general objects need.
#define VEGETATION_SMASH_IMPULSE 60.0f

// A felled trunk plant (a tree or a palm) topples like a light pole: finite
// mass, no uproot limit and a push at the contact point. A plant model has no
// OBJECT.DAT row, so the fall sets every physical field itself.
#define VEGETATION_FALL_MASS 700.0f
#define VEGETATION_FALL_TURN_MASS 5000.0f
#define VEGETATION_FALL_AIR_RESISTANCE 0.99f
#define VEGETATION_FALL_ELASTICITY 0.05f
// The object data buoyancy, 100/percentSubmerged * GRAVITY * mass, with the
// submerged part of the vegetation rows.
#define VEGETATION_FALL_PERCENT_SUBMERGED 120.0f
#define VEGETATION_FALL_BUOYANCY (100.0f/VEGETATION_FALL_PERCENT_SUBMERGED * GRAVITY * VEGETATION_FALL_MASS)
// The share of the hit impulse that tips the plant over.
#define VEGETATION_FALL_PUSH_SCALE 0.5f

// Smashes any vegetation entity: hides it, drops its collision and spawns the
// palm debris and sound. One implementation for palms and vegetation buildings.
void SmashVegetation(CEntity *entity);

// Tips a trunk plant over in place instead of hiding it: it stays visible and
// colliding and falls like a light pole. hitPos is the contact point (a zero
// point is lifted to half the model height), hitDir the direction the plant
// falls and impulse the hit strength. A vegetation building leaves an object
// copy in its place and is only hidden: the caller keeps using the entity.
void FellVegetation(CEntity *entity, const CVector &hitPos, const CVector &hitDir, float impulse);

bool IsObjectPointerValid(CObject* pObject);

#endif // __GTA_OBJECT_H__
