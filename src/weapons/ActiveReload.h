#ifndef __GTA_ACTIVERELOAD_H__
#define __GTA_ACTIVERELOAD_H__

#include "WeaponType.h"

class CWeapon;

// The active reload of the player's magazine weapons (see
// docs/adr/0010-active-reload.md). The player starts a reload with the reload
// key; a later press judges the timing window of that reload:
//   * inside the window the reload ends at once, a perfect reload;
//   * before the window the reload is jammed and takes about 1.5 times as
//     long;
//   * after the window the press does nothing.
// The first judging press decides; later presses are ignored. Only the
// player's reloads are tracked, and nothing happens while
// gActiveReloadEnabled is false. The state is not part of the save format.

// All active reload tuning lives in this block. The window and the jam are
// fractions of the base duration D: the weapon's m_nReload, divided by 4 when
// the fast reload flag is set.
#define ACTIVE_RELOAD_WINDOW_START	0.55f	// window opens here, fraction of D
#define ACTIVE_RELOAD_WINDOW_END	0.80f	// window closes here, fraction of D
#define ACTIVE_RELOAD_PERFECT_TAIL_MS	100	// left of a perfect reload, ms
#define ACTIVE_RELOAD_JAM_FRACTION	0.5f	// the jam adds this much of D

enum eActiveReloadResult
{
	ACTIVE_RELOAD_NONE,
	ACTIVE_RELOAD_PERFECT,
	ACTIVE_RELOAD_JAM,
};

// The bar the HUD draws under the crosshair.
struct ActiveReloadBar
{
	bool visible;		// a tracked reload is running
	float progress;		// how far the reload is, 0..1 of D
	float windowStart;	// the window zone, 0..1 of D
	float windowEnd;
	int result;		// the last judgment, eActiveReloadResult
	uint32 resultAge;	// ms since that judgment
};

class ActiveReload
{
public:
	// A reload of the player's weapon just started; duration is D, its base
	// duration. Called through CWeapon::StartReload.
	static void Start(eWeaponType weaponType, uint32 duration);

	// A judging press: the reload key went down while a reload runs. The
	// press that started the reload does not come here.
	static void Judge(CWeapon *weapon);

	// Per-frame upkeep: drops a reload that cannot be running any more, for
	// example after a weapon switch, a death or a load.
	static void Update(void);

	// A tracked reload is running and its window can still be judged.
	static bool InProgress(void);

	// True while this weapon's reload must keep the short timer a perfect
	// press gave it: the reload animation must not stretch it back.
	static bool JudgedPerfect(CWeapon *weapon);

	static ActiveReloadBar GetBarState(void);

private:
	// Forgets the reload and its result.
	static void Reset(void);

	// Forgets the result only; the reload itself is already gone.
	static void ResetResult(void);

	// Drops a reload that cannot be running any more: the player switched
	// weapon, died, respawned or loaded a game.
	static void ForgetStaleReload(void);
};

#endif // __GTA_ACTIVERELOAD_H__
