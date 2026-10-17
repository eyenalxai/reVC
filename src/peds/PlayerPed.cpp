#include "common.h"
#include "main.h"

#include "RwHelper.h"
#include "PlayerPed.h"
#include "Wanted.h"
#include "Fire.h"
#include "DMAudio.h"
#include "Pad.h"
#include "Camera.h"
#include "WeaponEffects.h"
#include "WeaponAim.h"
#include "ActiveReload.h"
#include "ModelIndices.h"
#include "World.h"
#include "RpAnimBlend.h"
#include "AnimBlendAssociation.h"
#include "General.h"
#include "Pools.h"
#include "Darkel.h"
#include "CarCtrl.h"
#include "MBlur.h"
#include "Streaming.h"
#include "Population.h"
#include "Script.h"
#include "Replay.h"
#include "PedPlacement.h"
#include "VarConsole.h"
#include "SaveBuf.h"

#define PAD_MOVE_TO_GAME_WORLD_MOVE 60.0f

// The player's movement tuning. m_vecMoveSpeed is in engine velocity units,
// one unit per 1/50 second, so one engine unit is 50 units per second; the
// accelerations and the friction below are 20 and 4 units per second squared.
// The ground turns the direction almost as fast as the body turns toward it, so
// the ped keeps its speed through a turn and the legs stay with the movement;
// the air steers with a small authority.
#define PLAYER_GROUND_ACCEL		(20.0f / 50.0f)
#define PLAYER_GROUND_FRICTION		(20.0f / 50.0f)
#define PLAYER_GROUND_TURN_RATE		12.0f	// radians per second
#define PLAYER_AIR_ACCEL		(4.0f / 50.0f)
#define PLAYER_AIR_TURN_RATE		1.6f	// radians per second

// The playback the game forces on the movement animations: the adrenaline
// speeds the legs up while the world runs slow, and the fixed camera slows the
// sprint animation down. The playback scaling in SetRealMoveAnim applies on top
// of this, and the controller targets the speed the legs really cover.
#define PLAYER_ADRENALINE_ANIM_SPEED		2.0f
#define PLAYER_FIXED_CAM_SPRINT_ANIM_SPEED	0.7f

// The measured animation speeds are in units per second; the controller and the
// playback scaling work in engine units (one unit per 1/50 second).
#define PLAYER_ENGINE_UNITS_PER_SECOND		50.0f

// The animation playback follows the real speed between these bounds; the clamp
// keeps a large mismatch from looking wrong
#define PLAYER_ANIM_SPEED_MIN		0.4f
#define PLAYER_ANIM_SPEED_MAX		2.0f

// DoWeaponSmoothSpray returns this when no smooth spray runs
#define PLAYER_NO_SMOOTH_SPRAY		(-1.0f)

// One row per ePlayerMovementState. The slide state will add its own row: a
// low friction and a high turn rate.
struct tPlayerMovementState
{
	float accel;		// engine units per second squared
	float friction;		// engine units per second squared
	float turnRate;		// radians per second, the air state steers with it
};

static const tPlayerMovementState playerMovementStates[PLAYERMOVE_NUMSTATES] = {
	{ PLAYER_GROUND_ACCEL, PLAYER_GROUND_FRICTION, PLAYER_GROUND_TURN_RATE },	// PLAYERMOVE_GROUND
	{ PLAYER_AIR_ACCEL, 0.0f, PLAYER_AIR_TURN_RATE },	// PLAYERMOVE_AIR
};

// Move a velocity toward a target, at most maxDelta units per second
static CVector2D
MoveVelocityTowards(const CVector2D &velocity, const CVector2D &target, float maxDelta)
{
	CVector2D change = target - velocity;
	float magnitude = change.Magnitude();
	if (magnitude <= maxDelta)
		return target;
	return velocity + change * (maxDelta / magnitude);
}

// Turn a velocity toward a direction without changing its speed
static CVector2D
SteerVelocityTowards(const CVector2D &velocity, const CVector2D &target, float maxTurn)
{
	float angle = Atan2(velocity.y, velocity.x);
	float targetAngle = Atan2(target.y, target.x);
	float turn = CGeneral::LimitRadianAngle(targetAngle - angle);
	if (turn > maxTurn)
		turn = maxTurn;
	else if (turn < -maxTurn)
		turn = -maxTurn;
	float newAngle = angle + turn;
	return CVector2D(Cos(newAngle), Sin(newAngle)) * velocity.Magnitude();
}

// The player's aim pose aims at a point this far along the aim ray
#define PLAYER_AIM_DISTANCE AIM_RAY_DISTANCE

bool CPlayerPed::bDontAllowWeaponChange;
#ifndef MASTER
bool CPlayerPed::bDebugPlayerInfo;
#endif

const uint32 CPlayerPed::nSaveStructSize =
#ifdef COMPATIBLE_SAVES
	1752;
#else
	sizeof(CPlayerPed);
#endif

int32 idleAnimBlockIndex;

CPad*
GetPadFromPlayer(CPlayerPed*)
{
	return CPad::GetPad(0);
}

CPlayerPed::~CPlayerPed()
{
	delete m_pWanted;
}

CPlayerPed::CPlayerPed(void) : CPed(PEDTYPE_PLAYER1)
{
	m_fMoveSpeed = 0.0f;
	SetModelIndex(MI_PLAYER);
#ifdef FIX_BUGS
	m_fCurrentStamina = m_fMaxStamina = 150.0f;
#endif
	SetInitialState();

	m_pWanted = new CWanted();
	m_pWanted->Initialise();
	m_pArrestingCop = nil;
	m_currentWeapon = WEAPONTYPE_UNARMED;
	m_nSelectedWepSlot = WEAPONSLOT_UNARMED;
	m_nSpeedTimer = 0;
	m_bSpeedTimerFlag = false;
	SetWeaponLockOnTarget(nil);
	SetPedState(PED_IDLE);
#ifndef FIX_BUGS
	m_fCurrentStamina = m_fMaxStamina = 150.0f;
#endif
	m_fStaminaProgress = 0.0f;
	m_nEvadeAmount = 0;
	m_pEvadingFrom = nil;
	m_nHitAnimDelayTimer = 0;
	m_fAttackButtonCounter = 0.0f;
	m_bHaveTargetSelected = false;
	m_bHasLockOnTarget = false;
	m_bCanBeDamaged = true;
	m_bNoPosForMeleeAttack = false;
	m_fWalkAngle = 0.0f;
	m_fFPSMoveHeading = 0.0f;
	m_pMinigunTopAtomic = nil;
	m_fGunSpinSpeed = 0.0;
	m_fGunSpinAngle = 0.0;
	m_nPadDownPressedInMilliseconds = 0;
	m_nTargettableObjects[0] = m_nTargettableObjects[1] = m_nTargettableObjects[2] = m_nTargettableObjects[3] = -1;
	unk1 = false;
	for (int i = 0; i < 6; i++) {
		m_vecSafePos[i] = CVector(0.0f, 0.0f, 0.0f);
		m_pPedAtSafePos[i] = nil;
		m_pMeleeList[i] = nil;
	}
	m_nAttackDirToCheck = 0;
	m_nLastBusFareCollected = 0;
	idleAnimBlockIndex = CAnimManager::GetAnimationBlockIndex("playidles");
	m_bWeaponAimActive = false;
}

void
CPlayerPed::ClearWeaponTarget()
{
	if (m_nPedType == PEDTYPE_PLAYER1) {
		SetWeaponLockOnTarget(nil);
		TheCamera.ClearPlayerWeaponMode();
		CWeaponEffects::ClearCrossHair();
	}
 	ClearPointGunAt();
}

void
CPlayerPed::SetWantedLevel(int32 level)
{
	m_pWanted->SetWantedLevel(level);
}

void
CPlayerPed::SetWantedLevelNoDrop(int32 level)
{
	m_pWanted->SetWantedLevelNoDrop(level);
}

void
CPlayerPed::MakeObjectTargettable(int32 handle)
{
	for (int i = 0; i < ARRAY_SIZE(m_nTargettableObjects); i++) {
		if (CPools::GetObjectPool()->GetAt(m_nTargettableObjects[i]) == nil) {
			m_nTargettableObjects[i] = handle;
			return;
		}
	}
}

// I don't know the actual purpose of parameter
void
CPlayerPed::AnnoyPlayerPed(bool annoyedByPassingEntity)
{
	if (m_pedStats->m_temper < 52) {
		m_pedStats->m_temper++;
	} else if (annoyedByPassingEntity && m_pedStats->m_temper < 55) {
		m_pedStats->m_temper++;
	} else if (annoyedByPassingEntity) {
		m_pedStats->m_temper = 46;
	}
}

void
CPlayerPed::ClearAdrenaline(void)
{
	if (m_bAdrenalineActive && m_nAdrenalineTime != 0) {
		m_nAdrenalineTime = 0;
		CTimer::SetTimeScale(1.0f);
	}
}

CPlayerInfo *
CPlayerPed::GetPlayerInfoForThisPlayerPed()
{
	if (CWorld::Players[0].m_pPed == this)
		return &CWorld::Players[0];

	return nil;
}

void
CPlayerPed::SetupPlayerPed(int32 index)
{
	CPlayerPed *player = new CPlayerPed();
	CWorld::Players[index].m_pPed = player;
#ifdef FIX_BUGS
	player->RegisterReference((CEntity**)&CWorld::Players[index].m_pPed);
#endif

	player->SetOrientation(0.0f, 0.0f, 0.0f);

	CWorld::Add(player);
	player->m_wepAccuracy = 100;

#ifndef MASTER
	VarConsole.Add("Debug PlayerPed", &CPlayerPed::bDebugPlayerInfo, true);
	VarConsole.Add("Tweak Vehicle Handling", &CVehicle::m_bDisplayHandlingInfo, true);
#endif
}

void
CPlayerPed::DeactivatePlayerPed(int32 index)
{
	CWorld::Remove(CWorld::Players[index].m_pPed);
}

void
CPlayerPed::ReactivatePlayerPed(int32 index)
{
	CWorld::Add(CWorld::Players[index].m_pPed);
}

void
CPlayerPed::UseSprintEnergy(void)
{
	if (m_fCurrentStamina > -150.0f && !CWorld::Players[CWorld::PlayerInFocus].m_bInfiniteSprint
		&& !m_bAdrenalineActive && !gInfiniteSprint) {
		m_fCurrentStamina = m_fCurrentStamina - CTimer::GetTimeStep();
		m_fStaminaProgress = m_fStaminaProgress + CTimer::GetTimeStep();
	}

	if (m_fStaminaProgress >= 500.0f) {
		m_fStaminaProgress = 0;
		if (m_fMaxStamina < 1000.0f)
			m_fMaxStamina += 10.0f;
	}
}

void
CPlayerPed::MakeChangesForNewWeapon(eWeaponType weapon)
{
	// The aim belongs to the weapon in hand: a switch puts it down and the
	// next frame decides for the new weapon
	StopWeaponAim();
	SetCurrentWeapon(weapon);
	m_nSelectedWepSlot = m_currentWeapon;

	// The stock weapon switch fills the clip. With the active reload on the
	// player reloads by hand, so the clip keeps its rounds (VICE-74).
	if (CWeapon::UsesStockClipRefill(this))
		GetWeapon()->m_nAmmoInClip = Min(GetWeapon()->m_nAmmoTotal, CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->m_nAmountofAmmunition);

	if (CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->IsFlagSet(WEAPONFLAG_CANAIM))
		ClearWeaponTarget();

	// WEAPONTYPE_SNIPERRIFLE? Wut?
	CAnimBlendAssociation* weaponAnim = RpAnimBlendClumpGetAssociation(GetClump(), GetPrimaryFireAnim(CWeaponInfo::GetWeaponInfo(WEAPONTYPE_SNIPERRIFLE)));
	if (weaponAnim) {
		weaponAnim->SetRun();
		weaponAnim->flags |= ASSOC_FADEOUTWHENDONE;
	}
	TheCamera.ClearPlayerWeaponMode();
}

void
CPlayerPed::MakeChangesForNewWeapon(int32 slot)
{
	if(slot != -1)
		MakeChangesForNewWeapon(m_weapons[slot].m_eWeaponType);
}

void
CPlayerPed::ReApplyMoveAnims(void)
{
	static AnimationId moveAnims[] = { ANIM_STD_WALK, ANIM_STD_RUN, ANIM_STD_RUNFAST, ANIM_STD_IDLE, ANIM_STD_STARTWALK };

	for(int i = 0; i < ARRAY_SIZE(moveAnims); i++) {
		CAnimBlendAssociation *curMoveAssoc = RpAnimBlendClumpGetAssociation(GetClump(), moveAnims[i]);
		if (curMoveAssoc) {
			if (CGeneral::faststrcmp(CAnimManager::GetAnimAssociation(m_animGroup, moveAnims[i])->hierarchy->name, curMoveAssoc->hierarchy->name)) {
				CAnimBlendAssociation *newMoveAssoc = CAnimManager::AddAnimation(GetClump(), m_animGroup, moveAnims[i]);
				newMoveAssoc->blendDelta = curMoveAssoc->blendDelta;
				newMoveAssoc->blendAmount = curMoveAssoc->blendAmount;
				curMoveAssoc->blendDelta = -1000.0f;
				curMoveAssoc->flags |= ASSOC_DELETEFADEDOUT;
			}
		}
	}
}

void
CPlayerPed::SetInitialState(void)
{
	m_nDrunkenness = 0;
	m_nFadeDrunkenness = 0;
	CMBlur::ClearDrunkBlur();
	m_nDrunkCountdown = 0;
	m_bAdrenalineActive = false;
	m_nAdrenalineTime = 0;
	CTimer::SetTimeScale(1.0f);
	m_pSeekTarget = nil;
	m_vecSeekPos = CVector(0.0f, 0.0f, 0.0f);
	m_fleeFromPos = CVector2D(0.0f, 0.0f);
	m_fleeFrom = nil;
	m_fleeTimer = 0;
	m_objective = OBJECTIVE_NONE;
	m_prevObjective = OBJECTIVE_NONE;
	bUsesCollision = true;
	ClearAimFlag();
	ClearLookFlag();
	bIsPointingGunAt = false;
	bRenderPedInCar = true;
	if (m_pFire)
		m_pFire->Extinguish();

	RpAnimBlendClumpRemoveAllAssociations(GetClump());
	SetPedState(PED_IDLE);
	SetMoveState(PEDMOVE_STILL);
	m_nLastPedState = PED_NONE;
	m_animGroup = ASSOCGRP_PLAYER;
	m_fMoveSpeed = 0.0f;
	m_nSelectedWepSlot = WEAPONSLOT_UNARMED;
	m_nEvadeAmount = 0;
	m_pEvadingFrom = nil;
	bIsPedDieAnimPlaying = false;
	SetRealMoveAnim();
	m_bCanBeDamaged = true;
	m_pedStats->m_temper = 50;
	m_fWalkAngle = 0.0f;
	m_fMoveHeading = 0.0f;
	m_nMovementState = PLAYERMOVE_GROUND;
	if (m_attachedTo && !bUsesCollision)
		bUsesCollision = true;

	m_attachedTo = nil;
	m_attachWepAmmo = 0;
}

void
CPlayerPed::SetRealMoveAnim(void)
{
	CAnimBlendAssociation *curWalkAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_WALK);
	CAnimBlendAssociation *curRunAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_RUN);
	CAnimBlendAssociation *curSprintAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_RUNFAST);
	CAnimBlendAssociation *curWalkStartAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_STARTWALK);
	CAnimBlendAssociation *curIdleAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_IDLE);
	CAnimBlendAssociation *curRunStopAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_RUNSTOP1);
	CAnimBlendAssociation *curRunStopRAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_RUNSTOP2);
	if (bResetWalkAnims) {
		if (curWalkAssoc)
			curWalkAssoc->SetCurrentTime(0.0f);
		if (curRunAssoc)
			curRunAssoc->SetCurrentTime(0.0f);
		if (curSprintAssoc)
			curSprintAssoc->SetCurrentTime(0.0f);
		bResetWalkAnims = false;
	}

	if (!curIdleAssoc)
		curIdleAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_IDLE_TIRED);
	if (!curIdleAssoc)
		curIdleAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_FIGHT_IDLE);
	if (!curIdleAssoc)
		curIdleAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_MELEE_IDLE_FIGHTMODE);

	if (!((curRunStopAssoc && curRunStopAssoc->IsRunning()) || (curRunStopRAssoc && curRunStopRAssoc->IsRunning()))) {

		if (curRunStopAssoc && curRunStopAssoc->blendDelta >= 0.0f || curRunStopRAssoc && curRunStopRAssoc->blendDelta >= 0.0f) {
			if (curRunStopAssoc) {
				curRunStopAssoc->flags |= ASSOC_DELETEFADEDOUT;
				curRunStopAssoc->blendAmount = 1.0f;
				curRunStopAssoc->blendDelta = -8.0f;
			} else if (curRunStopRAssoc) {
				curRunStopRAssoc->flags |= ASSOC_DELETEFADEDOUT;
				curRunStopRAssoc->blendAmount = 1.0f;
				curRunStopRAssoc->blendDelta = -8.0f;
			}
			
			RestoreHeadingRate();
			if (!curIdleAssoc) {
				if (m_fCurrentStamina < 0.0f && !bIsAimingGun && !CWorld::TestSphereAgainstWorld(GetPosition(), 0.5f,
						nil, true, false, false, false, false, false)) {
					curIdleAssoc = CAnimManager::BlendAnimation(GetClump(), ASSOCGRP_STD, ANIM_STD_IDLE_TIRED, 8.0f);

				} else {
					curIdleAssoc = CAnimManager::BlendAnimation(GetClump(), m_animGroup, ANIM_STD_IDLE, 8.0f);
				}
				m_nWaitTimer = CTimer::GetTimeInMilliseconds() + CGeneral::GetRandomNumberInRange(2500, 4000);
			}
			curIdleAssoc->blendAmount = 0.0f;
			curIdleAssoc->blendDelta = 8.0f;

		} else if (m_fMoveSpeed == 0.0f && !curSprintAssoc) {
			if (!curIdleAssoc) {
				if (m_fCurrentStamina < 0.0f && !bIsAimingGun && !CWorld::TestSphereAgainstWorld(GetPosition(), 0.5f,
						nil, true, false, false, false, false, false)) {
					curIdleAssoc = CAnimManager::BlendAnimation(GetClump(), ASSOCGRP_STD, ANIM_STD_IDLE_TIRED, 4.0f);
					
				} else {
					curIdleAssoc = CAnimManager::BlendAnimation(GetClump(), m_animGroup, ANIM_STD_IDLE, 4.0f);
				}

				m_nWaitTimer = CTimer::GetTimeInMilliseconds() + CGeneral::GetRandomNumberInRange(2500, 4000);
			}

			if ((m_fCurrentStamina > 0.0f || bIsAimingGun) && curIdleAssoc->animId == ANIM_STD_IDLE_TIRED) {
				CAnimManager::BlendAnimation(GetClump(), m_animGroup, ANIM_STD_IDLE, 4.0f);

			} else if (m_nPedState != PED_FIGHT) {
				if (m_fCurrentStamina < 0.0f && !bIsAimingGun && curIdleAssoc->animId != ANIM_STD_IDLE_TIRED
					&& !CWorld::TestSphereAgainstWorld(GetPosition(), 0.5f, nil, true, false, false, false, false, false)) {
					CAnimManager::BlendAnimation(GetClump(), ASSOCGRP_STD, ANIM_STD_IDLE_TIRED, 4.0f);

				} else if (curIdleAssoc->animId != ANIM_STD_IDLE) {
					CAnimManager::BlendAnimation(GetClump(), m_animGroup, ANIM_STD_IDLE, 4.0f);
				}
			}
			m_nMoveState = PEDMOVE_STILL;

		} else {
			if (curIdleAssoc) {
				if (curWalkStartAssoc) {
					curWalkStartAssoc->blendAmount = 1.0f;
					curWalkStartAssoc->blendDelta = 0.0f;
				} else {
					curWalkStartAssoc = CAnimManager::AddAnimation(GetClump(), m_animGroup, ANIM_STD_STARTWALK);
				}
				if (curWalkAssoc)
					curWalkAssoc->SetCurrentTime(0.0f);
				if (curRunAssoc)
					curRunAssoc->SetCurrentTime(0.0f);

				delete curIdleAssoc;
				delete RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_IDLE_TIRED);
				CAnimBlendAssociation *fightIdleAnim = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_FIGHT_IDLE);
				if (!fightIdleAnim)
					fightIdleAnim = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_MELEE_IDLE_FIGHTMODE);
				delete fightIdleAnim;
				delete curSprintAssoc;

				curSprintAssoc = nil;
				m_nMoveState = PEDMOVE_WALK;
			}
			if (curRunStopAssoc) {
				delete curRunStopAssoc;
				RestoreHeadingRate();
			}
			if (curRunStopRAssoc) {
				delete curRunStopRAssoc;
				RestoreHeadingRate();
			}
			if (!curWalkAssoc) {
				curWalkAssoc = CAnimManager::AddAnimation(GetClump(), m_animGroup, ANIM_STD_WALK);
				curWalkAssoc->blendAmount = 0.0f;
			}
			if (!curRunAssoc) {
				curRunAssoc = CAnimManager::AddAnimation(GetClump(), m_animGroup, ANIM_STD_RUN);
				curRunAssoc->blendAmount = 0.0f;
			}
			if (curWalkStartAssoc && !(curWalkStartAssoc->IsRunning())) {
				delete curWalkStartAssoc;
				curWalkStartAssoc = nil;
				curWalkAssoc->SetRun();
				curRunAssoc->SetRun();
			}
			if (m_nMoveState == PEDMOVE_SPRINT) {
				if (m_fCurrentStamina < 0.0f && (m_fCurrentStamina <= -150.0f || !curSprintAssoc || curSprintAssoc->blendDelta < 0.0f))
					m_nMoveState = PEDMOVE_STILL;

				if (curWalkStartAssoc)
					m_nMoveState = PEDMOVE_STILL;
			}

			if (curSprintAssoc && (m_nMoveState != PEDMOVE_SPRINT || m_fMoveSpeed < 0.4f)) {
				// Stop sprinting in various conditions
				if (curSprintAssoc->blendAmount == 0.0f) {
					curSprintAssoc->blendDelta = -1000.0f;
					curSprintAssoc->flags |= ASSOC_DELETEFADEDOUT;

				} else if (curSprintAssoc->blendDelta >= 0.0f || curSprintAssoc->blendAmount >= 0.8f) {
					if (m_fMoveSpeed < 0.4f) {
						AnimationId runStopAnim;
						if (curSprintAssoc->GetProgress() < 0.5) // double
							runStopAnim = ANIM_STD_RUNSTOP1;
						else
							runStopAnim = ANIM_STD_RUNSTOP2;
						CAnimBlendAssociation* newRunStopAssoc = CAnimManager::AddAnimation(GetClump(), ASSOCGRP_STD, runStopAnim);
						newRunStopAssoc->blendAmount = 1.0f;
						newRunStopAssoc->SetDeleteCallback(RestoreHeadingRateCB, this);
						m_headingRate = 0.0f;
						curSprintAssoc->flags |= ASSOC_DELETEFADEDOUT;
						curSprintAssoc->blendDelta = -1000.0f;
						curWalkAssoc->flags &= ~ASSOC_RUNNING;
						curWalkAssoc->blendAmount = 0.0f;
						curWalkAssoc->blendDelta = 0.0f;
						curRunAssoc->flags &= ~ASSOC_RUNNING;
						curRunAssoc->blendAmount = 0.0f;
						curRunAssoc->blendDelta = 0.0f;

					} else if (curSprintAssoc->blendDelta >= 0.0f) { // this condition is absent on mobile
						// Stop sprinting when tired
						curSprintAssoc->flags |= ASSOC_DELETEFADEDOUT;
						curSprintAssoc->blendDelta = -1.0f;
						curRunAssoc->blendDelta = 1.0f;
					}
				} else if (m_fMoveSpeed < 1.0f) {
					curSprintAssoc->blendDelta = -8.0f;
					curRunAssoc->blendDelta = 8.0f;
				}

			} else if (curWalkStartAssoc) {
				// Walk start and walk/run shouldn't run at the same time
				curWalkAssoc->flags &= ~ASSOC_RUNNING;
				curRunAssoc->flags &= ~ASSOC_RUNNING;
				curWalkAssoc->blendAmount = 0.0f;
				curRunAssoc->blendAmount = 0.0f;

			} else if (m_nMoveState == PEDMOVE_SPRINT) {
				if (curSprintAssoc) {
					// We have anim, do it
					if (curSprintAssoc->blendDelta < 0.0f) {
						curSprintAssoc->blendDelta = 2.0f;
						curRunAssoc->blendDelta = -2.0f;
					}
				} else {
					// Transition between run-sprint
					curWalkAssoc->blendAmount = 0.0f;
					curRunAssoc->blendAmount = 1.0f;
					curSprintAssoc = CAnimManager::BlendAnimation(GetClump(), m_animGroup, ANIM_STD_RUNFAST, 2.0f);
				}
				UseSprintEnergy();
			} else {
				if (m_fMoveSpeed < 1.0f) {
					curWalkAssoc->blendAmount = 1.0f;
					curRunAssoc->blendAmount = 0.0f;
					m_nMoveState = PEDMOVE_WALK;
				} else if (m_fMoveSpeed < 2.0f) {
					curWalkAssoc->blendAmount = 2.0f - m_fMoveSpeed;
					curRunAssoc->blendAmount = m_fMoveSpeed - 1.0f;
					m_nMoveState = PEDMOVE_RUN;
				} else {
					curWalkAssoc->blendAmount = 0.0f;
					curRunAssoc->blendAmount = 1.0f;
					m_nMoveState = PEDMOVE_RUN;
				}
				curWalkAssoc->blendDelta = 0.0f;
				curRunAssoc->blendDelta = 0.0f;
			}
		}
	}
	// Match the animation playback to the real speed the controller drives the
	// ped at, so the legs do not slide while the speed changes
	float animSpeed = 1.0f;
	if (!m_bAdrenalineActive && bIsStanding) {
		float referenceSpeed = GetMoveAnimReferenceSpeed();
		if (referenceSpeed > 0.0f) {
			CVector2D realVelocity = CVector2D(m_vecMoveSpeed) - GetSurfaceSpeed();
			animSpeed = Clamp(realVelocity.Magnitude() / referenceSpeed, PLAYER_ANIM_SPEED_MIN, PLAYER_ANIM_SPEED_MAX);
		}
	}
	if (m_bAdrenalineActive) {
		if (CTimer::GetTimeInMilliseconds() > m_nAdrenalineTime) {
			m_bAdrenalineActive = false;
			CTimer::SetTimeScale(1.0f);
			if (curWalkStartAssoc)
				curWalkStartAssoc->speed = 1.0f;
			if (curWalkAssoc)
				curWalkAssoc->speed = 1.0f;
			if (curRunAssoc)
				curRunAssoc->speed = 1.0f;
			if (curSprintAssoc)
				curSprintAssoc->speed = 1.0f;
		} else {
			CTimer::SetTimeScale(1.0f / 3);
			if (curWalkStartAssoc)
				curWalkStartAssoc->speed = PLAYER_ADRENALINE_ANIM_SPEED;
			if (curWalkAssoc)
				curWalkAssoc->speed = PLAYER_ADRENALINE_ANIM_SPEED;
			if (curRunAssoc)
				curRunAssoc->speed = PLAYER_ADRENALINE_ANIM_SPEED;
			if (curSprintAssoc)
				curSprintAssoc->speed = PLAYER_ADRENALINE_ANIM_SPEED;
		}
	} else {
		if (curWalkAssoc)
			curWalkAssoc->speed = animSpeed;
		if (curRunAssoc)
			curRunAssoc->speed = animSpeed;
		if (curSprintAssoc)
			curSprintAssoc->speed = GetMoveAnimPlayback(ANIM_STD_RUNFAST) * animSpeed;
	}
}

void
CPlayerPed::RestoreSprintEnergy(float restoreSpeed)
{
	if (m_fCurrentStamina < m_fMaxStamina)
		m_fCurrentStamina += restoreSpeed * CTimer::GetTimeStep() * 0.5f;
}

float
CPlayerPed::DoWeaponSmoothSpray(void)
{
	if (m_nPedState == PED_ATTACK && !m_pPointGunAt) {
		CWeaponInfo *weaponInfo = CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType);
		switch (GetWeapon()->m_eWeaponType) {
			case WEAPONTYPE_GOLFCLUB:
			case WEAPONTYPE_NIGHTSTICK:
			case WEAPONTYPE_BASEBALLBAT:
				if (GetFireAnimGround(weaponInfo, false) && RpAnimBlendClumpGetAssociation(GetClump(), GetFireAnimGround(weaponInfo, false)))
					return PI / 176.f;
				else
					return PLAYER_NO_SMOOTH_SPRAY;

			case WEAPONTYPE_CHAINSAW:
				if (GetMeleeStartAnim(weaponInfo) && RpAnimBlendClumpGetAssociation(GetClump(), GetMeleeStartAnim(weaponInfo))) {
#ifdef FREE_CAM
					if (TheCamera.Cams[0].Using3rdPersonMouseCam()) return PLAYER_NO_SMOOTH_SPRAY;
#endif
					return PI / 128.0f;
				}
				else if (GetFireAnimGround(weaponInfo, false) && RpAnimBlendClumpGetAssociation(GetClump(), GetFireAnimGround(weaponInfo, false)))
					return PI / 176.f;
				else
					return PI / 80.f;

			case WEAPONTYPE_PYTHON:
				return PI / 112.f;
			case WEAPONTYPE_SHOTGUN:
			case WEAPONTYPE_SPAS12_SHOTGUN:
			case WEAPONTYPE_STUBBY_SHOTGUN:
				return PI / 112.f;
			case WEAPONTYPE_UZI:
			case WEAPONTYPE_MP5:
				return PI / 112.f;
			case WEAPONTYPE_M4:
			case WEAPONTYPE_RUGER:
				return PI / 112.f;
			case WEAPONTYPE_FLAMETHROWER:
				return PI / 80.f;
			case WEAPONTYPE_M60:
			case WEAPONTYPE_MINIGUN:
			case WEAPONTYPE_HELICANNON:
				return PI / 176.f;
			default:
				return PLAYER_NO_SMOOTH_SPRAY;
		}
	} else if (bIsDucking)
		return PI / 112.f;
	else
		return PLAYER_NO_SMOOTH_SPRAY;
}

void
CPlayerPed::DoStuffToGoOnFire(void)
{
	// The aim goes with the fire: a burning player cannot hold the weapon
	StopWeaponAim();
}

bool
CPlayerPed::DoesTargetHaveToBeBroken(CVector target, CWeapon *weaponUsed)
{
	CVector distVec = target - GetPosition();

	if (distVec.Magnitude() > CWeaponInfo::GetWeaponInfo(weaponUsed->m_eWeaponType)->m_fRange)
		return true;

	return false;
}

// Cancels landing anim while running & jumping? I think
void
CPlayerPed::RunningLand(CPad *padUsed)
{
	CAnimBlendAssociation *landAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_FALL_LAND);
	if (landAssoc && landAssoc->currentTime == 0.0f && m_fMoveSpeed > 1.5f
		&& padUsed && (padUsed->GetPedWalkLeftRight() != 0.0f || padUsed->GetPedWalkUpDown() != 0.0f)) {

		landAssoc->blendDelta = -1000.0f;
		landAssoc->flags |= ASSOC_DELETEFADEDOUT;

		CAnimManager::AddAnimation(GetClump(), ASSOCGRP_STD, ANIM_STD_JUMP_LAND)->SetFinishCallback(FinishJumpCB, this);

		if (m_nPedState == PED_JUMP)
			RestorePreviousState();
	}
}

bool
CPlayerPed::IsThisPedAnAimingPriority(CPed *suspect)
{
	if (!suspect->bIsPlayerFriend)
		return true;

	if (suspect->m_pPointGunAt == this)
		return true;

	switch (suspect->m_objective) {
		case OBJECTIVE_KILL_CHAR_ON_FOOT:
		case OBJECTIVE_KILL_CHAR_ANY_MEANS:
			if (suspect->m_pedInObjective == this)
				return true;

			break;
		default:
			break;
	}
	return suspect->m_nPedState == PED_ABSEIL;
}

// The cycle keys pick the next or the previous weapon slot. It answers false
// when no cycle key went down or the camera forbids the cycle; the caller then
// checks the empty weapon.
bool
CPlayerPed::CycleWeaponInput(CPad *padUsed, bool inCar)
{
	if (m_pPointGunAt || bDontAllowWeaponChange || GetWeapon()->m_eWeaponType == WEAPONTYPE_DETONATOR)
		return false;

	if (padUsed->CycleWeaponRightJustDown(inCar)) {
		if (TheCamera.PlayerWeaponMode.Mode == CCam::MODE_M16_1STPERSON
			|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_M16_1STPERSON_RUNABOUT
			|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_SNIPER
			|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_SNIPER_RUNABOUT
			|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_ROCKETLAUNCHER
			|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_ROCKETLAUNCHER_RUNABOUT
			|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_CAMERA)
			return false;

		SelectCycledWeaponSlot(true, inCar);
		return true;
	}

	if (padUsed->CycleWeaponLeftJustDown(inCar)) {
		if (TheCamera.PlayerWeaponMode.Mode == CCam::MODE_M16_1STPERSON
			|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_SNIPER
			|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_ROCKETLAUNCHER
			|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_CAMERA)
			return false;

		SelectCycledWeaponSlot(false, inCar);
		return true;
	}

	return false;
}

// The forward cycle stops at the last slot and falls back to unarmed; the
// backward cycle wraps round. With inCar only the aimable weapons count,
// and the held weapon stays when no other one is left.
void
CPlayerPed::SelectCycledWeaponSlot(bool forward, bool inCar)
{
	if (forward) {
		for (m_nSelectedWepSlot = m_currentWeapon + 1;
			m_nSelectedWepSlot < TOTAL_WEAPON_SLOTS && !CanCycleToWeaponSlot(m_nSelectedWepSlot, inCar);
			++m_nSelectedWepSlot)
			;
		if (m_nSelectedWepSlot == TOTAL_WEAPON_SLOTS)
			m_nSelectedWepSlot = WEAPONSLOT_UNARMED;
	} else {
		// I don't know what kind of loop that was
		m_nSelectedWepSlot = m_currentWeapon - 1;
		do {
			if (m_nSelectedWepSlot < 0)
				m_nSelectedWepSlot = TOTAL_WEAPON_SLOTS - 1;

			if (m_nSelectedWepSlot == WEAPONSLOT_UNARMED)
				break;

			if (CanCycleToWeaponSlot(m_nSelectedWepSlot, inCar))
				break;

			--m_nSelectedWepSlot;
		} while (m_nSelectedWepSlot != WEAPONSLOT_UNARMED);
	}

	if (inCar && m_nSelectedWepSlot == WEAPONSLOT_UNARMED)
		m_nSelectedWepSlot = m_currentWeapon;
}

bool
CPlayerPed::CanCycleToWeaponSlot(int32 slot, bool inCar)
{
	if (!HasWeaponSlot(slot) || !GetWeapon(slot).HasWeaponAmmoToBeUsed())
		return false;

	return !inCar || IsAimableWeapon(GetWeapon(slot).m_eWeaponType);
}

// Makes the weapon of the selected slot the held one.
void
CPlayerPed::ApplySelectedWeaponSlot(void)
{
	if (m_nSelectedWepSlot != m_currentWeapon) {
		if (m_nPedState != PED_ATTACK && m_nPedState != PED_AIM_GUN && m_nPedState != PED_FIGHT) {
			RemoveWeaponAnims(m_currentWeapon, -1000.0f);
			MakeChangesForNewWeapon(m_nSelectedWepSlot);
		}
	}
}

// The driver cycles the aimable weapons with the car cycle keys. The weapon
// that comes back on the exit follows the choice, as the seat stored it.
void
CPlayerPed::ProcessVehicleWeaponSwitch(CPad *padUsed)
{
	if (m_nPedState != PED_DRIVING || !CycleWeaponInput(padUsed, true))
		return;

	if (m_nSelectedWepSlot != m_currentWeapon && m_storedWeapon != WEAPONTYPE_UNIDENTIFIED)
		m_storedWeapon = GetWeapon(m_nSelectedWepSlot).m_eWeaponType;

	ApplySelectedWeaponSlot();
}

// I think R* also used goto in here.
void
CPlayerPed::ProcessWeaponSwitch(CPad *padUsed)
{
	if (CDarkel::FrenzyOnGoing() || m_attachedTo)
		goto switchDetectDone;

	if (CycleWeaponInput(padUsed, false))
		goto switchDetectDone;

	if (CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->m_eWeaponFire != WEAPON_FIRE_MELEE
		&& (!padUsed->GetWeapon() || GetWeapon()->m_eWeaponType != WEAPONTYPE_MINIGUN)) {
		if (GetWeapon()->m_nAmmoTotal <= 0) {
			if (TheCamera.PlayerWeaponMode.Mode == CCam::MODE_M16_1STPERSON
				|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_SNIPER
				|| TheCamera.PlayerWeaponMode.Mode == CCam::MODE_ROCKETLAUNCHER)
				return;

			if (GetWeapon()->m_eWeaponType == WEAPONTYPE_DETONATOR
				&& GetWeapon(WEAPONSLOT_PROJECTILE).m_eWeaponType == WEAPONTYPE_DETONATOR_GRENADE)
				m_nSelectedWepSlot = WEAPONSLOT_PROJECTILE;
			else
				m_nSelectedWepSlot = m_currentWeapon - 1;

			for (; m_nSelectedWepSlot >= WEAPONSLOT_UNARMED; --m_nSelectedWepSlot) {

				// BUG: m_nSelectedWepSlot and GetWeapon(..) takes slot in VC but they compared them against weapon types in whole condition! jeez
#ifdef FIX_BUGS
				if (m_nSelectedWepSlot == WEAPONSLOT_MELEE ||
					GetWeapon(m_nSelectedWepSlot).m_nAmmoTotal > 0 && (m_nSelectedWepSlot != WEAPONSLOT_PROJECTILE || GetWeapon(WEAPONSLOT_PROJECTILE).m_eWeaponType == WEAPONTYPE_DETONATOR_GRENADE)) {
#else
				if (m_nSelectedWepSlot == WEAPONTYPE_BASEBALLBAT && GetWeapon(WEAPONTYPE_BASEBALLBAT).m_eWeaponType == WEAPONTYPE_BASEBALLBAT
					|| GetWeapon(m_nSelectedWepSlot).m_nAmmoTotal > 0
					&& m_nSelectedWepSlot != WEAPONTYPE_MOLOTOV && m_nSelectedWepSlot != WEAPONTYPE_GRENADE && m_nSelectedWepSlot != WEAPONTYPE_TEARGAS) {
#endif
					goto switchDetectDone;
				}
			}
			m_nSelectedWepSlot = WEAPONSLOT_UNARMED;
		}
	}

switchDetectDone:
	ApplySelectedWeaponSlot();
}

// The shared active reload input of the player: the reload key starts a
// reload or judges the running one, and a perfect judgment aborts the reload
// animation once the weapon is READY. The on-foot path and the three drive-by
// callers use this, so the input behaves the same on foot and in a vehicle
// (VICE-88, VICE-90). The timing itself lives in src/weapons/ActiveReload.
void
CPlayerPed::ProcessActiveReloadInput(CPad *padUsed)
{
	CWeapon *weapon = GetWeapon();

	if (padUsed->ReloadJustDown()) {
		if (ActiveReload::InProgress())
			ActiveReload::Judge(weapon);
		else if (weapon->CanReload())
			weapon->StartReload(this);
	}

	// The perfect judgment ends the timer almost at once, but the attack
	// refuses while the reload association runs. Once the weapon state is
	// READY, fade the animation out so the pose and the attack unblock
	// (VICE-88). While the state is still RELOADING the attack path can
	// blend a fresh reload animation and re-block, so wait.
	if (weapon->m_eWeaponState == WEAPONSTATE_READY && ActiveReload::ConsumePerfect())
		AbortReloadAnimation();
}

// The active reload of the player on foot: the shared input plus the reload
// animation of the player who reloads while standing still. A driver holds
// the drive-by pose, so only the input runs in a vehicle.
void
CPlayerPed::ProcessActiveReload(CPad *padUsed)
{
	CWeapon *weapon = GetWeapon();

	ProcessActiveReloadInput(padUsed);

	// Attack() starts the animation for the player who reloads in a fight;
	// this covers the player who reloads while standing still. The claim keeps
	// a long jam from starting the animation a second time. With the fast
	// reload flag the stock code skips the animation completely, and the
	// helper's side effects must not run every frame, so no per-frame attempt
	// is made then.
	if (weapon->m_eWeaponState == WEAPONSTATE_RELOADING
		&& !CWorld::Players[CWorld::PlayerInFocus].m_bFastReload
		&& ActiveReload::ClaimReloadAnimation())
		TryStartReloadAnimation();
}

// The held aim pose is an association that is present, does not run (a running
// one belongs to an attack) and does not fade out. Everything else is not the
// held pose (VICE-141).
static bool
HasHeldAimPose(CAnimBlendAssociation *aimAssoc)
{
	return aimAssoc && !aimAssoc->IsRunning() && aimAssoc->blendDelta >= 0.0f;
}

// Hold the weapon aim pose on the upper body while the player aims down the
// sights or aims with the mouse camera (VICE-141). The pose is the weapon's
// fire animation held at its loop start, like the stock/classic aim pose, but
// the lower body sequences are dropped: the arms and the torso hold the aim
// while the legs keep the walk, run or sprint cycle underneath. The pose is
// the same association the fire animation uses, so a burst that reuses it
// keeps the same rule.
void
CPlayerPed::HoldUpperBodyAimPose(void)
{
	CWeaponInfo *weaponInfo = CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType);
	if (weaponInfo->m_AnimToPlay == ASSOCGRP_STD)
		return;

	// The crouch pose owns the upper body while the player ducks, and the
	// reload owns it while it plays, with or without an animation
	if (bIsDucking || GetWeapon()->m_eWeaponState == WEAPONSTATE_RELOADING)
		return;

	CAnimBlendAssociation *aimAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_WEAPON_FIRE);

	// A running attack owns the association
	if (aimAssoc && aimAssoc->IsRunning())
		return;

	// Any other partial animation that runs (a reload, a hit, the crouch pose)
	// owns the upper body while it lasts: the aim pose waits for it
	for (CAnimBlendAssociation *assoc = RpAnimBlendClumpGetFirstAssociation(GetClump(), ASSOC_PARTIAL);
	     assoc; assoc = RpAnimBlendGetNextAssociation(assoc, ASSOC_PARTIAL)) {
		if (assoc->animId != ANIM_WEAPON_FIRE && assoc->IsRunning())
			return;
	}

	if (!HasHeldAimPose(aimAssoc)) {
		if (aimAssoc == nil) {
			aimAssoc = CAnimManager::AddAnimation(GetClump(), weaponInfo->m_AnimToPlay, ANIM_WEAPON_FIRE);
			if (aimAssoc == nil)
				return;
			aimAssoc->SetCurrentTime(weaponInfo->m_fAnimLoopStart);
			aimAssoc->flags &= ~ASSOC_RUNNING;
			aimAssoc->blendAmount = 0.0f;
		}
		// A released pose that the aim takes back must not fade out first:
		// bring it back where it is, so the arms do not dip while the fade
		// would run to its end
		aimAssoc->blendDelta = 8.0f;
		aimAssoc->flags &= ~ASSOC_DELETEFADEDOUT;
	}

	// Several fire animations carry a standing leg stance; the lower body keys
	// must not freeze the movement cycle. A rider on a bike keeps the seat's
	// spine as well.
	aimAssoc->MakeUpperBodyOnly(RidesBike());
}

// Fade the held aim pose out again when the sights go down and no attack or
// reload owns the association. The blend is the stock blend out of the aim
// pose, so the arms return to the movement animation without a snap.
void
CPlayerPed::ReleaseHeldAimPose(void)
{
	CAnimBlendAssociation *aimAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_WEAPON_FIRE);
	if (!HasHeldAimPose(aimAssoc))
		return;

	aimAssoc->flags |= ASSOC_DELETEFADEDOUT;
	aimAssoc->blendDelta = -4.0f;
}

// Aim the aim IK at a world position: the yaw the body holds and the pitch of
// the right arm. The stock CPedIK::PointGunAtPosition works the same way, but
// this path keeps the aim flag set so that CPed::AimGun applies the pose every
// frame and CPed::ClearAimFlag releases it.
void
CPlayerPed::AimAtPosition(const CVector &position)
{
	RwV3d armPos;
	m_pedIK.GetComponentPosition(armPos, PED_UPPERARMR);

	SetAimFlag(CGeneral::GetRadianAngleBetweenPoints(position.x, position.y, GetPosition().x, GetPosition().y));
	m_fFPSMoveHeading = CGeneral::GetRadianAngleBetweenPoints(position.z,
		Distance2D(GetPosition(), position.x, position.y), armPos.z, 0.0f);

	// The rocket launcher's data has no fire animation to hold: the arm IK
	// raises the launcher to the aim instead of the held upper body pose. The
	// scope and the viewfinder do not show the pose at all.
	if (!UsesScopeZoom(GetWeapon()->m_eWeaponType) && !IsPhotoCamera(GetWeapon()->m_eWeaponType) &&
	    GetWeapon()->GetInfo()->m_AnimToPlay == ASSOCGRP_STD)
		m_pedIK.m_flags |= CPedIK::AIMS_WITH_ARM;

	HoldUpperBodyAimPose();
}

// The one aim of the aiming weapon, gun and photo camera alike. The aim button
// holds it; every on-foot control path runs this each frame, so the aim follows
// the player through the walk, the run, the sprint, the jump and the fall, in
// both views. The pose follows the one aim ray: the crosshair of the third
// person mouse view, the camera of every other view. Only the scope and the
// viewfinder take a camera mode; a normal gun aims through the view it has.
void
CPlayerPed::AimWeapon(CPad *padUsed)
{
	eWeaponType weapon = GetWeapon()->m_eWeaponType;

	// The aim ends when the button goes, the weapon cannot aim, or the player
	// holds the weapon for an attachment instead of aiming it
	if (!padUsed->GetTarget() || !IsAimableWeapon(weapon) || m_attachedTo) {
		StopWeaponAim();
		return;
	}

	if (!m_bWeaponAimActive)
		StartWeaponAim();
	else
		// The weapon switched under the held aim: the scope or the viewfinder
		// follows the new weapon
		UpdateAimCameraMode();

	CVector source, target;
	TheCamera.GetPlayerAimRay(PLAYER_AIM_DISTANCE, GetPosition(), source, target);
	AimAtPosition(target);
}

// The start of the aim: the state, the end of any lock-on and the camera mode
// of the scope or the viewfinder. It is the one start, on foot and in a
// vehicle alike.
void
CPlayerPed::StartWeaponAim(void)
{
	// An aiming weapon never lock-ons: the stock lock-on belongs to the
	// melee, the thrown weapons and the detonator
	if (m_pPointGunAt || m_bHasLockOnTarget)
		ClearWeaponTarget();
	m_bWeaponAimActive = true;
	UpdateAimCameraMode();
}

// The camera mode of the held aim: the scope and the viewfinder own the
// camera, and every other gun aims through the view it has. A mode the aim
// never sets (the syphon, the heli cannon) stays.
void
CPlayerPed::UpdateAimCameraMode(void)
{
	eWeaponType weapon = GetWeapon()->m_eWeaponType;

	if (UsesScopeZoom(weapon)) {
		if (TheCamera.PlayerWeaponMode.Mode != CCam::MODE_SNIPER)
			TheCamera.SetNewPlayerWeaponMode(CCam::MODE_SNIPER, 0, 0);
	} else if (IsPhotoCamera(weapon)) {
		if (TheCamera.PlayerWeaponMode.Mode != CCam::MODE_CAMERA)
			TheCamera.SetNewPlayerWeaponMode(CCam::MODE_CAMERA, 0, 0);
	} else if (TheCamera.IsSightCameraMode(TheCamera.PlayerWeaponMode.Mode)) {
		TheCamera.ClearPlayerWeaponMode();
	}
}

// The end of the aim: the stock release puts the pose down, and the scope or
// the viewfinder leaves with it.
void
CPlayerPed::StopWeaponAim(void)
{
	ClearAimFlag();
	ClearAimCameraMode();
}

// The scope and the viewfinder leave with the aim flag: every end of the aim
// (the button, a reload that takes the pose, a state that cannot aim) must take
// its camera mode with it. A mode the aim never sets (the syphon, the heli
// cannon) stays.
void
CPlayerPed::ClearAimCameraMode(void)
{
	if (TheCamera.IsSightCameraMode(TheCamera.PlayerWeaponMode.Mode))
		TheCamera.ClearPlayerWeaponMode();
}

// The shot of the held fire button: the stationary sights and the airborne
// player fire straight from the hand, because neither runs the attack
// animation. The weapon's firing rate spaces the shots and an empty weapon
// only clicks.
void
CPlayerPed::FireWeaponWhenHeld(CPad *padUsed)
{
	CWeapon *weapon = GetWeapon();
	uint32 now = CTimer::GetTimeInMilliseconds();

	if (padUsed->GetWeapon() && now > weapon->m_nTimer) {
		if (weapon->m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO) {
			DMAudio.PlayFrontEndSound(SOUND_WEAPON_SNIPER_SHOT_NO_ZOOM, 0.f);
			weapon->m_nTimer = weapon->GetInfo()->m_nFiringRate + now;
		} else {
			CVector firePos = weapon->GetInfo()->m_vecFireOffset;
			TransformToNode(firePos, PED_HANDR);
			if (weapon->Fire(this, &firePos))
				weapon->m_nTimer = weapon->GetInfo()->m_nFiringRate + now;
			m_nPadDownPressedInMilliseconds = now;
		}
	} else if (now > weapon->m_nTimer &&
		now - CTimer::GetTimeStepInMilliseconds() < weapon->m_nTimer && weapon->m_eWeaponState != WEAPONSTATE_OUT_OF_AMMO) {
		DMAudio.PlayFrontEndSound(SOUND_WEAPON_AK47_BULLET_ECHO, weapon->m_eWeaponType);
	}
}

// The airborne player keeps the aim and the fire: the jump and the fall own
// the landing, so the attack state and its animation stay off and the weapon
// fires straight from the hand along the one aim ray. The one aim runs here
// too, so the state and the pose hold through the air (VICE-137).
void
CPlayerPed::ProcessAirborneWeapon(CPad *padUsed)
{
	CWeapon *weapon = GetWeapon();

	// The melee and the thrown weapons need the attack animation
	if (weapon->IsTypeMelee() || weapon->GetInfo()->IsFlagSet(WEAPONFLAG_THROW))
		return;

	AimWeapon(padUsed);

	FireWeaponWhenHeld(padUsed);
	weapon->Update(m_audioEntityId, this);
}

void
CPlayerPed::PlayerControlFighter(CPad *padUsed)
{
	float leftRight = padUsed->GetPedWalkLeftRight();
	float upDown = padUsed->GetPedWalkUpDown();
	float padMove = CVector2D(leftRight, upDown).Magnitude();

	// The fight moves drive the ped with their own root motion, so the
	// controller only turns the body and picks the animation blend here
	UpdatePlayerMovement(padUsed);

	if (padMove > 0.0f) {
		m_takeAStepAfterAttack = padMove > (2 * PAD_MOVE_TO_GAME_WORLD_MOVE);
		if (padUsed->GetSprint() && padMove > (1 * PAD_MOVE_TO_GAME_WORLD_MOVE))
			bIsAttacking = false;
	}

	if (!CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->IsFlagSet(WEAPONFLAG_HEAVY) && padUsed->JumpJustDown()) {
		if (m_nEvadeAmount != 0 && m_pEvadingFrom) {
			SetEvasiveDive((CPhysical*)m_pEvadingFrom, 1);
			m_nEvadeAmount = 0;
			m_pEvadingFrom = nil;
		} else {
			SetJump();
		}
	}

	// The fight stance keeps the fight weapons; the one aim ends here when the
	// player holds an aiming weapon (a weapon switch out of the stance)
	AimWeapon(padUsed);
}

void
CPlayerPed::PlayerControl1stPersonRunAround(CPad *padUsed)
{
	// The body faces the camera and the pad moves it: one controller for the
	// first person view and the mouse third person camera
	UpdatePlayerMovement(padUsed);

	if (m_nPedState == PED_ANSWER_MOBILE) {
		SetRealMoveAnim();
		return;
	}

	// The one aim runs before the sprint and the jump: the aim gates the heavy
	// weapon controls (VICE-137)
	AimWeapon(padUsed);

	if (PlayerWantsSprint(padUsed))
		m_nMoveState = PEDMOVE_SPRINT;
	if (m_nPedState != PED_FIGHT)
		SetRealMoveAnim();

	PlayerJumpOrDive(padUsed);

	// FIX: Fact that PlayIdleAnimations only called through PlayerControlZelda was making it visible to only Classic control players. This isn't fair!
#ifdef FIX_BUGS
	if (m_nPedState != PED_FIGHT)
		PlayIdleAnimations(padUsed);
#endif
}

// The jump and the evade dive of the on-foot player, shared by the two walk
// controls. The aim gates the heavy weapons: a heavy weapon refuses the jump,
// except while the aim is up, where the aim keeps the whole control (VICE-137).
// The dive owns the whole body, so the aim goes with it; the jump keeps the aim
// and the pose through the air.
void
CPlayerPed::PlayerJumpOrDive(CPad *padUsed)
{
	if (bIsInTheAir || m_nPedState == PED_JUMP || !padUsed->JumpJustDown())
		return;
	if (!m_bWeaponAimActive && CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->IsFlagSet(WEAPONFLAG_HEAVY))
		return;

	ClearAttack();
	if (m_nEvadeAmount != 0 && m_pEvadingFrom) {
		// The dive owns the whole body: the aim and the lock-on go with it
		StopWeaponAim();
		ClearWeaponTarget();
		SetEvasiveDive((CPhysical*)m_pEvadingFrom, 1);
		m_nEvadeAmount = 0;
		m_pEvadingFrom = nil;
	} else {
		// The lock-on target goes with the jump; the aim stays
		if (!m_bWeaponAimActive)
			ClearWeaponTarget();
		SetJump();
	}
}

// The sprint request of the on-foot player. The heavy weapons refuse it, except
// while the aim is up: one control, aiming included (VICE-137).
bool
CPlayerPed::PlayerWantsSprint(CPad *padUsed)
{
	if (!padUsed->GetSprint())
		return false;
	return m_bWeaponAimActive || !CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->IsFlagSet(WEAPONFLAG_HEAVY);
}

void
CPlayerPed::KeepAreaAroundPlayerClear(void)
{
	BuildPedLists();
	for (int i = 0; i < m_numNearPeds; ++i) {
		CPed *nearPed = m_nearPeds[i];
		if (nearPed->CharCreatedBy == RANDOM_CHAR && nearPed->m_nPedState != PED_DRIVING && !nearPed->DyingOrDead()) {
			if (nearPed->GetIsOnScreen()) {
				if (nearPed->m_objective == OBJECTIVE_NONE) {
					nearPed->SetFindPathAndFlee(this, 5000, true);
				} else {
					if (nearPed->EnteringCar())
						nearPed->QuitEnteringCar();

					nearPed->ClearObjective();
				}
			} else {
				nearPed->FlagToDestroyWhenNextProcessed();
			}
		}
	}
	CVector playerPos = (InVehicle() ? m_pMyVehicle->GetPosition() : GetPosition());

	CVector pos = GetPosition();
	int16 lastVehicle;
	CEntity *vehicles[8];
	CWorld::FindObjectsInRange(pos, CHECK_NEARBY_THINGS_MAX_DIST, true, &lastVehicle, 6, vehicles, false, true, false, false, false);

	for (int i = 0; i < lastVehicle; i++) {
		CVehicle *veh = (CVehicle*)vehicles[i];
		if (veh->VehicleCreatedBy != MISSION_VEHICLE) {
			if (veh->GetStatus() != STATUS_PLAYER && veh->GetStatus() != STATUS_PLAYER_DISABLED) {
				if ((veh->GetPosition() - playerPos).MagnitudeSqr() > 25.0f) {
					veh->AutoPilot.m_nTempAction = TEMPACT_WAIT;
					veh->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 5000;
				} else {
					if (DotProduct2D(playerPos - veh->GetPosition(), veh->GetForward()) > 0.0f)
						veh->AutoPilot.m_nTempAction = TEMPACT_REVERSE;
					else
						veh->AutoPilot.m_nTempAction = TEMPACT_GOFORWARD;

					veh->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 2000;
				}
				CCarCtrl::PossiblyRemoveVehicle(veh, FindPlayerCentreOfWorld(CWorld::PlayerInFocus));
			}
		}
	}
}

void
CPlayerPed::EvaluateNeighbouringTarget(CEntity *candidate, CEntity **targetPtr, float *lastCloseness, float distLimit, float angleOffset, bool lookToLeft, bool priority)
{
	// priority param is unused
	CVector distVec = candidate->GetPosition() - GetPosition();
	if (distVec.Magnitude2D() <= distLimit) {
		if (!DoesTargetHaveToBeBroken(candidate->GetPosition(), GetWeapon())) {
			float angleBetweenUs = CGeneral::GetATanOfXY(candidate->GetPosition().x - TheCamera.GetPosition().x,
				candidate->GetPosition().y - TheCamera.GetPosition().y);

			angleBetweenUs = CGeneral::LimitAngle(angleBetweenUs - angleOffset);
			float closeness;
			if (lookToLeft) {
				closeness = angleBetweenUs > 0.0f ? -Abs(angleBetweenUs) : -100000.0f;
			} else {
				closeness = angleBetweenUs > 0.0f ? -100000.0f : -Abs(angleBetweenUs);
			}

			if (closeness > *lastCloseness) {
				*targetPtr = candidate;
				*lastCloseness = closeness;
			}
		}
	}
}

void
CPlayerPed::EvaluateTarget(CEntity *candidate, CEntity **targetPtr, float *lastCloseness, float distLimit, float angleOffset, bool priority)
{
	CVector distVec = candidate->GetPosition() - GetPosition();
	float dist = distVec.Magnitude2D();
	if (dist <= distLimit) {
		if (!DoesTargetHaveToBeBroken(candidate->GetPosition(), GetWeapon())) {
			float angleBetweenUs = CGeneral::GetATanOfXY(distVec.x, distVec.y);
			angleBetweenUs = CGeneral::LimitAngle(angleBetweenUs - angleOffset);

			float closeness = -dist - 5.0f * Abs(angleBetweenUs);
			if (priority) {
				closeness += 30.0f;
			}

			if (closeness > *lastCloseness) {
				*targetPtr = candidate;
				*lastCloseness = closeness;
			}
		}
	}
}

bool
CPlayerPed::CanIKReachThisTarget(CVector target, CWeapon* weapon, bool zRotImportant)
{
	float angleToFace = CGeneral::GetRadianAngleBetweenPoints(target.x, target.y, GetPosition().x, GetPosition().y);
	float angleDiff = CGeneral::LimitRadianAngle(angleToFace - m_fRotationCur);

	return (!zRotImportant || CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType)->IsFlagSet(WEAPONFLAG_CANAIM_WITHARM) || Abs(angleDiff) <= HALFPI) &&
		(CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType)->IsFlagSet(WEAPONFLAG_CANAIM_WITHARM) || Abs(target.z - GetPosition().z) <= (target - GetPosition()).Magnitude2D());
}

void
CPlayerPed::RotatePlayerToTrackTarget(void)
{
	if (CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->IsFlagSet(WEAPONFLAG_CANAIM_WITHARM))
		return;

	float angleToFace = CGeneral::GetRadianAngleBetweenPoints(
		m_pPointGunAt->GetPosition().x, m_pPointGunAt->GetPosition().y,
		GetPosition().x, GetPosition().y);

	float angleDiff = CGeneral::LimitRadianAngle(m_fRotationCur - angleToFace);
	if (angleDiff < -DEGTORAD(25.0f)) {
		m_fRotationCur -= angleDiff + DEGTORAD(25.0f);
		m_fRotationDest -= angleDiff + DEGTORAD(25.0f);

	} else if (angleDiff > DEGTORAD(25.0f)) {
		m_fRotationCur -= angleDiff - DEGTORAD(25.0f);
		m_fRotationDest -= angleDiff - DEGTORAD(25.0f);
	}
}

bool
CPlayerPed::FindNextWeaponLockOnTarget(CEntity *previousTarget, bool lookToLeft)
{
	CEntity *nextTarget = nil;
	float weaponRange = CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->m_fRange;
	// nextTarget = nil; // duplicate
	float lastCloseness = -10000.0f;
	// CGeneral::GetATanOfXY(GetForward().x, GetForward().y); // unused
	CVector distVec = previousTarget->GetPosition() - TheCamera.GetPosition();
	float referenceBeta = CGeneral::GetATanOfXY(distVec.x, distVec.y);

	for (int h = CPools::GetPedPool()->GetSize() - 1; h >= 0; h--) {
		CPed *pedToCheck = CPools::GetPedPool()->GetSlot(h);
		if (pedToCheck) {
			if (pedToCheck != this && pedToCheck != previousTarget) {
				if (!pedToCheck->DyingOrDead()
#ifndef AIMING_VEHICLE_OCCUPANTS // Mobile thing
					&& (!pedToCheck->bInVehicle || (pedToCheck->m_pMyVehicle && pedToCheck->m_pMyVehicle->IsBike()))
#endif
					&& pedToCheck->m_leader != this && !pedToCheck->bNeverEverTargetThisPed
					&& OurPedCanSeeThisOne(pedToCheck, true) && CanIKReachThisTarget(pedToCheck->GetPosition(), GetWeapon(), true)) {

					EvaluateNeighbouringTarget(pedToCheck, &nextTarget, &lastCloseness,
						weaponRange, referenceBeta, lookToLeft, IsThisPedAnAimingPriority(pedToCheck));
				}
			}
		}
	}
	for (int i = 0; i < ARRAY_SIZE(m_nTargettableObjects); i++) {
		CObject *obj = CPools::GetObjectPool()->GetAt(m_nTargettableObjects[i]);
		if (obj && !obj->bHasBeenDamaged && CanIKReachThisTarget(obj->GetPosition(), GetWeapon(), true))
			EvaluateNeighbouringTarget(obj, &nextTarget, &lastCloseness, weaponRange, referenceBeta, lookToLeft, true);
	}
	if (!nextTarget)
		return false;

	SetWeaponLockOnTarget(nextTarget);
	bDontAllowWeaponChange = true;
	SetPointGunAt(nextTarget);
	return true;
}

bool
CPlayerPed::FindWeaponLockOnTarget(void)
{
	CEntity *nextTarget = nil;
	float weaponRange = CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->m_fRange;

	if (m_pPointGunAt) {
		CVector distVec = m_pPointGunAt->GetPosition() - GetPosition();
		if (distVec.Magnitude2D() > weaponRange) {
			SetWeaponLockOnTarget(nil);
			return false;
		} else {
			return true;
		}
	}

	// nextTarget = nil; // duplicate
	float lastCloseness = -10000.0f;
	float referenceBeta = CGeneral::GetATanOfXY(GetForward().x, GetForward().y);
	for (int h = CPools::GetPedPool()->GetSize() - 1; h >= 0; h--) {
		CPed *pedToCheck = CPools::GetPedPool()->GetSlot(h);
		if (pedToCheck) {
			if (pedToCheck != this) {
				if (!pedToCheck->DyingOrDead()
#ifndef AIMING_VEHICLE_OCCUPANTS // Mobile thing
					&& (!pedToCheck->bInVehicle || (pedToCheck->m_pMyVehicle && pedToCheck->m_pMyVehicle->IsBike()))
#endif
					&& pedToCheck->m_leader != this && !pedToCheck->bNeverEverTargetThisPed
					&& OurPedCanSeeThisOne(pedToCheck) && CanIKReachThisTarget(pedToCheck->GetPosition(), GetWeapon(), true)) {

					EvaluateTarget(pedToCheck, &nextTarget, &lastCloseness,
						weaponRange, referenceBeta, IsThisPedAnAimingPriority(pedToCheck));
				}
			}
		}
	}
	for (int i = 0; i < ARRAY_SIZE(m_nTargettableObjects); i++) {
		CObject *obj = CPools::GetObjectPool()->GetAt(m_nTargettableObjects[i]);
		if (obj && !obj->bHasBeenDamaged && CanIKReachThisTarget(obj->GetPosition(), GetWeapon(), true))
			EvaluateTarget(obj, &nextTarget, &lastCloseness, weaponRange, referenceBeta, true);
	}
	if (!nextTarget)
		return false;

	SetWeaponLockOnTarget(nextTarget);
	bDontAllowWeaponChange = true;
	SetPointGunAt(nextTarget);
	Say(SOUND_PED_AIMING);
	return true;
}

void
CPlayerPed::ProcessAnimGroups(void)
{
	AssocGroupId groupToSet;
#ifdef PC_PLAYER_CONTROLS
	if ((m_fWalkAngle <= -DEGTORAD(50.0f) || m_fWalkAngle >= DEGTORAD(50.0f))
		&& TheCamera.Cams[TheCamera.ActiveCam].Using3rdPersonMouseCam()
		&& CanStrafeOrMouseControl()) {

		if (m_fWalkAngle >= -DEGTORAD(130.0f) && m_fWalkAngle <= DEGTORAD(130.0f)) {
			if (m_fWalkAngle > 0.0f) {
				if (GetWeapon()->m_eWeaponType == WEAPONTYPE_ROCKETLAUNCHER)
					groupToSet = ASSOCGRP_ROCKETLEFT;
				else if (GetWeapon()->m_eWeaponType == WEAPONTYPE_CHAINSAW ||
					GetWeapon()->m_eWeaponType == WEAPONTYPE_FLAMETHROWER ||
					GetWeapon()->m_eWeaponType == WEAPONTYPE_MINIGUN)
					groupToSet = ASSOCGRP_CHAINSAWLEFT;
				else
					groupToSet = ASSOCGRP_PLAYERLEFT;
			} else {
				if (GetWeapon()->m_eWeaponType == WEAPONTYPE_ROCKETLAUNCHER)
					groupToSet = ASSOCGRP_ROCKETRIGHT;
				else if (GetWeapon()->m_eWeaponType == WEAPONTYPE_CHAINSAW ||
					GetWeapon()->m_eWeaponType == WEAPONTYPE_FLAMETHROWER ||
					GetWeapon()->m_eWeaponType == WEAPONTYPE_MINIGUN)
					groupToSet = ASSOCGRP_CHAINSAWRIGHT;
				else
					groupToSet = ASSOCGRP_PLAYERRIGHT;
			}
		} else {
			if (GetWeapon()->m_eWeaponType == WEAPONTYPE_ROCKETLAUNCHER)
				groupToSet = ASSOCGRP_ROCKETBACK;
			else if (GetWeapon()->m_eWeaponType == WEAPONTYPE_CHAINSAW ||
				GetWeapon()->m_eWeaponType == WEAPONTYPE_FLAMETHROWER ||
				GetWeapon()->m_eWeaponType == WEAPONTYPE_MINIGUN)
				groupToSet = ASSOCGRP_CHAINSAWBACK;
			else
				groupToSet = ASSOCGRP_PLAYERBACK;
		}
	} else
#endif
	{
		if (GetWeapon()->m_eWeaponType == WEAPONTYPE_ROCKETLAUNCHER) {
			groupToSet = ASSOCGRP_PLAYERROCKET;
		} else {
			if (GetWeapon()->m_eWeaponType == WEAPONTYPE_BASEBALLBAT
				 || GetWeapon()->m_eWeaponType == WEAPONTYPE_MACHETE)
				groupToSet = ASSOCGRP_PLAYERBBBAT;
			else if (GetWeapon()->m_eWeaponType == WEAPONTYPE_CHAINSAW ||
				GetWeapon()->m_eWeaponType == WEAPONTYPE_FLAMETHROWER ||
				GetWeapon()->m_eWeaponType == WEAPONTYPE_MINIGUN)
				groupToSet = ASSOCGRP_PLAYERCHAINSAW;
			else if (GetWeapon()->m_eWeaponType != WEAPONTYPE_COLT45 && GetWeapon()->m_eWeaponType != WEAPONTYPE_UZI
				// I hope this is an inlined function...
				&& GetWeapon()->m_eWeaponType != WEAPONTYPE_PYTHON && GetWeapon()->m_eWeaponType != WEAPONTYPE_TEC9
				&& GetWeapon()->m_eWeaponType != WEAPONTYPE_SILENCED_INGRAM && GetWeapon()->m_eWeaponType != WEAPONTYPE_MP5
				&& GetWeapon()->m_eWeaponType != WEAPONTYPE_GOLFCLUB && GetWeapon()->m_eWeaponType != WEAPONTYPE_KATANA
				&& GetWeapon()->m_eWeaponType != WEAPONTYPE_CAMERA) {
				if (!GetWeapon()->IsType2Handed()) {
					groupToSet = ASSOCGRP_PLAYER;
				} else {
					groupToSet = ASSOCGRP_PLAYER2ARMED;
				}
			} else {
				groupToSet = ASSOCGRP_PLAYER1ARMED;
			}
		}
	}

	if (m_animGroup != groupToSet) {
		m_animGroup = groupToSet;
		ReApplyMoveAnims();
	}
}

void
CPlayerPed::ProcessPlayerWeapon(CPad *padUsed)
{
	CWeaponInfo *weaponInfo = CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType);
	// The one aim owns every aiming weapon: it keeps the pose, the scope and
	// the viewfinder, and it never lock-ons
	bool aimingWeapon = IsAimableWeapon(GetWeapon()->m_eWeaponType);

	if (!aimingWeapon && m_bHasLockOnTarget && !m_pPointGunAt) {
		TheCamera.ClearPlayerWeaponMode();
		CWeaponEffects::ClearCrossHair();
		ClearPointGunAt();
	}

	if (padUsed->DuckJustDown() && !bIsDucking && m_nMoveState != PEDMOVE_SPRINT) {
#ifdef FIX_BUGS
		// fix tommy being locked into looking at the same spot if you duck just after starting to shoot
		if(!m_pPointGunAt)
			ClearPointGunAt();
#endif
		bCrouchWhenShooting = true;
		SetDuck(60000, true);
	} else if (bIsDucking && (padUsed->DuckJustDown() || m_nMoveState == PEDMOVE_SPRINT ||
		padUsed->GetSprint() || padUsed->JumpJustDown() || padUsed->ExitVehicleJustDown())) {

#ifdef FIX_BUGS
		// same fix as above except for standing up
		if(!m_pPointGunAt)
			ClearPointGunAt();
#endif
		ClearDuck(true);
		bCrouchWhenShooting = false;
	}

	if(weaponInfo->IsFlagSet(WEAPONFLAG_CANAIM))
		m_wepAccuracy = 95;
	else
		m_wepAccuracy = 100;

	if (!m_pFire) {
		// The aim button raises the weapon: the one aim owns the pose and the
		// scope in every state, and the attack path below fires from it
		AimWeapon(padUsed);
	}

	// The sprint stops the hip fire; the aim keeps it, so the aimed player
	// shoots while running (VICE-137)
	if (padUsed->GetWeapon() && (m_bWeaponAimActive || m_nMoveState != PEDMOVE_SPRINT)) {
		if (m_nSelectedWepSlot == m_currentWeapon) {
			if (m_pPointGunAt) {
				if (m_nPedState == PED_ATTACK) {
					m_fAttackButtonCounter *= Pow(0.94f, CTimer::GetTimeStep());
				} else {
					m_fAttackButtonCounter = 0.0f;
				}
				SetAttack(m_pPointGunAt);
			} else {
				if (m_nPedState == PED_ATTACK) {
					if (padUsed->WeaponJustDown()) {
						m_bHaveTargetSelected = true;
					} else if (!m_bHaveTargetSelected) {
						m_fAttackButtonCounter += CTimer::GetTimeStepNonClipped();
					}
				} else {
					m_fAttackButtonCounter = 0.0f;
					m_bHaveTargetSelected = false;
				}
				if (GetWeapon()->m_eWeaponType != WEAPONTYPE_UNARMED && GetWeapon()->m_eWeaponType != WEAPONTYPE_BRASSKNUCKLE &&
					!weaponInfo->IsFlagSet(WEAPONFLAG_FIGHTMODE)) {

					if (GetWeapon()->m_eWeaponType != WEAPONTYPE_DETONATOR && GetWeapon()->m_eWeaponType != WEAPONTYPE_DETONATOR_GRENADE ||
						padUsed->WeaponJustDown())
						SetAttack(nil);

				} else if (padUsed->WeaponJustDown()) {
					if (m_fMoveSpeed < 1.0f || m_nPedState == PED_FIGHT)
						StartFightAttack(padUsed->GetWeapon());
					else
						SetAttack(nil);
				}
			}
		}
	} else {
		m_pedIK.m_flags &= ~CPedIK::LOOKAROUND_HEAD_ONLY;
		if (m_nPedState == PED_ATTACK) {
			m_bHaveTargetSelected = true;
			bIsAttacking = false;
		}
	}

	// The stock lock-on belongs to the melee, the thrown weapons and the
	// detonator; an aiming weapon never picks a target, it aims through the
	// one aim ray
	if (!aimingWeapon && padUsed->GetTarget() && m_nSelectedWepSlot == m_currentWeapon && m_nMoveState != PEDMOVE_SPRINT && !TheCamera.Using1stPersonWeaponMode() && weaponInfo->IsFlagSet(WEAPONFLAG_CANAIM)) {
		if (m_pPointGunAt) {
			// what??
			if (!m_pPointGunAt
#ifdef FREE_CAM
				|| (!CCamera::bFreeCam && CCamera::m_bUseMouse3rdPerson)
#else
				|| CCamera::m_bUseMouse3rdPerson
#endif		
			) {
				ClearWeaponTarget();
				return;
			}

			if (m_pPointGunAt->IsPed() && (
#ifndef AIMING_VEHICLE_OCCUPANTS
				(((CPed*)m_pPointGunAt)->bInVehicle && (!((CPed*)m_pPointGunAt)->m_pMyVehicle || !((CPed*)m_pPointGunAt)->m_pMyVehicle->IsBike())) ||
#endif
				!CGame::nastyGame && ((CPed*)m_pPointGunAt)->DyingOrDead())) {
				ClearWeaponTarget();
				return;
			}
			if (CPlayerPed::DoesTargetHaveToBeBroken(m_pPointGunAt->GetPosition(), GetWeapon()) || 
				(!bCanPointGunAtTarget && !weaponInfo->IsFlagSet(WEAPONFLAG_CANAIM_WITHARM))) { // this line isn't on Mobile, idk why
				ClearWeaponTarget();
				return;
			}

			if (m_pPointGunAt) {
				RotatePlayerToTrackTarget();
			}

			if (m_pPointGunAt) {
				if (padUsed->ShiftTargetLeftJustDown())
					FindNextWeaponLockOnTarget(m_pPointGunAt, true);
				if (padUsed->ShiftTargetRightJustDown())
					FindNextWeaponLockOnTarget(m_pPointGunAt, false);
			}
			TheCamera.SetNewPlayerWeaponMode(CCam::MODE_SYPHON, 0, 0);
			TheCamera.UpdateAimingCoors(m_pPointGunAt->GetPosition());

		} else if (!CCamera::m_bUseMouse3rdPerson) {
			if (padUsed->TargetJustDown() || TheCamera.m_bJustJumpedOutOf1stPersonBecauseOfTarget)
				FindWeaponLockOnTarget();
		}
	} else if (m_pPointGunAt) {
		ClearWeaponTarget();
	}

	if (m_pPointGunAt) {
		CVector markPos;
		if (m_pPointGunAt->IsPed()) {
			((CPed*)m_pPointGunAt)->m_pedIK.GetComponentPosition(markPos, PED_MID);
		} else {
			markPos = m_pPointGunAt->GetPosition();
		}
		if (bCanPointGunAtTarget) {
			CWeaponEffects::MarkTarget(markPos, 64, 0, 0, 255, 0.8f);
		} else {
			CWeaponEffects::MarkTarget(markPos, 64, 32, 0, 255, 0.8f);
		}
	}
	m_bHasLockOnTarget = m_pPointGunAt != nil;
}

bool
CPlayerPed::MovementDisabledBecauseOfTargeting(void)
{
	return m_pPointGunAt && !CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType)->IsFlagSet(WEAPONFLAG_CANAIM_WITHARM);
}

// The movement controller owns the player's horizontal velocity whenever the
// on-foot controls are in charge. The fight state keeps its animation driven
// steps, the free camera hands the ped back to the animations, and the script
// driven states (car entry, get up, seek) are not player movement.
bool
CPlayerPed::IsMoveControllerInCharge(void)
{
	if (m_attachedTo || m_nPedState == PED_FIGHT)
		return false;

	if (m_nPedState != PED_JUMP) {
		switch (m_nPedState) {
		case PED_NONE:
		case PED_IDLE:
		case PED_FLEE_POS:
		case PED_FLEE_ENTITY:
		case PED_ATTACK:
		case PED_AIM_GUN:
		case PED_ANSWER_MOBILE:
		case PED_SNIPER_MODE:
			break;
		default:
			return false;
		}

		if (RpAnimBlendClumpGetFirstAssociation(GetClump(), ASSOC_BLOCK))
			return false;
	}

	return CanStrafeOrMouseControl();
}

// The playback speed the game forces on a movement animation: the adrenaline
// speeds the legs up while the world runs slow, and the fixed camera slows the
// sprint animation down. The playback scaling in SetRealMoveAnim applies on top
// of it.
float
CPlayerPed::GetMoveAnimPlayback(AnimationId animId) const
{
	if (m_bAdrenalineActive)
		return PLAYER_ADRENALINE_ANIM_SPEED;
	if (animId == ANIM_STD_RUNFAST && TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_FIXED)
		return PLAYER_FIXED_CAM_SPRINT_ANIM_SPEED;
	return 1.0f;
}

// The speed the current walk/run/sprint animation blend moves the ped at, in
// engine units: the ground speed of the playing animations at their playback
// speed. The strafe, backward and weapon groups have their own animations with
// their own measured speeds, so the legs and the movement agree in every
// direction and with every weapon. SetRealMoveAnim scales the playback by
// (real speed / this), and the controller moves the ped at this speed.
float
CPlayerPed::GetMoveAnimReferenceSpeed(void)
{
	static const AnimationId moveAnims[] = { ANIM_STD_STARTWALK, ANIM_STD_WALK, ANIM_STD_RUN, ANIM_STD_RUNFAST };
	float referenceSpeed = 0.0f;

	for (int i = 0; i < ARRAY_SIZE(moveAnims); i++) {
		CAnimBlendAssociation *moveAssoc = RpAnimBlendClumpGetAssociation(GetClump(), moveAnims[i]);
		if (moveAssoc)
			referenceSpeed += moveAssoc->blendAmount * moveAssoc->hierarchy->rootSpeed * GetMoveAnimPlayback(moveAnims[i]);
	}
	// The measured root speeds are in units per second; the movement and the
	// playback scaling use engine units (one unit per 1/50 second).
	return referenceSpeed / PLAYER_ENGINE_UNITS_PER_SECOND;
}

// One movement controller for the player, called by every on-foot control path
// in place of its movement block. It owns the player's horizontal velocity
// (m_vecMoveSpeed x/y) in both views: the camera only sets the view position
// and the direction basis. The movement state decides the acceleration and the
// friction, so a jump keeps the momentum and the air steering stays limited.
void
CPlayerPed::UpdatePlayerMovement(CPad *padUsed)
{
	float leftRight = padUsed->GetPedWalkLeftRight();
	float upDown = padUsed->GetPedWalkUpDown();
	// The smooth spray is a weapon behaviour; the fight stance has its own moves
	float smoothSprayRate = m_nPedState == PED_FIGHT ? -1.0f : DoWeaponSmoothSpray();
	// The aim owns the movement while it is up: the spray must not hold the
	// ped in place or turn the body under the aim (VICE-137)
	if (m_bWeaponAimActive)
		smoothSprayRate = -1.0f;
	bool smoothSprayWithoutMove = false;

	if (MovementDisabledBecauseOfTargeting()) {
		leftRight = 0.0f;
		upDown = 0.0f;
	}

	float padMoveInGameUnit = CVector2D(leftRight, upDown).Magnitude() / PAD_MOVE_TO_GAME_WORLD_MOVE;

	// The free camera cases (the first person view and the mouse third person
	// camera) keep the body facing the camera; the classic camera turns the body
	// toward the movement direction.
	bool bFacingCamera = TheCamera.Cams[0].Using3rdPersonMouseCam()
		|| TheCamera.Using1stPersonView()
		;

	// The classic camera holds the ped in place while a smooth spray turns it
	if (!bFacingCamera && smoothSprayRate > 0.0f && upDown > 0.0f) {
		padMoveInGameUnit = 0.0f;
		smoothSprayWithoutMove = true;
	}
#ifdef FREE_CAM
	if (TheCamera.Cams[0].Using3rdPersonMouseCam() && smoothSprayRate > 0.0f) {
		padMoveInGameUnit = 0.0f;
		smoothSprayWithoutMove = false;
	}
#endif

	bool bHasInput = padMoveInGameUnit > 0.0f || smoothSprayWithoutMove;
	if (bHasInput) {
		float padHeading = CGeneral::GetRadianAngleBetweenPoints(0.0f, 0.0f, -leftRight, upDown);
		m_fMoveHeading = CGeneral::LimitRadianAngle(padHeading - TheCamera.Orientation);

		if (!bFacingCamera && smoothSprayRate > 0.0f)
			m_fRotationDest = m_fRotationCur - leftRight / 128.0f * smoothSprayRate * CTimer::GetTimeStep();
		else if (bFacingCamera)
			m_fRotationDest = CGeneral::LimitRadianAngle(TheCamera.Orientation);
		else
			m_fRotationDest = m_fMoveHeading;

		// The walk angle picks the strafe animation groups
		m_fWalkAngle = bFacingCamera ? padHeading : 0.0f;

		// m_fMoveSpeed stays the pad ramp: it picks the walk/run/sprint blend,
		// and SetRealMoveAnim scales the animation playback to the real speed
		m_fMoveSpeed = Min(padMoveInGameUnit, m_fMoveSpeed + 0.07f * CTimer::GetTimeStep());
	} else {
		m_fMoveSpeed = 0.0f;
		m_fWalkAngle = 0.0f;
	}

	if (!IsMoveControllerInCharge())
		return;

	// The ground and the air tune the acceleration and the friction; the slide
	// state will plug in here.
	m_nMovementState = bIsStanding ? PLAYERMOVE_GROUND : PLAYERMOVE_AIR;
	const tPlayerMovementState &state = playerMovementStates[m_nMovementState];

	// The ped's own velocity is the velocity without the surface it stands on
	CVector2D surfaceSpeed = GetSurfaceSpeed();
	CarryWithSurfaceTurn();
	CVector2D velocity = CVector2D(m_vecMoveSpeed) - surfaceSpeed;

	float dt = CTimer::GetTimeStepInSeconds();
	float ownSpeed = velocity.Magnitude();
	// The smooth spray holds the ped in place: the facing turns but the input
	// does not move it
	bool bMoving = padMoveInGameUnit > 0.0f;
	// The ped moves at the ground speed of the animation it plays, so the legs
	// stay with the movement in every direction and with every weapon
	float targetSpeed = bMoving ? GetMoveAnimReferenceSpeed() : 0.0f;
	CVector2D moveDirection(-Sin(m_fMoveHeading), Cos(m_fMoveHeading));
	CVector2D targetVelocity = moveDirection * targetSpeed;

	if (m_nMovementState == PLAYERMOVE_AIR) {
		if (bMoving) {
			// Keep the momentum and steer toward the input
			if (ownSpeed > 0.0f)
				velocity = SteerVelocityTowards(velocity, moveDirection, state.turnRate * dt);
			if (ownSpeed < targetSpeed) {
				// Air control speeds the ped up to the target, never past it
				velocity = MoveVelocityTowards(velocity, targetVelocity, state.accel * dt);
				float newSpeed = velocity.Magnitude();
				if (newSpeed > targetSpeed)
					velocity *= targetSpeed / newSpeed;
			}
		}
	} else if (m_nSurfaceTouched == SURFACE_STEEP_CLIFF && (m_vecDamageNormal.x != 0.0f || m_vecDamageNormal.y != 0.0f)) {
		// A steep slope pushes the ped off instead of taking the input
		PushOffSteepSlope(velocity);
	} else if (bMoving) {
		// The direction follows the input at the ground turn rate, so the legs
		// stay with the movement; the speed follows the target at the
		// acceleration or the friction.
		if (ownSpeed > 0.0f)
			velocity = SteerVelocityTowards(velocity, moveDirection, state.turnRate * dt);
		float rate = ownSpeed > targetSpeed ? state.friction : state.accel;
		velocity = MoveVelocityTowards(velocity, targetVelocity, rate * dt);
	} else {
		velocity = MoveVelocityTowards(velocity, CVector2D(0.0f, 0.0f), state.friction * dt);
	}

	m_vecMoveSpeed.x = velocity.x + surfaceSpeed.x;
	m_vecMoveSpeed.y = velocity.y + surfaceSpeed.y;
	m_moved = velocity;
}

void
CPlayerPed::PlayerControlZelda(CPad *padUsed)
{
	// The classic camera turns the body toward the movement direction; the
	// controller owns the velocity
	UpdatePlayerMovement(padUsed);

	if (m_nPedState == PED_ANSWER_MOBILE) {
		SetRealMoveAnim();
		return;
	}

	// The one aim runs before the sprint and the jump: the aim gates the heavy
	// weapon controls (VICE-137)
	AimWeapon(padUsed);

	if (PlayerWantsSprint(padUsed)) {
		if (!m_pCurrentPhysSurface || (!m_pCurrentPhysSurface->bInfiniteMass || m_pCurrentPhysSurface->m_phy_flagA08))
			m_nMoveState = PEDMOVE_SPRINT;
	}

	if (m_nPedState != PED_FIGHT)
		SetRealMoveAnim();

	PlayerJumpOrDive(padUsed);
	PlayIdleAnimations(padUsed);
}

// Finds nice positions for peds to duck and shoot player. And it's inside PlayerPed, this is treachery!
void
CPlayerPed::FindNewAttackPoints(void)
{
	for (int i=0; i<ARRAY_SIZE(m_pPedAtSafePos); i++) {
		CPed *safeNeighbour = m_pPedAtSafePos[i];
		if (safeNeighbour) {
			if (safeNeighbour->m_nPedState == PED_DEAD || safeNeighbour->m_pedInObjective != this) {
				m_vecSafePos[i].x = 0.0f;
				m_vecSafePos[i].y = 0.0f;
				m_vecSafePos[i].z = 0.0f;
				m_pPedAtSafePos[i] = nil;
			}
		} else {
			m_vecSafePos[i].x = 0.0f;
			m_vecSafePos[i].y = 0.0f;
			m_vecSafePos[i].z = 0.0f;
		}
	}
	CEntity *entities[6];
	int16 numEnts;
	float rightMult, fwdMult;
	CWorld::FindObjectsInRange(GetPosition(), 18.0f, true, &numEnts, 6, entities, true, false, false, true, false);
	for (int i = 0; i < numEnts; ++i) {
		CEntity *ent = entities[i];
		int16 mi = ent->GetModelIndex();
		if (!ent->IsObject() || ((CObject*)ent)->m_nSpecialCollisionResponseCases == COLLRESPONSE_FENCEPART)
			if (!IsTreeModel(mi))
				continue;

		if (mi == MI_TRAFFICLIGHTS) {
			rightMult = 2.957f;
			fwdMult = 0.147f;

		} else if (mi == MI_SINGLESTREETLIGHTS1) {
			rightMult = 0.744f;
			fwdMult = 0.0f;

		} else if (mi == MI_SINGLESTREETLIGHTS2) {
			rightMult = 0.043f;
			fwdMult = 0.0f;

		} else if (mi == MI_SINGLESTREETLIGHTS3) {
			rightMult = 1.143f;
			fwdMult = 0.145f;

		} else if (mi == MI_DOUBLESTREETLIGHTS) {
			rightMult = 0.744f;
			fwdMult = 0.0f;

		} else if (mi == MI_LAMPPOST1) {
			rightMult = 0.744f;
			fwdMult = 0.0f;

		} else if (mi == MI_TRAFFICLIGHT01) {
			rightMult = 2.957f;
			fwdMult = 0.147f;

		} else if (mi == MI_LITTLEHA_POLICE) {
			rightMult = 0.0f;
			fwdMult = 0.0f;

		} else if (mi == MI_PARKBENCH) {
			rightMult = 0.0f;
			fwdMult = 0.0f;

		} else if (IsTreeModel(mi)) {
			rightMult = 0.0f;
			fwdMult = 0.0f;
		} else
			continue;

		CVector entAttackPoint(rightMult * ent->GetRight().x + fwdMult * ent->GetForward().x + ent->GetPosition().x,
			rightMult * ent->GetRight().y + fwdMult * ent->GetForward().y + ent->GetPosition().y,
			ent->GetPosition().z);
		CVector attackerPos = GetPosition() - entAttackPoint; // for now it's dist, not attackerPos
		CVector dirTowardsUs = attackerPos;
		dirTowardsUs.Normalise();
		dirTowardsUs *= 2.0f;
		attackerPos = entAttackPoint - dirTowardsUs; // to make cop farther from us
		CPedPlacement::FindZCoorForPed(&attackerPos);
		if (CPedPlacement::IsPositionClearForPed(attackerPos))
			m_vecSafePos[i] = attackerPos;
	}
}

void
CPlayerPed::ProcessControl(void)
{
	// Mobile has some debug/abandoned cheat thing in here: "gbFrankenTommy"

	if (m_nEvadeAmount != 0)
		--m_nEvadeAmount;

	if (m_nEvadeAmount == 0)
		m_pEvadingFrom = nil;

	if (m_pWanted->GetWantedLevel() > 0)
		FindNewAttackPoints();
	
	UpdateMeleeAttackers();

	if (m_pCurrentPhysSurface && m_pCurrentPhysSurface->IsVehicle() && ((CVehicle*)m_pCurrentPhysSurface)->IsBoat()) {
		bTryingToReachDryLand = true;

	} else if (!(((uint8)CTimer::GetFrameCounter() + m_randomSeed) & 0xF)) {
		CVehicle *nearVeh = (CVehicle*)CWorld::TestSphereAgainstWorld(GetPosition(), 7.0f, nil, false, true, false, false, false, false);
		if (nearVeh && nearVeh->IsBoat())
			bTryingToReachDryLand = true;
		else
			bTryingToReachDryLand = false;
	}

	if (m_nFadeDrunkenness) {
		if (m_nDrunkenness - 1 > 0) {
			--m_nDrunkenness;
		} else {
			m_nDrunkenness = 0;
			CMBlur::ClearDrunkBlur();
			m_nFadeDrunkenness = 0;
		}
	}
	if (m_nDrunkenness != 0) {
		CMBlur::SetDrunkBlur(m_nDrunkenness / 255.f);
	}
	CPed::ProcessControl();
	SetNearbyPedsToInteractWithPlayer();
	if (bWasPostponed)
		return;

	CPad *padUsed = GetPadFromPlayer(this);
	m_pWanted->Update();
	PruneReferences();

	if (GetWeapon()->m_eWeaponType == WEAPONTYPE_MINIGUN) {
		CWeaponInfo *weaponInfo = CWeaponInfo::GetWeaponInfo(GetWeapon()->m_eWeaponType);
		CAnimBlendAssociation *fireAnim = RpAnimBlendClumpGetAssociation(GetClump(), GetPrimaryFireAnim(weaponInfo));
		if (fireAnim && fireAnim->currentTime - fireAnim->timeStep < weaponInfo->m_fAnimLoopEnd && m_nPedState == PED_ATTACK) {
			if (m_fGunSpinSpeed < 0.45f) {
				m_fGunSpinSpeed = Min(0.45f, m_fGunSpinSpeed + CTimer::GetTimeStep() * 0.013f);
			}

			if (padUsed->GetWeapon() && GetWeapon()->m_nAmmoTotal > 0 && fireAnim->currentTime >= weaponInfo->m_fAnimLoopStart) {
				DMAudio.PlayOneShot(m_audioEntityId, SOUND_WEAPON_MINIGUN_ATTACK, 0.0f);
			} else {
				DMAudio.PlayOneShot(m_audioEntityId, SOUND_WEAPON_MINIGUN_2, m_fGunSpinSpeed * (20.f / 9));
			}
		} else {
			if (m_fGunSpinSpeed > 0.0f) {
				if (m_fGunSpinSpeed >= 0.45f) {
					DMAudio.PlayOneShot(m_audioEntityId, SOUND_WEAPON_MINIGUN_3,  0.0f);
				}
				m_fGunSpinSpeed = Max(0.0f, m_fGunSpinSpeed - CTimer::GetTimeStep() * 0.003f);
			}
		}
	}
	if (GetWeapon()->m_eWeaponType == WEAPONTYPE_CHAINSAW && m_nPedState != PED_ATTACK && !bInVehicle) {
		DMAudio.PlayOneShot(m_audioEntityId, SOUND_WEAPON_CHAINSAW_IDLE, 0.0f);
	}

	if (m_nMoveState != PEDMOVE_RUN && m_nMoveState != PEDMOVE_SPRINT)
		RestoreSprintEnergy(1.0f);
	else if (m_nMoveState == PEDMOVE_RUN)
		RestoreSprintEnergy(0.3f);

	if (m_nPedState == PED_DEAD) {
		StopWeaponAim();
		ClearWeaponTarget();
		return;
	}
	if (m_nPedState == PED_DIE) {
		StopWeaponAim();
		ClearWeaponTarget();
		if (CTimer::GetTimeInMilliseconds() > m_bloodyFootprintCountOrDeathTime + 4000)
			SetDead();
		return;
	}
	if (m_nPedState == PED_DRIVING && m_objective != OBJECTIVE_LEAVE_CAR) {
		// The seat owns the aim: the vehicle branch starts and stops it, so
		// this path must not clear the state the vehicle just set.
		if (!CReplay::IsPlayingBack() || m_pMyVehicle) {
			if (m_pMyVehicle->IsCar() && ((CAutomobile*)m_pMyVehicle)->Damage.GetDoorStatus(DOOR_FRONT_LEFT) == DOOR_STATUS_SWINGING) {
				CAnimBlendAssociation *rollDoorAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_CAR_CLOSE_DOOR_ROLLING_LHS);

				if (m_pMyVehicle->m_nGettingOutFlags & CAR_DOOR_FLAG_LF || rollDoorAssoc || (rollDoorAssoc = RpAnimBlendClumpGetAssociation(GetClump(), ANIM_STD_CAR_CLOSE_DOOR_ROLLING_LO_LHS))) {
					if (rollDoorAssoc)
						m_pMyVehicle->ProcessOpenDoor(CAR_DOOR_LF, ANIM_STD_CAR_CLOSE_DOOR_ROLLING_LHS, rollDoorAssoc->currentTime);

				} else {
					// These comparisons are wrong, they return uint16
					if (padUsed && (padUsed->GetAccelerate() != 0.0f || padUsed->GetSteeringLeftRight() != 0.0f || padUsed->GetBrake() != 0.0f)) {
						if (rollDoorAssoc)
							m_pMyVehicle->ProcessOpenDoor(CAR_DOOR_LF, ANIM_STD_CAR_CLOSE_DOOR_ROLLING_LHS, rollDoorAssoc->currentTime);

					} else {
						m_pMyVehicle->m_nGettingOutFlags |= CAR_DOOR_FLAG_LF;
						if (m_pMyVehicle->bLowVehicle)
							rollDoorAssoc = CAnimManager::AddAnimation(GetClump(), ASSOCGRP_STD, ANIM_STD_CAR_CLOSE_DOOR_ROLLING_LO_LHS);
						else
							rollDoorAssoc = CAnimManager::AddAnimation(GetClump(), ASSOCGRP_STD, ANIM_STD_CAR_CLOSE_DOOR_ROLLING_LHS);

						rollDoorAssoc->SetFinishCallback(PedAnimDoorCloseRollingCB, this);
					}
				}
			}
		}
		return;
	}
	if (m_objective == OBJECTIVE_NONE)
		m_nMoveState = PEDMOVE_STILL;
	if (bIsLanding)
		RunningLand(padUsed);

	if (padUsed && padUsed->WeaponJustDown() && !TheCamera.Using1stPersonWeaponMode()) {
		// ...Really?
		eWeaponType playerWeapon = FindPlayerPed()->GetWeapon()->m_eWeaponType;
		if (playerWeapon == WEAPONTYPE_SNIPERRIFLE || playerWeapon == WEAPONTYPE_LASERSCOPE) {
			DMAudio.PlayFrontEndSound(SOUND_WEAPON_SNIPER_SHOT_NO_ZOOM, 0);
		} else if (playerWeapon == WEAPONTYPE_ROCKETLAUNCHER) {
			DMAudio.PlayFrontEndSound(SOUND_WEAPON_ROCKET_SHOT_NO_ZOOM, 0);
		}
	}

	switch (m_nPedState) {
		case PED_NONE:
		case PED_IDLE:
		case PED_FLEE_POS:
		case PED_FLEE_ENTITY:
		case PED_ATTACK:
		case PED_FIGHT:
		case PED_AIM_GUN:
		case PED_ANSWER_MOBILE:
			if (!RpAnimBlendClumpGetFirstAssociation(GetClump(), ASSOC_BLOCK) && !m_attachedTo) {
				if (padUsed) {
					// One on-foot control per view: the camera-facing walk
					// (the first person and the mouse third person views, and
					// the scope and the viewfinder views, where the player
					// keeps moving), then the fight stance and the classic
					// walk.
					if (TheCamera.Using1stPersonView()
						|| TheCamera.IsSightCameraMode(TheCamera.PlayerWeaponMode.Mode)
#ifdef FREE_CAM
						|| TheCamera.Cams[0].Using3rdPersonMouseCam() && !CCamera::bFreeCam
#else
						|| TheCamera.Cams[0].Using3rdPersonMouseCam()
#endif
						)
						PlayerControl1stPersonRunAround(padUsed);
					else if (m_nPedState == PED_FIGHT)
						PlayerControlFighter(padUsed);
					else
						PlayerControlZelda(padUsed);
				}
			}
			if (IsPedInControl() && m_nPedState != PED_ANSWER_MOBILE && padUsed)
				ProcessPlayerWeapon(padUsed);
			break;
		case PED_SEEK_ENTITY:
			m_vecSeekPos = m_pSeekTarget->GetPosition();

			// fall through
		case PED_SEEK_POS:
			switch (m_nMoveState) {
				case PEDMOVE_WALK:
					m_fMoveSpeed = 1.0f;
					break;
				case PEDMOVE_RUN:
					m_fMoveSpeed = 1.8f;
					break;
				case PEDMOVE_SPRINT:
					m_fMoveSpeed = 2.5f;
					break;
				default:
					m_fMoveSpeed = 0.0f;
					break;
			}
			SetRealMoveAnim();
			if (Seek()) {
				RestorePreviousState();
				SetMoveState(PEDMOVE_STILL);
			}
			break;
		case PED_SNIPER_MODE:
			// The gunner attached to an entity (the heli cannon): the
			// attachment owns the state and the weapon, the view aims and the
			// held fire button shoots along it. The on-foot aim never runs for
			// an attached player.
			if (m_attachedTo && padUsed)
				FireWeaponWhenHeld(padUsed);
			break;
		case PED_SEEK_CAR:
		case PED_SEEK_IN_BOAT:
			if (bVehEnterDoorIsBlocked || bKindaStayInSamePlace) {
				m_fMoveSpeed = 0.0f;
			} else {
				m_fMoveSpeed = Min(2.0f, 2.0f * (m_vecSeekPos - GetPosition()).Magnitude2D());
			}
			if (padUsed && !padUsed->ArePlayerControlsDisabled()) {
				if (padUsed->GetTarget() || padUsed->GetLeftStickXJustDown() || padUsed->GetLeftStickYJustDown() ||
					padUsed->GetDPadUpJustDown() || padUsed->GetDPadDownJustDown() || padUsed->GetDPadLeftJustDown() ||
					padUsed->GetDPadRightJustDown()) {

					RestorePreviousState();
					if (m_objective == OBJECTIVE_ENTER_CAR_AS_PASSENGER || m_objective == OBJECTIVE_ENTER_CAR_AS_DRIVER) {
						RestorePreviousObjective();
					}
				}
			}
			if (padUsed && padUsed->GetSprint())
				m_nMoveState = PEDMOVE_SPRINT;
			SetRealMoveAnim();
			break;
		case PED_JUMP:
			if (padUsed)
				PlayerControlZelda(padUsed);
			if (bIsLanding)
				break;

			// The airborne player keeps the aim and fires: the jump state owns
			// the landing, so the shot comes straight from the weapon and the
			// attack state stays off.
			if (padUsed)
				ProcessAirborneWeapon(padUsed);

			// This has been added later it seems
			return;
		case PED_FALL:
			// The falling player keeps the aim and fires: the aim and the pose
			// hold through the air (VICE-137), and so does the weapon
			if (padUsed)
				ProcessAirborneWeapon(padUsed);
			return;
		case PED_GETUP:
		case PED_ENTER_TRAIN:
		case PED_EXIT_TRAIN:
		case PED_CARJACK:
		case PED_DRAG_FROM_CAR:
		case PED_ENTER_CAR:
		case PED_STEAL_CAR:
		case PED_EXIT_CAR:
			// The aim cannot continue into a scripted move or a car
			StopWeaponAim();
			ClearWeaponTarget();
			break;
		case PED_ARRESTED:
			if (m_nLastPedState == PED_DRAG_FROM_CAR && m_pVehicleAnim)
				BeingDraggedFromCar();
			break;
		default:
			// Every other state cannot hold the aiming weapon
			StopWeaponAim();
			break;
	}
	if (padUsed && IsPedShootable() && m_nPedState != PED_ANSWER_MOBILE && m_nLastPedState != PED_ANSWER_MOBILE) {
		ProcessWeaponSwitch(padUsed);
		ProcessActiveReload(padUsed);
		GetWeapon()->Update(m_audioEntityId, this);
	}
	ProcessAnimGroups();
	if (padUsed) {
		if (TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_FOLLOWPED
			&& TheCamera.Cams[TheCamera.ActiveCam].DirectionWasLooking == LOOKING_BEHIND) {

			m_lookTimer = 0;
			float camAngle = CGeneral::LimitRadianAngle(TheCamera.Cams[TheCamera.ActiveCam].Front.Heading());
			float angleBetweenPlayerAndCam = Abs(camAngle - m_fRotationCur);

			if (m_nPedState != PED_ATTACK && angleBetweenPlayerAndCam > DEGTORAD(30.0f) && angleBetweenPlayerAndCam < DEGTORAD(330.0f)) {
				if (angleBetweenPlayerAndCam > DEGTORAD(150.0f) && angleBetweenPlayerAndCam < DEGTORAD(210.0f)) {
					float rightTurnAngle = CGeneral::LimitRadianAngle(m_fRotationCur - DEGTORAD(150.0f));
					float leftTurnAngle = CGeneral::LimitRadianAngle(DEGTORAD(150.0f) + m_fRotationCur);

					if (m_fLookDirection == 999999.0f || bIsDucking)
						camAngle = rightTurnAngle;
					else if (Abs(rightTurnAngle - m_fLookDirection) < Abs(leftTurnAngle - m_fLookDirection))
						camAngle = rightTurnAngle;
					else
						camAngle = leftTurnAngle;
				}
				SetLookFlag(camAngle, true);
				SetLookTimer(CTimer::GetTimeStepInMilliseconds() * 5.0f);
			} else {
				ClearLookFlag();
			}
		}
	}
	if (m_nMoveState == PEDMOVE_SPRINT && bIsLooking) {
		ClearLookFlag();
		SetLookTimer(250);
	}

	if (m_vecMoveSpeed.Magnitude2D() < 0.1f) {
		if (m_nSpeedTimer) {
			if (CTimer::GetTimeInMilliseconds() > m_nSpeedTimer)
				m_bSpeedTimerFlag = true;
		} else {
			m_nSpeedTimer = CTimer::GetTimeInMilliseconds() + 500;
		}
	} else {
		m_nSpeedTimer = 0;
		m_bSpeedTimerFlag = false;
	}

	if (bDontAllowWeaponChange && FindPlayerPed() == this) {
		if (!CPad::GetPad(0)->GetTarget())
			bDontAllowWeaponChange = false;
	}

	if (m_nPedState != PED_SNIPER_MODE && (GetWeapon()->m_eWeaponState == WEAPONSTATE_FIRING || m_nPedState == PED_ATTACK))
		m_nPadDownPressedInMilliseconds = CTimer::GetTimeInMilliseconds();

	if (!bIsVisible)
		UpdateRpHAnim();
}

bool
CPlayerPed::DoesPlayerWantNewWeapon(eWeaponType weapon, bool onlyIfSlotIsEmpty)
{
	// GetPadFromPlayer(); // unused
	uint32 slot = CWeaponInfo::GetWeaponInfo(weapon)->m_nWeaponSlot;

	if (!HasWeaponSlot(slot) || GetWeapon(slot).m_eWeaponType == weapon)
		return true;

	if (onlyIfSlotIsEmpty)
		return false;

	// Check if he's using that slot right now.
	return m_nPedState != PED_ATTACK && m_nPedState != PED_AIM_GUN || slot != m_currentWeapon;
}

void
CPlayerPed::PlayIdleAnimations(CPad *padUsed)
{
	CAnimBlendAssociation* assoc;

	if (TheCamera.m_WideScreenOn || bIsDucking)
		return;

	struct animAndGroup {
		AnimationId animId;
		AssocGroupId groupId;
	};

	const animAndGroup idleAnims[] = {
		{ANIM_PLAYER_IDLE1, ASSOCGRP_PLAYER_IDLE},
		{ANIM_PLAYER_IDLE2, ASSOCGRP_PLAYER_IDLE},
		{ANIM_PLAYER_IDLE3, ASSOCGRP_PLAYER_IDLE},
		{ANIM_PLAYER_IDLE4, ASSOCGRP_PLAYER_IDLE},
		{ANIM_STD_XPRESS_SCRATCH, ASSOCGRP_STD},
	};

	static int32 lastTime = 0;
	static int32 lastAnim = -1;

	bool hasIdleAnim = false;
	CAnimBlock *idleAnimBlock = CAnimManager::GetAnimationBlock(idleAnimBlockIndex);
	uint32 sinceLastInput = padUsed->InputHowLongAgo();
	if (sinceLastInput <= 30000) {
		if (idleAnimBlock->isLoaded) {
			for (assoc = RpAnimBlendClumpGetFirstAssociation(GetClump()); assoc; assoc = RpAnimBlendGetNextAssociation(assoc)) {
				if (assoc->flags & ASSOC_IDLE) {
					hasIdleAnim = true;
					assoc->blendDelta = -8.0f;
				}
			}
			if (!hasIdleAnim)
				CStreaming::RemoveAnim(idleAnimBlockIndex);
		} else {
			lastTime = 0;
		}
	} else {
		CStreaming::RequestAnim(idleAnimBlockIndex, STREAMFLAGS_DONT_REMOVE);
		if (idleAnimBlock->isLoaded) {
			for(CAnimBlendAssociation *assoc = RpAnimBlendClumpGetFirstAssociation(GetClump()); assoc; assoc = RpAnimBlendGetNextAssociation(assoc)) {
				int firstIdle = idleAnimBlock->firstIndex;
				int index = assoc->hierarchy - CAnimManager::GetAnimation(0);
				if (index >= firstIdle && index < firstIdle + idleAnimBlock->numAnims) {
					hasIdleAnim = true;
					break;
				}
			}

			if (!hasIdleAnim && !bIsLooking && !bIsRestoringLook && sinceLastInput - lastTime > 25000) {
				int anim;
				do
					anim = CGeneral::GetRandomNumberInRange(0, ARRAY_SIZE(idleAnims));
				while (lastAnim == anim);

				assoc = CAnimManager::BlendAnimation(GetClump(), idleAnims[anim].groupId, idleAnims[anim].animId, 8.0f);
				assoc->flags |= ASSOC_IDLE;
				lastAnim = anim;
				lastTime = sinceLastInput;
			}
		}
	}
}

void
CPlayerPed::SetNearbyPedsToInteractWithPlayer(void)
{
	if (CGame::noProstitutes)
		return;

	for (int i = 0; i < m_numNearPeds; ++i) {
		CPed *nearPed = m_nearPeds[i];
		if (nearPed && nearPed->m_objectiveTimer < CTimer::GetTimeInMilliseconds() && !CTheScripts::IsPlayerOnAMission()) {
			int mi = nearPed->GetModelIndex();
			if (CPopulation::CanSolicitPlayerOnFoot(mi)) {
				CVector distToMe = nearPed->GetPosition() - GetPosition();
				CVector dirToMe = GetPosition() - nearPed->GetPosition();
				dirToMe.Normalise();
				if (DotProduct(dirToMe, nearPed->GetForward()) > 0.707 && DotProduct(GetForward(), nearPed->GetForward()) < -0.707 // those are double
					&& distToMe.MagnitudeSqr() < 9.0f && nearPed->m_objective == OBJECTIVE_NONE) {
					nearPed->SetObjective(OBJECTIVE_SOLICIT_FOOT, this);
					nearPed->m_objectiveTimer = CTimer::GetTimeInMilliseconds() + 10000;
					nearPed->Say(SOUND_PED_SOLICIT);
				}
			} else if (CPopulation::CanSolicitPlayerInCar(mi)) {
				if (InVehicle() && m_pMyVehicle->IsVehicleNormal()) {
					if (m_pMyVehicle->IsCar()) {
						CVector distToVeh = nearPed->GetPosition() - m_pMyVehicle->GetPosition();
						if (distToVeh.MagnitudeSqr() < 25.0f && m_pMyVehicle->IsRoomForPedToLeaveCar(CAR_DOOR_LF, nil) && nearPed->m_objective == OBJECTIVE_NONE) {
							nearPed->SetObjective(OBJECTIVE_SOLICIT_VEHICLE, m_pMyVehicle);
						}
					}
				}
			}
		}
	}
}

void
CPlayerPed::UpdateMeleeAttackers(void)
{
	CVector attackCoord;
	if (((CTimer::GetFrameCounter() + m_randomSeed + 7) & 3) == 0) {
		GetMeleeAttackCoords(attackCoord, m_nAttackDirToCheck, 2.0f);

		// Check if there is any vehicle/building inbetween us and m_nAttackDirToCheck. Peds will be able to attack us from those available directions.
		if (CWorld::GetIsLineOfSightClear(GetPosition(), attackCoord, true, true, false, true, false, false, false)
			&& !CWorld::TestSphereAgainstWorld(attackCoord, 0.4f, m_pMeleeList[m_nAttackDirToCheck], true, true, false, true, false, false)) {
			if (m_pMeleeList[m_nAttackDirToCheck] == this)
				m_pMeleeList[m_nAttackDirToCheck] = nil; // mark it as available
		} else {
			m_pMeleeList[m_nAttackDirToCheck] = this; // slot not available. useful for m_bNoPosForMeleeAttack
		}
		if (++m_nAttackDirToCheck >= ARRAY_SIZE(m_pMeleeList))
			m_nAttackDirToCheck = 0;
	}
	// 6 directions
	for (int i = 0; i < ARRAY_SIZE(m_pMeleeList); ++i) {
		CPed *victim = m_pMeleeList[i];
		if (victim && victim != this) {
			if (victim->m_nPedState != PED_DEAD && victim->m_pedInObjective == this)  {
				if (victim->m_objective == OBJECTIVE_KILL_CHAR_ON_FOOT || victim->m_objective == OBJECTIVE_KILL_CHAR_ANY_MEANS || victim->m_objective == OBJECTIVE_KILL_CHAR_ON_BOAT) {
					GetMeleeAttackCoords(attackCoord, i, 2.0f);
					if ((attackCoord - GetPosition()).MagnitudeSqr() > 12.25f)
						m_pMeleeList[i] = nil;
				} else {
					m_pMeleeList[i] = nil;
				}
			} else {
				m_pMeleeList[i] = nil;
			}
		}
	}
	m_bNoPosForMeleeAttack = m_pMeleeList[0] == this && m_pMeleeList[1] == this && m_pMeleeList[2] == this
#ifdef FIX_BUGS
		&& m_pMeleeList[3] == this
#endif
		&& m_pMeleeList[4] == this && m_pMeleeList[5] == this;
}

void
CPlayerPed::RemovePedFromMeleeList(CPed *ped)
{
	for (uint16 i = 0; i < ARRAY_SIZE(m_pMeleeList); i++) {
		if (m_pMeleeList[i] == ped) {
			m_pMeleeList[i] = nil;
			ped->m_attackTimer = 0;
			return;
		}
	}
}

void
CPlayerPed::GetMeleeAttackCoords(CVector& coords, int8 dir, float dist)
{
	coords = GetPosition();
	switch (dir) {
		case 0:
			coords.y += dist;
			break;
		case 1:
			coords.x += Sqrt(3.f / 4.f) * dist;
			coords.y += 0.5f * dist;
			break;
		case 2:
			coords.x += Sqrt(3.f / 4.f) * dist;
			coords.y -= 0.5f * dist;
			break;
		case 3:
			coords.y -= dist;
			break;
		case 4:
			coords.x -= Sqrt(3.f / 4.f) * dist;
			coords.y -= 0.5f * dist;
			break;
		case 5:
			coords.x -= Sqrt(3.f / 4.f) * dist;
			coords.y += 0.5f * dist;
			break;
		default:
			break;
	}
}

int32
CPlayerPed::FindMeleeAttackPoint(CPed *victim, CVector &dist, uint32 &endOfAttackOut)
{
	endOfAttackOut = 0;
	bool thereIsAnEmptySlot = false;
	int dirToAttack = -1;
	for (int i = 0; i < ARRAY_SIZE(m_pMeleeList); i++) {
		CPed* pedAtThisDir = m_pMeleeList[i];
		if (pedAtThisDir) {
			if (pedAtThisDir == victim) {
				dirToAttack = i;
			} else {
				if (pedAtThisDir->m_attackTimer > endOfAttackOut)
					endOfAttackOut = pedAtThisDir->m_attackTimer;
			}
		} else {
			thereIsAnEmptySlot = true;
		}
	}

	// We don't have victim ped in our melee list
	if (dirToAttack == -1 && thereIsAnEmptySlot) {
		float angle = Atan2(-dist.x, -dist.y);
		float adjustedAngle = angle + DEGTORAD(30.0f);
		if (adjustedAngle < 0.f)
			adjustedAngle += TWOPI;

		int wantedDir = Floor(adjustedAngle / DEGTORAD(60.0f));

		// And we have another ped at the direction of victim ped, so store victim to next empty direction to it's real direction. (Bollocks)
		if (m_pMeleeList[wantedDir]) {
			int closestDirToPreferred = -99;
			int preferredDir = wantedDir;

			for (int i = 0; i < ARRAY_SIZE(m_pMeleeList); i++) {
				if (!m_pMeleeList[i]) {
					if (Abs(i - preferredDir) < Abs(closestDirToPreferred - preferredDir))
						closestDirToPreferred = i;
				}
			}
			if (closestDirToPreferred > 0)
				dirToAttack = closestDirToPreferred;
		} else {

			// Luckily the direction of victim ped is already empty, good
			dirToAttack = wantedDir;
		}

		if (dirToAttack != -1) {
			m_pMeleeList[dirToAttack] = victim;
			victim->RegisterReference((CEntity**) &m_pMeleeList[dirToAttack]);
			if (endOfAttackOut > CTimer::GetTimeInMilliseconds())
				victim->m_attackTimer = endOfAttackOut + CGeneral::GetRandomNumberInRange(1000, 2000);
			else
				victim->m_attackTimer = CTimer::GetTimeInMilliseconds() + CGeneral::GetRandomNumberInRange(500, 1000);
		}
	}
	return dirToAttack;
}

#ifdef COMPATIBLE_SAVES
#define CopyFromBuf(buf, data) memcpy(&data, buf, sizeof(data)); SkipSaveBuf(buf, sizeof(data));
#define CopyToBuf(buf, data) memcpy(buf, &data, sizeof(data)); SkipSaveBuf(buf, sizeof(data));
void
CPlayerPed::Save(uint8*& buf)
{
	CPed::Save(buf);
	ZeroSaveBuf(buf, 16);
	CopyToBuf(buf, m_fMaxStamina);
	ZeroSaveBuf(buf, 28);
	CopyToBuf(buf, m_nTargettableObjects[0]);
	CopyToBuf(buf, m_nTargettableObjects[1]);
	CopyToBuf(buf, m_nTargettableObjects[2]);
	CopyToBuf(buf, m_nTargettableObjects[3]);
	ZeroSaveBuf(buf, 164);
}

void
CPlayerPed::Load(uint8*& buf)
{
	CPed::Load(buf);
	SkipSaveBuf(buf, 16);
	CopyFromBuf(buf, m_fMaxStamina);
	SkipSaveBuf(buf, 28);
	CopyFromBuf(buf, m_nTargettableObjects[0]);
	CopyFromBuf(buf, m_nTargettableObjects[1]);
	CopyFromBuf(buf, m_nTargettableObjects[2]);
	CopyFromBuf(buf, m_nTargettableObjects[3]);
	SkipSaveBuf(buf, 164);
}
#undef CopyFromBuf
#undef CopyToBuf
#endif
