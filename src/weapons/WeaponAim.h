#ifndef __GTA_WEAPONAIM_H__
#define __GTA_WEAPONAIM_H__

#include "WeaponAimRules.h"
#include "WeaponInfo.h"

// The one classification of the aiming weapons over the loaded weapon data:
// every gun aims and fires through the one system, and the photo camera aims
// through it with its viewfinder. The scope zoom (the sniper and the laser
// scope) and the viewfinder are views of that path, not paths of their own.
// The pure rules live in WeaponAimRules.h.

inline bool
IsGunWeapon(eWeaponType type)
{
	return IsGunFire(CWeaponInfo::GetWeaponInfo(type)->m_eWeaponFire, type);
}

// The weapons the aim button raises: every gun and the photo camera.
inline bool
IsAimableWeapon(eWeaponType type)
{
	return IsAimableFire(CWeaponInfo::GetWeaponInfo(type)->m_eWeaponFire, type);
}

#endif // __GTA_WEAPONAIM_H__
