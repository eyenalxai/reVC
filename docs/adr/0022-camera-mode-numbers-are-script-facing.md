# ADR 0022: The camera mode numbers are script facing; never renumber them

Date: 2026-10-16
Status: Accepted

## Context

`CCam::eMode` (`src/core/Camera.h`) numbers the camera modes. The numbers are
not private to the camera code:

- The mission scripts pass the mode number themselves:
  `COMMAND_POINT_CAMERA_AT_PLAYER`, `COMMAND_POINT_CAMERA_AT_CAR` and
  `COMMAND_POINT_CAMERA_AT_CHAR` hand `ScriptParams[1]` straight to
  `CCamera::TakeControl` (`src/control/Script2.cpp:244-266`), and
  `COMMAND_POINT_CAMERA_AT_POINT` does the same with `ScriptParams[3]`. The
  shipped Vice City scripts use the numbers, and the missions are data, not
  code this repository can grep.
- The debug camera takes any value (`DebugCamMode`, the debug menu).
- A save stores `CarZoomIndicator` and `PedZoomIndicator` as raw numbers
  (`src/save/GenericGameStorage.cpp:214-224`), and those values pick the
  on-foot zoom camera. A stock save can therefore hold `CAM_ZOOM_TOPDOWN` or
  `CAM_ZOOM_CINEMATIC`, values this fork's camera cycle can no longer produce.

VICE-140 removed the cinematic (Obbe) camera and closed the mode audit. The
audit found modes that nothing in this codebase selects, and the temptation is
to delete their enum values and renumber the rest.

## Decision

1. The mode numbers are an interface. The enum keeps its numbering: a mode
   whose value nothing selects stays as a hole, and the empty dispatch case
   stays as a comment, so the value remains harmless when a script or a debug
   menu passes it. `MODE_TOPDOWN`, `MODE_GTACLASSIC`, `MODE_CIRCLE` and
   `MODE_AIMING` are such holes today.
2. Removing a camera removes its code and its selectors, never its number.
   VICE-140 removed the Obbe path, its methods and its control path and kept
   the values.
3. The values whose cameras are gone are normalised where a save can still
   carry them: the on-foot ped zoom maps `CAM_ZOOM_1STPRS` and
   `CAM_ZOOM_CINEMATIC` back into the three live levels, the way the stock
   clamp did (`src/core/Camera.cpp`, the ped branch of `CamControl`).
   `CAM_ZOOM_TOPDOWN` is not normalised: it selects the top down on-foot
   camera by design, so a save that holds it now gets that camera (the stock
   clamp used to normalise it away; the audit keeps it).

## Consequences

- A camera mode cannot be removed without leaving its number behind; the
  audit table of VICE-140 is the record of which numbers are live.
- A `PedZoomIndicator` of `CAM_ZOOM_TOPDOWN` in an old save now shows the top
  down camera on foot, where the stock clamp normalised it to a follow zoom.
  That is the one deliberate old-save behaviour change of VICE-139/140.
- A debug camera or a script that passes a hole's number gets no camera of its
  own: the dispatch case exists and does nothing, the same as before.
