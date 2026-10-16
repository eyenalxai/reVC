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

The old Display settings slider `SpawnDistance`. It scaled the ring around the
player: pedestrians were created at 100-130 m and removed at 131/145 m, cars
spawned between 120 m and 250 m and despawned at 280 m, all times the value.
VICE-125 removed the slider, because the range is the draw distance now. An old
file that raised the value and never moved the draw distance is loaded with the
draw distance that asks for the same ring (`1.2 x` the old value, capped by the
visible range like every ring); the file loses the old key with the next save,
so until that save a later load maps it again.

## Draw distance (range control)

The Display settings slider `DrawDistance`, the one control of the range since
VICE-125. It scales the LOD multiplier the renderer draws the world with
(`CRenderer::ms_lodDistScale`, 0.925 to 16.0, 1.2 at stock), and the traffic
ring follows it: the ring scale is the LOD distance over its stock value. The
ring of a lowered draw distance stops at the stock ring, so the lower part of
the slider moves the world only. Its bounds and the LOD distance base live in
`src/core/DrawDistance.h`.

## LOD distance

The distance the world is drawn at: the LOD distance base (300 m) times the LOD
multiplier, which is the FOV factor times the draw distance (`Camera.cpp` sets
the multiplier; `Renderer.h` LOD_DISTANCE and the ring both read the base). The
visible range takes the smaller of the LOD distance and the weather far clip,
and the ring request is the LOD distance over its stock value (300 x 1.2).

## Traffic ring

One spawn or despawn radius of the traffic: the car side band, the car on-screen
band, the car despawn, the camera minimum, the generator ring or the pedestrian
band. The stock radii and the rules that build them live in
`src/control/TrafficRings.h`; `CurrentTrafficRings()` returns the values of the
frame.

## Visible range

The distance the traffic ring may reach: the smaller of the LOD distance and
the weather far clip of the camera. The far clip is weather data (SUNNY 2000 m,
FOGGY 150 m).

## Ring scale

The one number that scales the traffic ring table: the LOD distance over its
stock value, capped so the on-screen car ring stays inside the visible range and
inside the local cap (`RING_MAX_SCALE` 2, so the front band is at most 500 m:
the budgets are fixed, so a ring that follows a 2 km draw distance would spread
the same cars thin and empty the near streets). 1.0 at the stock settings (the
default draw distance 1.2, FOV 70), where the stock LOD distance is 360 m (300 x
1.2); the draw distance drives the ring up to the cap.

## Despawn reserve

The 30 m between a spawn ring and the threshold that removes a car: the stock
step from the on-screen ring (250 m) to the despawn (280 m) and down to the
inner edge (220 m). It holds the vehicle half length, the lane offset and the
curve placement of a fresh spawn, so a fresh spawn is never removed on the
frame it is created.

## Density

How many cars and pedestrians the game keeps for an area. The Ped Density and
Car Density settings scale it.

## Population budget

The number of pedestrians an area keeps ON FOOT. It comes from the zone density,
the road density and the density settings, and it grows with the square of the
ring scale, because the ring area does. The game stops adding pedestrians when
the budget is reached. The drivers of the cars count in `ms_nTotalPeds` with the
pedestrians, so the budget adds them back before the comparison: the drivers and
the passengers of the cars do not shrink the crowd on the sidewalk.

## Cap

The hard top for a budget or a count: pedestrians, cars in use, random cars, the
parked cars of a generator set. The caps live in one table,
`src/core/PopulationBudget.h` (VICE-123), with the fill rates and the streaming
model budget, the pool each number protects and the arithmetic that ties them
together; the table's invariants are checked at compile time. The caps stay
below the pools.

## Pool

The fixed storage for the entities and their list nodes: pedestrians, vehicles,
audio entities. A pool never grows; the caps keep the counts below it. The
population paths of the caps also refuse to create when the free spaces of the
ped pool fall below the pool reserve, because the passenger count is a random
draw and the police crews are not counted at all.

## Driver

The pedestrian that drives a car of the traffic: a `CPed` of the ped pool, one
per car, created with the car (`CVehicle::SetUpDriver`). A driver counts in
`ms_nTotalPeds` with the pedestrians, so the on-foot budget adds the drivers of
the cars back; the car it drives counts in the car budget.

## Parked car

A car a car generator holds at its fixed map position (`CCarGenerator`), not a
car of the traffic. The generator holds it while the player is inside its ring
and the car stays until the car despawn, so the curbs fill along the route the
player drives. A parked car counts in the car budget (`NumParkedCars`) and
carries no driver while it is parked.

## Pedestrian node network

The ped path nodes (`PATH_PED`): the walkable network of the map. The game
creates pedestrians on it (`GeneratePedCreationCoors`) and walks them along it
(`FindNextNodeWandering`). The closest-node search has no radius of its own, so
an area without nodes stays empty: nothing spawns there before VICE-122.

## Off-network spawn

A pedestrian created off the pedestrian node network. The population picks a
random point in the creation ring and takes it only if it is free: ground at
the level of the ring, no surface more than a step above it (no roof or
canopy), ground above the water line, a clearance sphere clear of the static
world, clear of other pedestrians, and a ped node inside the walk bound and on
the same level. The wander state walks the pedestrian to the closest node from
there. A bounded fraction of the spawns takes this path
(`PED_OFF_NETWORK_FRACTION`, a fraction of zero draws no random number); the
rest keeps the node path (VICE-122).

## Stray (off-network pedestrian)

An off-network pedestrian that has not reached the network yet. The population
watches the strays in a bounded table until each reaches a node. The cull
removes a stray that walks for the stall budget without coming closer to its
node, or one whose feet are under the water line; it only removes a pedestrian
the camera does not see, and each counter carries one removal
(`CPopulation::ms_nOffNetworkSpawns`, `ms_nOffNetworkStuck`,
`ms_nOffNetworkDrowned`). A stray with no node to head for keeps its watch for
the no-target time and is then left alone, uncounted. The members of a gang or
couple group are validated at their own spots and watched like any other stray;
a pedestrian that follows another leaves the watch (VICE-122).

## Dummy object

A `CDummyObject`: the stand-in for a map object that is far from the player, or
for a far script object. It has no physics and no collision. A map instance
whose model has an OBJECT.DAT row loads as a dummy. The dummy pool holds the
far ones, the object pool the near ones, and
`CPopulation::ConvertToDummyObject` and `CPopulation::ConvertToRealObject` move
an object between the two pools as the player moves.

## Dummy pool

The fixed storage (`NUMDUMMIES` slots) for the dummy objects: the map's dummy
instances plus every far object that is converted into one. The shipped map
already fills 2336 of the 2800 slots, so a conversion can fail when the pool is
full. That is not an error: the object stays real and the next scan retries.

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

## First person view

The fork's persistent first person camera, on foot and in a vehicle: the view
setting keeps the camera at the player's eyes, the mouse aims and WASD stays
with the movement. The stock look-around view (the setting off) is a separate,
stock path and stays as it is.

## Camera toggle

The `CAMERA_CHANGE_VIEW_ALL_SITUATIONS` action (Home by default) flips the
first person view setting, on foot and in a vehicle. The choice is the state
the Display setting and the settings file use, so it holds across entering and
leaving a vehicle and a save remembers it (VICE-139). The view change glides
through the camera handover. The stock ped zoom cycle the action used to run
is gone.

## Sight view

The view the shot leaves from and the weapon holds its pose at while the player
aims down the sights. `CCamera::GetSightCameraMode` names it: the stock sight
camera mode, or the first person player camera standing in for it. The first
person view serves every aiming weapon with the player camera, so the player
keeps moving and WASD stays with the movement in every movement state
(VICE-137). The photo camera keeps its stock stationary viewfinder, because the
photograph reads its mode. `CCamera::GetSightOverlayMode` names the overlay the
HUD draws: the sniper scope and the rocket sight draw theirs over the player
camera, and the plain first person view draws the on-foot reticle of the aim
(VICE-143), the same sprite the stock sight camera draws.

## Scope zoom

The zoom of the sniper and the laser scope in the sight view. The first person
view keeps the wheel zoom and the range of the stock sniper camera and starts
at the FOV of the view the scope takes over, so the scope looks and zooms like
the scope it stands in for.

## Camera handover

One glide of the game camera with an ease out curve. A view change that would
snap in one frame takes it: entering or leaving a vehicle in the first person
view, the view switching on or off, a sight camera or a script taking the view
over, and the handback. The from pose is the pose the last frame presented and
the target is the live pose of the new view, so input during the glide works on
the live pose and the glide ends exactly on the new pose. The view changes take
`CAMERA_HANDOVER_TIME` (650 ms) and the sight zoom takes
`CAMERA_SIGHTS_HANDOVER_TIME` (250 ms).

## Look recenter

The return of the first person vehicle look to straight ahead of the car. The
mouse look is an offset from the car's heading; when the look rests for
`CAMERA_LOOK_RECENTER_IDLE` (400 ms) the offset returns to zero over
`CAMERA_LOOK_RECENTER_TIME` (800 ms) with an ease out curve. New look input
cancels the return at once and takes the live offset over, and a held aim keeps
the view where the mouse put it.

## Car ADS

Aiming from the driver's seat with the mouse aim button: the follow camera
pulls in and the free aim fires along the camera instead of the stock sideways
drive-by. Only the drive-by vehicles (car, bike, boat) and the drive-by weapon
take it, and the bullets ignore the player's own car.

## First person sights

Aiming down the sights on foot in the first person view. The plain first person
camera keeps the player moving (the M16 camera mode is the stock stationary
aim). The sight predicate, `CCamera::UsingFirstPersonSights`, drives the sight
zoom, the shot ray and the reticle together.
## Aim pose

The pose of the player's upper body while the sights are up: the weapon's fire
animation held at its loop start, the way the stock third person aim holds it.
On the player it drives only the upper body, so the arms and the torso hold the
aim while the legs keep the movement animation, the sprint cycle included.

## Partial association

An animation association that layers over the movement animation instead of
replacing it (`ASSOC_PARTIAL`). It drives the bones its keyframes cover; the
bones it has no keys for keep the animation below it. The weapon fire, reload
and fight animations are partials.

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
## Triangle basis

The per-triangle frame the sphere/triangle tests work in: the normalized first
edge, the in-plane vector at right angles to it, and the third vertex's
coordinates in that frame. It depends only on the triangle, so the collision
model computes it once (VICE-121).

## Broad phase

The part of `ProcessColModels` that picks which of model B's triangles are
worth the narrow phase's math. It appends the candidates in ascending triangle
index order, because the narrow phase keeps the first of equal distances.

## Acceleration tree

A per-model binary tree over contiguous, ascending runs of triangles. Each node
bounds its run; the broad phase descends only into the nodes the query sphere
can reach. The model builds it lazily, on its first collision.
>>>>>>> origin/refactor/collision-cost
