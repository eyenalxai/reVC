# ADR 0010: Active reload is a manual timed reload for the player's magazine weapons

Date: 2026-10-16
Status: Accepted

## Context

The owner wants a Gears of War style active reload for reVC (spec VICE-72).
The player starts a reload with a key, a timing window runs during the reload,
a press inside the window finishes the reload at once, and a press before the
window jams the reload and makes it longer.

The pieces already exist: the reload state and timer (`WEAPONSTATE_RELOADING`,
`m_nTimer = CTimer::GetTimeInMilliseconds() + GetInfo()->m_nReload`,
`src/weapons/Weapon.cpp`), the keybind system takes new actions
(`PED_RELOAD`, default key R), and the HUD draws plain rectangles, so no new
art is needed. The state lives in a new player-only module,
`src/weapons/ActiveReload.*`, and it is not part of the save format.

The owner decisions of 2026-10-16: a missed press jams the reload (about 1.5x
longer); damage is out of scope (the owner rebalances health and damage later);
the scope is magazine weapons only; the player's reload is manual only; a
timing bar sits under the crosshair and stays visible in the zoomed modes.

## Decision

1. The player's reload is manual only, and only for magazine weapons. A
   magazine weapon has a clip of two or more rounds and fires instant hits
   (`m_nAmountofAmmunition >= 2` and
   `m_eWeaponFire == WEAPON_FIRE_INSTANT_HIT`). The clip never refills by
   itself: firing the last round leaves the weapon READY with an empty clip,
   and a weapon switch or an ammo pickup moves the ammo to the reserve, not to
   the clip. The reload key starts a reload when the clip is not full, the
   reserve has ammo, and the state is READY, FIRING or OUT_OF_AMMO; a finished
   reload clears OUT_OF_AMMO.
2. The reload window runs from 55% to 80% of the base reload time D
   (`CWeaponInfo::m_nReload`, divided by 4 when `m_bFastReload` is set).
3. A press inside the window finishes the reload in 100 ms
   (`m_nTimer = now + 100 ms`). The reload animation extension in
   `CWeapon::Update` must not push the timer back for this reload.
4. A press before the window adds half the base time (D / 2) to the reload,
   once: the reload becomes about 1.5 times the base time. This is the reload
   jam.
5. The first press judges the reload. Later presses are ignored.
6. No damage value changes. A perfect reload gives no damage bonus; the owner
   rebalances health and damage later.
7. AI peds, drive-by shooting (`CWeapon::FireFromCar`,
   `FireInstantHitFromCar`) and the clip-1 weapons (shotgun, stubby shotgun,
   sniper, rocket launcher) keep stock behavior.
8. The feature has a `[Weapons] ActiveReload` toggle in `reVC.ini`, default
   on. Off restores stock behavior: the automatic reload returns, and there is
   no window and no timing bar. The key still does a plain reload.

## Alternatives considered

- **Keep the stock automatic reload.** Rejected: the owner asked for the
  timed mechanic.
- **A damage bonus for a perfect reload.** Deferred: the owner rebalances
  health and damage later, so this work must not change any damage value.
- **Apply the mechanic to every weapon.** Rejected: the clip-1 weapons keep
  stock behavior; their reload is part of the shot cycle, not a magazine.
- **Share the change with every shooter in `CWeapon`.** Rejected: the state
  lives in a player-only module, so AI peds, drive-by shooting and missions
  keep the original behavior (the same shape as ADR 0001).
- **Jam on a press after the window too.** Rejected: a late press does
  nothing. The window is the reward zone, and the jam punishes the press that
  comes too soon.
- **Re-judge the reload on every press.** Rejected: the first press judges,
  so the player cannot gamble and retry.

## Consequences

- The player must press the reload key; the clip never refills by itself. A
  magazine that runs dry stays dry until the reload finishes.
- A press inside the window ends the reload in 100 ms, so a skilled player
  wins time. An early press costs about half the base time on top of the
  reload; a late press leaves the reload at its base length.
- AI peds, drive-by shooting and the clip-1 weapons keep the original
  behavior, so missions, traffic and races are untouched.
- The toggle is on by default; with it off the game behaves as stock. The
  module is player-only and inert when the toggle is off.
- The state is not saved, so a load or a new game starts with no active
  reload.
- No damage value changes, so the owner's pending health and damage balance
  pass is not affected.
- The HUD shows the window as a slim bar under the crosshair, drawn before
  the viewfinder gate, so the sniper scope and the M16 mode still show it.
- The feel needs the owner's play-test (there is no test suite); the state,
  the window and the jam live in one module, so a change of feel is cheap.
