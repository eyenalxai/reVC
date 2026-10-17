#ifndef __GTA_PLAYERPED_H__
#define __GTA_PLAYERPED_H__

#include "Ped.h"
#include "PlayerMoveSolver.h"

class CPad;
class CCopPed;
class CWanted;

class CPlayerPed : public CPed
{
public:
	CWanted *m_pWanted;
	CCopPed *m_pArrestingCop;
	float m_fMoveSpeed;
	float m_fCurrentStamina;
	float m_fMaxStamina;
	float m_fStaminaProgress;
	int8 m_nSelectedWepSlot;
	bool m_bSpeedTimerFlag;
	uint8 m_nEvadeAmount;
	uint32 m_nSpeedTimer; // m_nStandStillTimer?
	uint32 m_nHitAnimDelayTimer; // m_nShotDelay?
	float m_fAttackButtonCounter;
	bool m_bHaveTargetSelected;	// may have better name
	CEntity *m_pEvadingFrom;	// is this CPhysical?
	int32 m_nTargettableObjects[4];
	uint32 m_nAdrenalineTime;
	uint8 m_nDrunkenness;             // Needed to work out whether we lost target this frame
	uint8 m_nFadeDrunkenness;
	uint8 m_nDrunkCountdown; //countdown in frames when the drunk effect ends
	bool m_bAdrenalineActive;
	bool m_bHasLockOnTarget;
	bool m_bCanBeDamaged;
	bool m_bNoPosForMeleeAttack;
	bool unk1;
	CVector m_vecSafePos[6]; // safe places from the player, for example behind a tree
	CPed *m_pPedAtSafePos[6];
	CPed *m_pMeleeList[6]; // reachable peds at each direction(6)
	int16 m_nAttackDirToCheck;
	float m_fWalkAngle; //angle between heading and walking direction
	float m_fFPSMoveHeading;
	float m_fMoveHeading; // world heading of the controller's movement direction
	// The movement solver owns the player's horizontal velocity
	// (PlayerMoveSolver.h); the controller only builds the intent, steps it and
	// writes the snapshot back.
	CPlayerMoveSolver m_MoveSolver;
	RpAtomic* m_pMinigunTopAtomic; //atomic for the spinning part of the minigun model
	float m_fGunSpinSpeed; // for minigun
	float m_fGunSpinAngle;
	unsigned int m_nPadDownPressedInMilliseconds;
	unsigned int m_nLastBusFareCollected;
	// The player holds the aim: one state for every aiming weapon, in both
	// views and in every movement state. The on-foot control paths start,
	// update and stop it through AimWeapon (PlayerPed.cpp).
	bool m_bWeaponAimActive;

	static bool bDontAllowWeaponChange;
#ifndef MASTER
	static bool bDebugPlayerInfo;
#endif

	CPlayerPed();
	~CPlayerPed();
	void SetMoveAnim() { };

	void ReApplyMoveAnims(void);
	void ClearWeaponTarget(void);
	void SetWantedLevel(int32 level);
	void SetWantedLevelNoDrop(int32 level);
	void KeepAreaAroundPlayerClear(void);
	void AnnoyPlayerPed(bool);
	void MakeChangesForNewWeapon(int32);
	void MakeChangesForNewWeapon(eWeaponType);
	void SetInitialState(void);
	void ProcessControl(void);
	void ClearAdrenaline(void);
	void UseSprintEnergy(void);
	// The adapter of the movement solver (PlayerMoveSolver.h): the pad block,
	// the gates, the intent and the write-back of the snapshot.
	void UpdatePlayerMovement(CPad *padUsed);
	bool IsAimingWeapon(void) const { return m_bWeaponAimActive; }
	// The one aim of the on-foot player: the aim button raises the weapon and
	// every control path runs this each frame while the player can aim
	void AimWeapon(CPad *padUsed);
	// The start of the aim: the state, the camera mode of the scope or the
	// viewfinder, and the end of any lock-on. The vehicle aim starts here too.
	void StartWeaponAim(void);
	void StopWeaponAim(void);
	void UpdateAimCameraMode(void);
	void ClearAimCameraMode(void);
	void AimAtPosition(const CVector &position);
	bool IsMoveControllerInCharge(void);
	// The views in which the body keeps facing the camera: the first person
	// view and the mouse third person camera. The ped walks sideways and
	// backwards there, so the strafe and back animation groups are the correct
	// ones and the legs follow the movement direction. The classic camera
	// turns the body toward the movement direction instead.
	bool BodyFacesCamera(void);
	float GetMoveAnimPlayback(AnimationId animId) const;
	float GetMoveAnimReferenceSpeed(void);
	class CPlayerInfo *GetPlayerInfoForThisPlayerPed();
	void SetRealMoveAnim(void);
	void RestoreSprintEnergy(float);
	void DoStuffToGoOnFire(void);
	bool DoesTargetHaveToBeBroken(CVector, CWeapon*);
	void RunningLand(CPad*);
	bool IsThisPedAnAimingPriority(CPed*);
	void FireWeaponWhenHeld(CPad*);
	void ProcessAirborneWeapon(CPad*);
	void PlayerControlFighter(CPad*);
	void ProcessWeaponSwitch(CPad*);
	void ProcessActiveReload(CPad*);
	void ProcessActiveReloadInput(CPad*);
	void MakeObjectTargettable(int32);
	void PlayerControl1stPersonRunAround(CPad *padUsed);
	void EvaluateNeighbouringTarget(CEntity*, CEntity**, float*, float, float, bool, bool);
	void EvaluateTarget(CEntity*, CEntity**, float*, float, float, bool);
	bool FindNextWeaponLockOnTarget(CEntity*, bool);
	bool FindWeaponLockOnTarget(void);
	void ProcessAnimGroups(void);
	void ProcessPlayerWeapon(CPad*);
	// The player's held upper body aim pose (VICE-141): the weapon aim pose
	// rides on the upper body only while the legs keep the movement animation.
	void HoldUpperBodyAimPose(void);
	void ReleaseHeldAimPose(void);
	void PlayerControlZelda(CPad*);
	// The jump and the evade dive of the on-foot player, shared by the
	// camera-facing and the classic walk controls.
	void PlayerJumpOrDive(CPad *padUsed);
	// Whether the on-foot player's sprint request is accepted: the heavy
	// weapons refuse it, unless the aim is up.
	bool PlayerWantsSprint(CPad *padUsed);
	bool DoesPlayerWantNewWeapon(eWeaponType, bool);
	void PlayIdleAnimations(CPad*);
	void RemovePedFromMeleeList(CPed*);
	void GetMeleeAttackCoords(CVector&, int8, float);
	int32 FindMeleeAttackPoint(CPed*, CVector&, uint32&);
	bool CanIKReachThisTarget(CVector, CWeapon*, bool);
	void RotatePlayerToTrackTarget(void);
	bool MovementDisabledBecauseOfTargeting(void);
	void FindNewAttackPoints(void);
	void SetNearbyPedsToInteractWithPlayer(void);
	void UpdateMeleeAttackers(void);

	static void SetupPlayerPed(int32);
	static void DeactivatePlayerPed(int32);
	static void ReactivatePlayerPed(int32);

#ifdef COMPATIBLE_SAVES
	virtual void Save(uint8*& buf);
	virtual void Load(uint8*& buf);
#endif

	static const uint32 nSaveStructSize;
};

//VALIDATE_SIZE(CPlayerPed, 0x5F0);

#endif // __GTA_PLAYERPED_H__
