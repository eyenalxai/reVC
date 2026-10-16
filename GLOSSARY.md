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

## Drift snapshot

One read of the drift layer's live state — the phase, the active and
handbrake-turn flags, the slip angle, the target, the yaw rate, the grip cut,
the grip scale, the charge, the boost and the entry speed — taken at the point
of the read, so the feedback, the telemetry and the on-screen readout all see
the same values. The drift layer's state stays private.

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
direction basis only. The controller targets the reference speed and owns the
velocity; the fight state, the attached states and the states that hold a
blocking animation keep the animation driven movement.

## Movement state

One of the controller's states (`ePlayerMovementState`): ground and air today.
Each state has its own acceleration, friction and turn rate row; the planned
slide state will add a row. The ground state moves the velocity toward the
target speed, turns the direction and slows the ped down with the friction. The
air state keeps the momentum and steers with a smaller authority.

## Engine velocity unit

The unit of `m_vecMoveSpeed`: one unit per 1/50 second, so one engine velocity
unit is 50 units per second. The tuning constants in `PlayerPed.cpp` divide the
measured speeds and accelerations by 50.

## Reference speed

The ground speed of the walk, run and sprint animation blend the ped plays: the
net root motion of each playing animation over one cycle, at the playback speed
the state forces on it. The animations of the strafe, backward and weapon groups
carry their own speeds, so the reference follows the direction the legs move in
and the weapon the ped holds. The controller targets it and scales the animation
playback by (real speed / reference speed), so the legs stay planted while the
speed changes.

## Magazine weapon

A weapon that fires instant hits and has a clip of two or more rounds. The
definition covers every such weapon, the SPAS-12 included. The player's
shotgun, stubby shotgun and sniper rifle are also magazine weapons; the
rocket launcher stays one-shot. AI peds keep the per-shot cycle for the
magazine weapons. The active reload applies to magazine weapons only.

## Per-shot cycle

The pump or bolt cycle an AI ped pays after each shot of a magazine weapon,
so the AI does not use the magazines.

## Active reload

The timed reload of a magazine weapon. The player starts it with the reload
key; a press inside the reload window finishes the reload fast, and a press
before the window jams it. The reload also works in a vehicle, with the same
window and timing bar. With the feature on, the player's reload is manual
only: the clip never refills by itself.

## Reload window

The part of the active reload between 55% and 80% of the weapon's reload time,
counted from the start of the reload, when a press gives the fast reload.

## Reload jam

The longer reload after a press before the reload window. The press adds half
the weapon's reload time once, so the reload takes about 1.5 times as long.

## Cheat

A word that the player types during play to change the world. The game
compares the newest typed characters against an obfuscated word list in
`CPad::AddToPCCheatString`. A hit runs the cheat effect.

## Population

The cars and pedestrians the game keeps around the player: how many exist and
where they come from.

## Spawn distance

The distance from the player where the game creates and removes cars and
pedestrians. The Display settings slider `SpawnDistance` scales it. The base is
a ring around the player: pedestrians are created at 100-130 m and removed at
131/145 m, cars spawn between 120 m and 250 m and despawn at 280 m.

## Density

How many cars and pedestrians the game keeps for an area. The Ped Density and
Car Density settings scale it.

## Population budget

The number of pedestrians an area keeps. It comes from the zone density, the
road density and the density settings, and it grows with the square of the
spawn distance, because the ring area does. The game stops adding pedestrians
when the budget is reached.

## Cap

The hard top for a budget or a count: pedestrians, cars in use, random cars.
The caps stay below the pools.

## Pool

The fixed storage for the entities and their list nodes: pedestrians, vehicles,
audio entities. A pool never grows; the caps keep the counts below it.

## Destructible object

An object (`CObject`) that breaks when it takes enough damage. Every model with
an OBJECT.DAT row is destructible; a building is not.

## Vegetation model

A model whose name identifies it as a plant. Two kinds of plant exist. A trunk
plant is a tree or a palm, and the name contains `palm`, `palwee` or `tree`. A
leafy plant is any other plant, and the name contains `bush`, `hedge`, `plant`,
`fern`, `ivy`, `grass`, `shrub`, `rose` or `cactus`. The word test ignores
case. A name that contains `lod`, `shad`, `reflect` or `street` is not a plant:
it marks a LOD, a shadow, a reflection or a street light.

A vegetation model without an OBJECT.DAT row spawns as a building. A vehicle
that hits the building gets the soft hit. Above the smash impulse the plant
falls: a trunk plant topples like a light pole and stays in the world, a leafy
plant breaks and disappears.

## Felled plant

A trunk plant a hit has pushed over. It stays visible and colliding, and it
topples with the physics of a light pole: finite mass, no uproot limit, and a
push at the contact point. A plant building cannot move, so a dynamic object
copy with the same model and placement takes its place. The building itself is
only hidden, never removed.

## Soft hit

A vehicle collision with an object, a pedestrian, or a vegetation building. The
vehicle's speed loss and collision damage from a soft hit are one tenth of the
original values.

## Car ADS

Aiming from the driver's seat with the mouse aim button: the follow camera
pulls in and the free aim fires along the camera instead of the stock sideways
drive-by. Only the drive-by vehicles (car, bike, boat) and the drive-by weapon
take it, and the bullets ignore the player's own car.

## Replay block

One of the eight fixed buffers of `REPLAYBUFFERSIZE` (100000) bytes that hold
the replay. A frame is written into the current block; a frame that does not
fit the block starts the next one. No frame may write past its block.

## Replay record radius

The distance from the camera (120 m, `REPLAY_RECORD_RADIUS`) inside which the
replay records vehicles and pedestrians. Entities outside it are left out, so
the per-frame size does not grow with the whole world.

## Replay entity cap

The most entities (`MAX_REPLAY_ENTITIES`, 500) one replay frame records. It
keeps the worst case frame well inside one replay block.

## Wheel pose

The frame transform of one car wheel in `CAutomobile::PreRender`: the spin, the
hover tilt or the road tilt, and the wheel scale. One row of `aWheelPoses` drives
it, so the rear, mid, Dodo, tank and front wheels share one pass. A row keeps its
own hover splash order: the two random arguments are drawn in the order the row
lists them, and swapping a pair would shift the global random stream.

## Scene-graph sync

The librw pass that turns a frame hierarchy into world-space matrices. A frame
is marked when something in its hierarchy was written. The pass (the walk) then
recomputes the local-to-world matrix (LTM) of a frame only when the frame is
marked, when its own matrix was written (the `CHANGED` flag), or when the LTM of
its parent changed, and syncs the objects that hang on the frame only when its
LTM changed. `Frame::syncDirty` runs the pass once for each camera begin-update,
so the walk is part of the frame cost. Marking a frame whose matrices did not
change costs a visit but no LTM work. See ADR 0014.
