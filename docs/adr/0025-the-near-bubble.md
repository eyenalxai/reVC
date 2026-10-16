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
- **The pedestrians** keep their own visibility gates (VICE-122): a spawn
  position is taken only if `TheCamera.IsSphereVisible` says what the band
  wants, so an off-screen position is never created in view *at the moment of
  creation*.

What was missing is the rule for the event itself: the traffic spawner places
at the side band and the on-screen band around the camera (`GenerateOneRandomCar`
with `TheCamera.CamFrontXNorm` and the angle limits), and it accepts any
placement that survives the collision checks, whatever the camera will do a
frame later. The owner turning the camera finds the car that was created behind
them.

## Decision

1. **The near bubble.** A 360-degree disc of `NEAR_BUBBLE_RADIUS` (90 m) around
   the player, view independent: creation inside it is not allowed, removal
   inside it is not allowed. The radius is the car side band (120 m) less the
   placement reserve the despawn already holds (30 m), so no placement of the
   band lands inside the bubble, and the fit is a compile-time check over every
   band the bubble reasons about (the car, the boat, the generator) and a
   matrix check over the whole control of the ring table
   (`utils/near-bubble`). The bubble is the traffic's: the pedestrian bands
   (the off-screen band is 35..65 m) are inside it by the ring table's own
   numbers, and this ticket may not move a ring radius (VICE-120/145).
2. **The creation rule is written against one predicate, the catchable set.**
   `NearBubblePositionIsCatchable` (NearBubble.h) is "in the frustum, inside
   the fade and the fog of the weather, not occluded": the positions the player
   could catch an event at. The ladder allows an event only outside that set, or
   under the fade that covers the screen, and every arm is a statement about the
   view:
   - **occluded** (`COcclusion::IsPositionOccluded`): an occluder volume covers
     the position, so no turn of the camera reveals it. **The occluder set is a
     frame old**: `COcclusion::ProcessBeforeRendering` builds it during the
     render (`Renderer.cpp`), after the car block of `Game.cpp` the gates run
     in, so `IsPositionOccluded` tests the *previous* frame's screen-space
     volumes against the *current* camera. A wrong "occluded" is exactly the
     pop the counters could not see. The answer is therefore trusted only while
     the camera still matches the view the set was built for
     (`COcclusion::ms_vecProcessedCamera*`, written by the render; the near
     bubble compares the position, the direction and the FOV against
     `NEAR_BUBBLE_OCCLUDER_*`: 5 m, a direction cosine of 0.99, 5 degrees). A
     stale set is ignored and the other arms decide - a refused spawn, not a
     pop. A boot frame (no set) answers "not occluded", the conservative
     direction, because the counters read the same folded field: a position the
     folded field calls not-occluded that is nevertheless created moves the
     acceptance counter.
   - **beyond the fade/fog** (the ring table's `visibleRange` =
     min(LOD distance, weather far clip)): the world fades and the fog hides
     there, in or out of the frustum, so the ring's on-screen band keeps
     working exactly where the table always said it does (TrafficRings.h).
   - **the soft arm**: off the frustum and at least `NEAR_BUBBLE_SOFT_DISTANCE`
     (120 m, the bubble plus its reserve) away. This is the traffic behind the
     camera, and it is the arm that keeps a street with no occluders alive:
     the ring's bands sit *inside* the visible range by the table's own design
     (the visible cap keeps them one reserve inside it), so with the occluders
     and the fog alone an open street has no creation site at all and its
     traffic drains (VICE-127 review; the model in `utils/near-bubble`
     Section 6 shows the strict reading at 0 creations a frame against a need
     of 0.036 at stock and 0.121 at the owner's settings). A car created here
     starts at alpha 0 - the game fades every fresh car in - and only a turn of
     the camera can reveal it.
   - **the close ray**: off the frustum and *inside* the soft distance, a
     blocked line of sight (`CWorld::GetIsLineOfSightClear`, buildings only)
     allows the creation. The ray is the one expensive test and a frame may
     spend `NEAR_BUBBLE_LOS_RAYS_PER_FRAME` (4) of them; a candidate that finds
     the budget spent is refused.
   Everything left - in the frustum, inside the fade, not occluded - is refused
   unless the fade covers the screen. A refusal costs one ladder and the
   placement loop of the caller moves to its next candidate. The harness proves
   the binding over the whole qualitative case list *and* an exhaustive sweep of
   the candidate fields (288 combinations): the ladder and the counters share
   the predicate, so a future edit that drops an arm or moves a condition fails
   the sweep.
3. **The removal rule.** A car may only be removed when all of this holds: the
   fresh-spawn grace (`NEAR_BUBBLE_GRACE_MS`, 5 s from the creation, held in
   the vehicle's `m_nRemovalProtectedUntil`); the distance beyond the removal
   threshold (`max` of the ring's despawn and the bubble plus its reserve,
   120 m) - the despawn already sits the reserve past the spawn band, the
   hysteresis of the table; and hidden (outside the catchable set: off the
   frustum, occluded with a current set, or beyond the fade). A removal spends
   no ray: it is the game's soft event (the car fades out over 16 frames), and
   the cheap arms decide. A car the player can see keeps driving; the game fades
   it out only where a turn cannot reveal the change.
   - **One clock, one meaning.** `m_nRemovalProtectedUntil` (renamed from
     `m_nSetPieceExtendedRangeTime`) is a "do not remove before this time"
     stamp, and the removal rule honours it in every cull. The set pieces
     (10-25 s) and the road blocks (7 s) already wrote it to keep their cars
     alive during their scene, and the wreck cull always read it; the traffic's
     fresh-spawn grace is the same clock. The behaviour change this makes: a
     set-piece or road-block car is now also protected from the distance and
     stuck culls until its stamp passes, which is what those writers asked for -
     their cars are meant to hold while the scene plays. It is documented here
     because the previous code only read the clock in the wreck rule.
   - **The rendered bikes and boats.** `CBike::Render` and `CBoat::Render`
     refresh the same clock (`VEHICLE_RENDERED_GRACE_MS`, 3000, named in
     Vehicle.h for the two): the original guard that a vehicle the renderer
     drew (the player can see it) is not culled for three seconds. The write is
     a `Max` now, so a fresh spawn's 5 s grace is not shortened by its first
     render, and a rendered bike or boat can never be removed inside the 3 s its
     render asks for.
   - **The wrecks.** The wreck cull (60 s after death, off-screen, beyond
     6.5 m) now also needs the removal distance, so a wreck inside the bubble
     stays until the player moves 120 m away (or it is hidden beyond the fog).
     The estimate: wrecks count in the car budget like any random car, so the
     traffic thins by the wrecks the player personally leaves within their own
     disc - a few dozen at the very most, all returned the moment the player
     drives on - and the vehicle pool's valve line (2552 of 2560) stays 152
     slots beyond the car budget (2400), so the accumulation cannot reach it.
   The pool valve (`RemoveCarsIfThePoolGetsFull`) asks the rule for its closest
   candidate and, when nothing passes and the pool is at its last slots, takes
   the closest car at all: a full pool would break the whole traffic. That
   fallback is counted apart (`ms_nNearBubbleValveRemovals`), never in the
   acceptance counter, so an emergency the player can see is visible in the
   live numbers instead of hiding inside the culls'.
4. **The fill burst keeps its exit.** The fade of a transition covers the
   screen (`FillBurstIsHidden`, VICE-126) and hides the fill, so a *catchable*
   position under a fade is allowed and counted as fade-exempt
   (`ms_nNearBubbleFadeCreations` / `ms_nNearBubbleFadeRemovals`): the counters
   subdivide, so the fade row is exactly the fade's own work and the acceptance
   (the frustum rows stay zero) reads cleanly. The hidden events of a faded
   frame are counted with the hidden ones. The bubble still applies under the
   fade: a car placed at the player's elbow is there when the fade lifts.
5. **The counters.** In `CCarCtrl` (the pattern of the fill burst), live for
   the `/proc` rig of `utils/caps-audit/live-counters.py`:
   - `ms_nNearBubbleCreationsInFrustum` and `ms_nNearBubbleRemovalsInFrustum`:
     the acceptance. A catchable event that the fade did not cover moves one;
     the gate never allows one, so a path that bypasses the gate shows up here.
     Both must stay 0 in a play session.
   - `ms_nNearBubbleFadeCreations` / `ms_nNearBubbleFadeRemovals`: the catchable
     events a fade covered, exempt.
   - `ms_nNearBubbleHiddenCreations` / `ms_nNearBubbleHiddenRemovals`: the
     events the rule allowed outside the catchable set - its work, so a silent
     rule (one that refuses everything) is visible too.
   - `ms_nNearBubbleRefused`: the events the rule stopped.
   - `ms_nNearBubbleBypassed`: the events the top-down camera's view allowed.
   - `ms_nNearBubbleRays`: the rays of the current frame (reset by
     `NearBubbleBeginFrame`, so it never exceeds the budget) and
     `ms_nNearBubbleRaysTotal`, the same since the counters were last zeroed.
   - `ms_nNearBubbleFill` and `ms_fNearBubbleTarget`: the traffic (the cars the
     spawner created) inside the bubble and the share of the car target its
     area asks for (`target x (90 / despawn)^2`: 6 at stock, 10 at the owner's
     settings).
6. **The fill counts the spawner's cars; the rest of the budget is the other
   fill.** The car target counts every `RANDOM_VEHICLE`, so the scripts, the
   set pieces, the road blocks, the car parks, the garages, the cheat cars and
   the RC car are part of what the target describes - but they are not the
   traffic the bubble watches. A positive flag (`CVehicle::bIsTrafficCar`, set
   by the spawner's two creation paths) splits the two: `ms_nNearBubbleFill`
   counts the spawner's cars, `ms_nNearBubbleOtherFill` the rest, and the
   orchestrator compares their sum against the target. Their *creations* are
   carried apart as `ms_nNearBubbleOtherCreations` and
   `ms_nNearBubbleOtherCreationsInFrustum` (a car that appears in view moves the
   second), so a scripted pop is visible in the live numbers and never inside
   the traffic's counters. The set-piece and road-block cars already start at
   alpha 0 (their fade-in); the emergency services join them (one line added).
   The parked cars of the generators are `PARKED_VEHICLE`s and were never in
   either fill.
7. **The parked cars** of the generators go through the same creation gate but
   cannot pay a ray per candidate: an empty generator retries every four frames
   while it is in range, and the generators run before the traffic in the frame
   (`CTheCarGenerators::Process`, `Game.cpp`), so their blind-spot arm is off
   and a spot behind the camera that no occluder covers waits for the next
   look. Their own gate (off the frustum or occluded) stands, and the grace
   protects them like the traffic.

## What bounds the work

- The rule's cheap arms are two subtractions (the player distance, the camera
  distance), the occluder test (a handful of screen-space point tests), and
  the ring read - no per-triangle work.
- The blind-spot arm spends a ray from a budget of 4 a frame, each ray at most
  120 m (**the soft distance**, 3 sectors of 50 m) and buildings-only, with the
  see-through materials skipped. The ray is the collision path's line walk and
  nothing else; the collision path itself is not changed.
- The candidate count behind the rule is bounded by the placement loops of the
  fill rate (6 car calls x 4 placements a frame at the ring cap, 25 attempts on
  a visible burst frame), so a frame's extra collision work is at most four
  rays; every other candidate is refused by comparisons.
- The frame's budget is reset by `GenerateRandomCars`, the earliest gate that
  can spend a ray. The gates that run earlier in the frame (the parked-car gate
  of `CTheCarGenerators::Process`) never spend one - `NearBubbleAllowsCreation`
  tests its `rayAllowed` flag before it reads the budget - so the positional
  order cannot leak a stale budget into a spend.
- The fill scan is the vehicle loop `RemoveDistantCars` already makes; the two
  counts add one distance test per vehicle and nothing else.

## The scope boundaries, stated

- **The pedestrians** keep their merged rules (the ring table's bands, the
  VICE-122 visibility gates, the VICE-126 burst). The bubble cannot cover the
  crowd without moving the off-screen band out of it, and the radii are
  VICE-120/145's. The counters read the traffic; the crowd's churn is the ped
  tickets' ground.
- **The script, the mission, the road-block, the car-park, the garage and the
  cheat cars** are carried by the "other" counters (point 6) instead of the
  traffic's: they are deliberate game events (a chase, a blockade, a scripted
  car at a scripted place), and gating them could break the scripts that own
  them. Their creations are counted, so nothing hides; their in-view ones are
  the `other creations in frustum` row.
- **The top-down camera** (and only it) bypasses the visibility arms of the
  rule: the street level is not the subject of that view and the traffic must
  keep flowing. The bubble still holds - a car at the player's elbow is there
  when the camera returns - and the bypassed events are counted as such.
- **A cutscene** needs no bypass: the traffic spawns are gated during one and
  the culls read the cutscene camera (the counter reads what the player sees,
  which is the cutscene).
- **The parked cars on an open boulevard** wait for a hidden frame: the price
  of the ticket is that a car is never seen to appear in view. The occluder
  volumes of the city cover most parking spots; the open stretches are the
  visible cost.
- **A car behind the camera on open ground** (the soft arm) is the one
  appearance the ticket accepts in place of an empty street: it starts at
  alpha 0, it is at or beyond the removal gap, and no part of the frustum is
  involved. The camera-turn test can find such a car fading in; the in-view
  counters cannot, by construction.

## Alternatives considered

- **A ray for every candidate.** The collision path is the frame's top cost;
  a ray per candidate of the trickle (24 a frame) and of the burst (25 a frame)
  is exactly the per-triangle work the ticket forbids. Rejected; the budget of
  4 bounds it.
- **The strict reading: occluders, fog and rays only.** It leaves an open
  street with no creation site (the ring bands are inside the visible range by
  the table's design) and the traffic drains there; Section 6 of
  `utils/near-bubble` models it. Rejected in favour of the soft arm.
- **A ray for the parked cars.** The generators run before the traffic and an
  empty one retries every four frames; the rays would starve the traffic.
  Rejected.
- **Trusting the occluder set as it is.** The set is a frame old and its
  screen-space answer against a turned camera is a false "occluded" the
  counters could not see. Rejected; the stamp and the thresholds bound it.
- **An exemption for the emergency services.** The starvation the review found
  is the strict reading's, not the path's: with the soft arm the ambulance and
  the fire truck take the ring's placement behind the camera and drive to the
  scene, which is the behaviour the service wants. No exemption was needed; the
  one change the path got is the alpha-0 start every other fresh car has.
- **Spawn only behind the camera's own bearing.** The bubble is 360 degrees and
  the camera turns; a side or rear band is what the owner catches. Rejected.
- **Move the spawn band out of the bubble** (the peds included). The radii are
  the ring table's (VICE-120/145) and this ticket must not move them. Rejected.
- **A dedicated removal ray.** A removal is a fade the player can watch; the
  cheap arms cover it, and a ray per removal would spend the budget of the
  creations. Rejected.

## Consequences

- **The acceptance.** In a play session the two frustum counters stay 0 (a
  creation or a removal the player could catch), the bubble's fill (the traffic
  plus the other fill) is compared against its target live (6 at stock, 10 at
  the owner's settings), and the camera-turn test has nothing to find in the
  frustum: a car came out of a hidden spot, a faded band, the soft gap behind
  the camera, or it was not created. The fade events of the fill burst are
  counted apart, and the cars outside the spawner have their own rows.
- **The spawn sites narrow to where the world hides them, plus the soft gap.**
  In the city the occluder volumes and the buildings between the camera and the
  side band keep the flow; on the open ground the traffic comes from behind the
  camera (the soft arm) and the fills of the transitions. Section 6 of
  `utils/near-bubble` shows the open-street rate clearing the ring's need by 3x
  to 5x even in its pessimistic layout; the refusal counter makes the balance
  visible live. If the orchestrator finds a street too empty, the knob is
  `NEAR_BUBBLE_SOFT_DISTANCE` (or the ray budget for the close band), not the
  rings.
- **A removal in view waits.** A car beyond the despawn that the player can
  still see keeps driving until it is hidden (or the fade of the weather takes
  it): the count may sit above the target while the player watches a long
  street. The pass of the player (the car behind, or the car passed) ends the
  wait; the ring is not widened.
- **The fresh-spawn grace** holds every fresh car for 5 s. It is the same 5 s
  the stuck cull waits, so a car that was just placed is never the one a cull
  gives up on. A car the player outran during its grace lives a few seconds
  past the ring, so the counted traffic can sit a little above the target at
  high speed.
- **The occluder trust costs a few rejections on fast camera moves.** While the
  camera sweeps more than the thresholds, the occluder arm is off and the
  candidates of the close band need the ray (or are refused); the frame's work
  stays bounded and the open ground is not affected (the soft arm does not read
  the occluders).
- **The counters are cumulative and live**: the orchestrator reads them with
  `utils/caps-audit/live-counters.py` before and after the camera turns, at the
  stock and the owner's settings, and compares `ms_nNearBubbleFill` plus
  `ms_nNearBubbleOtherFill` with `ms_fNearBubbleTarget`.
