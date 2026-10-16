#include "common.h"
#include "main.h"

#include "Timer.h"
#include "Weapon.h"
#include "Ped.h"
#include "PlayerPed.h"
#include "PlayerInfo.h"
#include "World.h"

#include "ActiveReload.h"

// Only the player can have an active reload, so one set of state is enough.
static bool ms_bActive;			// a tracked reload runs
static eWeaponType ms_eWeaponType;	// the tracked weapon
static uint32 ms_nStart;		// when the reload started
static uint32 ms_nDuration;		// D, the base duration
static uint32 ms_nWindowStart;		// the window, absolute times
static uint32 ms_nWindowEnd;
static bool ms_bJudged;			// the first judging press came in
static bool ms_bAnimationRequested;	// the reload animation is started
static eActiveReloadResult ms_eResult;	// the last judgment, for the HUD
static uint32 ms_nResultTime;

// The tracked weapon, or nil when the player is not the tracked shooter.
static CWeapon*
TrackedWeapon(void)
{
	CPlayerPed *player = FindPlayerPed();
	if (player == nil || player->DyingOrDead())
		return nil;

	CWeapon *weapon = player->GetWeapon();
	if (weapon->m_eWeaponType != ms_eWeaponType)
		return nil;

	return weapon;
}

// Drops a reload that cannot be running any more: the player switched weapon,
// died, respawned or loaded a game. The result stays after the reload itself
// ends, so the HUD can still flash it; it goes when the weapon is gone. Every
// entry point that acts on the state calls this first, so the healing needs no
// separate per-frame step.
void
ActiveReload::ForgetStaleReload(void)
{
	if (!ms_bActive)
		return;

	if (!gActiveReloadEnabled) {
		ActiveReload::Reset();
		return;
	}

	CWeapon *weapon = TrackedWeapon();
	if (weapon == nil) {
		ActiveReload::Reset();
		return;
	}

	if (weapon->m_eWeaponState != WEAPONSTATE_RELOADING) {
		ms_bActive = false;
		ms_bAnimationRequested = false;
	}
}

void
ActiveReload::Reset(void)
{
	ms_bActive = false;
	ms_bAnimationRequested = false;
	ResetResult();
}

void
ActiveReload::ResetResult(void)
{
	ms_eResult = ACTIVE_RELOAD_NONE;
	ms_nResultTime = 0;
}

void
ActiveReload::Start(eWeaponType weaponType, uint32 duration)
{
	if (!gActiveReloadEnabled)
		return;

	ms_bActive = true;
	ms_eWeaponType = weaponType;
	ms_nStart = CTimer::GetTimeInMilliseconds();
	ms_nDuration = Max(duration, 1u);
	ms_nWindowStart = ms_nStart + (uint32)(ms_nDuration*ACTIVE_RELOAD_WINDOW_START);
	ms_nWindowEnd = ms_nStart + (uint32)(ms_nDuration*ACTIVE_RELOAD_WINDOW_END);
	ms_bJudged = false;
	ms_bAnimationRequested = false;
	ResetResult();
}

void
ActiveReload::Judge(CWeapon *weapon)
{
	if (!ms_bActive || ms_bJudged)
		return;

	if (weapon == nil || weapon->m_eWeaponType != ms_eWeaponType || weapon->m_eWeaponState != WEAPONSTATE_RELOADING)
		return;

	uint32 now = CTimer::GetTimeInMilliseconds();

	if (now < ms_nWindowStart) {
		// Too soon: the reload is jammed and takes half the base time more.
		weapon->m_nTimer += (uint32)(ms_nDuration*ACTIVE_RELOAD_JAM_FRACTION);
		ms_bJudged = true;
		ms_eResult = ACTIVE_RELOAD_JAM;
		ms_nResultTime = now;
	} else if (now <= ms_nWindowEnd) {
		// In the window: the reload finishes almost at once.
		weapon->m_nTimer = now + ACTIVE_RELOAD_PERFECT_TAIL_MS;
		ms_bJudged = true;
		ms_eResult = ACTIVE_RELOAD_PERFECT;
		ms_nResultTime = now;
	}
	// Later presses and presses after the window do nothing.
}

bool
ActiveReload::InProgress(void)
{
	ForgetStaleReload();
	return ms_bActive;
}

bool
ActiveReload::Judged(CWeapon *weapon)
{
	if (!ms_bActive || !ms_bJudged)
		return false;

	return TrackedWeapon() == weapon;
}

bool
ActiveReload::ClaimReloadAnimation(void)
{
	ForgetStaleReload();

	if (!ms_bActive || ms_bAnimationRequested)
		return false;

	ms_bAnimationRequested = true;
	return true;
}

ActiveReloadBar
ActiveReload::GetBarState(void)
{
	ForgetStaleReload();

	ActiveReloadBar bar;
	bar.visible = ms_bActive;
	bar.progress = 0.0f;
	if (ms_bActive)
		bar.progress = Min((CTimer::GetTimeInMilliseconds() - ms_nStart)/(float)ms_nDuration, 1.0f);
	bar.windowStart = ACTIVE_RELOAD_WINDOW_START;
	bar.windowEnd = ACTIVE_RELOAD_WINDOW_END;
	bar.result = ms_eResult;
	bar.resultAge = ms_nResultTime != 0 ? CTimer::GetTimeInMilliseconds() - ms_nResultTime : 0;

	return bar;
}
