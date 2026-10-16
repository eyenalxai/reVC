# ADR 0026: The far traffic layer

Date: 2026-10-16
Status: Accepted
Relates to: VICE-128 (the implementation), VICE-119 (the asks), VICE-120/145
(the ring radii), VICE-123 (the caps), VICE-126 (the fill burst), VICE-127 (the
near bubble), ADR 0009 (the count follows the ring's area), ADR 0013 (the ring
table), ADR 0017 (the draw distance is the one range control), ADR 0020 (the
caps), ADR 0024 (the path-node grid the placements run on), ADR 0025 (the near
bubble)

## Context

The owner's ask 9 of VICE-119: a long sight line must show traffic all the way
to the fog. The merged line bounds the near traffic at the ring's own despawn
(`rings.carDespawn`: 280 m at stock, 560 m at the local cap of VICE-145), and
VICE-127's near bubble (ADR 0025) keeps every near creation out of view. The
draw distance, on the other hand, draws the world to `min(LOD distance, weather
far clip)` - at the owner's settings a two kilometre fog - so on a straight
boulevard the road is bare from the ring's edge to the horizon, and the near
traffic that does drive out of the ring is deleted (hidden, correctly) rather
than seen to recede.

Three facts bound any change here:

- **The car budget is the near traffic's.** `MAX_CARS_IN_USE` (2400) counts
  every car the game simulates; a whole second population of simulated cars
  would either starve the near traffic or overflow the pool (NUMVEHICLES is
  2560, and the cars outside the budget - the planes, the trains, the
  helicopter, the set pieces, the shortcut car - live in the same 160 slots of
  margin).
- **The near bubble's rule is the creation and removal rule of the game now.**
  Anything the player can catch appearing or disappearing is a regression
  (VICE-127), whatever the module.
- **A car that is rendered is not cheap, but a car that is simulated is much
  less cheap.** A pool vehicle with a streamed model and the ordinary render
  path costs its geometry and its LOD; the same car with `STATUS_PHYSICS`, a
  driver and the AI costs the control, the physics, the wheel work, the
  collision scans of everyone else (the near AI brakes for a car it can see),
  the ped pool slot of its driver and the audio entity of an engine.

## Decision

1. **The representation: a pool vehicle in a far state.** A far car is a real
   `CVehicle` of the pool, created by the near spawner's own function
   (`CCarCtrl::CreateCarFromPlacement` in its far mode), so it is rendered by
   the ordinary vehicle path - the streamed model, the LODs, the shadow, the
   alpha reveal of VICE-127 - with no second renderer to maintain. What makes
   it *far* is one state, applied in one place
   (`CFarTraffic::ApplyFarState`):
   - `VehicleCreatedBy = FAR_TRAFFIC_VEHICLE` (a new value of the existing
     enum): the car is outside every budget (`CCarCtrl::UpdateCarCount` ignores
     it), it cannot be saved (only mission cars are, `CPools::IsVehicleSaved`)
     and it is removable (`CanBeDeleted` returns true: the layer gives its own
     cars up and a cutscene's cleanup may take them).
   - static and off the moving entity list: `CWorld::Process` never runs its
     control, so there is no car AI, no physics, no wheel work and no damage.
   - `bUsesCollision = false`: `CPhysical::ProcessCollisionSectorList` skips it
     as a body, and the near AI's own scans
     (`SlowCarDownForCarsSectorList`, the weave scans) test the same flag, so
     the player cannot touch a far car and the near traffic cannot even brake
     for one.
   - the audio entity inactive: `cAudioManager::ProcessEntity` skips it, so no
     engine sound of a car kilometres away is serviced.
   - no driver: the car holds no ped of the pool while it is far.
   The layer moves the car itself, along the path nodes, with the curve
   arithmetic the on-rails simple car uses - the pose core is shared
   (`CCarCtrl::ComputeCarOnRailPose`, extracted from `UpdateCarOnRails`) - and
   the car's height comes from the path nodes' own coordinates. The far layer
   therefore touches **no collision query, no sector scan, no path search and
   no physics**: its whole per-frame work is arithmetic on the cars' own
   matrices plus the creation attempts.
2. **The band is the annulus between the near traffic and the fog.**
   `inner = rings.carDespawn`, `outer = rings.visibleRange`, both from the one
   ring table (VICE-120/145), so the layer follows the draw distance slider and
   the weather for free. The band is *open* only while it has width
   (`FAR_TRAFFIC_MIN_BAND`, 160 m: two of the ring's own reserve steps). At the
   stock draw distance the ring ends 80 m inside the visible range and the band
   stays closed - the near traffic already ends in the fog and a far car would
   be an 80 m shell of churn; the layer appears with the draw distance, which
   is exactly the owner's long-road case. When the weather closes the band (a
   storm's far clip below the despawn) the layer empties, and the fog hides
   every removal of the drain.
3. **The cap is a module constant, scaled by the band's area.** The hard top is
   `FAR_TRAFFIC_MAX` (64) over `FAR_TRAFFIC_REF_AREA` (4 km2, the band of a two
   kilometre draw distance), floored at `FAR_TRAFFIC_MIN_CARS` (8) for the
   first band that opens: about sixty cars at the owner's settings. The cap
   follows the band the way the traffic target follows the ring (ADR 0009), so
   the count per square metre of drawn world is roughly constant and a thin
   band is never a jam. It is a setting constant of the module, not a slider;
   the measurement in `utils/far-traffic` and the live counters are how it is
   tuned (the arithmetic of the whole layer at the hard top is under a
   microsecond a frame on the development machine, so the bound that matters is
   the pool margin below, not the update).
   **The pool accounting, stated:** `MAX_CARS_IN_USE (2400) +
   FAR_TRAFFIC_MAX (64) <= NUMVEHICLES (2560)` and
   `2 x FAR_TRAFFIC_MAX <= NUMVEHICLES - MAX_CARS_IN_USE` (both compile-time
   asserts), so the layer can never take the slot of a car the budget counts and
   the cars outside the budget (the planes, the trains, the helicopter, the set
   pieces, the shortcut car) keep at least as much of the margin as the layer
   takes - the remaining 96 slots against the layer's 64. A creation
   additionally refuses while the vehicle pool is down to
   `FAR_TRAFFIC_POOL_RESERVE` (16) free slots or the ped pool to
   `PED_POOL_RESERVE` (332, the near spawner's own reserve), and a promotion's
   driver is not created while the ped pool cannot hold it (the promotion
   happens without one; see Decision 6). **No path of the layer asserts**:
   every refusal is a counter (`ms_nFarTrafficRefused`, `...RefusedModel`,
   `...RefusedPool`).
4. **The events go through the near bubble's rule, not beside it.** A far
   creation is a creation: it is built by the near spawner's function, which
   asks `CCarCtrl::NearBubbleAllowsCreation` with the final position and, when
   it is allowed, moves the acceptance counters
   (`NearBubbleCountCreation`). The layer's own sites are the two the bubble
   leaves on an open road:
   - **the fog site**: `CPathFind::CollectCarCreationCoors` at
     `outer + FAR_TRAFFIC_FADE_MARGIN` (50 m), so the placement's own segment
     brackets behind the fade (the bubble's `NEAR_BUBBLE_ARM_FADE_DISTANCE` is
     `distToCamera >= visibleRange`; the margin also covers the segment length
     and the camera's offset from the player centre). This is the oncoming
     traffic of a long straight road: a car forms out of the haze.
   - **the behind site**: the same placement call at `inner`, keeping the rear
     half of the camera (a direction cosine of 0, where the near spawner's side
     band keeps 0.707). The bubble's soft arm - off the frustum by the margin,
     at or beyond the soft distance (102.5 m) - allows it with no ray.

   The layer passes `rayAllowed = false`: a far site is two kilometres out, and
   a line-of-sight ray of that length is the wrong work for a position the fog
   or the camera's back already hides. The price is exact and conservative: an
   occluded far site is refused, and the counts that bypass the gate cannot
   pass unnoticed because the counters are the bubble's own.
5. **Removals ask the bubble's removal rule, and the layer pays no ray for
   them either.** `FarTrafficRemoveCar` calls
   `CCarCtrl::NearBubbleAllowsRemoval` - the same gate the near cull uses (the
   distance beyond the bubble and its reserve, the fresh-spawn grace, and a
   hidden arm: off the frustum by the margin, beyond the fade, under a fade) -
   with the gate's ray option false (the gate grew the option for the layer,
   `NearBubbleAllowsRemoval(..., bool rayAllowed = true)`; the near paths are
   unchanged and keep their ray). The removal arms of the layer are therefore
   the fog's and the camera's back; an occluded hint is refused, not confirmed,
   which costs a frame of waiting and never a two-kilometre ray.
   The frame's bound counts the *gates the layer asks*, not the cars it
   removes: `removalGates` is spent by every answer, a refusal included, so one
   frame cannot ask more than `FAR_TRAFFIC_REMOVE_ATTEMPTS` (2) answers and
   cannot touch the near traffic's ray budget at all. A car that must go and
   does not get its answer waits for the next frame (a car beyond the fog is
   hidden-ish and leaves a frame later, exactly as the near cull's own removals
   do). The removals the layer makes on its own are the band's own (beyond
   `removeOuter = createOuter + FAR_TRAFFIC_OUTER_HYSTERESIS` (100 m), the
   closed band, or a cap that dropped), and those are beyond the fog.
6. **The switches: one home each, and the honest list of what they write.**
   - **Promotion** (`FarTrafficPromoteCar`): a far car that reaches `inner`
     becomes traffic again - the class back to `RANDOM_VEHICLE` and the car
     budget count with it (`CCarCtrl::UpdateCarCount`), `SetUpDriver` from the
     population's own path, collision, the moving list and the audio
     (`CFarTraffic::ApplyTrafficState`). The car's own facts - the matrix, the
     model, the colours, the status (`STATUS_SIMPLE` on both sides), the clump
     and its alpha - are untouched, so no frame can show the car change. The
     gate pass checks every car every frame (not only the staggered share), so
     a car sits inside the band for one frame at most. The promotion distance
     is the near traffic's own despawn: the engine's floor for it is
     `RING_CAR_DESPAWN` (280 m), and even the harness's below-floor stress cell
     (a 70/110 FOV factor, which the engine clamps away) puts it at 189 m - both
     outside the reach (120 m) with room to spare, and the harness checks the
     rule over the whole control matrix of the ring table.
     **The two facts a promotion does write that the player could see**: the
     driver ped (created by `SetUpDriver`; the engine's own in-car rule draws it
     only inside `25 x TheCamera.LODDistMultiplier` metres and on screen,
     `CPed::Render`) and the audio entity (switched on; the engine sound of a
     car at the band's edge is a distance-culled sample, not a new source).
     The driver is held to the car's own reveal: when the car still holds its
     invisible frame (alpha 0, the VICE-127 soft-arm reveal), the layer clears
     the driver's `bRenderPedInCar` and sets `CVehicle::bDriverWaitsForReveal`;
     `CVehicle::UpdateClumpAlpha` releases it at the car's last fade step, so a
     promotion of an unseen car cannot show a driver before its car. A promotion
     of a car the player is already watching adds a few pixels of a driver
     inside an opaque car at 280 m or more. The harness carries the fact list
     with this one exception (one written-and-drawn fact, held).
   - **The reach is sealed** (the safety promotion): when the ped pool is short
     of `PED_POOL_RESERVE`, the switch cannot give the car a driver. Outside the
     player's reach (`NEAR_BUBBLE_REMOVE_DISTANCE`, the bubble plus its
     reserve) the car stays far and waits for the pool - a far car is a shell
     the player cannot touch from there. Inside the reach it is made real
     anyway, without a driver (`FAR_TRAFFIC_SAFETY_PROMOTE`): collision, the
     moving list, the audio, the budget - a driverless traffic car that the
     near cull will clear like any other car. The layer never leaves a car the
     player could drive through, and the acceptance gauge
     `ms_nFarTrafficInReach` (the far cars inside the reach after a frame) must
     read 0; `ms_nFarTrafficSafetyPromotions` and `ms_nFarTrafficRefusedPool`
     record how often the pool forced one.
   - **Hand-over** (`CFarTraffic::AdoptOnCull`, the demotion): the near cull
     (`CCarCtrl::PossiblyRemoveVehicle`) has already decided a traffic car is
     outside the bubble, past its grace and hidden, and is about to delete it;
     the layer takes it instead when the band and the cap want it - the
     occupants are flagged away (the ped is removed on the next
     `CWorld::Process`, while the car is already a viewed shell), the budget
     count is given back, the class becomes `FAR_TRAFFIC_VEHICLE` and the far
     state is applied. The player watches a car recede into the fog instead of
     blinking out, and the switch is a blind-spot event by the near bubble's own
     rule, because the cull asked that rule first. The layer takes only the
     spawner's own cars (`RANDOM_VEHICLE` and `bIsTrafficCar`), never a mission
     or a script car, never a boat or a set piece (`bExtendedRange`), and it
     starts `FAR_TRAFFIC_ADOPT_HYSTERESIS` (40 m) past the despawn, so a car
     sitting on the band's edge is not handed over the frame it is culled.
7. **The stagger.** One frame moves the share of the layer whose index falls on
   the frame's step (`FAR_TRAFFIC_STAGGER`, 3: about 10 Hz at the game's 30 Hz,
   the rate the ticket names). The curve of a car is a function of the wall
   clock (`m_nTimeEnteredCurve + m_nTimeToSpendOnCurrentCurve`), so a car moved
   a third as often still travels at its speed; the alpha reveal of a creation
   runs on the same update, which means a far car that is created out of sight
   holds its invisible frame until the player can see it and then fades in over
   sixteen updates (1.6 s) - the VICE-127 reveal at the layer's own rate.
   A frame makes at most `FAR_TRAFFIC_CREATE_ATTEMPTS` (2) creation attempts,
   and only while the layer is under its cap: the fill of a fresh band takes
   about a second and a half, and a full layer costs no placement scan at all.
8. **The model residency rule.** A far car is built only with a model the
   streamer holds (`CStreaming::HasModelLoaded`), and the model comes from the
   population's own loaded class arrays (`CCarCtrl::ChooseCarRating` and
   `ChooseCarModel`, the zone's civil ratings): the far layer never spends a
   streaming slot of its own, never asks for a model the near traffic of the
   area would not ask for, and never places a car it cannot draw. A refusal is
   `ms_nFarTrafficRefusedModel`. The layer's model choice deliberately skips the
   cop and gang branches of `CCarCtrl::ChooseModel`: a far car is a civilian
   car, and a police car promoted at the band's edge would need the law
   enforcer's own setup (`CCarAI::AddPoliceCarOccupants`) that the layer does
   not do.
9. **The frame's order.** `CFarTraffic::Process` runs last in the car block of
   `CGame::Process`, after `CCarCtrl::GenerateRandomCars`, the road blocks,
   `RemoveDistantCars` and the pool valve: the near spawner has had its say
   about the pool and the ped reserve, the culls have made this frame's
   hand-overs (which the layer receives inside that scan), and the bubble's ray
   budget of the frame has been spent by the near traffic's own creations. The
   layer's creations then cannot starve the near frame of anything. The layer
   pauses in a cutscene and an interior (the near spawner's own gates), pruning
   its list so the counters stay live; a cutscene's cleanup itself deletes far
   cars (`CanBeDeleted` is true) and the layer rebuilds after it.
10. **Randomness, stated.** The layer draws from the one global stream: the
    zone's model choice, the placements' reservoir sampling and the path picks
    of the cars (`CCarCtrl::PickNextNodeRandomly`, which the near AI uses too).
    Nothing reseeds, so the shift is the same documented one the fill burst
    makes (ADR 0021); the car AI's own route seed keeps the cars that pick
    later unshifted.
11. **The counters** (live in `utils/caps-audit/live-counters.py`):
    `ms_nFarTrafficCars` and `ms_nFarTrafficCap` (the live pair: a cap of zero
    is a closed band, not a fault), `ms_nFarTrafficCreations`,
    `ms_nFarTrafficRemovals`, `ms_nFarTrafficPromotions`,
    `ms_nFarTrafficSafetyPromotions` (the driverless reach seals),
    `ms_nFarTrafficDemotions` (the hand-overs), `ms_nFarTrafficInReach` (the
    acceptance gauge, must stay 0), the refusals by cause (`Refused`,
    `RefusedModel`, `RefusedPool`), `ms_nFarTrafficAttempts` and
    `ms_nFarTrafficUpdates` (the stagger's work).

## The reading of "hidden" for a switch

The ticket says the two switches happen hidden. Under VICE-127's rule that
phrase has one meaning for the *events* (creation and removal: a blind spot,
checked by the ladder) and a narrower one for the *switches*: the car the switch
touches is already there, and what the player could catch is a *change* to it.
The switch therefore keeps every fact the renderer reads of the car - the
matrix, the model, the colours, the status, the clump and its alpha, the
visibility flag - and the only drawn fact it writes is the promotion's driver,
which the layer holds to the car's own reveal (`bDriverWaitsForReveal`, released
by `UpdateClumpAlpha`), so a driver cannot appear before its car and only
appears at all inside the engine's own in-car draw range
(`25 x LODDistMultiplier`, far inside the bubble at the stock settings). The
two readings meet at the same place: the promotion happens at the band's inner
edge (>= 280 m out by the engine's own floor, 189 m even under the harness's
below-floor stress cell - outside the bubble and its reserve either way), and
the hand-over happens only where the near cull's own blind-spot rule already
allowed a removal. The evidence program carries both: the sites through the
bubble's ladder, and the switch's fact list (7 written of 13, 1 drawn and held).

## What bounds the work

The numbers below are the harness's, quoted once (its Section 5 print; the
arithmetic varies a few per cent run to run). The first three are a **lower
bound** of the layer's work: they are the mirrored arithmetic, and the engine
calls each update pays - the node pick's branch, the alpha ramp's on-screen
test, the sector re-insert (`RemoveAndAdd`) and the RW-frame write - are not in
them. The engine-side gates and the car build are not linked in the harness at
all; the live frame read and the owner's play-test are their measurement.

- The gate pass is a distance and a switch per car, every frame: 4 ns a car
  (0.2 us at the hard top of 64).
- The staggered movement is the rail pose of a car - the lane offsets, the
  curve, the heading, the path's own height, the matrix write - 20-25 ns a car,
  so the frame's share (a third of ~58 at the owner's settings) is 0.4-0.6 us;
  the layer's whole own arithmetic is 0.6-0.7 us a frame, under 0.002 % of a
  33 ms frame. It touches no collision query: the near rail path's ground cast
  (`DragCarToPoint`) is not part of it.
- The creation attempts are the near spawner's own work: the node scan of
  `CollectCarCreationCoors` at the far radii (a walk of the ~3,351 car nodes -
  ADR 0024's count - with the per-link bracket test) and a car build. The
  harness mirrors that scan over a synthetic graph of the shipped counts: about
  12 us for the fog site's whole-graph radius and 7 us for the behind site's, so
  both sites of an attempt and both attempts of a filling frame are about
  39 us. The mirror is not the engine call (the water-path branch, the accepted
  pairs' interpolation, the reservoir shuffle and the car build's object
  queries are not in it); it says the right order of magnitude, and the layer
  makes no attempt at all while it is full. The live frame read decides the
  rest.
- The streams (`RemoveDistantCars`, the pool valve) skip a far car on one
  compare; the layer's list is at most 64 entries, validated against the pool
  slot before use.
- The removals ask at most two gate answers a frame and never a ray (Decision
  5), so the near traffic's four-ray budget is untouched.
- A promotion creates one driver ped (one `CPopulation::AddPedInCar`), only
  while the ped pool holds `PED_POOL_RESERVE`; when it does not, the reach's
  seal makes the car real without the driver.

## The scope boundaries, stated

- **Missions and scripted vehicles** are untouched: the layer creates its own
  cars and hands over only the spawner's own traffic cars the cull was deleting.
  A mission car, a set piece, a parked car and the player's car are never
  adopted.
- **The boats** are not the layer's: a water path placement is skipped and
  `bExtendedRange` cars are refused at the hand-over; the water rings are the
  ring table's own.
- **The pedestrians** are untouched.
- **The near bubble and the ring's radii** are VICE-127/120/145's; the layer
  reads them and asks their gates.
- **The render cost of a far car** is the ordinary distant-vehicle cost. The
  layer's cap bounds it, and the cars are inside the draw distance the player
  chose, which is what the LOD system is for; the layer adds no renderer path.
- **The overlap seam.** A near car that is still visible past the despawn waits
  for a hiding frame (VICE-127), so a promoted or adopted car can share a road
  with near traffic inside the band's inner edge for a moment; the placement
  checks (`FindObjectsKindaColliding` in the near spawner's function) keep
  creations apart, and the far cars are collision-invisible to the near AI, so
  the visible residue is a near car passing through a far one at a few hundred
  metres, briefly, in the band's innermost metres. It is accepted, not hidden.

## Alternatives considered

- **A separate light object with its own renderer.** It would use no pool slot,
  but it would need its own clump, LOD, shadow, alpha and streaming handling -
  a second vehicle renderer to keep in step with the first, and a promotion
  that re-creates the car (a visible switch). Rejected; the pool vehicle in a
  far state reuses everything, and the switches change state only - the one
  drawn fact a promotion writes, the driver, rides the car's own reveal.
- **`PERMANENT_VEHICLE` for the far class.** It is already outside every budget,
  but `CanBeDeleted` refuses it and the save/load paths treat it as the planes
  and the trains; a new enum value is the honest one.
- **Counting far cars in the car budget.** They would eat the near traffic's
  target (`GetRandomCarTarget`, `NumCarsInUse`) - the layer would trade the
  street it fills the distance of for the street at the player's feet.
  Rejected; the pool margin carries them instead.
- **A ray for the far sites or removals.** A two kilometre line-of-sight ray per
  attempt or per removal, against the near traffic's four rays a frame.
  Rejected; the fog and the camera's back are the layer's sites, the occluder
  arm waits, and the removal gate grew a ray option so the layer can refuse it
  without touching the near paths' behaviour.
- **A constant cap of 64.** It jams a thin band (an 80 m shell of churn at a
  barely open draw distance). Rejected in favour of the band-area scaling with
  the constant as the top.
- **Promotion far out (the fog) or by a timer.** A promoted car is simulated
  traffic with a driver and a budget slot; doing that at the fog pays the near
  traffic's price for a car no one can reach. Rejected; the switch is at the
  near band's edge.
- **A promotion that refuses and waits when the pool has no driver.** It was the
  first version and it is a ghost path: the car stays collision-off while the
  player walks up to it (the removal gate refuses to delete a visible car, so
  the layer cannot tidy up in view either). Rejected; the reach's seal promotes
  the car without a driver instead (Decision 6).
- **Promotion only while the car is hidden.** Then the oncoming cars of an open
  road would never promote while the player watches them and would arrive as
  ghosts (or as a driverless car appearing at the reach). Rejected; the driver
  is held to the car's own reveal instead, which makes a promotion of an unseen
  car independent of the moment it happens.
- **Demoting near cars into the layer everywhere.** The near cull uses the
  hand-over at its own removal site (hidden, past the hysteresis); adopting
  cars that are still inside the ring would double-handle them (the near AI and
  the layer both moving one car). Rejected.
- **Moving the far cars every frame.** The rail pose of sixty cars is cheap,
  but the alpha reveal and the node picks multiply with it, and the ticket's own
  rate is 10-15 Hz: at a few hundred metres a car's step of half a metre a
  frame is invisible. Rejected in favour of the stagger of three.

## Consequences

- **A long straight road shows traffic to the fog**: oncoming cars form out of
  the haze at the visible range and drive in, and the cars the player passes
  recede into it behind at the near band's edge (the hand-over) instead of
  blinking out. At the stock draw distance the band is closed and nothing
  changes.
- **The pool cannot overflow**: the budget, the far cap and the reserve are an
  arithmetic of constants with two compile-time asserts, and every refusal is
  counted. A full far layer costs the near traffic nothing of its target, and
  the cars outside the budget keep at least as much of the pool's margin as the
  layer takes.
- **A far car is never reachable**: the promotion happens at the near traffic's
  own despawn (280 m by the engine's floor, 189 m under the harness's
  below-floor stress cell), checked over the whole control matrix; and a car the
  pool cannot give a driver is made real without one inside the player's reach
  (the reach's seal, `FAR_TRAFFIC_SAFETY_PROMOTE`). The gauge
  `ms_nFarTrafficInReach` reads the end state of every frame and must stay 0.
- **The switches cannot be seen, with one fact held back**: the car's own facts
  are untouched by every switch, and the one drawn fact a promotion writes - the
  driver - is held to the car's own reveal and drawn by the engine only inside
  its own in-car range. The hand-over is a blind-spot event by the near bubble's
  rule.
- **The frame cost is bounded and measured**: the layer's own arithmetic (a
  lower bound; the engine calls each update pays are not in the harness's
  number) is 0.6-0.7 us a frame at the owner's settings - under 0.002 % of a
  33 ms frame - the creations are bounded at two attempts a frame (about 39 us a
  frame when the mirrored scans are counted, and nothing at all while the layer
  is full), and the removals at two ray-free gate answers a frame. What remains
  to be read live is the layer's effect on the frame at the owner's settings
  (the counters and the frame recipe of the ticket), and the owner's own note on
  the long roads - the play-test list in the ticket's comment.
- **The random stream shifts** as the burst's does (Decision 10); it is a
  documented, measurable price, not a hidden one.
- **What the evidence cannot reach**: the harness reads the rules and mirrors
  the arithmetic; it cannot exercise the engine-side gates
  (`NearBubbleAllowsCreation/Removal`'s occlusion, fade and ray budget), the
  real node graph of the placement scan, the car build or the promotions'
  drivers. Those are the live rig's counters and the owner's frame read.
