# Glossary

The domain words for this project. Game words keep the meaning the game gives them.

## Drift

A slide that the player controls with the handbrake. The car's nose points away
from the direction the car travels. The player holds the angle with counter-steer.

## Drift angle (slip angle)

The angle between the car's forward direction and the direction the car moves.
A positive angle means the nose points to the right of the travel direction.

## Counter-steer

Steering against the slide. Counter-steer reduces the drift angle.

## Handbrake

The parking brake input. Below the drift speed it brakes the rear wheels as in
the original game. At speed it starts a drift.

## Rear grip cutoff

The reduction of the rear wheels' sideways grip that starts and holds a drift.

## Drift assist

The small stabilizing forces that keep a drift controllable. The assist is
light: a wild player can still spin the car.

## Drift boost

The small forward push that the car gets when the player releases the handbrake
after a long drift.

## Drift layer

The module `src/vehicles/DriftControl.*`. It adds drift handling to the player's
car. Every other vehicle keeps the original handling path.

## Friction circle

The existing model in `CVehicle::ProcessWheel`: one grip budget per wheel,
shared between the forward force and the sideways force. The drift layer changes
how the budget is spent; it does not change the wheel list.
