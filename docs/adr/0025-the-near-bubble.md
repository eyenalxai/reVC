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
2. **The creation rule.** A position outside the bubble may receive a car only
   while the player cannot see it. The ladder is cheapest first:
   - **occluded** (`COcclusion::IsPositionOccluded`): an occluder volume covers
     the position, so no turn of the camera reveals it;
   - **beyond the fade/fog** (the ring table's `visibleRange` =
     min(LOD distance, weather far clip)): the world fades and the fog hides
     there, in or out of the frustum, so the ring's on-screen band keeps
     working exactly where the table always said it does (TrafficRings.h);
   - **the blind spot** (off the frustum and inside `NEAR_BUBBLE_LOS_RANGE`,
     300 m): a line-of-sight ray (`CWorld::GetIsLineOfSightClear`, buildings
     only) that is blocked by a building means a turn cannot reveal the
     position. The ray is the one expensive test, and a frame may spend
     `NEAR_BUBBLE_LOS_RAYS_PER_FRAME` (4) of them; a candidate that finds the
     budget spent is refused, not created.
   Everything else - in the frustum, inside the fade, not occluded - is
   refused. A refusal costs one ladder and the placement loop of the caller
   moves to its next candidate.
3. **The removal rule.** A car may only be removed when all of this holds: the
   fresh-spawn grace (`NEAR_BUBBLE_GRACE_MS`, 5 s from the creation, held in
   the vehicle's `m_nRemovalProtectedUntil` - the field the set pieces and the
   road blocks already used to keep a car alive, renamed for the one meaning);
   the distance beyond the removal threshold (`max` of the ring's despawn and
   the bubble plus its reserve, 120 m) - the despawn already sits the reserve
   past the spawn band, the hysteresis of the table; and hidden (off the
   frustum, occluded, or beyond the fade). A removal spends no ray: it is the
   game's soft event (the car fades out over 16 frames), and the cheap arms
   decide. A car the player can see keeps driving; the game fades it out only
   where a turn cannot reveal the change.
4. **The fill burst keeps its exit.** The fade of a transition covers the
   screen (`FillBurstIsHidden`, VICE-126) and hides the fill, so the rule's
   first arm after the bubble is the fade: an event it covers is allowed. The
   counters mark those events **fade-exempt** (`ms_nNearBubbleFadeCreations`,
   `ms_nNearBubbleFadeRemovals`), so the acceptance of the ticket - the frustum
   counters stay zero - reads cleanly while the burst fills. The bubble still
   applies under the fade: a car placed at the player's elbow is there when the
   fade lifts.
5. **The counters.** In `CCarCtrl` (the pattern of the fill burst), live for
   the `/proc` rig of `utils/caps-audit/live-counters.py`:
   - `ms_nNearBubbleCreationsInFrustum` and `ms_nNearBubbleRemovalsInFrustum`:
     the acceptance. An event at a catchable position - in the frustum, inside
     the fade, not occluded - moves them; the gate never allows one, so a path
     that bypasses the gate shows up here. Both must stay 0 in a play session.
   - `ms_nNearBubbleFadeCreations` / `ms_nNearBubbleFadeRemovals`: the events a
     fade covered, exempt.
   - `ms_nNearBubbleHiddenCreations` / `ms_nNearBubbleHiddenRemovals`: the
     events the rule allowed - the work of the bubble, so a silent rule (one
     that refuses everything) is visible too.
   - `ms_nNearBubbleRefused`: the events the rule stopped.
   - `ms_nNearBubbleBypassed`: the events the top-down camera bypassed.
   - `ms_nNearBubbleRays`: the rays spent since the frame began, at most the
     budget.
   - `ms_nNearBubbleFill` and `ms_fNearBubbleTarget`: the traffic (the random
     cars) inside the bubble and the share of the car target its area asks for
     (`target x (90 / despawn)^2`: 6 at stock, 10 at the owner's settings).
6. **The parked cars** of the generators go through the same creation gate but
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
  300 m (6 sectors of 50 m) and buildings-only, with the see-through materials
  skipped. The ray is the collision path's line walk and nothing else; the
  collision path itself is not changed.
- The candidate count behind the rule is bounded by the placement loops of the
  fill rate (6 car calls x 4 placements a frame at the ring cap, 25 attempts on
  a visible burst frame), so a frame's extra collision work is at most four
  rays; every other candidate is refused by comparisons.
- The fill scan is the vehicle loop `RemoveDistantCars` already makes; the
  count adds one distance test per vehicle and nothing else.

## The scope boundaries, stated

- **The pedestrians** keep their merged rules (the ring table's bands, the
  VICE-122 visibility gates, the VICE-126 burst). The bubble cannot cover the
  crowd without moving the off-screen band out of it, and the radii are
  VICE-120/145's. The counters read the traffic; the crowd's churn is the ped
  tickets' ground.
- **The script, the mission and the road-block cars** are not counted: the
  counters read the churn of the traffic population. A script that deletes a car
  in view, or a road block that appears ahead of the player, is a scripted or
  gameplay event, not the spawner's. The parked cars and the emergency cars are
  counted: they go through the same gate.
- **The top-down camera** (and only it) bypasses the rule: the street level is
  not the subject of that view and the traffic must keep flowing. The bypassed
  events are counted as such.
- **A cutscene** needs no bypass: the traffic spawns are gated during one and
  the culls read the cutscene camera (the counter reads what the player sees,
  which is the cutscene).
- **The parked cars on an open boulevard** wait for a hidden frame: the price
  of the ticket is that a car is never seen to appear. The occluder volumes of
  the city cover most parking spots; the open stretches are the visible cost.

## Alternatives considered

- **A ray for every candidate.** The collision path is the frame's top cost;
  a ray per candidate of the trickle (24 a frame) and of the burst (25 a frame)
  is exactly the per-triangle work the ticket forbids. Rejected; the budget of
  4 bounds it.
- **A ray for the parked cars.** 176 generators and their retries cannot share
  a budget of 4 without starving the traffic. The generator's own gate stands.
  Rejected.
- **Spawn only outside the frustum** (the shipped arrangement). The owner's
  exact complaint: the camera turns and the car is there. Rejected.
- **Spawn only behind the camera's own bearing.** The bubble is 360 degrees and
  the camera turns; a side or rear band is what the owner catches. Rejected.
- **Move the spawn band out of the bubble** (the peds included). The radii are
  the ring table's (VICE-120/145) and this ticket must not move them. Rejected.
- **A dedicated removal ray.** A removal is a fade the player can watch; the
  cheap arms cover it, and a ray per removal would spend the budget of the
  creations. Rejected.

## Consequences

- **The acceptance.** In a play session the two frustum counters stay 0 (a
  creation or a removal the player could catch), the bubble's fill is compared
  against its target live (6 at stock, 10 at the owner's settings), and the
  camera-turn test has no car to find: a car came out of a hidden spot or a
  faded band or it was not created. The fade events of the fill burst are
  counted apart, so a transition does not read as a violation.
- **The spawn sites narrow to where the world hides them.** In the city the
  occluder volumes and the buildings between the camera and the side band keep
  the flow; in the open (the beach, the long boulevards) the side band's ray
  sees through and the traffic comes only from the faded band, the hidden spots
  and the fills of the transitions. The refusal counter makes that visible; if
  the orchestrator finds a street too empty, the knob is `NEAR_BUBBLE_LOS_RANGE`
  or the ray budget, not the rings.
- **A removal in view waits.** A car beyond the despawn that the player can
  still see keeps driving until it is hidden (or the fade of the weather takes
  it): the count may sit above the target while the player watches a long
  street. The pass of the player (the car behind, or the car passed) ends the
  wait; the ring is not widened.
- **The fresh-spawn grace** holds every fresh car for 5 s. It is the same 5 s
  the stuck cull waits, so a car that was just placed is never the one a cull
  gives up on, and the shared field (`m_nRemovalProtectedUntil`) means one
  meaning for the set pieces, the road blocks, the wreck cull and the traffic.
  A car the player outran during its grace lives a few seconds past the ring,
  so the counted traffic can sit a little above the target at high speed.
- **The pool valve** (`RemoveCarsIfThePoolGetsFull`) asks the rule for its
  closest candidate and falls back to the closest car at all when nothing
  passes: a full pool would break the whole traffic, and the fallback's removal
  is counted, so an emergency the player can see shows up in the acceptance
  instead of hiding. The valve runs only when the vehicle pool is down to
  single free digits, which the car budget leaves room for; a normal session
  never reaches it.
- **The parked cars appear late on open streets.** The generator's timer keeps
  the car waiting (no retry cost: `Process` runs every four frames), and the
  first hidden frame places it. A parking spot covered by no occluder and
  visible from the player's position is the one a player can find empty.
- The counters are cumulative and live: the orchestrator reads them with
  `utils/caps-audit/live-counters.py` before and after the camera turns, at
  the stock and the owner's settings, and compares `ms_nNearBubbleFill` with
  `ms_fNearBubbleTarget`.
