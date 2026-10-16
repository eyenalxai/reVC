#ifndef __GTA_WEAPON_H__
#define __GTA_WEAPON_H__

#include "WeaponType.h"

#define CAR_DRIVEBYAUTOAIMING_MAXDIST (2.5f)
#define DOOMAUTOAIMING_MAXDIST    (9000.0f)

class CEntity;
class CPhysical;
class CVehicle;
class CPed;
struct CColPoint;
class CWeaponInfo;

class CWeapon
{
public:
	eWeaponType m_eWeaponType;
	eWeaponState m_eWeaponState;
	int32 m_nAmmoInClip;
	int32 m_nAmmoTotal;
	uint32 m_nTimer;
	bool m_bAddRotOffset;
	
	static bool bPhotographHasBeenTaken;
	
	CWeapon() {
		m_bAddRotOffset = false;
	}
	CWeapon(eWeaponType type, int32 ammo);
	
	CWeaponInfo *GetInfo();

	static void InitialiseWeapons(void);
	static void ShutdownWeapons  (void);
	static void UpdateWeapons    (void);
	
	void Initialise(eWeaponType type, int32 ammo);
	void Shutdown();
	
	bool Fire          (CEntity *shooter, CVector *fireSource);
	// Fires the drive-by; aiming fires the free aim along the camera instead,
	// and left/right only apply to the drive-by
	bool FireFromCar   (CVehicle *shooter, bool left, bool right, bool aiming);
	// The drive-by fire of the vehicle callers: fires and then sets the
	// 70 ms drive-by cadence only when a shot left the barrel. A failed
	// attempt, an empty clip or a started reload leaves the timer alone,
	// so the call can never shorten a running reload (VICE-90). The aiming
	// flag fires the free aim along the camera instead (VICE-110).
	void FireFromCarWithCadence(CVehicle *shooter, bool left, bool right, bool aiming);
	bool FireMelee     (CEntity *shooter, CVector &fireSource);
	bool FireInstantHit(CEntity *shooter, CVector *fireSource);
	
	static void AddGunFlashBigGuns(CVector start, CVector end);
	void AddGunshell   (CEntity *shooter, CVector const &source, CVector2D const &direction, float size);
	void DoBulletImpact(CEntity *shooter, CEntity *victim, CVector *source, CVector *target, CColPoint *point, CVector2D ahead);
	
	bool FireShotgun   (CEntity *shooter, CVector *fireSource);
	bool FireProjectile(CEntity *shooter, CVector *fireSource, float power);
	
	static void GenerateFlameThrowerParticles(CVector pos, CVector dir);
	
	bool FireAreaEffect       (CEntity *shooter, CVector *fireSource);
	bool LaserScopeDot        (CVector *pOutPos, float *pOutSize);
	bool FireSniper           (CEntity *shooter);
	bool TakePhotograph       (CEntity *shooter);
	bool FireM16_1stPerson    (CEntity *shooter);
	bool FireInstantHitFromCar(CVehicle *shooter, bool left, bool right);
#ifdef FIRST_PERSON
	// The free aim from a car: the target comes from the camera and there is no
	// auto aim
	bool FireInstantHitFromCarFreeAim(CVehicle *shooter);
#endif
	// The shot from a car after the source and the target are known
	bool FireFromCarAt        (CVehicle *shooter, CVector &source, CVector &target);
	
	static void DoDoomAiming       (CEntity *shooter, CVector *source,   CVector *target);
	static void DoTankDoomAiming   (CEntity *shooter, CEntity *driver,   CVector *source, CVector *target);
	static void DoDriveByAutoAiming(CEntity *driver,  CVehicle *vehicle, CVector *source, CVector *target);
	
	void Reload(void);
	// Starts a reload and sets the timer; the fast reload flag shortens D.
	void StartReload(CEntity *shooter);
	// True while the manual reload rule applies to this shooter: a magazine
	// weapon, the player and the ActiveReload feature on. AI peds and the
	// toggle off keep the stock automatic reload.
	bool UsesManualReload(CEntity *shooter);
	// True while the weapon uses the stock clip refill for this ped: the
	// weapon switch, a weapon give and a pickup fill the clip by themselves,
	// and an AI ped always refills. The player's manual reload (ADR 0010)
	// turns the player's magazine refill off. The rule reads IsPlayer(), not
	// FindPlayerPed() like UsesManualReload, so every player ped keeps the
	// stock path. The toggle is read before the ped; both operands are pure.
	static bool UsesStockClipRefill(CPed *ped);
	// True while the player's reload key can start a reload: a magazine
	// weapon, a clip that is not full, ammo in the reserve and a state that
	// takes a reload.
	bool CanReload(void);
	void Update(int32 audioEntity, CPed *pedToAdjustSound);
	bool IsTypeMelee  (void);
	bool IsType2Handed(void);
	// A magazine weapon has a clip of two or more rounds and fires instant
	// hits. The active reload applies to these weapons only.
	bool IsMagazineWeapon(void);
	
	static void MakePedsJumpAtShot(CPhysical *shooter, CVector *source, CVector *target);
	
	bool HitsGround(CEntity *holder, CVector *fireSource, CEntity *aimingTo);
	static void BlowUpExplosiveThings(CEntity *thing);
	bool HasWeaponAmmoToBeUsed(void);
	
	static bool IsShotgun(int weapon) { return weapon == WEAPONTYPE_SHOTGUN || weapon == WEAPONTYPE_SPAS12_SHOTGUN || weapon == WEAPONTYPE_STUBBY_SHOTGUN; }

	static bool ProcessLineOfSight(CVector const &point1, CVector const &point2, CColPoint &point, CEntity *&entity, eWeaponType type, CEntity *shooter, bool checkBuildings, bool checkVehicles, bool checkPeds, bool checkObjects, bool checkDummies, bool ignoreSeeThrough, bool ignoreSomeObjects);

	static void CheckForShootingVehicleOccupant(CEntity **victim, CColPoint *point, eWeaponType weapon, CVector const& source, CVector const& target);

#ifdef COMPATIBLE_SAVES
	void Save(uint8*& buf);
	void Load(uint8*& buf);
#endif
};
VALIDATE_SIZE(CWeapon, 0x18);

void FireOneInstantHitRound(CVector *source, CVector *target, int32 damage);

#endif // __GTA_WEAPON_H__
