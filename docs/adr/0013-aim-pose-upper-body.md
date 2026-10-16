# ADR 0013: The player's aim pose is an upper body partial

Date: 2026-10-16
Status: Accepted

## Context

The stock aim pose is the weapon's fire animation held at its loop start:
`CPed::SetPointGunAt` adds `ANIM_WEAPON_FIRE` of the weapon's animation group
(`colt45_fire`, `UZI_fire`, `RIFLE_fire`, ...) and `CPed::PointGunAt` keeps it
frozen. The association is `ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL`.

Two problems follow for the fork's aim-down-the-sights work (VICE-141):

- The first person sights and the third person mouse aim had no aim pose
  association at all; `CPlayerPed::UpdateFirstPersonAim` set the aim flag and
  the aim IK held the right arm over whatever movement animation played. A
  sprint then moved the gun with its arm swing.
- The data of several fire animations covers the whole body: `UZI_fire`,
  `RIFLE_fire`, `shotgun_fire` and `python_fire` have 22 sequences, the root,
  the pelvis and both legs included; the leg keys hold a standing stance close
  to `IDLE_stance`. A partial association overrides every bone it has keys
  for, so the frozen pose also froze the legs while the player moved.

The camera and aiming branch (VICE-135 to VICE-140) runs in parallel and owns
the control and the camera code; this decision stays in the animation layer.

## Decision

1. The aim pose stays the stock association. The player's copy drops every
   sequence below the spine: the root, the pelvis and the legs. The upper body
   is the spine, spine1, the neck, the head, both clavicles, the arms, the
   hands and the fingers. `CAnimBlendAssociation::RemoveLowerBodySequences`
   does this on the association instance.
2. `CPed::SetPointGunAt` applies the rule to the player's standing aim pose.
   `CPlayerPed::HoldUpperBodyAimPose` holds the same pose for the first person
   sights and the third person mouse aim, and `CPed::ClearAimFlag` releases
   it. The crouch pose (`ANIM_WEAPON_CROUCHFIRE`) and the AI aim poses keep
   their legs.
3. The hold waits while the crouch pose, a reload, a running attack or any
   other running partial association owns the upper body. The pose is the same
   association the fire animation uses, so a burst that reuses it keeps the
   same rule.
4. The pose is held whenever the sights are up, not only at a run or a sprint:
   the arms must not shift when the movement speed changes, and the movement
   and the legs are the part that must not change.

## Considered alternatives

- **A new association flag checked in the frame update.** Rejected: the
  sequence map already carries the bone information. Clearing the lower body
  sequences composes with the existing partial weights, the velocity
  extraction and the fire and reload reuse with no change to the blend code.
- **Use the fire animation as it ships.** Rejected: a full-body partial freezes
  the legs; that is the reported bug.
- **Hold the pose only while the run or the sprint plays.** Rejected: the arms
  would shift when the movement speed changes, which is the reported artifact.
  The pose belongs to the sights.
- **Keep the IK-only aim of the fork.** Rejected: the sprint arm swing moves
  the gun, and the pose the stock game holds is available.

## Consequences

- With the sights up, the arms and the torso hold the aim pose at every speed
  and the legs keep the idle, walk, run or sprint cycle. The aim IK then
  points the pose.
- Firing reuses the same association, so a burst keeps the arms on the pose
  and the legs on the cycle. The reload animations release the pose through
  `ClearAimFlag` and layer on the upper body alone.
- The AI peds, the crouch poses, the sniper scope and the car ADS are
  untouched. The shotgun and stubby reloads (`WEAPON_pump`,
  `buddy_fireRELOAD`) still carry leg keys and layer exactly as before.
- Control-side gates still stop the aim and the fire while sprinting in third
  person (`src/peds/PlayerPed.cpp:1614`, `1665`, `1732`); VICE-137 owns them.
  The animation layering here applies unchanged when they are lifted.
- The feel needs the owner's play-test; there is no test suite.
