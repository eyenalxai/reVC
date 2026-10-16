# ADR 0011: Active reload follow-up: magazines, vehicle reload, animation abort, voice

Date: 2026-10-16
Status: Accepted

## Context

ADR 0010 added the active reload: a manual timed reload for the player's
magazine weapons, with a timing window, a jam and a `[Weapons] ActiveReload`
toggle. That work kept the clip-1 weapons (shotgun, stubby shotgun, sniper
rifle, rocket launcher) and drive-by shooting on stock behavior.

The owner asked for a second round (spec VICE-85) and gave the decisions on
2026-10-16:

- The Shotgun gets 7 shells, the Stubby shotgun 6 and the Sniper rifle 5. The
  rocket launcher stays one-shot.
- Only the player uses the magazines. AI peds keep the per-shot pump/bolt
  cycle, so mission balance does not change.
- The reload reuses the shipped animations. No new assets.
- The reload key also works in a vehicle, and the free clip refill while not
  aiming stops.
- The protagonist speaks on a perfect reload and on a jam.
- The damage bonus stays out of scope; the owner rebalances health and damage
  later.

## Decision

1. Magazines for the player only. The Shotgun holds 7 shells, the Stubby
   shotgun 6 and the Sniper rifle 5. The clip size lives in `weapon.dat`
   column F, which is not in the repository, so a small override table applies
   the sizes after `CWeaponInfo::LoadWeaponData` parses the file. The same
   table sets `WEAPONFLAG_RELOAD` for the Shotgun and the Stubby. The
   `IsMagazineWeapon` test (a clip of two or more rounds and instant hit) then
   covers the three weapons on its own.
2. The Sniper rifle keeps a bolt cycle. The game recomputes its firing rate
   from the animation loop to 90 ms, so the override table sets 700 ms
   instead. A magazine must not turn the rifle semi-automatic.
3. The rocket launcher stays one-shot, with its stock 1.2 s cycle and no
   reload window.
4. AI peds keep the per-shot cycle. After a shot by a non-player shooter with
   a magazine weapon, `CWeapon::Fire` enters the reload as a one-round weapon
   does today, whatever the clip count: the AI fires one shell, pays the pump
   or bolt time, and refills, exactly as today. The player's manual-only rule
   is unchanged (`UsesManualReload` tests the player ped).
5. The shipped reload animations are wired. The shotgun animation group gets
   `WEAPON_pump` and the stubby's group (the `buddy` group) gets
   `buddy_fireRELOAD`, both as the reload slot; the crouch-reload slot gets an
   entry too, or a ducking reload reads past the group array. The Sniper rifle
   gets no reload animation and no reload flag; it reloads by timer only, like
   the SPAS-12.
6. A perfect reload aborts the reload animation. A one-shot perfect signal in
   the ActiveReload module is set on a perfect judgment and cleared on start
   and reset. The player consumes it when the weapon state is
   `WEAPONSTATE_READY`, and the reload association fades out with
   `blendDelta = -1000.0f` and `ASSOC_DELETEFADEDOUT`. The finish callback
   then runs once and restores the crouch pose. The abort waits for the READY
   state, so the attack path cannot blend a fresh reload animation and
   re-block, and the association is never removed without the delete flag.
   Weapons with no reload animation are a no-op.
7. The reload key works in a vehicle. The driving input map sets the reload
   pad field, so the same window, the same timing bar and the same
   manual-only rule apply in a car. When the radio shares the physical key,
   the reload key wins, so a press of R in a car reloads and does not change
   the station. The on-foot and in-vehicle paths share one input hook, so the
   logic is not duplicated. The free clip refill in the drive-by callers
   stops while the feature is on, and the timer write after a fire attempt
   skips a weapon that is `WEAPONSTATE_RELOADING`. The timing bar keeps its
   screen-center position; a car has no crosshair.
8. The protagonist speaks on the judgment. A perfect reload plays
   `SOUND_PED_ON_FIRE` (the cocky shooting line) and a jam plays
   `SOUND_PED_ANNOYED_DRIVER` (the annoyed line), through `CPed::Say`, so the
   game's own cooldowns and guards apply. The two sound constants stay in one
   place with a comment that names the intent. Existing samples only.
9. The damage bonus stays deferred. No damage value changes; the owner
   rebalances health and damage later.

This ADR supersedes the clip-1 exclusion and the drive-by exception in
ADR 0010 (decision 7). The rest of ADR 0010 stands: the player-only state
module, the manual-only rule, the window, the jam, the toggle and the AI
exception.

## Alternatives considered

- **Keep the clip-1 weapons on stock behavior.** Rejected: the owner asked for
  magazines on the shotgun, the stubby and the sniper.
- **Give the magazines to AI peds too.** Rejected: mission balance must not
  change, so the AI keeps the per-shot cycle.
- **Reuse the sniper's 90 ms animation rate under the magazine.** Rejected: a
  5-round clip at 90 ms makes the rifle semi-automatic; the 700 ms bolt cycle
  stays.
- **Keep the stock shotgun and stubby reload animations unused.** Rejected:
  the shipped pump and reload animations cover the work; no new assets are
  needed.
- **Keep the drive-by exception and skip the same-key rule.** Rejected: the
  owner asked for the reload in a vehicle, and without the rule a press of R
  both reloads and changes the radio station.
- **Keep the free clip refill in a vehicle.** Rejected: the manual-only rule
  must hold everywhere; the refill stays only with the toggle off.
- **A damage bonus for a perfect reload.** Still deferred: the owner
  rebalances health and damage later.

## Consequences

- The player's Shotgun, Stubby shotgun and Sniper rifle now hold magazines,
  and the HUD shows reserve-clip counts. The rocket launcher keeps its stock
  cycle.
- Mission enemies with these weapons fight exactly as before: one shell, the
  pump or bolt time, then a refill.
- A perfect reload ends the reload animation at once, so the player fires
  immediately. The finish callback runs once and ducking does not break the
  crouch pose. The zoomed sight modes stay timer-only; the 100 ms perfect tail
  already covers them.
- In a vehicle the reload key starts the same timed reload with the window and
  the bar. The clip does not refill for free, and the radio does not change
  when the keys clash.
- The protagonist says a line on a perfect reload and on a jam; the game's own
  cooldowns limit how often the lines play.
- No damage value changes, so the owner's pending health and damage balance
  pass is not affected.
- With the ActiveReload toggle off the game behaves as stock everywhere: the
  automatic reload returns, and the vehicle path, the new animations and the
  voice lines go inert.
- The feel needs the owner's play-test (there is no test suite). The shared
  input hook keeps the on-foot and in-vehicle behavior in one place, so a
  change of feel stays cheap.
