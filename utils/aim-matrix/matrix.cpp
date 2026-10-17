/*
 * The aim matrix: one row per weapon in weapon.dat, with the one-system
 * classification of WeaponAimRules.h.
 *
 * The aim rework replaced the hardcoded weapon list (the stock sights of the
 * M4, Ruger, M60, sniper, laser scope, rocket launcher and camera, and the
 * free aim of the other weapons) with one rule over the weapon data: a gun
 * fires an instant hit, a flame or a rocket, every gun aims through the one
 * aim system, the sniper and the laser scope keep their zoom, and the photo
 * camera keeps its viewfinder. This program reads weapon.dat on its own and
 * checks that every weapon gets a deliberate classification, so a weapon that
 * falls through the new rule shows up here instead of in the game.
 *
 * Build and run:
 *
 *	c++ -std=c++17 -I src/weapons -O2 -o /tmp/opencode/aim-matrix utils/aim-matrix/matrix.cpp
 *	/tmp/opencode/aim-matrix "$HOME/Games/Steam/steamapps/common/Grand Theft Auto Vice City/data/weapon.dat"
 *
 * The exit code is 0 when every row matches the expected class, 1 otherwise.
 */

#include "WeaponAimRules.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// The weapon names of CWeaponInfo (WeaponInfo.cpp). The order is the
// eWeaponType order and the weapon.dat order, so the check is a guard against
// a file that moves a row.
static const char *aExpectedNames[] = {
	"Unarmed", "BrassKnuckle", "ScrewDriver", "GolfClub", "NightStick",
	"Knife", "BaseballBat", "Hammer", "Cleaver", "Machete", "Katana",
	"Chainsaw",
	"Grenade", "DetonateGrenade", "TearGas", "Molotov", "Rocket",
	"Colt45", "Python",
	"Shotgun", "Spas12Shotgun", "StubbyShotgun",
	"Tec9", "Uzi", "SilencedIngram", "Mp5",
	"m4", "Ruger",
	"SniperRifle", "LaserScope",
	"RocketLauncher", "FlameThrower",
	"M60", "Minigun",
	"Detonator", "HeliCannon",
	"Camera",
};

enum Expected {
	EXPECT_MELEE,		// the stock melee path
	EXPECT_THROW,		// the stock thrown path
	EXPECT_DETONATOR,	// the stock detonator
	EXPECT_ROCKET_PROJECTILE, // the projectile of the launcher, never held
	EXPECT_GUN,		// the one aim system, no zoom
	EXPECT_SCOPE,		// the one aim system with the scope zoom
	EXPECT_CAMERA,		// the one aim system with the viewfinder
};

static const char *
ExpectedName(enum Expected expected)
{
	switch (expected) {
	case EXPECT_MELEE: return "melee";
	case EXPECT_THROW: return "throw";
	case EXPECT_DETONATOR: return "detonator";
	case EXPECT_ROCKET_PROJECTILE: return "rocket projectile";
	case EXPECT_GUN: return "gun";
	case EXPECT_SCOPE: return "scope gun";
	case EXPECT_CAMERA: return "camera";
	}
	return "?";
}

static enum Expected
ExpectedOf(enum eWeaponType type)
{
	if (type <= WEAPONTYPE_CHAINSAW)
		return EXPECT_MELEE;
	switch (type) {
	case WEAPONTYPE_GRENADE:
	case WEAPONTYPE_DETONATOR_GRENADE:
	case WEAPONTYPE_TEARGAS:
	case WEAPONTYPE_MOLOTOV:
		return EXPECT_THROW;
	case WEAPONTYPE_ROCKET:
		return EXPECT_ROCKET_PROJECTILE;
	case WEAPONTYPE_DETONATOR:
		return EXPECT_DETONATOR;
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_LASERSCOPE:
		return EXPECT_SCOPE;
	case WEAPONTYPE_CAMERA:
		return EXPECT_CAMERA;
	default:
		return EXPECT_GUN;
	}
}

static bool
FindFire(const char *name, enum eWeaponFire *fire)
{
	if (strcmp(name, "MELEE") == 0) *fire = WEAPON_FIRE_MELEE;
	else if (strcmp(name, "INSTANT_HIT") == 0) *fire = WEAPON_FIRE_INSTANT_HIT;
	else if (strcmp(name, "PROJECTILE") == 0) *fire = WEAPON_FIRE_PROJECTILE;
	else if (strcmp(name, "AREA_EFFECT") == 0) *fire = WEAPON_FIRE_AREA_EFFECT;
	else if (strcmp(name, "CAMERA") == 0) *fire = WEAPON_FIRE_CAMERA;
	else return false;
	return true;
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s path/to/weapon.dat\n", argv[0]);
		return 2;
	}

	FILE *file = fopen(argv[1], "r");
	if (file == NULL) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 2;
	}

	char line[512];
	int weapons = 0;
	int failures = 0;
	int held = 0;

	printf("%-3s %-16s %-12s %-8s %-4s %-6s %-6s %-6s %s\n",
		"#", "weapon", "fire", "flags", "gun", "aim", "scope", "camera", "class");
	while (fgets(line, sizeof(line), file)) {
		char name[64], fireName[64];
		unsigned flags;
		int slot;

		// The weapon rows start at the first non-space, non-# character.
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '#' || *p == '\n' || *p == '\r')
			continue;
		if (strncmp(p, "ENDWEAPONDATA", 13) == 0)
			break;

		// A row: name fire range rate reload ammo damage speed radius
		// lifespan spread x y z anim R S T U V W X model model2 flags slot.
		// The name, the fire type, the flags and the slot are all the rules
		// need, and the field count guards the row.
		char *token[32];
		int tokens = 0;
		for (char *t = strtok(p, " \t\r\n"); t != NULL && tokens < 32; t = strtok(NULL, " \t\r\n"))
			token[tokens++] = t;
		if (tokens < 26) {
			fprintf(stderr, "short row: %.40s\n", p);
			failures++;
			continue;
		}
		strncpy(name, token[0], sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
		strncpy(fireName, token[1], sizeof(fireName) - 1);
		fireName[sizeof(fireName) - 1] = '\0';
		flags = (unsigned)strtoul(token[24], NULL, 16);
		slot = atoi(token[25]);

		enum eWeaponType type = (enum eWeaponType)weapons;
		enum eWeaponFire fire;
		if (!FindFire(fireName, &fire)) {
			fprintf(stderr, "unknown fire type %s on %s\n", fireName, name);
			failures++;
		}

		bool gun = IsGunFire(fire, type);
		bool aimable = IsAimableFire(fire, type);
		bool scope = UsesScopeZoom(type);
		bool camera = IsPhotoCamera(type);
		enum Expected expected = ExpectedOf(type);
		bool expectedGun = expected == EXPECT_GUN || expected == EXPECT_SCOPE;
		bool expectedAim = expectedGun || expected == EXPECT_CAMERA;
		bool expectedScope = expected == EXPECT_SCOPE;
		bool expectedCamera = expected == EXPECT_CAMERA;
		bool ok = gun == expectedGun && aimable == expectedAim &&
			scope == expectedScope && camera == expectedCamera;

		if (weapons < (int)(sizeof(aExpectedNames) / sizeof(aExpectedNames[0]))) {
			if (strcmp(name, aExpectedNames[weapons]) != 0) {
				fprintf(stderr, "row %d is %s, expected %s\n",
					weapons, name, aExpectedNames[weapons]);
				ok = false;
			}
		} else {
			ok = false;
		}

		if (expected != EXPECT_ROCKET_PROJECTILE)
			held++;

		printf("%-3d %-16s %-12s %06x %-4s %-6s %-6s %-6s %s%s\n",
			weapons, name, fireName, flags,
			gun ? "yes" : "no", aimable ? "yes" : "no",
			scope ? "yes" : "no", camera ? "yes" : "no",
			ExpectedName(expected), ok ? "" : "  <-- MISMATCH");

		if (!ok)
			failures++;
		weapons++;
	}
	fclose(file);

	if (weapons != (int)(sizeof(aExpectedNames) / sizeof(aExpectedNames[0]))) {
		fprintf(stderr, "read %d weapons, expected %d\n", weapons, (int)(sizeof(aExpectedNames) / sizeof(aExpectedNames[0])));
		failures++;
	}

	printf("\n%d weapons, %d held, %d mismatches\n", weapons, held, failures);
	return failures == 0 ? 0 : 1;
}
