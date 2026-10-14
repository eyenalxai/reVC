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

## Handbrake turn

A turn on the handbrake below the entry speed. The rear grip is cut hard and a
fixed yaw kick turns the car. It needs no entry speed, only steering input and
some forward motion.

## Counter-steer

Steering against the slide. Counter-steer reduces the drift angle; a full flick
past zero ends the drift.

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

The forward push that the car gets when the player ends a long drift. A boost
that is still running takes a new one on top, up to a cap (a boost chain).

## Drift layer

The module `src/vehicles/DriftControl.*`. It adds drift handling to the player's
car. Every other vehicle keeps the original handling path.

## Feedback layer

The module `src/vehicles/DriftFeedback.*`. It turns the drift layer's state into
what the player sees: the car lean, the camera response, and the skid marks and
smoke. It changes nothing in the physics.

## Boost chain

The stack of drift boosts. A clean drift that ends while a boost is still
running adds its tier on top of the level the boost is at.

## Friction circle

The existing model in `CVehicle::ProcessWheel`: one grip budget per wheel,
shared between the forward force and the sideways force. The drift layer changes
how the budget is spent; it does not change the wheel list.
