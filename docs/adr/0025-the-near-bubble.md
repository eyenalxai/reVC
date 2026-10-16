# ADR 0025: The near bubble

Date: 2026-10-16
Status: Accepted
Relates to: VICE-127 (the implementation), ADR 0013 (the ring table), ADR 0017
(the draw distance is the one range control), ADR 0021 (the fill burst),
VICE-119 (the asks), VICE-120/145 (the ring radii), VICE-126 (the burst)

## Context

The owner's ask 4 of VICE-119: zero visible pop-in. The owner catches the game
creating a car in view, and a car appears from nowhere when the camera turns.
The merged line already holds the parts the fix builds on:

- **The ring table** (`src/control/TrafficRings.h`, VICE-120) is the one table
  of the spawn bands, the despawn thresholds and the camera minimum, with the
  invariant `spawn radius + reserve <= despawn threshold`. Its cap keeps the
  ring inside the visible range and the local cap (VICE-145), and the header
  states the remaining hole: "the stricter blind-spot creation is VICE-127's
  work".
- **The birth cull and the lifetime cull** (`CarCtrl.cpp`) already read the
  same row of the table, so a fresh spawn can never be culled on its frame and
  a car is never removed right before it comes into view. The *removal in view*
  stays: a car beyond the despawn fades out where the player can see it, and
  the stuck and wreck culls delete off-screen cars at 22 m and 6.5 m.
- **The fill burst** (VICE-126) fills the street in a bounded window, and its
  hidden frames create behind a fade; the counters of the burst are live in
  `utils/caps-audit/live-counters.py`.
- **The parked cars** (`CCarGenerator::CheckIfWithinRangeOfAnyPlayers`) appear
  only while the position is off the frustum or occluded - the one path that
  already had a blind-spot rule before this ticket.
- **The pedestrians** keep their own visibility gates (VICE-122).
- **The vehicle fade-in**: every fresh car is created with its clump alpha 0
  and `CVehicle::UpdateClumpAlpha` ramps it to opaque, 16 steps a frame.

What was missing is the rule for the event itself: the traffic spawner places
at the side band and the on-screen band around the camera (`GenerateOneRandomCar`
with `TheCamera.CamFrontXNorm` and the angle limits), and it accepts any
placement that survives the collision checks, whatever the camera will do a
frame later.

## Decision

1. **The near bubble.** A 360-degree disc of `NEAR_BUBBLE_RADIUS` (90 m) around
   the player, view independent: creation inside it is not allowed, removal
   inside it is not allowed. The radius is the car side band (120 m) less the
   placement reserve the despawn already holds (30 m); the fit is a compile-time
   check over the bands the bubble reasons about (the car, the boat, the
   generator) and a matrix check over the whole control of the ring table
   (`utils/near-bubble`). The bubble is the traffic's: the pedestrian bands are
   inside it by the ring table's own numbers and the radii are VICE-120/145's.
2. **The reveal is the fade.** A car whose alpha is 0 holds it until it is
   first on screen (`CVehicle::UpdateClumpAlpha` with `CEntity::GetIsOnScreen`),
   so an off-frustum creation is *invisible* until a camera turn brings it into
   the view, and what the turn reveals is a car forming over 16 frames, never
   an opaque one that appeared from nowhere. In-view creations - the stock
   ring traffic, the VICE-126 burst, the script cars - are on screen at once
   and keep the original ramp; the removal's own `bFadeOut` is untouched; and
   nothing in the game culls or skips a vehicle on alpha 0 (the one read of it
   outside the ramp is the removal's fade-out completion, which requires
   `bFadeOut`). While the fade of a transition covers the screen the ramp runs
   at the hidden step (64, four frames to opaque): the dark head of a fade is
   a quarter of its duration and shorter than the 16 steps of the normal ramp,
   so the mass a fill burst creates is opaque when the screen returns instead
   of forming in view. A car created in the last frames of a fade can finish a
   *few* frames of ramp in view; that is the residual, and it is a fade at the
   ring, not a pop.
3. **The creation rule is written against one predicate, the catchable set,
   and every arm is an arm of one function.** `NearBubblePositionIsCatchable`
   is "in the frustum, inside the fade and the fog of the weather, not
   occluded"; `NearBubbleArmFor` returns the arm; the answer is derived from
   the arm; the counters subdivide by the arm. The arms:
   - **the occluded hint** (`COcclusion::IsPositionOccluded`): the active
     occluder set is built during the render, one frame before the gates read
     it, by projecting the volumes with the *previous* camera. A screen-space
     answer against a turned camera can therefore be wrong, so the hint does
     not allow anything: it selects the position for the **confirmation ray**,
     the exact test. A stale or wrong set can waste a ray or refuse a spawn,
     never allow a car the player can see. The hint is read only while the
     camera still matches the set's view (position, direction, FOV against
     `NEAR_BUBBLE_OCCLUDER_*`: 2 m, a direction cosine of 0.9995, 2 degrees),
     so a turned camera spends no rays on another view's candidates.
   - **beyond the fade/fog** (the ring table's `visibleRange`): the world fades
     and the fog hides there, in or out of the frustum.
   - **the soft arm**: off the frustum *by the margin*
     (`NEAR_BUBBLE_SOFT_MARGIN`, a radius margin, about 16 degrees of bearing
     at the soft distance, so a flick cannot reveal a fresh creation) and at
     least `NEAR_BUBBLE_SOFT_DISTANCE` away. The distance is the side band less
     the widest lane offset of the shipped paths
     (`NEAR_BUBBLE_SIDE_PLACEMENT_SLACK`, 17.5 m: `(4 - 1 + 0.5) x 5`, the
     arithmetic `utils/traffic-rings` derives), so *every* placement of the
     side band sits at or beyond the arm - the first version put the arm on the
     band's ring and left the lane-pulled placements inside it, which is where
     the review's 1.6x at the owner's settings came from. This arm is what
     keeps a street with no occluders alive: the ring's bands sit *inside* the
     visible range by the table's own design, so with the occluders and the fog
     alone an open street has no creation site at all and its traffic drains.
   - **the confirmation ray** (`CWorld::GetIsLineOfSightClear`, buildings
     only): what a blocked line of sight means is that a building stands
     between the camera and the position, so no turn reveals it. The ray is the
     one expensive test and a frame may spend
     `NEAR_BUBBLE_LOS_RAYS_PER_FRAME` (4) of them; a candidate that finds the
     budget spent is refused. The occluded hints and the close blind spot (off
     the frustum by the margin, inside the soft distance) share the budget, so
     the exact test backs both.
   Everything left - in the frustum or within the margin of it, not occluded,
   inside the fade - is refused unless the fade covers the screen. The harness
   proves the binding over the qualitative case list *and* a 576-combination
   sweep of the candidate fields, including the answer's agreement with the arm
   and the margin's hold on the soft arm.
4. **The removal rule.** A car may only be removed when all of this holds: the
   fresh-spawn grace (`NEAR_BUBBLE_GRACE_MS`, 5 s from the creation, held in
   the vehicle's `m_nRemovalProtectedUntil`); the distance beyond the removal
   threshold (`max` of the ring's despawn and the bubble plus its reserve,
   120 m); and the position hidden by an arm that does not rest on stale data -
   off the frustum by the margin, beyond the fade, under a fade, or behind the
   confirmation ray (the occluded hint is confirmed exactly like a creation's,
   so a stale occluder cannot fade a car out in view). A removal spends from
   the same ray budget and, when the budget is spent, simply waits: the car is
   hidden-ish and leaves a frame later.
   - **One clock, one meaning.** `m_nRemovalProtectedUntil` (renamed from
     `m_nSetPieceExtendedRangeTime`) is a "do not remove before this time"
     stamp, honoured in every cull. The set pieces (10-25 s) and the road
     blocks (7 s) already wrote it to keep their cars alive during their scene,
     and the wreck cull always read it; the traffic's fresh-spawn grace is the
     same clock. The behaviour change: those cars are now also protected from
     the distance and stuck culls until their stamp passes, which is what those
     writers asked for.
   - **The rendered bikes and boats.** `CBike::Render` and `CBoat::Render`
     refresh the same clock (`VEHICLE_RENDERED_GRACE_MS`, 3000, named in
     Vehicle.h): the original guard that a vehicle the renderer drew is not
     culled for three seconds. The write is a `Max` now, so a fresh spawn's 5 s
     grace is not shortened by its first render.
   - **The wrecks.** The wreck cull (60 s after death, off-screen, beyond
     6.5 m) also needs the removal distance, so a wreck inside the bubble stays
     until the player moves 120 m away (or it is hidden beyond the fog). The
     estimate: wrecks count in the car budget, so the traffic thins by the
     wrecks the player personally leaves within their own disc - a few dozen at
     the very most, all returned the moment the player drives on - and the
     vehicle pool's valve line (2552 of 2560) stays 152 slots beyond the car
     budget (2400).
   The pool valve (`RemoveCarsIfThePoolGetsFull`) asks the rule for its closest
   candidate and, when nothing passes and the pool is at its last slots, takes
   the closest car at all: a full pool would break the whole traffic. That
   fallback is counted apart (`ms_nNearBubbleValveRemovals`), never in the
   acceptance counter, and the rig's verdict reads it together with the two
   frustum counters.
5. **The fill burst keeps its exit.** The fade of a transition covers the
   screen (`FillBurstIsHidden`, VICE-126) and hides the fill, so a *catchable*
   position under a fade is allowed and counted as fade-exempt
   (`ms_nNearBubbleFadeCreations` / `ms_nNearBubbleFadeRemovals`): the fade row
   is exactly the fade's own work, and the hidden events of a faded frame are
   counted with the hidden ones. The bubble still applies under the fade. The
   burst's mass runs at the hidden ramp (point 2), so it is opaque when the
   screen returns.
6. **The counters.** In `CCarCtrl` (the pattern of the fill burst), live for
   the `/proc` rig of `utils/caps-audit/live-counters.py`:
   - `ms_nNearBubbleCreationsInFrustum`, `ms_nNearBubbleRemovalsInFrustum`: the
     acceptance. A catchable event that the fade did not cover moves one; the
     gate never allows one, so a path that bypasses the gate shows up here.
   - `ms_nNearBubbleFadeCreations` / `ms_nNearBubbleFadeRemovals`: the catchable
     events a fade covered, exempt.
   - `ms_nNearBubbleHiddenCreations` / `ms_nNearBubbleHiddenRemovals`: the
     events the rule allowed outside the catchable set.
   - `ms_nNearBubbleOccluderCreations` / `ms_nNearBubbleOccluderRemovals` and
     `ms_nNearBubbleRayCreations`: the arms those hidden events used (the
     occluded hint, confirmed by the ray, and the close blind spot), so the
     play-test can quantify how much rests on the occluders.
   - `ms_nNearBubbleRefused`, `ms_nNearBubbleBypassed`.
   - `ms_nNearBubbleRays` (the frame, at most the budget) and
     `ms_nNearBubbleRaysTotal`.
   - `ms_nNearBubbleFill`, `ms_nNearBubbleOtherFill` and
     `ms_fNearBubbleTarget`.
   - `ms_nNearBubbleValveRemovals`: the pool valve's own fallback deletions;
     the rig's PASS/FAIL line reads them with the two frustum counters.
7. **The fill counts the spawner's cars; the rest of the budget is the other
   fill.** The car target counts every `RANDOM_VEHICLE`, so the scripts, the
   set pieces, the road blocks, the car parks, the garages, the cheat cars and
   the RC car are part of what the target describes - but they are not the
   traffic the bubble watches. A positive flag (`CVehicle::bIsTrafficCar`, set
   by the spawner's two creation paths) splits the two:
   `ms_nNearBubbleFill` counts the spawner's cars, `ms_nNearBubbleOtherFill`
   the rest, and the orchestrator compares their sum against the target. Their
   *creations* are carried apart as `ms_nNearBubbleOtherCreations` and
   `ms_nNearBubbleOtherCreationsInFrustum`, so a scripted pop is visible in the
   live numbers and never inside the traffic's counters. A car restored from a
   save is classified on load (`CPools::LoadVehiclePool`: a random-class car is
   the traffic's own again), so the loaded traffic does not hide in the other
   fill. The parked cars of the generators are `PARKED_VEHICLE`s and were never
   in either fill. The trains are created once at boot at the origin
   (`CTrain::InitTrains`, from `CGame::Initialise` and the restart/load paths)
   as permanent, locked vehicles: not a traffic event, not counted, and no
   bubble rule applies to a one-time map construction. The planes and the
   helicopter come from the scripts (`COMMAND_CREATE_CAR`), the cheats
   (`VehicleCheat`, counted by the other rows) and the set pieces; there is no
   ambient plane or helicopter spawner in the traffic.
8. **The parked cars** of the generators go through the same creation gate but
   cannot pay a ray per candidate: an empty generator retries every four frames
   while it is in range, and the generators run before the traffic in the frame
   (`CTheCarGenerators::Process`, `Game.cpp`), so their hint arms are off (the
   gate's `rayAllowed` false) and a spot behind the camera that no occluder
   covers waits for the next look. Their own gate stands, the grace protects
   them, and - with the reveal gate (point 2) - a parked car that is created
   off screen fades in when the player first sees it.

## What bounds the work

- The rule's cheap arms are two subtractions (the player distance, the camera
  distance), the occluder test (a handful of screen-space point tests), the
  margin's frustum test and the ring read - no per-triangle work.
- The rays are the only collision-path work: at most 4 a frame, each at most
  the soft distance (102.5 m, three sectors of 50 m), buildings-only, to the
  first hit. The occluded hints and the close blind spot share the budget, so
  the in-view hidden sites are capped by it too - four a frame against the one
  car a second the ring replaces.
- The candidate count behind the rule is bounded by the placement loops of the
  fill rate (6 car calls x 4 placements a frame at the ring cap, 25 attempts on
  a visible burst frame).
- The frame's budget is reset by `GenerateRandomCars`, the earliest gate that
  can spend a ray; the parked-car gate that runs earlier never spends one
  (`rayAllowed` false is tested before the budget is read).
- The fill scan is the vehicle loop `RemoveDistantCars` already makes, and the
  alpha gate is one frustum test per vehicle that is still fading in.

## The scope boundaries, stated

- **The pedestrians** keep their merged rules (the ring table's bands, the
  VICE-122 visibility gates, the VICE-126 burst); the radii are VICE-120/145's.
- **The script, mission, road-block, car-park, garage and cheat cars** are
  carried by the "other" counters instead of the traffic's; their in-view ones
  are expected (a road block is meant to be seen; a cheat car is asked for), so
  that row informs but does not fail the rig's verdict.
- **The top-down camera** bypasses the visibility arms: the street level is not
  the subject of that view. The bubble still holds and the bypassed events are
  counted.
- **A cutscene** needs no bypass: the traffic spawns are gated during one and
  the culls read the cutscene camera.
- **The parked cars on an open boulevard** wait for a hidden frame: the price
  of the ticket is that a car is not seen to appear in view.
- **A car behind the camera on open ground** (the soft arm) is the appearance
  the ticket accepts in place of an empty street: it holds alpha 0 until it is
  seen, it is at or beyond the side band less the lane offset, and the camera
  must turn by the margin before it can be revealed.

## Alternatives considered

- **Trusting the occluder answer as an allow.** It is a frame old, projected
  with the previous camera; at a polygon edge a small turn can falsely say
  "occluded" and the creation would be allowed and counted hidden - the
  acceptance would share the blind spot. Rejected; the hint is confirmed by the
  ray, and the counter row makes the arm measurable.
- **Projecting the candidate with the camera the set was built with.** It makes
  the test self-consistent for the *past* view, but a camera that turned since
  can still have made the position visible; the time difference is the error,
  not the matrix. Rejected in favour of the exact ray.
- **The strict reading (occluders, fog and rays only).** It leaves an open
  street with no creation site (the ring bands are inside the visible range by
  the table's design) and the traffic drains there; Section 6 of
  `utils/near-bubble` models it. Rejected in favour of the soft arm.
- **The soft arm at the side band's ring (120 m).** The lane offset pulls half
  the band's placements inside it, so on open ground they need the ray and the
  effective rate drops (the review's re-derivation: 2.7x stock and 1.6x at the
  owner's, below the harness's 2x line). Rejected; the arm sits the widest lane
  offset inside itself (`NEAR_BUBBLE_SIDE_PLACEMENT_SLACK`).
- **A ray for the parked cars.** The generators run before the traffic and an
  empty one retries every four frames; the rays would starve the traffic.
  Rejected.
- **A ray for every candidate, or a dedicated removal ray.** The collision path
  is the frame's top cost. Rejected; the budget of 4 bounds it.
- **An exemption for the emergency services.** With the soft arm the ambulance
  and the fire truck take the ring's placement behind the camera and drive to
  the scene; no exemption is needed.
- **Moving the spawn band out of the bubble** (the peds included). The radii
  are the ring table's. Rejected.

## Consequences

- **The acceptance.** In a play session the two frustum counters and the valve
  row stay 0 (a creation or a removal the player could catch), the bubble's fill
  (the traffic plus the other fill) is compared against its target live (6 at
  stock, 10 at the owner's settings), and a camera turn reveals only a car
  forming: the soft arm's cars hold alpha 0 until seen, and every hint rests on
  a confirmation ray.
- **The open street: the corrected numbers.** The model of Section 6 of
  `utils/near-bubble` uses the real branch mix (the fast-driving branch sends
  the side band a quarter of its calls; the others a half - the worst is used),
  the margin (its pass share derived from the FOV's half plus the margin's own
  bearing) and the lane-offset allowance. Against the ring's need (0.036 cars a
  frame at stock, 0.121 at the owner's) the conservative rate - no per-call
  placement aggregation, only 70 % of the placements on the soft distance -
  clears it **14.1x** at stock and **7.0x** at the owner's; with the call's four
  placements the same column is **41x** and **23x**. The strict reading is
  0.000. The in-view creation counter stays 0 in every arm.
- **A removal in view waits.** A car beyond the despawn that the player can
  still see keeps driving until it is hidden; a stale occluder cannot remove it
  (the hint is confirmed). The pass of the player ends the wait.
- **The fresh-spawn grace** holds every fresh car for 5 s (and a render no
  longer shortens it); a car the player outran during its grace lives a few
  seconds past the ring, so the counted traffic can sit a little above the
  target at high speed.
- **The occluder arm is exactly as strong as its confirmation.** The hint's
  window (2 m, 1.8 degrees, 2 degrees of FOV) keeps the waste small, and the
  harness prints the whole stale error (a 1.8 degree turn plus the 1.1 degree
  parallax of a 2 m frame at the soft distance) inside the car's own angular
  size (3.4 degrees). The `ms_nNearBubbleOccluderCreations` row tells the
  play-test how much of the fill rested on it.
- **The counters are cumulative and live**: the orchestrator reads them with
  `utils/caps-audit/live-counters.py` before and after the camera turns, at the
  stock and the owner's settings, and compares `ms_nNearBubbleFill` plus
  `ms_nNearBubbleOtherFill` with `ms_fNearBubbleTarget`.

## Amendment: the in-view band returns (2026-10-16, VICE-146)

The first live session after the merge proved the rule above had overshot.
Measured at `fc218652` while the owner played (draw distance 16, first-person
FOV 110, densities 3.4, sunny):

- `NumRandomCars` sat at 103..177 against `GetRandomCarTarget()` 408, stable
  across reads (the pre-merge range was 149..201: the fix's target was never
  reached live).
- The near bubble's `refused` counter ran 2,089 -> 87,170 over minutes, about
  800 refusals a second, while the creations ran about 1.5 a second; the fill
  burst's last window spent 2,825 attempts for 4 placements.
- `visibleRange = min(lodDistance, farClip) = min(4800, ~1985)` at these
  settings, so the "beyond the fade/fog" arm never fires inside the ring.
- Three of the four spawn cases aim the candidate inside the view cone (the
  front bands at +-31/+-45 degrees, `CarCtrl.cpp:319-420`). At FOV 110 the
  frustum covers them, so the near bubble refused them (in the frustum, not
  occluded, inside the visible range). Only the side band and the occluded
  spots remained - far too few for the 408-car target.
- The pedestrian fill burst disarmed at boot with 0 attempts: the boot area
  target is 0 before the zone and the ring are live, and the stop line (0 is
  at 90 % of 0) ended the run before it could fill.

What the live acceptance did hold: the frustum counters stayed 0, the pool
valve stayed 0, the frame read 74 fps (twice) at the owner's settings, and the
collision profile's top symbol fell to 5.85 % (it was 12..21 %).

### The corrected rule

The stock game always had an on-screen band: a fresh car ahead of the view at
or beyond `rings.carOnScreenSpawnMin` (220 m stock, 440 m at the owner's
settings), the line the spawner's on-screen branch aims past
(`carOnScreenSpawnMax`) and the birth cull holds a fresh on-screen car at. The
car's own reveal covers it - the alpha of a fresh car starts at 0 and ramps up
from the first render (`CVehicle::UpdateClumpAlpha`, point 2) - and the band
sits far inside the fog. The near bubble forbade that band, and with it every
legal site of the spawner's front calls on an open street.

The band is restored as the **last** arm of the same one-function ladder, after
the screen fade:

- **In the frustum, beyond the bubble: allowed when `distToCamera >=
  rings.carOnScreenSpawnMin`.** The arm is reached only for a catchable
  position that the screen fade does not already cover. The strict bubble (the
  90 m disc) and every other arm are unchanged: occluded + confirmation ray,
  beyond the fade/fog, off the frustum by the margin (soft arm or a blocked
  ray), and the screen fade (the fill burst).
- **The removal side reads the same arm** (the ladder is one function): a car
  in the in-view band may be despawned as the stock game did, with the fade-out
  the removal path already had; under the band minimum the removal rule is
  unchanged.
- **The counters read the zones apart, with names that carry the meaning.**
  The acceptance pair was renamed from the frustum counters to
  `ms_nNearBubbleCatchableCreations` / `ms_nNearBubbleCatchableRemovals`: they
  now mean the catchable zone - in the frustum nearer than the band minimum -
  and must stay 0. `ms_nNearBubbleBandCreations` / `ms_nNearBubbleBandRemovals`
  count the in-view band's events and rise as the traffic fills (a creation is
  covered by the car's own reveal, a removal by the fade-out of the despawn).
  The scripted cars' in-view row was renamed with them
  (`ms_nNearBubbleOtherCreationsInView`; it keeps the wider catchable set, the
  band included, and informs without failing). The rig prints the zones and the
  PASS line reads the catchable pair.
- **The boot burst waits for its target.** The pedestrian and the car burst no
  longer burn their window on a zero target at boot (the zone, the ring and
  the budget line are not live yet): a frame with `target <= 0` waits in the
  same bounded way the streaming wait uses - it does not spend the window and
  counts against the give-up bound - and the burst runs the moment the area is
  known. The burst counters gain the since-boot totals
  (`ms_nFillBurstTotalAttempts` / `ms_nFillBurstTotalPlaced`, the boot burst
  readable after a later arming reset the window rows) and the end reasons
  (`ms_nFillBurstEndedLine`, `ms_nFillBurstEndedBudget` - the car burst's full
  budget line, apart from the target's, the pedestrian frame having no budget
  line of its own - `EndedStalls` and `EndedWindow`).

The harness (`utils/near-bubble`) gained the band in its case table (in the
frustum at `bandMin - 1` refused, at `bandMin` allowed, the fade still first),
in the exhaustive sweep (the camera distances straddle the band minimum) and in
the open-street model (the on-screen band is a second creation site next to the
soft arm; the conservative rate of the worst branch is ~51x the ring's need at
stock and ~31x at the owner's, where the soft arm alone gave 14.1x and 7.0x).
`utils/burst-fill` gained the boot-wait case and the end-reason checks.

### Expected live numbers

`NumRandomCars` climbs toward the 408 target within about two minutes at the
owner's settings; the catchable-zone counters stay 0; the band counters rise;
the burst's since-boot totals and end reasons are readable after an interior
exit or a cutscene end reset the window rows.
