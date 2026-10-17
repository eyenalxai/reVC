# ADR 0029: The stance carries its own collision set

Date: 2026-10-18
Status: Accepted
Relates to: VICE-178 (the implementation), VICE-176 and VICE-177 (the stances), VICE-179 (the camera reads the same sets)

## Context

Every ped model shares one collision model, `CTempColModels::ms_colModelPed1`:
three spheres of radius 0.35 stacked around the model's origin, the origin
`FEET_OFFSET` (1.04) above the feet. The skinned *hit* model
(`CPedModelInfo::m_hitColModel`) is a separate per-model buffer that serves line
of sight, melee and blades; the ped's own collision never reads it.

A crouched or prone player needs a body that is short or long and low. The
collision model is per *model*, not per entity, so a stance cannot swap it in
place.

## Decision

1. **A set per stance.** `ms_colModelPedCrouch` (the standing stack, shortened)
   and `ms_colModelPedProne` (a low, long set along the body axis) sit beside
   `ms_colModelPed1` in `TempColModels.cpp`. All three are `LEVEL_GENERIC` with
   `ownsCollisionVolumes = false`, the same shape as the standing set, so
   `CColStore::RemoveCol` (which clears the volumes of a streamed level with no
   ownership check) can never free them.
2. **One seam.** `CEntity::GetCollisionColModel()` returns `GetColModel()` and
   `CPed` overrides it to return the stance's set, but only when the model uses
   the shared ped model; a cutscene special keeps its own. The ped's collision
   path (`CPed::ProcessEntityCollision`, `CPhysical::ProcessEntityCollision`,
   and the vehicle paths that collide against a ped) asks the seam.
3. **The origin convention holds.** The origin stays `FEET_OFFSET` above the
   feet in every stance, so the 19 sites that convert between the origin and the
   ground keep their meaning. The prone below-torso probe starts at the prone
   body's top instead of the origin, and its proximity gate follows the prone
   set's bounding sphere instead of the standing pelvis assumption; those two
   are the sites a stance invalidates.
4. **Not every reader follows a stance.** The melee target spheres, the
   set-piece spawn checks and the scripted placements keep the standing set,
   because they are about a ped standing somewhere. They are listed in the
   ticket.
5. **One headroom test for the exits.** `CPed::IsHeadroomClear()` casts one
   vertical line from the standing top to the ground against the buildings,
   vehicles, objects and dummies (never peds) and refuses the rise when the hit
   sits above the current posture's own top. It is the one test every stand-up
   exit asks, and it costs one line cast.

## Consequences

- A stance changes what the world collides with, not where the body's origin
  sits, so the movement, the ground snaps and the surface handling are unchanged.
- The camera reads the same sets for its stance offsets, so the camera and the
  collision cannot disagree about how tall a stance is.
- The hit model keeps following the bones, so a crouched or prone ped is still
  hit where it looks.
