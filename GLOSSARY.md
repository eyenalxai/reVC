# Glossary

The domain words for this project. Game words keep the meaning the game gives them.

## Drift

A slide that the player controls with the handbrake. The car's nose points away
from the direction the car travels. The player holds the angle with counter-steer.

## Drift angle (slip angle)

The angle between the car's forward direction and the direction the car moves.
The layer measures it as `atan2(lateral speed, forward speed)`, so a positive
angle means the car travels to the right of where its nose points (the car
slides to its right).

## Hold angle

The slip angle an active drift holds. Entry sets it to the base hold angle, and
steering moves it: steering into the slide widens it, counter-steer unwinds it.

## Velocity alignment

The drift layer's steering of the direction of travel. While a drift grips, the
car's velocity is rotated towards the hold angle, so the angle is reached at
street speed and not only at high speed. On exit, the same rotation bleeds the
slip that is left towards zero. It rotates the velocity and never changes its
length.

## Curve rate

The rate at which the drift turns the whole car — the nose and the direction of
travel together — while it grips, so the path curves and the slip stays at the
hold angle. It follows the target angle (110 degrees per second at the full
angle), is signed by the entry direction, and fades with the grip cut. A faster
drift takes a wider corner: the path radius is the speed over the curve rate.

## Yaw authority (turn authority)

How hard the drift layer may turn the car's nose: the maximum yaw rate — the
rate the nose turns around the car's up axis — and how fast the yaw rate may
change. The authority is absolute: it does not scale with speed, so a drift
turns the same at any speed. It also caps the curve rate.

## Handbrake turn

A turn on the handbrake below the entry speed. The rear grip is cut hard and a
fixed yaw kick turns the car. It needs no entry speed, only steering input and
some forward motion.

## Counter-steer

Steering against the slide. Counter-steer reduces the drift angle; a full flick
past zero ends the drift, or flips it into the opposite drift when the
handbrake stays held (see Drift flip).

## Handbrake

The parking brake input. Held with steering it turns the car: a handbrake turn
below the entry speed, a drift above it. Without steering it brakes the rear
wheels as in the original game.

## Rear grip cutoff

The reduction of the rear wheels' sideways grip that starts and holds a drift.
It follows the slip angle: the tyre is sticky just off centre and lets go as
the slide grows.

## Drift assist

The small stabilizing forces that keep a drift controllable. The assist is
light: a wild player can still spin the car.

## Drift boost

The push along the nose that the car gets when the player ends a long
drift. A boost that is still running takes a new one on top, up to a cap
(a boost chain).

## Drift layer

The module `src/vehicles/DriftControl.*`. It adds drift handling to the player's
car. Every other vehicle keeps the original handling path.

## Feedback layer

The module `src/vehicles/DriftFeedback.*`. It turns the drift layer's state into
what the player sees: the car lean, the camera response, and the skid marks and
smoke. It changes nothing in the physics.

## Telemetry layer

The module `src/vehicles/DriftTelemetry.*`. It writes the drift state to
`drift-telemetry.csv` while the debug readout is on, so a played session can be
read back. The game frame thread only appends text to a buffer; one writer
thread owns the file. It changes nothing in the physics.

## Boost chain

The stack of drift boosts. A clean drift that ends while a boost is still
running adds its tier on top of the level the boost is at.

## Drift chain

Two drifts back to back: the second starts from the recovery of the first,
while its exit ramp is still running. The player either releases the handbrake
and presses it again with steering, or keeps it held through a counter-steer
(a drift flip). The second drift enters with the full rear grip cut, so a
chicane passes without waiting for the recovery.

## Drift flip

A drift chain entered with the handbrake held through a counter-steer. The
flick ends the old drift and grants its boost, and the next frame the new drift
starts in the other direction with the full rear grip cut, so the counter-steer
of the first corner becomes the entry of the second. To end a drift, release
the handbrake.

## Friction circle

The existing model in `CVehicle::ProcessWheel`: one grip budget per wheel,
shared between the forward force and the sideways force. The drift layer changes
how the budget is spent; it does not change the wheel list.

## Movement controller

The module that owns the player's horizontal velocity on foot:
`CPlayerPed::UpdatePlayerMovement` in `src/peds/PlayerPed.cpp`. Every on-foot
control path calls it, in both views. The camera sets the view position and the
direction basis only. The animation root motion no longer drives the player.

## Movement state

One of the controller's states (`ePlayerMovementState`): ground and air today.
Each state has its own acceleration and friction row; the planned slide state
will add a row. The ground state moves the velocity toward the target speed and
slows it down with the friction. The air state keeps the momentum and steers
with a smaller authority.

## Engine velocity unit

The unit of `m_vecMoveSpeed`: one unit per 1/50 second, so one engine velocity
unit is 50 units per second. The tuning constants in `PlayerPed.cpp` divide the
measured speeds and accelerations by 50.

## Reference speed

The speed the current walk/run/sprint animation blend moves the ped at with the
association speed 1.0. The controller targets it and scales the animation
playback by (real speed / reference speed), so the legs stay planted while the
speed changes.
