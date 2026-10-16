# ADR 0018: One camera handover glides the first person view changes

Date: 2026-10-16
Status: Accepted

## Context

The first person camera serves views that change in one frame: the player sits
down in a vehicle or steps out (the camera target changes from the walking ped
to the seated one, and the camera mode does not change at all), the view
setting turns on or off, a sight camera or a script takes the view and gives it
back, and the aim zooms in and out. The stock game played each of these as a
cut, and the owner asked for no instant camera transitions anywhere (VICE-135).

The stock transition (`CCamera::StartTransition`) already glides the mode
changes the game does not cut, but a jump cut never starts it, and it
interpolates toward the pose the new view computes with its own two phase curve
and a geometry clip. The first person changes are jump cuts, so they had
nothing.

## Decision

1. `CCamera` owns one camera handover. `StartCameraHandover(duration)` records
   the pose the last frame presented (`m_vecPresentedSource`, `Front` and
   `FOV`, written once per frame), and `ApplyCameraHandover` glides the pose
   the new view computed toward it with an ease out cubic curve: position,
   direction (the short way round: the yaw and pitch of the direction are
   interpolated, `DirectionToYawPitch`) and FOV. The from pose is fixed and the
   target stays live, so player input during the glide works on the live pose,
   and the curve ends exactly on the new pose.
2. Two durations: `CAMERA_HANDOVER_TIME` (650 ms) for a view change and
   `CAMERA_SIGHTS_HANDOVER_TIME` (250 ms) for the sight zoom.
3. The callers that change the view in one frame ask for it: the first person
   camera when the view anchor changes (the player sits down or steps out,
   driver and passenger alike), the camera process when the mode changes with
   the first person camera on either side, and the view setting when it turns
   off. Every request is gated on the first person view setting, so the stock
   first person view (the setting off) and third person changes keep their own
   switches, and a change the stock transition interpolates keeps its own
   values.
4. The glided pose is published back to `Cams[ActiveCam]` for the frame, so
   everything that reads the view (the shot ray, the weapon pose, the laser
   dot, the sight border) follows what the player sees and not the pose the
   glide still moves toward. The blend lags the live pose by one frame, the
   same as the rendered camera does.
5. A script or a cutscene camera owns its own switch. While the camera does not
   look at the player (`m_bLookingAtPlayer`), a running glide stops and never
   blends the view it does not own.

## Consequences

- Every first person view change glides: entering and leaving a vehicle, the
  view setting on and off, the aim zoom and the camera handback.
- The glide passes through geometry (the glide into a seat crosses the car
  body); the geometry clip of the stock transition stays out of it on purpose.
- The camera speed readers (the rain streaks, the pedestrian chatter) see the
  glide as a fast camera move for its duration.
- The stock first person view (the setting off), the scripted cameras and the
  cutscene cameras are untouched.
- The handover is the one place a first person view change blends; a new view
  change asks for a handover instead of cutting.
