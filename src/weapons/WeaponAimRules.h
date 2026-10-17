#ifndef __GTA_WEAPONAIMRULES_H__
#define __GTA_WEAPONAIMRULES_H__

#include "WeaponType.h"

// The one classification of the aiming weapons, as pure rules over the weapon
// data. WeaponAim.h wraps them with the loaded CWeaponInfo, and the aim matrix
// harness checks them against weapon.dat without the engine.
//
// A gun shoots a bullet, a shell or a flame. The throwables, the detonator,
// the melee weapons and the photo camera are not guns. The scope zoom and the
// viewfinder are views of the one aiming path, not paths of their own.

inline bool
IsGunFire(eWeaponFire fire, eWeaponType type)
{
	switch (fire) {
	case WEAPON_FIRE_INSTANT_HIT:
	case WEAPON_FIRE_AREA_EFFECT:
		return true;
	case WEAPON_FIRE_PROJECTILE:
		return type == WEAPONTYPE_ROCKETLAUNCHER;
	default:
		return false;
	}
}

// The sniper and the laser scope keep their zoom. No other sight zooms.
inline bool
UsesScopeZoom(eWeaponType type)
{
	return type == WEAPONTYPE_SNIPERRIFLE || type == WEAPONTYPE_LASERSCOPE;
}

// The photo camera keeps its viewfinder.
inline bool
IsPhotoCamera(eWeaponType type)
{
	return type == WEAPONTYPE_CAMERA;
}

// The weapons the aim button raises: every gun and the photo camera.
inline bool
IsAimableFire(eWeaponFire fire, eWeaponType type)
{
	return IsGunFire(fire, type) || IsPhotoCamera(type);
}

#endif // __GTA_WEAPONAIMRULES_H__
