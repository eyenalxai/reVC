# ADR 0023: The camera toggle writes the first person view setting

Date: 2026-10-16
Status: Accepted

## Context

VICE-139 asks for the camera toggle (`CAMERA_CHANGE_VIEW_ALL_SITUATIONS`, Home
by default) to switch between the first person and the third person view, on
foot and in a vehicle, and to remember the choice. The stock action cycled the
ped zoom levels on foot (and a car camera cycle that VICE-71 had already
removed).

The choice could have lived in a new runtime flag, or in the Display setting
the first person view already uses (`FrontEndMenuManager.m_bFirstPersonView`,
read through `CCamera::FirstPersonViewEnabled`).

## Decision

The toggle writes the Display setting. `CCamera::ToggleFirstPersonView` flips
`FrontEndMenuManager.m_bFirstPersonView` and plays the HUD feedback the stock
cycle played; `CamControl` calls it from the action before the target kind is
handled, so on foot and in a vehicle take the same path.

Consequences of that choice, deliberate:

1. The choice is one state for both views, so entering or leaving a vehicle
   keeps it with no extra bookkeeping.
2. It is remembered exactly like a change on the Display Setup page: the
   settings save (`CMenuManager::SaveSettings`) and the platform shutdown
   (`SaveINISettings`) write it. No session-only override exists, and none is
   needed; a process killed before any save loses it, the same as a Display
   change made without saving.
3. The stock ped zoom cycle is removed with the action (VICE-139): the
   levels stay at the stock default and only a script sets the zoom
   (`SetZoomValueFollowPedScript`) or an old save carries another level.
4. The transition is a mode change and a setting turn off, so the VICE-135
   camera handover glides it; the toggle never cuts.
5. The classic camera's transient look-around first person view stays, but it
   cannot override the setting: `UpdateFirstPersonView` forces the first
   person state while the setting is on, and the transient path only runs
   without the mouse third person camera.

## Consequences

- The Display menu row and the toggle show and write the same value; a player
  who toggles sees the Display setting change.
- A future view setting with a different persistence need must not reuse this
  state; it gets its own.
