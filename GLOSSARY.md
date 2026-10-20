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

## Fill rate

The spawn attempts a frame makes for a count: 3 pedestrian calls and 3 random car
calls per frame, times the ring scale, so the rate follows the ring like the
target does (`PED_ATTEMPTS_PER_FRAME`, `RANDOM_CAR_ATTEMPTS_PER_FRAME` in
`src/core/PopulationBudget.h`). The rates are not caps: they bound the time a
count takes to reach its top, and a count that is already at its top stops the
attempts at the first check.

## Fill burst

The transition fill of the population and the traffic (VICE-126). After boot, an
interior exit and a cutscene end, the counts reach 90 % of their local targets at
once instead of trickling up. A burst is a bounded work window: 60 frames that
each spend 25 spawn attempts, or 100 while the fade covers the screen (the black
frames of a transition and the dark head of a fade), so a burst asks for at most
6000 attempts. A frame the models cannot serve after the run has
started waits without spending its window and counts against the give-up bound;
30 frames in a row without a placement give the burst up. Before the run's
first attempt the world is still loading (the local target is not known yet, or
the zone's models are not resident yet): those frames ride their own boot wait
(`FILL_BURST_BOOT_WAIT_FRAMES`, 600), spending neither the window nor the
give-up bound, and a world that never becomes ready gives the burst up with a
counted reason (`ms_nFillBurstEndedBootWait`). The state machine is
`src/core/FillBurst.h`, the bounds are in `PopulationBudget.h`, and the triggers
are the two boot countdowns, the area writer (`CGame::SetCurrArea`) and the end
of a cutscene (`CCutsceneMgr::DeleteCutsceneData`); ADR 0021 holds the
decisions and the restated acceptance.

## Near bubble

The 360-degree disc of 90 m around the player that the traffic never creates or
removes an entity inside (VICE-127). The radius is the car side band (120 m)
less the placement reserve of the ring table (30 m), so no spawn band places
inside it; the population inside the bubble is the traffic that drove in from
outside. A creation outside the bubble is allowed only while the player cannot
see the position - beyond the visible range (the fade and the fog), behind the
soft distance (the traffic behind the camera, off the view cone by the margin),
behind the confirmation ray (the occluded hint and the close blind spot are
never allowed on their own: a blocked line of sight decides, bounded per frame)
- or in the in-view band (VICE-146: the on-screen spawn band at or beyond
`rings.carOnScreenSpawnMin`, covered by the car's own reveal). A removal needs
the fresh-spawn grace, the removal distance (the bubble plus its reserve, at
least the ring's despawn) and the same conditions: hidden, or in the in-view
band (covered by the fade-out of the despawn). A car created off the view cone
holds its alpha 0 until it is first on screen, so the reveal itself is the fade.
The rules live in `src/control/NearBubble.h`, the engine arms and the counters
in `NearBubble.cpp`; ADR 0025 holds the decisions (and the VICE-146 amendment).

## Soft distance

The distance at which the soft arm of the near bubble begins (VICE-127): the car
side band less the widest lane offset of the shipped paths (17.5 m), so every
placement of the band sits at or beyond it. A car off the view cone by the
margin and at or beyond it may be created - the traffic behind the camera - and
inside it only the confirmation ray decides. It is the arm that keeps a street
with no occluders alive for the traffic behind the camera, because the ring's
bands sit inside the visible range by the ring table's design
(`NEAR_BUBBLE_SOFT_DISTANCE`); the front calls of the spawner are the in-view
band's since VICE-146.

## Confirmation ray

The one test that allows a car to be created or removed behind the near bubble's
hints (VICE-127): `CWorld::GetIsLineOfSightClear` against the buildings, so a
blocked line of sight means no turn of the camera can reveal the position. The
occluded hint is a frame old and can be wrong against a turned camera, so it
only selects the position; the close blind spot (off the view cone by the
margin, inside the soft distance) uses the same ray. A frame spends at most four
of them (`NEAR_BUBBLE_LOS_RAYS_PER_FRAME`).

## Catchable zone

The positions of the near bubble's acceptance (VICE-146): in the frustum and
nearer than the in-view band minimum. A creation or removal there is one the
player could catch, so the acceptance counters of the zone must stay 0 unless a
fade covered the event; the occluded and beyond-the-fade positions of the
frustum sit outside the acceptance (a confirmed ray or the fog hides them, and
the rule counts them as hidden). The zone is named by the two counters, the
catchable-zone counters.

## Catchable-zone counter

The acceptance pair of the near bubble (VICE-127; the frustum counters renamed
at VICE-146): `ms_nNearBubbleCatchableCreations` and
`ms_nNearBubbleCatchableRemovals`, read live by
`utils/caps-audit/live-counters.py`. An event the player could catch - in the
catchable zone - moves them unless a fade covered it; the rule never allows one,
so a path that bypasses the rule shows up there. Both must stay 0 in a play
session; the catchable events a fade covers are counted apart (fade creations
and fade removals), the in-view band's events have their own pair (the band
counters), and the cars created outside the traffic spawner have their own rows
(`ms_nNearBubbleOtherCreations` and its in-view subdivision), so neither a
transition nor a scripted car reads as a traffic violation.

## In-view band

The stock game's on-screen spawn band, restored as the last allowed arm of the
near bubble (VICE-146): a position in the frustum at or beyond
`CurrentTrafficRings().carOnScreenSpawnMin` (220 m at the stock ring, 440 m at
the owner's settings) may be created or removed even though the player can see
it. The spawner's on-screen branch aims at `carOnScreenSpawnMax` and the birth
cull holds a fresh on-screen car at or beyond the minimum, so the band is the
band the stock game always had; the near bubble had refused it and starved the
traffic. A creation there is covered by the car's own reveal (its alpha starts
at 0 and ramps up from the first render), a removal by the fade-out of the
despawn (`CVehicle::bFadeOut`); the strict bubble keeps refusing everything
nearer than the band (`NEAR_BUBBLE_ARM_ONSCREEN_BAND`).

## Band counter

One of the two counters of the in-view band, read live by
`utils/caps-audit/live-counters.py`: `ms_nNearBubbleBandCreations` and
`ms_nNearBubbleBandRemovals`. They count the events the band arm allowed (in
the frustum, at or beyond the band minimum, no fade); they rise as the traffic
fills and are not violations.

## Fresh-spawn grace

The 5 seconds a freshly created car is protected from every cull
(`NEAR_BUBBLE_GRACE_MS`, VICE-127). The vehicle holds it in
`m_nRemovalProtectedUntil`, the one field the set pieces, the road blocks and
the wreck cull already used as a not-before clock; the grace is set where a car
of the traffic (or a parked car) is created. It is the same 5 s the stuck cull
waits, so a car that was just placed is never the one a cull gives up on.

## Bubble fill

The number of cars the traffic spawner created (the random cars and the
emergency services) inside the near bubble, read live as `ms_nNearBubbleFill`
(VICE-127). The target is the share of the car budget the bubble's area takes of
the despawn disc (`ms_fNearBubbleTarget`): about 6 cars at the stock settings
and 10 at the owner's. The cars of the same budget that the spawner did not
create (the scripts, the set pieces, the road blocks, the car parks, the
garages, the cheats) are `ms_nNearBubbleOtherFill`; the orchestrator compares
the sum of the two against the target while driving and during a camera turn.

## Far traffic layer

The cheap traffic of the band between the near traffic and the fog (VICE-128).
A far car is a real vehicle of the pool in a far state: rendered by the ordinary
vehicle path (its streamed model, the LOD, the clump alpha reveal), but static
and away from the moving entity list, with collision off and its audio entity
inactive, so `CWorld::Process` and the collision path never touch it and the
near AI never brakes for it. The layer moves its own cars along the path nodes
with the same curve arithmetic the on-rails simple car uses, on a stagger of a
third of the layer a frame (about 10 Hz), and it asks the near bubble's rule for
every creation and removal (a car the player can see goes by the in-view band's
fade-out, VICE-146). The rules live in `src/control/FarTraffic.h`, the engine
side (the applicator, and the only place a far car is written) in
`FarTraffic.cpp`, the simulation in `FarTrafficWorker.{h,cpp}` (VICE-133, its own
thread), and ADR 0026 and ADR 0027 hold the decisions.

## Far traffic worker

The thread the far traffic layer's simulation runs on (VICE-133): the path pick,
the rail pose, the path's own height, the heading, the stagger's share and the
car's own random stream, over the `FarTrafficWorld` snapshot of the graph and
its own per-car state. It writes nothing but its published pose block; the
applicator on the main thread does every creation, removal, promotion,
hand-over and world write. The worker's window is one frame's tail: the cycle is
handed over at the end of `Idle` (after the render) and joined at the top of
`CGame::Process` (before `CWorld::Process`), so it never runs while the world is
processed or the scene is drawn, and a frame with no layer work does not wake
it. The car-on-rails arithmetic it runs is the near traffic's own, shared
through `CarOnRails.h` and checked bit for bit by `utils/far-traffic-worker`;
the worker's list of cars is kept in lockstep with the applicator's by the
ADD/DROP command list, keyed by the pool slot, and the published poses are
checked against both before they are applied. Its counters are
`ms_nFarTrafficWorkerCycles` (the cycles), `ms_nFarTrafficWorkerLost` (a
command or a pose the cycle and the layer disagreed about: must stay 0),
`ms_nFarTrafficShortCurves` (the pick's clamped curves) and
`ms_nFarTrafficCatchUps` (the pending cycle the applicator ran inline when the
window between two cycles grew the command ring: 0 in normal play);
`FAR_TRAFFIC_WORKER` (`FarTraffic.h`) selects the worker or the same cycle run
inline.

## Far band

The annulus a frame's far layer fills, from the one ring table (VICE-128): the
inner edge is the near traffic's despawn (`rings.carDespawn`) and the outer edge
the visible range (`rings.visibleRange`, min(LOD distance, weather far clip)).
The band is open only while it has width (`FAR_TRAFFIC_MIN_BAND`, 160 m); its
count cap follows the band's area up to `FAR_TRAFFIC_MAX` (64), and the cars
outside the car budget keep the rest of the vehicle pool's margin. The creation
sites are tried farthest first: the fog (a car forming out of the haze ahead)
and the near site - the band's inner edge plus `FAR_TRAFFIC_SITE_MARGIN` (50 m,
the collector's placement lands at or inside its target, so a site aimed at the
edge itself lands in the near band and is refused) - in both cones, ahead (the
oncoming traffic) and behind (the traffic the player passed); a placement of the
fog site's own segment that lands inside the fade is allowed by the in-view band
(VICE-146, the car's own reveal covers it), and a car the player can see goes by
the same band's fade-out. A refused attempt is counted by the stage it reached
(`ms_nFarTrafficRefusedNoSite` / `...RefusedBand` / `...RefusedGate`).

## Refused pose

A pose of the far traffic layer the world cannot hold (VICE-168): the position
or a direction is not finite, or the x/y fall outside the sector grid of the
world (`World.h`: x [-2400, 1600), y [-2000, 2000)). The grid's guard covers
only the upper side, so writing such a pose and calling
`CPhysical::RemoveAndAdd` walks out of the sector array. One rule
(`FarTrafficPoseIsInWorld`, `src/control/FarTraffic.h`) serves the simulation
and the applicator: the simulation marks the refusal in its published pose and
the applicator refuses the pose again whatever producer wrote it, counts it
(`ms_nFarTrafficRefusedPoses`), never writes it, and hands the car to the near
side by the no-driver promotion. A healthy layer reads 0.

## Promotion

The switch of a far car into a full traffic car when it reaches the far band's
inner edge (VICE-128): the car becomes a `RANDOM_VEHICLE` again (so it counts in
the car budget), gets its driver (`CVehicle::SetUpDriver`), collision, the moving
entity list and its audio. The car's own facts - the matrix, the model, the
colours, the status, the clump and its alpha - are unchanged, and the driver
rides the car's own reveal (`CVehicle::bDriverWaitsForReveal`), so a switch of an
unseen car cannot show a driver before its car. When the ped pool is short of its
reserve the car is made real anyway inside the player's reach, without a driver
(the reach's seal, `FAR_TRAFFIC_SAFETY_PROMOTE`): a far car is a shell the
player can drive through, which is not allowed where the player can be.

## Hand-over (demotion)

The switch of a leaving traffic car into a far car instead of the deletion the
near cull would make (VICE-128). The cull has already decided the car is outside
the near bubble, past its fresh-spawn grace and hidden; the layer flags its
occupants away, takes its class out of the car budget and applies the far state,
so the player watches a car recede into the fog instead of blinking out. The
hand-over starts `FAR_TRAFFIC_ADOPT_HYSTERESIS` (40 m) past the despawn and
refuses when the band is closed or the layer is at its cap.

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

Since VICE-155 the creation is the spawn service's **parked kind**: the
generator makes one `CSpawnService::RequestParked` call per attempt, and the
service owns the placement - the parked target gate (`ParkedCarTarget`), the
model policy (a fixed model, or the dynamic generator's draw from the zone's
loaded class arrays), the spot's own validity (the blockage, the ground, the
near bubble without rays) and the creation with its setup (the `PARKED_VEHICLE`
tag, `STATUS_ABANDONED`, the colours, the alarm and lock rolls). The generator
keeps the policy it always had: when it asks (the timer and the range gate),
the uses, the handle, the blocking flag and the state the save file carries;
the OCCUPANCY refusal is its blockage's flag and `m_nTimer += 4`. No placement
decision or dice is left in `CarGen.cpp`.

## Pedestrian node network

The ped path nodes (`PATH_PED`): the walkable network of the map. The game
creates pedestrians on it (`GeneratePedCreationCoors`) and walks them along it
(`FindNextNodeWandering`). The closest-node search has no radius of its own, so
an area without nodes stays empty: nothing spawns there before VICE-122.

## Off-network spawn

A pedestrian created off the pedestrian node network. The population holds the
share of these spawns at a target ratio (`PED_OFF_NETWORK_TARGET_RATIO`, 0.15;
a target of zero asks for nothing and keeps the node path bit-exact) and takes
the position from a validated standable cell (VICE-154); a request the cells
cannot answer falls back to the node path for that spawn. The wander state
walks the pedestrian to the closest node from there, and the stray cull
watches it (ADR 0019, which VICE-154 replaces in part).

## Standable cell

One 16 m cell of the world grid the off-network pedestrian spawns stand on
(VICE-154, `src/peds/PedSites.h`): 250 x 250 cells over the world, a cell
unvalidated until its build. The build probes a 4 x 4 lattice of the cell on
the main thread and keeps up to four spread points that pass the predicate:
the ground cast down from the level of the request (3 m of tolerance), no
water (the ground above the line plus 0.1 m), the top surface over the point
no more than a step (0.5 m) above the ground the point stands on (a roof, a
canopy or a bridge deck rejects), a free clearance sphere over the feet, and a
ground not steeper than the walk limit (cos 45, the new slope predicate). A
request maps the drawn position to its cell: a built cell answers with its
stored point nearest the draw, a built cell with no point (a *dead* cell) and
an unvalidated one fall back to the node path (the unvalidated one enters the
bounded build queue first). The queue (512 requested cells, nearest-first) and
the cache (1024 built cells, the farthest evicted) are bounded, the pump
spends two builds a frame, and every world query stays on the main thread:
the planner worker reads the store's plain answers only.

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
aims. There is one aiming path: every gun aims at the pointer, in both views
and in a vehicle, and the shot follows the aim ray. A normal gun draws no zoom
and moves no camera; `CCamera::GetPlayerAimRay` is the one ray. The sniper and
the laser scope run their own `MODE_SNIPER` camera with the stock zoom, and the
photo camera keeps `MODE_CAMERA` and its viewfinder. `CCamera::GetSightOverlayMode`
names the overlay the HUD draws: the scope for the sniper and the laser scope,
the viewfinder for the photo camera, and no overlay for a normal gun, which
draws the one reticle of its view.

## Scope zoom

The zoom of the sniper and the laser scope. The scope runs its own `MODE_SNIPER`
camera in both views, with the wheel zoom, the range and the sniper motion blur
of the stock scope.

## Camera handover

One glide of the game camera with an ease out curve. A view change that would
snap in one frame takes it: entering or leaving a vehicle in the first person
view, the view switching on or off, a sight camera or a script taking the view
over, and the handback. The from pose is the pose the last frame presented and
the target is the live pose of the new view, so input during the glide works on
the live pose and the glide ends exactly on the new pose. The view changes take
`CAMERA_HANDOVER_TIME` (650 ms).

## Look recenter

The return of the first person vehicle look to straight ahead of the car. The
mouse look is an offset from the car's heading; when the look rests for
`CAMERA_LOOK_RECENTER_IDLE` (400 ms) the offset returns to zero over
`CAMERA_LOOK_RECENTER_TIME` (800 ms) with an ease out curve. New look input
cancels the return at once and takes the live offset over, and a held aim keeps
the view where the mouse put it.

## Car ADS (car aim)

Aiming from the driver's seat with the mouse aim button. The driver's aim state
raises any gun, the aim ray is the camera ray, and one held weapon pose follows
it: the arms and the torso follow the aim while the vehicle heading stays the
drive heading, so the body never turns sideways. The bullets ignore the
player's own car. The stock sideways drive-by of the look keys is gone
(VICE-138); a bike keeps its stock forward drive-by on the plain fire button.

## Car weapon cycle

The driver's switch to the next or the previous aimable weapon with the car
cycle keys (`VEHICLE_CYCLE_WEAPON_LEFT` and `VEHICLE_CYCLE_WEAPON_RIGHT`). Only
the guns and the photo camera with ammo count (`IsAimableWeapon`). When the seat
stored the weapon of the foot, the choice replaces the stored weapon, so the
exit brings back the weapon the driver last chose.

## Aim state

`CPlayerPed::m_bWeaponAimActive`, the one state the aim button raises for every
gun and the photo camera, on foot and in a vehicle, in both views and in every
movement state. The sniper and the laser scope add their scope camera and the
photo camera its viewfinder; no other weapon changes the view.

## Aim ray

The one ray the gun pose and the shot follow, `CCamera::GetPlayerAimRay`. The
third person mouse view aims through its crosshair, so the shot goes where the
pointer points; the first person view, the scopes and the viewfinder aim along
their own camera.

## Gun

A weapon that aims through the one aim system: it fires an instant hit, a flame
or a rocket. The throwables, the detonator, the melee weapons and the photo
camera are not guns; the camera aims with its viewfinder and the melee and the
throwables keep the stock behavior. The rule lives in `WeaponAim.h`
(`IsGunWeapon`, `IsAimableWeapon`).

## First person sights

The first person view with the weapon at the sights: `CPlayerPed::IsAimingWeapon`
is true while the player aims, and the first person view keeps the player FOV.
There is no sight zoom and no sight camera; the reticle and the aim ray alone
serve the aim.

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

## Closest-node search

`CPathFind::FindNodeClosestToCoors`: the lookup that maps a world position to
the path node nearest to it. The distance is the sum of the axis gaps with the
z gap weighted three times, and the search returns the node of the lowest
distance, the lowest node index on a tie. `DoPathSearch` asks it twice per
search (the target node and the start node) and the spawn and ped paths call it
directly, so it is the most-called pathfinding routine. See ADR 0024.

## Path node grid

The uniform grid of path nodes (32 m cells) that the closest-node search walks
instead of scanning every node of the type: rings of cells around the query
point, stopped when no farther node can tie or beat the closest one found.
Built once after `PreparePathData`, 53 KB inside `ThePaths`; the plain scan
stays as the fallback for a map the grid cannot hold. See ADR 0024.

## Mission car (mission vehicle)

A vehicle the game created for a script, a mission or a set piece:
`VehicleCreatedBy == MISSION_VEHICLE` (Vehicle.h). It counts in the car budget
as `NumMissionCars`. A full save carries the mission cars - the pool save
writes a vehicle out when `IsVehicleSaved` reads it as a mission car
(Pools.cpp) - because the traffic and the parked cars are rebuilt around the
player after a load; a mission-replay quicksave also carries the player's car,
whatever its class (the same predicate, over `IsQuickSave`). A loaded mission
car is counted once in `NumMissionCars` (VICE-163).

## Law flag

The `CVehicle::bIsLawEnforcer` flag: the car chases the player's wanted level.
A car of any class can carry it - a set piece chase car of the random class, a
mission car a cop driver enters (Ped.cpp), or a car restored from a save,
whatever its class. `CVehicle::ChangeLawEnforcerState` sets and clears the flag
and moves `NumLawEnforcerCars` with it, and the count follows the flag, not the
class (VICE-166): the car counts when the flag is set, and the count is given
back when the flag is cleared or the car is destroyed. A flagged car of a
non-random class therefore counts in its class counter and in
`NumLawEnforcerCars` while the flag is set (the documented double count;
PopulationBudget.h).

## Vehicle counter

One of the car classes' live counters in `CCarCtrl`: `NumRandomCars`,
`NumLawEnforcerCars`, `NumMissionCars`, `NumParkedCars` and `NumPermanentCars`.
A creation counts once in the class the vehicle carries (the `CVehicle`
constructor) and a deletion takes the count back (the destructor); a class
switch moves it (`CCarCtrl::UpdateCarCount`). `NumLawEnforcerCars` follows the
law flag, not the class (VICE-166): a car of any class that carries it counts
when the flag is set, and the count is given back when the flag is cleared or
the car is destroyed, so the counter balances whatever the class.
`NumCarsInUse` sums four of the counters (random, law, mission, parked) and the
two emergency counters; the permanent class and the far traffic stay outside
it. A law car of the random class counts in both `NumLawEnforcerCars` and
`NumRandomCars`; a law-flagged car of a non-random class counts twice the same
way while the flag is set, so the sum counts each of them twice and the budget
blocks one car early per live law car; the double count is documented and on
the safe side (`PopulationBudget.h`). `CCarCtrl::ReInit` zeroes every counter,
and the save load calls it before the vehicle pool restore; each restored
vehicle counts once in the class the save carries (VICE-163), and the law flag
of the save rides that count (VICE-166). The offline checks are
`utils/save-vehicle-count` (the restore count, VICE-163) and
`utils/spawn-law-counter` (the law flag on both sides of the count, VICE-166).

## Occupancy store

The spawn service's memory of where the population is (`src/control/
SpawnOccupancy.h`, VICE-149): the presence grid of what stands where, and the
short reservations of the sites the planner promised. The store holds no
placement and makes no decision; it is fed from the entity lifecycle (the
vehicle and pedestrian constructors and destructors, the world add, the physics
step, the aircraft, the far layer, the crane, the respray and the carried
pedestrian's seat), so the paths that remove or move entities outside any spawn
call - a world clear, the pool valve, a cull, a replay restore, a save load -
leave no phantom. A constructor only registers the handle (it must not read the
unplaced matrix); the first placement counts it. Each handle remembers the cell
it was counted in, so a removal is exact whatever the entity did in between.
`CReplay::RestoreStuffFromMem` and the boot call `CSpawnOccupancy::Resync`, the
one true rebuild, and `CWorld::ClearForRestart` calls `Reset`. The spawn
service's planner reads the cell counts (the capacity filter) and the site
reservations since VICE-150; the director is a later reader.

## Presence grid

The 16 m cell grid inside the occupancy store (VICE-149): 250 x 250 cells over
the world, a vehicle count and a pedestrian count per cell. The planner reads a
cell for its capacity check and the director for its density; the grid cannot
express lanes, two-way occupancy or the ground, so it is a coarse filter and
the final collision query at creation stays the true guard.

## Site reservation

The occupancy store's short promise on a site (VICE-149): the planner reserves
a site id - the site index's directed connection - before it creates the actor
there, so two requests cannot plan the same connection at the same time. The
run releases a created site's own reservation at once (the created car's
presence in the store is the guard from then on); the reservation expires
after two seconds (`SPAWN_OCCUPANCY_RESERVE_MS`), so an abandoned plan frees
the site by itself. A held entry is one whose remaining lifetime is inside the
reserve window: a clock that moved backwards (a load) reads the site free, and
a stale entry cannot read held.
## Spawn site

A place a vehicle may be created on the car path graph, one per **directed**
connection (VICE-148). A car path link record is one road segment and both
travel directions share it, so a site is the (entry node -> connected node)
pair itself and every travelled-side fact lives per orientation: the lanes of
the travelled side (`numLeftLanes` or `numRightLanes`, the rule
`CCarCtrl::CreateCarFromPlacement` reads), the travel direction (the link's
direction with the sign of the node pair) and the flags. A site carries the
connection's midpoint - the node pair's own midpoint, never the link record's
position (the external links' positions are the synthetic midpoints of the
merged external nodes) - the node pair, the connection and its link, the width,
the rarity (`spawnRate`, the *smaller* of the two nodes: the acceptance
threshold the spawner's dice read) and the info-zone index. A directed
connection is not a site when the travelled side has no lanes (the wrong
direction of a one-way street) or when the pair's ends disagree about water:
boat sites are directed water-water pairs, road sites land-land pairs. The
ground is never validated at build time (the collision world is not resident
at boot): a site starts unvalidated and the ground is validated lazily, on the
main thread.

## Spawn site index

The flat array of the spawn sites plus a 32 m spatial hash, in the style of the
path node grid (ADR 0024): one record per directed connection, up to
`NUM_PATHCONNECTIONS` (20,400), built once per load right after
`PreparePathData` (`CSpawnSites::Build`, `CGame::Initialise`) and never
rebuilt. The script's road toggles and a save load move the graph's node flags
only (`CPathFind::m_nodeFlagsVersion`), and `CSpawnSites::RefreshFlags`
re-derives the sites' flags when it moved - the `FarTrafficRefreshWorldFlags`
pattern: one comparison a frame, no rebuild. The record holds the info-zone
index, never a `CZoneInfo` (the zone records are filled by the script after
boot and replaced by a save load, so the service resolves the info live). The
rules and the storage are in `SpawnSites.h`/`SpawnSitesIndex.cpp` (engine-free),
the snapshot out of `ThePaths` and the info-zone indices in `SpawnSites.cpp`
(the engine side), and `utils/spawn-service` is the evidence program. The
spawn service's planner reads it (VICE-150); the far layer's migration is a
later requester.

## Model readiness

The spawn service's rule for "the model can be used now" (VICE-151):
`HasModelLoaded && HasTxdLoaded && HasAnimLoaded` - the model's own stream, its
texture dictionary and its animation block. A "loaded" model can have an
evicted animation block (the streamer's memory eviction moves txds and anim
blocks without moving the model), so a check of `HasModelLoaded` alone can
lie. A model without an animation block has nothing to evict: the animation
index is -1 when the model's `.ide` row names no animation file (most
pedestrians, most cars) or names one the image does not hold (the peds' "man"
has no `.ifp`, so the block resolves to -1). An empty set - no class row
holds a model, or every group slot is a -1 hole - is never ready: the service
has nothing to choose from. The whole-set predicate iterates the arrays
the service plans from - the car class arrays (`CCarCtrl::LoadedCarsArray`,
the models a class chooses from) and the pedestrian group
(`CPopulation::ms_pPedGroups`) - never
`CStreaming::ms_bIsPedFromPedGroupLoaded`, which is set on *request*, not on
load. `CSpawnReadiness` is the engine side; `SpawnReadiness.h` is engine-free
and `utils/spawn-service/readiness.cpp` is the evidence program. The spawn
service's readiness gate (`CSpawnService::RequestCar`) reads
`CSpawnReadiness::IsModelReady` for the requester's model.

## Prewarm set

A set of models the spawn service requests before the planning frames need it
(VICE-151). The car class set is global - every model of
`CCarCtrl::CarArrays`, the shipped data's 56 class rows - requested once per
load (the boot, after a save load) and pinned for the session. The pedestrian
group and the gang sets are per zone: the zone's pedestrian group (the 16
models) and the gangs the zone's thresholds select
(`CTheZones::GetZoneGangPedMask`/`GetZoneGangCarMask`, the rule
`StreamZoneModels` loads by), requested when the zone changes and released
when it changes again - and released on an interior entry too (the interior
budget is flat; the street's set would hold its bytes through the stay) and
requested again on the exit. Every model, txd and animation request carries
`STREAMFLAGS_PRIORITY`; in a cover (the drawn fade, the boot and loading
screen, the death/arrest fade, the taxi shortcut, a save load) the set is read
with the blocking `CStreaming::LoadAllRequestedModels(false)` path as
`CStreaming::LoadScene` does, and the readiness is verified per model. No
prewarm runs during a cutscene. `CSpawnReadiness::PrewarmInCover` is the
blocking host, the per-frame pump lives in `CStreaming::Update`.

## Prewarm pin

The `STREAMFLAGS_DONT_REMOVE` bit a prewarm set puts on its model, its txd and
its animation block, so the streaming's eviction cannot take the set
(VICE-151). The ledger remembers which flags the plan set itself (the owned
bits), so a release never clears a flag another system holds, and the pump
re-asserts a flag that was cleared under a pin - or left on a stream that was
removed (`FlushRequestList`, a `RemoveModel`), which it re-requests. The
vehicle pins - the ones that take a `CStreaming::ms_vehiclesLoaded` tracker
slot - stop at `SPAWN_READY_PIN_BUDGET` (`MAXVEHICLESLOADED` 90 less the
reserve of `SPAWN_READY_PIN_RESERVE` 25 slots). The pins are never removable
while held, so those 25 slots are the streaming's own headroom, taken from
the live census (the tracker normally holds 57..65 of 90 and the catalogue's
56 are the bulk of it); a pin request that does not fit is counted deferred
and not made. The shipped sets fit: the class catalogue's 56 models plus the
widest zone set are 61 vehicle pins against the budget of 65, and the tracker
keeps 29 slots beyond the pins.

## Spawn service

The one writer of spawn intent (`src/control/SpawnService.h` and
`SpawnService.cpp`, VICE-150): a producer requests, the service decides where
the actor may legally appear and creates it there. The request envelope
carries three creation kinds: a **road-site placement** (the planner:
candidates from the site index, filtered by purpose, the live zone, the
occupancy store and the visibility ladder, ranked, reserved, then created by
the primitive), an **explicit** creation (the exact position, model and
lifetime given by a script, a mission, a set piece or a special actor:
synchronous, no visibility and no ring, with a force bypass over the budgets
and the reserves, and the same-frame handle; the ped arm creates the roadblock
and set piece cops, the accident's dead pedestrian and the scripts' characters
through `CPopulation::AddPed`/`AddPedInCar`) and the **parked
kind** (VICE-155: a car generator's own position and model with the spot's own
validity - the blockage, the ground, the near bubble without rays - and the
`PARKED_VEHICLE` tag; see the parked car entry). Every kind is counted. The
refusal reasons are the ticket's own - no site, occupancy, visibility, zone,
readiness, budget, pool, reachability - and every refusal event (a request's
gate, a plan drop or an attempt's refusal) is counted, so refusals after
planning are measured, not hidden. The service's telemetry is one static
`SSpawnServiceTelemetry` instance, `CSpawnService::ms_Telemetry`, read live by
`utils/caps-audit/live-counters.py`; the near bubble's gates and counters sit
behind the service's ladder API (`CSpawnService::AllowsCreation`,
`AllowsRemoval` and the counter notes), so no spawn decision outside
`SpawnService.cpp` asks them directly. The ambient traffic is the first
requester (`CCarCtrl::GenerateOneRandomCar`).

Since VICE-158 the remaining direct creators are explicit requests with a
purpose: the set pieces are MISSION, the roadblocks and their cops are POLICE
(the wanted response), the garage restores, the cheats, the taxi shortcut, the
RC car and the accident's dead pedestrian are SPECIAL, and the PedAI self-car
is SCENARIO. Every one forces, because the old path could not fail on a
scheduling decision (the creators' own guards - a model's residency, a
collision, a tilt - stay and run around the request). No creator outside the
service constructs a vehicle or a pedestrian: the vehicle primitive is
`CCarCtrl::CreateCarFromPlacement` plus the explicit constructor, and the
pedestrian primitive is `CPopulation::AddPed` (`AddPedInCar` for a seat).

The service states its deltas from the old spawner, rather than hiding them:
the plan's candidates are the sites whose **midpoint** is in the ring band,
not the node pairs that bracket the ring (the index carries no per-node
positions, and the rank's nearest-to-the-ring keeps the placement on the
ring); the ped pool's reserve is checked once per request as well as once per
frame and once per car; the top-down camera's bypass arm allows a creation
the old birth cull's top-down disjunct refused (the ladder owns the view);
and the RNG stream shifts, because the removed rarity draw and the moved
budget gate consume the shared dice differently (the fill burst's own shift,
ADR 0021). The plan's catchable-midpoint conservatism and the rarity weight
are stated in the planner's entry below.

## Spawn planner

The road-site half of the spawn service (VICE-150): `SpawnOfferSites` takes
the candidates of the request's ring band from the site index's hash (the old
collector's outer distance and cone, the purpose's water policy and the
disabled flag), the frame resolves the cheap facts of each candidate (the
ladder's candidate for the site's midpoint, without a ray) and the ranked
prefix's deep facts (the site's live zone and `CPathFind::
TestCoorsCloseness`, the path search the primitive used to pay per creation),
and `SpawnPlanSites` filters by reason, ranks - hidden arms first, then the
emptier presence cell (spread), then the nearest to the request's ring, then
the rarity weight, then the site id - and reserves the returned placements in
the occupancy store while the plan is outstanding, so two requests cannot plan
the same connection at the same time. The rarity is the rank's last key by
decision: it is the weight the old dice became, so the order's own keys decide
first and the rate only breaks their ties (not a filter - that would be the
dice again; the dump path prints the shipped rates). A candidate whose
midpoint arm is catchable is dropped (VISIBILITY) and never reaches the
primitive - the plan's stated conservatism: the old path attempted it and a
final-position occluded hint could allow it by ray. `SpawnRunPlan` tries the
plan's sites in order through the primitive and counts every post-plan
refusal. The rank keeps the first `SPAWN_SERVICE_DEEP_CANDIDATES` (8)
candidates for the deep facts, and the plan returns at most
`SPAWN_PLAN_MAX_PLACEMENTS` (4).

## Creation primitive (spawn)

`CCarCtrl::CreateCarFromPlacement` after VICE-150: the one car creation of the
spawn service and the far layer. It keeps the route and lane state (the lane
draw on the travelled side, the direction sign, the current and next link,
the curve), the ground and water height, the physics and status, the occupants
with the pool caps and reserves, the final ground and collision validation and
the counters. It no longer rolls the rarity dice (the planner's weight
replaced it) or makes the camera decisions, and the reachability test moved to
the planner. An `ESpawnTier` passes in: the near tier creates with
`RANDOM_VEHICLE` and runs the near ring's birth cull and the occupants; the
far tier skips the cull, leaves the car in the far state and skips the
occupants. The driver contract: `SetUpDriver` needs `RANDOM_VEHICLE`, so a
kind that wants occupants creates with the random tag (the far promotion sets
the tag before its own call). The return is an `ESpawnRefusal`, so the service
counts the primitive's refusals by reason.
