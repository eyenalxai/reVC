#include "common.h"

#include "main.h"
#include "Lights.h"
#include "Pools.h"
#include "Radar.h"
#include "Object.h"
#include "DummyObject.h"
#include "ModelIndices.h"
#include "Particle.h"
#include "General.h"
#include "ObjectData.h"
#include "World.h"
#include "Floater.h"
#include "soundlist.h"
#include "WaterLevel.h"
#include "Timecycle.h"
#include "Stats.h"
#include "SpecialFX.h"

#define BEACHBALL_MAX_SCORE 250
// the proportion of the ball speed compared to the player speed when it hits the player
#define BEACHBALL_SPEED_PROPORTION 0.4f
// the particles a fell scatters; a smash throws more of them
#define VEGETATION_FALL_DEBRIS_PARTICLES 16

int16 CObject::nNoTempObjects;
//int16 CObject::nBodyCastHealth = 1000;
float CObject::fDistToNearestTree;

// Object pools tends to be full sometimes, let's free a temp. object in this case.
#ifdef FIX_BUGS
void *CObject::operator new(size_t sz) throw() {
	CObject *obj = CPools::GetObjectPool()->New();
	if (!obj) {
		CObjectPool *objectPool = CPools::GetObjectPool();
		for (int32 i = 0; i < objectPool->GetSize(); i++) {
			CObject *existing = objectPool->GetSlot(i);
			if (existing && existing->ObjectCreatedBy == TEMP_OBJECT) {
				int32 handle = objectPool->GetIndex(existing);
				CWorld::Remove(existing);
				delete existing;
				obj = objectPool->New(handle);
				break;
			}
		}
	}
	return obj;
}
#else
void *CObject::operator new(size_t sz) throw() { return CPools::GetObjectPool()->New(); }
#endif
void *CObject::operator new(size_t sz, int handle) throw() { return CPools::GetObjectPool()->New(handle); };

void CObject::operator delete(void *p, size_t sz) throw() { CPools::GetObjectPool()->Delete((CObject*)p); }
void CObject::operator delete(void *p, int handle) throw() { CPools::GetObjectPool()->Delete((CObject*)p); }

CObject::CObject(void)
{
	m_type = ENTITY_TYPE_OBJECT;
	m_fUprootLimit = 0.0f;
	m_nCollisionDamageEffect = 0;
	m_nSpecialCollisionResponseCases = COLLRESPONSE_NONE;
	m_bCameraToAvoidThisObject = false;
	ObjectCreatedBy = UNKNOWN_OBJECT;
	m_nEndOfLifeTime = 0;
	//	m_nRefModelIndex = -1;	// duplicate
	//	bUseVehicleColours = false;	// duplicate
	m_colour2 = 0;
	m_colour1 = m_colour2;
	m_nBonusValue = 0;
	m_nCostValue = 0;
	bIsPickup = false;
	bPickupObjWithMessage = false;
	bOutOfStock = false;
	bGlassCracked = false;
	bGlassBroken = false;
	bHasBeenDamaged = false;
	m_nRefModelIndex = -1;
	bUseVehicleColours = false;
	//	bIsStreetLight = false;		// duplicate
	m_pCurSurface = nil;
	m_pCollidingEntity = nil;
	m_nBeachballBounces = 0;
	bIsStreetLight = false;
	bIsFelled = false;
	m_area = AREA_EVERYWHERE;
}

CObject::CObject(int32 mi, bool createRW)
{
	if (createRW)
		SetModelIndex(mi);
	else
		SetModelIndexNoCreate(mi);
	Init();
}

// Gives a felled plant the physics it topples with again; the helper is defined
// with the other vegetation code below. The restore path needs it right here.
static void SetVegetationFallPhysics(CObject *object);

CObject::CObject(CDummyObject *dummy)
{
	SetModelIndexNoCreate(dummy->GetModelIndex());

	if (dummy->m_rwObject)
		AttachToRwObject(dummy->m_rwObject);
	else
		SetMatrix(dummy->GetMatrix());

	m_objectMatrix = dummy->GetMatrix();
	dummy->DetachFromRwObject();
	Init();
	// Get the state a converted object had back, so a smashed or damaged object
	// stays that way over the 80 m dummy round trip. The dummy keeps invisibility
	// itself, a hidden object (smashed or glass) stays hidden. Map dummies carry
	// no state and leave the object's collision and damage flags as Init() made
	// them. bHasBeenDamaged has no field on the dummy, so it is lost here.
	bIsVisible = dummy->bIsVisible;
	if (dummy->bConvertedFromObject) {
		bUsesCollision = dummy->bObjectUsesCollision;
		bRenderDamaged = dummy->bObjectRenderDamaged;
		bExplosionProof = dummy->bObjectExplosionProof;
		// A plant that fell comes back fallen. Init() restored the OBJECT.DAT
		// row (an infinite-mass palm) or left a building copy at whatever the
		// pool slot held, so the fall physics go on top. The object stays
		// non-static and CWorld::Add puts it on the moving list itself.
		if (dummy->bObjectFelled) {
			SetVegetationFallPhysics(this);
			bIsFelled = true;
		}
	}
	m_level = dummy->m_level;
	m_area = dummy->m_area;
}

CObject::~CObject(void)
{
	CRadar::ClearBlipForEntity(BLIP_OBJECT, CPools::GetObjectPool()->GetIndex(this));

	if (m_nRefModelIndex != -1)
		CModelInfo::GetModelInfo(m_nRefModelIndex)->RemoveRef();

	if (ObjectCreatedBy == TEMP_OBJECT && nNoTempObjects != 0)
		nNoTempObjects--;
}

void
CObject::ProcessControl(void)
{
	CVector point, impulse;
	if (m_nCollisionDamageEffect)
		ObjectDamage(m_fDamageImpulse);
	CPhysical::ProcessControl();
	if (mod_Buoyancy.ProcessBuoyancy(this, m_fBuoyancy, &point, &impulse)) {
		bIsInWater = true;
		SetIsStatic(false);
		ApplyMoveForce(impulse);
		ApplyTurnForce(impulse, point);
		float fTimeStep = Pow(0.97f, CTimer::GetTimeStep());
		m_vecMoveSpeed *= fTimeStep;
		m_vecTurnSpeed *= fTimeStep;
	}
	int16 mi = GetModelIndex();
	if ((mi == MI_EXPLODINGBARREL || mi == MI_PETROLPUMP || mi == MI_PETROLPUMP2) && bHasBeenDamaged && bIsVisible
		&& (CGeneral::GetRandomNumber() & 0x1F) == 10) {
		bExplosionProof = true;
		bIsVisible = false;
		bUsesCollision = false;
		bAffectedByGravity = false;
		m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
	}
	if (mi == MI_RCBOMB) {
		float fTurnForce = -(m_fTurnMass / 20.0f);
		CPhysical::ApplyTurnForce(m_vecMoveSpeed * fTurnForce, -GetForward());
		float fScalar = 1.0f - m_vecMoveSpeed.MagnitudeSqr() / 5.0f;
		float fScalarTimed = Pow(fScalar, CTimer::GetTimeStep());
		m_vecMoveSpeed *= fScalarTimed;
	}
	if (mi == MI_BEACHBALL) {
		float fTimeStep = Pow(0.95f, CTimer::GetTimeStep());
		float fPreviousVecSpeedMag = m_vecMoveSpeed.Magnitude2D();
		m_vecMoveSpeed.x *= fTimeStep;
		m_vecMoveSpeed.y *= fTimeStep;
		m_vecMoveSpeed.z += fPreviousVecSpeedMag - m_vecMoveSpeed.Magnitude2D();
		if (!FindPlayerVehicle()) {
			CVector distance;
			distance.x = FindPlayerCoors().x - GetPosition().x;
			distance.y = FindPlayerCoors().y - GetPosition().y;
			distance.z = FindPlayerCoors().z - GetPosition().z;
			if (distance.z > 0.0 && distance.z < 1.5f && distance.Magnitude2D() < 1.0f) {
				CVector playerSpeed = FindPlayerSpeed();
				if (fPreviousVecSpeedMag < 0.05f && playerSpeed.Magnitude() > 0.1f) {
					playerSpeed.z = 0.0f;
					playerSpeed.Normalise();
					playerSpeed.z = 0.3f;
					m_vecMoveSpeed = CVector(
						playerSpeed.x * BEACHBALL_SPEED_PROPORTION,
						playerSpeed.y * BEACHBALL_SPEED_PROPORTION,
						0.3f          * BEACHBALL_SPEED_PROPORTION
					);
					PlayOneShotScriptObject(SCRIPT_SOUND_HIT_BALL, GetPosition());
					m_vecTurnSpeed += CVector(
						((CGeneral::GetRandomNumber() % 16) - 7) / 10.0f,
						((CGeneral::GetRandomNumber() % 16) - 7) / 10.0f,
						0.0f);
					if (m_nBeachballBounces > 0) {
						m_nBeachballBounces++;
					}
					if (m_nBeachballBounces > 0) {
						sprintf(gString, "%d", m_nBeachballBounces);
						CMoneyMessages::RegisterOne(GetPosition(), gString, 255, 50, 0, 0.6f, 0.5f);
						CStats::RegisterHighestScore(3, m_nBeachballBounces);
					}
				}
			}
			if (distance.z > -1.05 && distance.z < -0.6 && m_vecMoveSpeed.z < 0.0f && distance.Magnitude2D() < 0.9f) {
				m_vecMoveSpeed.x += (CGeneral::GetRandomNumber() % 8 - 3) / 100.0f;
				m_vecMoveSpeed.y += (CGeneral::GetRandomNumber() % 8 - 3) / 100.0f;
				m_vecMoveSpeed.z = Max(m_vecMoveSpeed.z + 0.3f, 0.2f);
				PlayOneShotScriptObject(SCRIPT_SOUND_HIT_BALL, GetPosition());
				m_vecTurnSpeed.x += (CGeneral::GetRandomNumber() % 16 - 7) / 10.0f;
				m_vecTurnSpeed.y += (CGeneral::GetRandomNumber() % 16 - 7) / 10.0f;
				m_nBeachballBounces++;
				m_nBeachballBounces = Min(m_nBeachballBounces, BEACHBALL_MAX_SCORE);
				sprintf(gString, "%d", m_nBeachballBounces);
				CMoneyMessages::RegisterOne(GetPosition(), gString, 255, 50, 0, 0.6f, 0.5f);
				CStats::RegisterHighestScore(3, m_nBeachballBounces);
			}
		}
	}
	if (bIsBIGBuilding) {
		bIsInSafePosition = true;
	}
	// A felled plant keeps its fall pose in the object matrix: the physics move
	// only the placeable matrix, and the 80 m dummy round trip rebuilds the
	// plant from the object matrix. Without the copy the plant comes back
	// standing up at its placement.
	if (bIsFelled)
		m_objectMatrix = GetMatrix();
}

void
CObject::Teleport(CVector vecPos)
{
	CWorld::Remove(this);
	GetMatrix().GetPosition() = vecPos;
	GetMatrix().UpdateRW();
	UpdateRwFrame();
	CWorld::Add(this);
}

void
CObject::Render(void)
{
	if (bDoNotRender)
		return;

	if (m_nRefModelIndex != -1 && ObjectCreatedBy == TEMP_OBJECT && bUseVehicleColours) {
		CVehicleModelInfo *mi = (CVehicleModelInfo*)CModelInfo::GetModelInfo(m_nRefModelIndex);
		assert(mi->GetModelType() == MITYPE_VEHICLE);
		mi->SetVehicleColour(m_colour1, m_colour2);
	}

	float red = (0.8f * CTimeCycle::GetDirectionalRed() + CTimeCycle::GetAmbientRed_Obj()) * 165.75f;
	float green = (0.8f * CTimeCycle::GetDirectionalGreen() + CTimeCycle::GetAmbientGreen_Obj()) * 165.75f;
	float blue = (0.8f * CTimeCycle::GetDirectionalBlue() + CTimeCycle::GetAmbientBlue_Obj()) * 165.75f;

	red = Clamp(red, 0.0f, 255.0f);
	green = Clamp(green, 0.0f, 255.0f);
	blue = Clamp(blue, 0.0f, 255.0f);

	int alpha = CGeneral::GetRandomNumberInRange(196, 225);

	RwRGBA color = { (uint8)red, (uint8)green, (uint8)blue, (uint8)alpha };

	if (this->GetModelIndex() == MI_YT_MAIN_BODY) {
		float moveSpeedMagnitude = this->GetMoveSpeed().Magnitude();
		if (moveSpeedMagnitude > 0.0f) {
			float scaleMax = GetColModel()->boundingBox.max.y * 0.85f;

			CVector dir = this->GetMoveSpeed() + 0.3f * this->GetRight() - 0.5f * this->GetForward();
			dir.z += 0.05f * moveSpeedMagnitude;

			CVector pos = scaleMax * this->GetForward() + 2.25f * this->GetRight() + this->GetPosition();

			float fWaterLevel;
			CWaterLevel::GetWaterLevel(pos.x, pos.y, pos.z, &fWaterLevel, true);
			pos.z = fWaterLevel + 0.75f;

			CParticle::AddParticle(PARTICLE_BOAT_SPLASH, pos, dir, nil, 1.2f * moveSpeedMagnitude, color,
				CGeneral::GetRandomNumberInRange(0.0f, 0.4f), CGeneral::GetRandomNumberInRange(0.0f, 45.0f), 0, 0);

			float scaleMin = GetColModel()->boundingBox.min.y;

			dir = this->GetMoveSpeed() - 0.5f * this->GetForward();
			dir.z += 0.05f * moveSpeedMagnitude;

			pos = scaleMin * this->GetForward() + 4.5f * this->GetRight() + this->GetPosition();

			CWaterLevel::GetWaterLevel(pos.x, pos.y, pos.z, &fWaterLevel, true);
			pos.z = fWaterLevel + 0.55f;

			CParticle::AddParticle(PARTICLE_BOAT_SPLASH, pos, dir, nil, 0.9f, color,
				CGeneral::GetRandomNumberInRange(0.0f, 0.4f), CGeneral::GetRandomNumberInRange(0.0f, 45.0f), 0, 0);

			pos = scaleMin * 1.1f * this->GetForward() + 2.25f * this->GetRight() + this->GetPosition();

			CWaterLevel::GetWaterLevel(pos.x, pos.y, pos.z, &fWaterLevel, true);
			pos.z = fWaterLevel + 0.55f;

			CParticle::AddParticle(PARTICLE_BOAT_SPLASH, pos, dir, nil, 0.9f, color,
				CGeneral::GetRandomNumberInRange(0.0f, 0.4f), CGeneral::GetRandomNumberInRange(0.0f, 45.0f), 0, 0);

			pos = scaleMin * 1.1f * this->GetForward() - 0.05f * this->GetRight() + this->GetPosition();

			CWaterLevel::GetWaterLevel(pos.x, pos.y, pos.z, &fWaterLevel, true);
			pos.z = fWaterLevel + 0.55f;

			CParticle::AddParticle(PARTICLE_BOAT_SPLASH, pos, dir, nil, 0.9f, color,
				CGeneral::GetRandomNumberInRange(0.0f, 0.4f), CGeneral::GetRandomNumberInRange(0.0f, 45.0f), 0, 0);
		}
	}

	if (this->GetModelIndex() == MI_YT_MAIN_BODY2) {
		float moveSpeedMagnitude = this->GetMoveSpeed().Magnitude();
		if (moveSpeedMagnitude > 0.0f) {
			float scaleMax = GetColModel()->boundingBox.max.y * 0.85f;

			CVector dir = this->GetMoveSpeed() - 0.3f * this->GetRight() - 0.5f * this->GetForward();
			dir.z += 0.05f * moveSpeedMagnitude;

			CVector pos = scaleMax * this->GetForward() - 2.25f * this->GetRight() + this->GetPosition();

			float fWaterLevel;
			CWaterLevel::GetWaterLevel(pos.x, pos.y, pos.z, &fWaterLevel, true);
			pos.z = fWaterLevel + 0.75f;

			CParticle::AddParticle(PARTICLE_BOAT_SPLASH, pos, dir, nil, 1.2f * moveSpeedMagnitude, color,
				CGeneral::GetRandomNumberInRange(0.0f, 0.4f), CGeneral::GetRandomNumberInRange(0.0f, 45.0f), 0, 0);

			float scaleMin = GetColModel()->boundingBox.min.y;

			dir = this->GetMoveSpeed() - 0.5f * this->GetForward();
			dir.z += 0.05f * moveSpeedMagnitude;

			pos = scaleMin * this->GetForward() - 4.5f * this->GetRight() + this->GetPosition();

			CWaterLevel::GetWaterLevel(pos.x, pos.y, pos.z, &fWaterLevel, true);
			pos.z = fWaterLevel + 0.55f;

			CParticle::AddParticle(PARTICLE_BOAT_SPLASH, pos, dir, nil, 0.9f, color,
				CGeneral::GetRandomNumberInRange(0.0f, 0.4f), CGeneral::GetRandomNumberInRange(0.0f, 45.0f), 0, 0);

			pos = scaleMin * 1.1f * this->GetForward() - 2.25f * this->GetRight() + this->GetPosition();

			CWaterLevel::GetWaterLevel(pos.x, pos.y, pos.z, &fWaterLevel, true);
			pos.z = fWaterLevel + 0.55f;

			CParticle::AddParticle(PARTICLE_BOAT_SPLASH, pos, dir, nil, 0.9f, color,
				CGeneral::GetRandomNumberInRange(0.0f, 0.4f), CGeneral::GetRandomNumberInRange(0.0f, 45.0f), 0, 0);
		}
	}

	CEntity::Render();
}

bool
CObject::SetupLighting(void)
{
	if (bRenderScorched) {
		WorldReplaceNormalLightsWithScorched(Scene.world, 0.1f);
		return true;
	} else if (bIsPickup) {
		SetFullAmbient();
		return true;
	} else if (bIsWeapon) {
		ActivateDirectional();
		SetAmbientColoursForPedsCarsAndObjects();
		return true;
	}
	return false;
}

void
CObject::RemoveLighting(bool reset)
{
	if (reset) {
		SetAmbientColours();
		DeActivateDirectional();
	}
}

// The debris burst and sound a vegetation entity gives off, shared by the
// smash and the fall. The fall scatters fewer particles than the smash.
static void
SpawnVegetationDebris(const CVector &vecPos, float fRadius, int32 nParticles)
{
	static RwRGBA primaryColor1 = { 0x39, 0x4D, 0x29, 0xff };
	static RwRGBA primaryColor2 = { 0x94, 0x7D, 0x73, 0xff };
	static int8 nFrameGen = 0;

	for (int32 i = 0; i < nParticles; i++) {
		CVector particleDir = CVector(
			CGeneral::GetRandomNumberInRange(-0.25f, 0.25f),
			CGeneral::GetRandomNumberInRange(-0.25f, 0.25f),
			CGeneral::GetRandomNumberInRange(-0.05f, 0.05f)
		);
		CVector particlePos = vecPos;
		particlePos.z += CGeneral::GetRandomNumberInRange(0.0f, 1.0f) * fRadius;
		++nFrameGen;
		int32 nRotationSpeed = CGeneral::GetRandomNumberInRange(-40, 40);
		int32 nCurFrame = nFrameGen & 0x3;
		float fSize = CGeneral::GetRandomNumberInRange(0.02f, 0.20f);
		RwRGBA& particleColor = nFrameGen & 1 ? primaryColor1 : primaryColor2;
		CParticle::AddParticle(PARTICLE_CAR_DEBRIS, particlePos, particleDir, nil, fSize, particleColor, nRotationSpeed, 0, nCurFrame, 0);
		if ((i % 7) == 0) {
			static RwRGBA secondaryColor = { 0x9A, 0x99, 0x99, 0x3E };
			CParticle::AddParticle(PARTICLE_DEBRIS, particlePos, particleDir, nil, 0.3f, secondaryColor, nRotationSpeed);
		}
	}
	PlayOneShotScriptObject(SCRIPT_SOUND_BOX_DESTROYED_2, vecPos);
}

// Breaks any vegetation entity: hides it, drops its collision and spawns the
// palm debris and sound. Palms are objects, vegetation buildings are not, so
// the moving-list and speed reset only apply to the physical ones.
void
SmashVegetation(CEntity *entity)
{
	entity->bIsVisible = false;
	entity->bUsesCollision = false;
	if (entity->IsObject()) {
		CObject *object = (CObject*)entity;
		if (!object->GetIsStatic())
			object->RemoveFromMovingList();
		object->SetMoveSpeed(0.0f, 0.0f, 0.0f);
		object->SetTurnSpeed(0.0f, 0.0f, 0.0f);
	}
	entity->SetIsStatic(true);
	entity->bExplosionProof = true;

	const CVector &vecPos = entity->GetPosition();
	float fRadius = entity->GetColModel()->boundingSphere.radius;
	SpawnVegetationDebris(vecPos, fRadius, 32);
}

// Gives a trunk plant the physics it topples with. A plant model has no
// OBJECT.DAT row, so nothing fills these fields by itself, and Init() must not
// run either: it would reset the state the plant just got.
static void
SetVegetationFallPhysics(CObject *object)
{
	object->m_fMass = VEGETATION_FALL_MASS;
	object->m_fTurnMass = VEGETATION_FALL_TURN_MASS;
	object->m_fAirResistance = VEGETATION_FALL_AIR_RESISTANCE;
	object->m_fElasticity = VEGETATION_FALL_ELASTICITY;
	object->m_fBuoyancy = VEGETATION_FALL_BUOYANCY;
	object->m_fUprootLimit = 0.0f;
	object->m_fCollisionDamageMultiplier = 1.0f;
	object->m_nCollisionDamageEffect = DAMAGE_EFFECT_NONE;
	object->m_nSpecialCollisionResponseCases = COLLRESPONSE_NONE;
	object->m_bCameraToAvoidThisObject = false;
	object->bInfiniteMass = false;
	object->m_phy_flagA08 = false;
	object->bAffectedByGravity = true;
	object->bExplosionProof = false;
	object->SetIsStatic(false);
}

// Pushes a felled plant over with the hit impulse, the same way a collision
// turns a light pole. The force acts at the contact point, so the plant gains
// torque; a hit without a contact point is lifted to half the model height so
// the fall does not become a slide.
static void
PushFelledVegetation(CPhysical *plant, const CVector &hitPos, const CVector &hitDir, float impulse)
{
	CVector point = hitPos - plant->GetPosition();
	if (point.IsZero())
		point.z = 0.5f * plant->GetColModel()->boundingBox.max.z;

	const CVector force = hitDir * (impulse * VEGETATION_FALL_PUSH_SCALE);
	plant->ApplyMoveForce(force);
	plant->ApplyTurnForce(force, point);
}

// Tips a trunk plant over in place instead of hiding it. A palm object only
// becomes physical; a vegetation building is a CBuilding without physics, so a
// dynamic object copy takes its place. The building is not deleted: this runs
// inside the collision loop and the caller keeps reading the entity.
void
FellVegetation(CEntity *entity, const CVector &hitPos, const CVector &hitDir, float impulse)
{
	CVector dir = hitDir;
	dir.Normalise();

	CObject *plant;
	if (entity->IsObject()) {
		plant = (CObject*)entity;
		// Only a plant that still stands falls. The damage effect is cleared
		// with the fall and marks it, so the other contact points of the same
		// hit do not fell it a second time.
		if (plant->m_nCollisionDamageEffect != DAMAGE_EFFECT_SMASH_VEGPALM)
			return;
		SetVegetationFallPhysics(plant);
		plant->AddToMovingList();
	} else {
		// A plant building has no physics. The object copy takes its matrix
		// and its RW object, so the plant does not blink out of view. A full
		// object pool leaves the plant standing.
		plant = new CObject;
		if (plant == nil)
			return;
		plant->SetModelIndexNoCreate(entity->GetModelIndex());
		plant->SetMatrix(entity->GetMatrix());
		if (entity->m_rwObject) {
			plant->AttachToRwObject(entity->m_rwObject);
			entity->DetachFromRwObject();
		}
		plant->m_objectMatrix = plant->GetMatrix();
		SetVegetationFallPhysics(plant);
		plant->ObjectCreatedBy = GAME_OBJECT;
		plant->bIsVisible = true;
		plant->bUsesCollision = true;
		plant->m_level = entity->m_level;
		plant->m_area = entity->m_area;

		entity->bIsVisible = false;
		entity->bUsesCollision = false;
		entity->bExplosionProof = true;
		CWorld::Add(plant);
	}

	plant->bIsFelled = true;
	PushFelledVegetation(plant, hitPos, dir, impulse);
	const CVector &debrisPos = hitPos.IsZero() ? entity->GetPosition() : hitPos;
	SpawnVegetationDebris(debrisPos, entity->GetColModel()->boundingSphere.radius, VEGETATION_FALL_DEBRIS_PARTICLES);
}

void
CObject::ObjectDamage(float amount)
{
	if (!m_nCollisionDamageEffect || !bUsesCollision)
		return;
	static int8 nFrameGen = 0;
	bool bBodyCastDamageEffect = false;
#if 0
	if (GetModelIndex() == MI_BODYCAST) {
		if (amount > 50.0f)
			nBodyCastHealth = (int16)(nBodyCastHealth - 0.5f * amount);
		if (nBodyCastHealth < 0)
			nBodyCastHealth = 0;
		if (nBodyCastHealth < 200)
			bBodyCastDamageEffect = true;
		amount = 0.0f;
	}
#endif
	// Vegetation breaks at a low impulse of its own, not at the general 150
	// threshold: palms have an infinite mass and a 0.5 damage multiplier.
	const bool bBreakVegetation = m_nCollisionDamageEffect == DAMAGE_EFFECT_SMASH_VEGPALM
		&& amount > VEGETATION_SMASH_IMPULSE;
	if ((amount * m_fCollisionDamageMultiplier > 150.0f || bBodyCastDamageEffect || bBreakVegetation) && m_nCollisionDamageEffect) {
		const CVector& vecPos = GetMatrix().GetPosition();
		const float fDirectionZ = 0.0002f * amount;
		switch (m_nCollisionDamageEffect) {
			case DAMAGE_EFFECT_CHANGE_MODEL:
			case DAMAGE_EFFECT_CHANGE_THEN_SMASH: {
				// Both effects swap to the damaged model on the first hit;
				// the next hit falls through and smashes the object.
				if (!bRenderDamaged) {
					bRenderDamaged = true;
					return;
				}
				// fall through
			}
			case DAMAGE_EFFECT_SMASH_AND_DAMAGE_TRAFFICLIGHTS:
			{
				static RwRGBA debrisColor = { 0xc8,0xc8,0xc8,0xff };
				// The first hit only cracks the lights, the next one smashes them.
				if (!bRenderDamaged) {
					bRenderDamaged = true;
					CVector min = 0.85f * GetColModel()->boundingBox.min;
					CVector max = 0.85f * GetColModel()->boundingBox.max;
					min.z = max.z;
					min = GetMatrix() * min;
					max = GetMatrix() * max;
					CVector temp = (max - min) * 0.02f;
					for (int32 i = 0; i < 50; i++) {
						CVector vecDir = CVector(
							CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
							CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
							CGeneral::GetRandomNumberInRange(0.10f, 0.25f)
						);
						++nFrameGen;
						int32 currentFrame = nFrameGen & 3;
						CVector pos = min + temp * (float)i;
						float fSize = CGeneral::GetRandomNumberInRange(0.02f, 0.20f);
						float fColorFactor = CGeneral::GetRandomNumberInRange(0.6f, 1.2f);
						RwRGBA color = debrisColor;
						color.red *= fColorFactor;
						color.green *= fColorFactor;
						int32 nRotationSpeed = CGeneral::GetRandomNumberInRange(-0.40f, 0.40f);
						CParticle::AddParticle(PARTICLE_CAR_DEBRIS, pos, vecDir, nil, fSize, color, nRotationSpeed, 0, currentFrame, 0);
					}
					PlayOneShotScriptObject(SCRIPT_SOUND_METAL_COLLISION, min);
					return;
				}
				// fall through
			}
			case DAMAGE_EFFECT_SMASH_COMPLETELY: {
				static const RwRGBA color = { 128, 128, 128, 255 };
				bIsVisible = false;
				bUsesCollision = false;
				if (!GetIsStatic()) {
					RemoveFromMovingList();
				}
				SetIsStatic(true);
				bExplosionProof = true;
				SetMoveSpeed(0.0f, 0.0f, 0.0f);
				SetTurnSpeed(0.0f, 0.0f, 0.0f);
				for (int32 i = 0; i < 25; i++) {
					CVector vecDir = CVector(
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(0.10f, 0.25f) + fDirectionZ
					);
					++nFrameGen;
					int32 currentFrame = nFrameGen & 3;
					float fRandom = CGeneral::GetRandomNumberInRange(0.5f, 1.0f);
					RwRGBA randomColor = { uint8(color.red * fRandom), uint8(color.green * fRandom), uint8(color.blue * fRandom), color.alpha };
					float fSize = CGeneral::GetRandomNumberInRange(0.02f, 0.20f);
					int32 nRotationSpeed = CGeneral::GetRandomNumberInRange(-40, 40);
					CParticle::AddParticle(PARTICLE_CAR_DEBRIS, vecPos, vecDir, nil, fSize, randomColor, nRotationSpeed, 0, currentFrame, 0);
				}
				PlayOneShotScriptObject(SCRIPT_SOUND_BOX_DESTROYED_1, vecPos);
				break;
			}
			case DAMAGE_EFFECT_SMASH_CARDBOARD_COMPLETELY:
			case DAMAGE_EFFECT_SMASH_YELLOW_TARGET_COMPLETELY:
			{
				bIsVisible = false;
				bUsesCollision = false;
				if (!GetIsStatic()) {
					RemoveFromMovingList();
				}
				SetIsStatic(true);
				bExplosionProof = true;
				SetMoveSpeed(0.0f, 0.0f, 0.0f);
				SetTurnSpeed(0.0f, 0.0f, 0.0f);
				const RwRGBA color = { 96, 48, 0, 255 };
				for (int32 i = 0; i < 25; i++) {
					CVector vecDir = CVector(
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(0.10f, 0.25f) + fDirectionZ
					);
					++nFrameGen;
					int32 currentFrame = nFrameGen & 3;
					RwRGBA randomColor = color;
					switch (m_nCollisionDamageEffect) {
						case DAMAGE_EFFECT_SMASH_CARDBOARD_COMPLETELY: {
							float fRandom = CGeneral::GetRandomNumberInRange(0.01f, 1.0f);
							randomColor.red *= fRandom;
							randomColor.green *= fRandom;
							randomColor.blue *= fRandom;
							break;
						}
						case DAMAGE_EFFECT_SMASH_YELLOW_TARGET_COMPLETELY: {
							randomColor.red = 0xff;
							randomColor.green = 0xfc;
							break;
						}
					}
					float fSize = CGeneral::GetRandomNumberInRange(0.02f, 0.20f);
					int32 nRotationSpeed = CGeneral::GetRandomNumberInRange(-40, 40);
					CParticle::AddParticle(PARTICLE_CAR_DEBRIS, vecPos, vecDir, nil, fSize, randomColor, nRotationSpeed, 0, currentFrame, 0);
				}
				PlayOneShotScriptObject(SCRIPT_SOUND_BOX_DESTROYED_2, vecPos);
				break;
			}
			case DAMAGE_EFFECT_SMASH_WOODENBOX_COMPLETELY:
			{
				bIsVisible = false;
				bUsesCollision = false;
				if (!GetIsStatic()) {
					RemoveFromMovingList();
				}
				SetIsStatic(true);
				bExplosionProof = true;
				SetMoveSpeed(0.0f, 0.0f, 0.0f);
				SetTurnSpeed(0.0f, 0.0f, 0.0f);
				static const RwRGBA color = { 128, 128, 128, 255 };
				CVector position = GetPosition();
				for (int32 i = 0; i < 45; i++) {
					CVector vecDir = CVector(
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(0.10f, 0.25f) + fDirectionZ
					);
					++nFrameGen;
					int32 currentFrame = nFrameGen & 3;
					float fRandom = CGeneral::GetRandomNumberInRange(0.5f, 1.0f);
					RwRGBA randomColor = { uint8(color.red * fRandom), uint8(color.green * fRandom), uint8(color.blue * fRandom), color.alpha };
					float fSize = CGeneral::GetRandomNumberInRange(0.02f, 0.20f);
					int32 nRotationSpeed = CGeneral::GetRandomNumberInRange(-40, 40);
					CParticle::AddParticle(PARTICLE_CAR_DEBRIS, vecPos, vecDir, nil, fSize, randomColor, nRotationSpeed, 0, currentFrame, 0);
				}
				PlayOneShotScriptObject(SCRIPT_SOUND_BOX_DESTROYED_1, vecPos);
				break;
			}
			case DAMAGE_EFFECT_SMASH_TRAFFICCONE_COMPLETELY:
			case DAMAGE_EFFECT_BURST_BEACHBALL:
			{
				bIsVisible = false;
				bUsesCollision = false;
				if (!GetIsStatic()) {
					RemoveFromMovingList();
				}
				SetIsStatic(true);
				bExplosionProof = true;
				SetMoveSpeed(0.0f, 0.0f, 0.0f);
				SetTurnSpeed(0.0f, 0.0f, 0.0f);
				const RwRGBA color1 = { 200, 0, 0, 255 };
				const RwRGBA color2 = { 200, 200, 200, 255 };
				for (int32 i = 0; i < 10; i++) {
					CVector vecDir = CVector(
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(0.10f, 0.25f) + fDirectionZ
					);
					++nFrameGen;
					int32 currentFrame = nFrameGen & 3;
					RwRGBA color = color2;
					if (nFrameGen & 1)
						color = color1;
					float fSize = CGeneral::GetRandomNumberInRange(0.02f, 0.20f);
					int32 nRotationSpeed = CGeneral::GetRandomNumberInRange(-40, 40);
					CParticle::AddParticle(PARTICLE_CAR_DEBRIS, vecPos, vecDir, nil, fSize, color, nRotationSpeed, 0, currentFrame, 0);
				}
				if (m_nCollisionDamageEffect == DAMAGE_EFFECT_BURST_BEACHBALL) {
					PlayOneShotScriptObject(SCRIPT_SOUND_HIT_BALL, vecPos);
				} else {
					PlayOneShotScriptObject(SCRIPT_SOUND_TIRE_COLLISION, vecPos);
				}
				break;
			}
			case DAMAGE_EFFECT_SMASH_BARPOST_COMPLETELY:
			{
				bIsVisible = false;
				bUsesCollision = false;
				if (!GetIsStatic()) {
					RemoveFromMovingList();
				}
				SetIsStatic(true);
				bExplosionProof = true;
				SetMoveSpeed(0.0f, 0.0f, 0.0f);
				SetTurnSpeed(0.0f, 0.0f, 0.0f);
				const RwRGBA color1 = { 200, 0, 0, 255 };
				const RwRGBA color2 = { 200, 200, 200, 255 };
				SetMoveSpeed(0.0f, 0.0f, 0.0f);
				SetTurnSpeed(0.0f, 0.0f, 0.0f);
				for (int32 i = 0; i < 32; i++) {
					CVector vecDir = CVector(
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(0.10f, 0.25f) + fDirectionZ
					);
					++nFrameGen;
					int32 currentFrame = nFrameGen & 3;
					const RwRGBA &color = nFrameGen & 1 ? color1 : color2;
					float fSize = CGeneral::GetRandomNumberInRange(0.02f, 0.20f);
					int32 nRotationSpeed = CGeneral::GetRandomNumberInRange(-40, 40);
					CParticle::AddParticle(PARTICLE_CAR_DEBRIS, vecPos, vecDir, nil, fSize, color, nRotationSpeed, 0, currentFrame, 0);
				}
				PlayOneShotScriptObject(SCRIPT_SOUND_METAL_COLLISION, vecPos);
				break;
			}
			case DAMAGE_EFFECT_SMASH_NEWSTANDNEW1:
			case DAMAGE_EFFECT_SMASH_NEWSTANDNEW2:
			case DAMAGE_EFFECT_SMASH_NEWSTANDNEW3:
			case DAMAGE_EFFECT_SMASH_NEWSTANDNEW4:
			case DAMAGE_EFFECT_SMASH_NEWSTANDNEW5:
			{
				bIsVisible = false;
				bUsesCollision = false;
				if (!GetIsStatic()) {
					RemoveFromMovingList();
				}
				SetIsStatic(true);
				bExplosionProof = true;
				SetMoveSpeed(0.0f, 0.0f, 0.0f);
				SetTurnSpeed(0.0f, 0.0f, 0.0f);
				CRGBA possibleColor1;
				CRGBA possibleColor2;
				switch (m_nCollisionDamageEffect) {
					case DAMAGE_EFFECT_SMASH_NEWSTANDNEW1:
						possibleColor1 = CRGBA(0xC0, 0x3E, 0xC, 0xFF);
						possibleColor2 = possibleColor1;
						break;
					case DAMAGE_EFFECT_SMASH_NEWSTANDNEW2:
						possibleColor1 = CRGBA(0xA3, 0x36, 0x21, 0xFF);
						possibleColor2 = possibleColor1;
						break;
					case DAMAGE_EFFECT_SMASH_NEWSTANDNEW3:
						possibleColor1 = CRGBA(0x12, 0x31, 0x24, 0xFF);
						possibleColor2 = possibleColor1;
						break;
					case DAMAGE_EFFECT_SMASH_NEWSTANDNEW4:
						possibleColor1 = CRGBA(0xC0, 0xC8, 0xBE, 0xFF);
						possibleColor2 = CRGBA(0x10, 0x57, 0x85, 0xFF);
						break;
					case DAMAGE_EFFECT_SMASH_NEWSTANDNEW5:
						possibleColor1 = CRGBA(0xD0, 0x94, 0x1B, 0xFF);
						possibleColor2 = possibleColor1;
						break;
				}
				for (int32 i = 0; i < 16; i++) {
					CVector vecDir(
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(0.10f, 0.15f) + fDirectionZ
					);
					float fSize = CGeneral::GetRandomNumberInRange(0.02f, 0.20f);
					int32 nRotationSpeed = CGeneral::GetRandomNumberInRange(-40, 40);
					++nFrameGen;
					int32 nCurFrame = nFrameGen & 0x3;
					CRGBA &selectedColor = nFrameGen & 0x1 ? possibleColor1 : possibleColor2;
					CParticle::AddParticle(PARTICLE_CAR_DEBRIS, vecPos, vecDir, nil, fSize, selectedColor, nRotationSpeed, 0, nCurFrame, 0);
					if (!(i % 7)) {
						static CRGBA secondParticleColors[4] = {
							CRGBA(0xA0, 0x60, 0x60, 0xFF),
							CRGBA(0x60, 0xA0, 0x60, 0xFF),
							CRGBA(0x60, 0x60, 0xA0, 0xFF),
							CRGBA(0xA0, 0xA0, 0xA0, 0xFF)
						};
						vecDir *= 0.5f;
						CRGBA &secondParticleColor = secondParticleColors[nFrameGen & 3];
						int32 nSecondRotationSpeed = CGeneral::GetRandomNumberInRange(-40, 40);
						CParticle::AddParticle(PARTICLE_DEBRIS, vecPos, vecDir, nil, 0.1f, secondParticleColor, nSecondRotationSpeed, 0, 1, 0);
					}
				}
				PlayOneShotScriptObject(SCRIPT_SOUND_METAL_COLLISION, vecPos);
				break;
			}
			case DAMAGE_EFFECT_SMASH_VEGPALM:
				// A trunk plant topples like a light pole; any other effect-110
				// model, pot_02 for one, still breaks.
				if (GetPlantKind(GetModelIndex()) == PLANT_TRUNK) {
					CVector hitDir;
					if (m_pDamageEntity && !m_vecDamageNormal.IsZero()) {
						// The recorded normal points from the plant to the
						// damager, so the plant falls the other way.
						hitDir = -m_vecDamageNormal;
					} else {
						// Melee and bullets record no direction. Fall away from
						// the player, the one swinging or shooting; use the
						// plant's right when the player stands on it.
						hitDir = GetPosition() - FindPlayerCoors();
						hitDir.z = 0.0f;
						if (hitDir.MagnitudeSqr() < 0.01f)
							hitDir = GetRight();
						hitDir.Normalise();
					}
					FellVegetation(this, GetPosition(), hitDir, amount);
				} else
					SmashVegetation(this);
				break;
			case DAMAGE_EFFECT_SMASH_BLACKBAG:
			case DAMAGE_EFFECT_SMASH_BEACHLOUNGE_WOOD:
			case DAMAGE_EFFECT_SMASH_BEACHLOUNGE_TOWEL:
			{
				bIsVisible = false;
				bUsesCollision = false;
				if (!GetIsStatic()) {
					RemoveFromMovingList();
				}
				SetIsStatic(true);
				bExplosionProof = true;
				SetMoveSpeed(0.0f, 0.0f, 0.0f);
				SetTurnSpeed(0.0f, 0.0f, 0.0f);
				CRGBA possibleColor1;
				CRGBA possibleColor2;
				switch (m_nCollisionDamageEffect) {
					case DAMAGE_EFFECT_SMASH_BLACKBAG:
						possibleColor1 = CRGBA(0, 0, 0, 0xFF);
						possibleColor2 = possibleColor1;
						break;
					case DAMAGE_EFFECT_SMASH_BEACHLOUNGE_WOOD:
						possibleColor1 = CRGBA(0x8F, 0x8A, 0x8C, 0xFF);
						possibleColor2 = CRGBA(0x73, 0x75, 0x7B, 0xFF);
						break;
					case DAMAGE_EFFECT_SMASH_BEACHLOUNGE_TOWEL:
						possibleColor1 = CRGBA(0x52, 0x92, 0x4A, 0xFF);
						possibleColor2 = CRGBA(0xCE, 0xCF, 0xCE, 0xFF);
						break;
				}
				for (int32 i = 0; i < 16; i++) {
					CVector vecDir(
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(-0.35f, 0.35f),
						CGeneral::GetRandomNumberInRange(0.10f, 0.25f) + fDirectionZ
					);
					++nFrameGen;
					int32 nCurFrame = nFrameGen & 0x3;
					CRGBA &selectedColor = nFrameGen & 0x1 ? possibleColor1 : possibleColor2;
					float fSize = CGeneral::GetRandomNumberInRange(0.02f, 0.20f);
					int32 nRotationSpeed = CGeneral::GetRandomNumberInRange(-40, 40);
					CParticle::AddParticle(PARTICLE_CAR_DEBRIS, vecPos, vecDir, nil, fSize, selectedColor, nRotationSpeed, 0, nCurFrame, 0);
				}
				if (m_nCollisionDamageEffect == DAMAGE_EFFECT_SMASH_BLACKBAG) {
					PlayOneShotScriptObject(SCRIPT_SOUND_BOX_DESTROYED_2, vecPos);
				} else if (m_nCollisionDamageEffect == DAMAGE_EFFECT_SMASH_BEACHLOUNGE_WOOD) {
					PlayOneShotScriptObject(SCRIPT_SOUND_METAL_COLLISION, vecPos);
				}
				break;
			}
			default:
				DEV("Unhandled collision damage effect id: %d\n", m_nCollisionDamageEffect);
				return;
		}
	}
}

void
CObject::RefModelInfo(int32 modelId)
{
	m_nRefModelIndex = modelId;
	CModelInfo::GetModelInfo(modelId)->AddRef();
}

void
CObject::Init(void)
{
	m_type = ENTITY_TYPE_OBJECT;
	CObjectData::SetObjectData(GetModelIndex(), *this);
	m_nEndOfLifeTime = 0;
	ObjectCreatedBy = GAME_OBJECT;
	SetIsStatic(true);
	bIsPickup = false;
	bPickupObjWithMessage = false;
	bOutOfStock = false;
	bGlassCracked = false;
	bGlassBroken = false;
	bHasBeenDamaged = false;
	bUseVehicleColours = false;
	m_nRefModelIndex = -1;
	m_colour1 = 0;
	m_colour2 = 0;
	m_nBonusValue = 0;
	bIsWeapon = false;
	bIsFelled = false;
	m_nCostValue = 0;
	m_pCollidingEntity = nil;
	CColPoint point;
	CEntity *outEntity = nil;
	const CVector& vecPos = GetMatrix().GetPosition();
	if (CWorld::ProcessVerticalLine(vecPos, vecPos.z - 10.0f, point, outEntity, true, false, false, false, false, false, nil))
		m_pCurSurface = outEntity;
	else
		m_pCurSurface = nil;

	if (GetModelIndex() == MI_BUOY)
		bTouchingWater = true;

	if (CModelInfo::GetModelInfo(GetModelIndex())->GetModelType() == MITYPE_WEAPON)
		bIsWeapon = true;
	bIsStreetLight = IsLightObject(GetModelIndex());

	m_area = AREA_EVERYWHERE;
}

bool
CObject::CanBeDeleted(void)
{
	switch (ObjectCreatedBy) {
		case GAME_OBJECT:
			return true;
		case MISSION_OBJECT:
			return false;
		case TEMP_OBJECT:
			return true;
		case CUTSCENE_OBJECT:
			return false;
		case CONTROLLED_SUB_OBJECT:
			return false;
		default:
			return true;
	}
}

void
CObject::DeleteAllMissionObjects()
{
	CObjectPool *objectPool = CPools::GetObjectPool();
	for (int32 i = 0; i < objectPool->GetSize(); i++) {
		CObject *pObject = objectPool->GetSlot(i);
		if (pObject && pObject->ObjectCreatedBy == MISSION_OBJECT) {
			CWorld::Remove(pObject);
			delete pObject;
		}
	}
}

void
CObject::DeleteAllTempObjects()
{
	CObjectPool *objectPool = CPools::GetObjectPool();
	for (int32 i = 0; i < objectPool->GetSize(); i++) {
		CObject *pObject = objectPool->GetSlot(i);
		if (pObject && pObject->ObjectCreatedBy == TEMP_OBJECT) {
			CWorld::Remove(pObject);
			delete pObject;
		}
	}
}

void
CObject::DeleteAllTempObjectsInArea(CVector point, float fRadius)
{
	CObjectPool *objectPool = CPools::GetObjectPool();
	for (int32 i = 0; i < objectPool->GetSize(); i++) {
		CObject *pObject = objectPool->GetSlot(i);
		if (pObject && pObject->ObjectCreatedBy == TEMP_OBJECT && (point - pObject->GetPosition()).MagnitudeSqr() < SQR(fRadius)) {
			CWorld::Remove(pObject);
			delete pObject;
		}
	}
}

bool
IsObjectPointerValid(CObject *pObject)
{
	if (!pObject)
		return false;
	int index = CPools::GetObjectPool()->GetJustIndex_NoFreeAssert(pObject);
#ifdef FIX_BUGS
	if (index < 0 || index >= CPools::GetObjectPool()->GetSize())
#else
	if (index < 0 || index > CPools::GetObjectPool()->GetSize())
#endif
		return false;
	return pObject->bIsBIGBuilding || pObject->m_entryInfoList.first;
}
