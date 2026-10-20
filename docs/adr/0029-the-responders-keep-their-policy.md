# ADR 0029: The responders and the police keep their policy

Date: 2026-10-20
Status: Accepted
Relates to: VICE-156 (the implementation), VICE-150 (the spawn service),
VICE-127 (the near bubble), VICE-151 (the readiness gate), ADR 0021 (the fill
burst's RNG shift)

## Context

The spawn service owns the placement of every spawned car (VICE-150), but the
two event requesters still created their cars themselves:

- **The emergency services** (`CCarCtrl::GenerateEmergencyServicesCar` and
  `GenerateOneEmergencyServicesCar`) rolled their own placement: up to five
  `CPathFind::GenerateCarCreationCoors` attempts on the on-screen ring around
  the player, a raw `CAutomobile` with no lane or route state, a ground cast,
  the near bubble gate, then the duty flag, the siren and the crew. The
  accident or fire position became the autopilot destination afterwards.
- **The police** were a branch inside `GenerateOneRandomCar`: the wanted level
  and the caps chose a cop class and `ChoosePoliceCarModel`, and the ambient
  request path placed the car when its model dice happened to land on the
  branch.

Both count in the car budget and both were subject to the near bubble, so they
were partly on the service's gates and partly outside them. The ticket asks for
one placement writer, with the triggers, cooldowns and caps still owned by the
event systems, and for a **destination** mode: a responder drives to its
incident, not to a cruise route.

## Decision

1. **The policy is engine-free and shared.** `src/control/ResponderPolicy.h`
   holds the emergency triggers (two active accidents, three active fires),
   the 30/35 second cooldowns, the one-on-duty guard, the two-cars-per-model
   cap, the nearest-incident range and the police gate (stars, law-enforcer and
   cop caps, interior, the 5/8 second cadence). The engine resolves the live
   facts and calls the same functions the evidence program calls. A refusal
   costs no service request, no plan, no path search and no ledger row.

2. **The police are their own requester.** `CCarCtrl::GeneratePoliceCars` is
   called once per frame from `GenerateRandomCars`, after the cutscene and ped
   pool gates and before the ambient fill. The wanted system stays the policy
   owner (`CWanted`'s stars, caps and `ChoosePoliceCarModel`'s tiers); the
   request carries `SPAWN_PURPOSE_POLICE`, `allowBoats` (a water site takes the
   Predator) and the traffic ring helper's own ring. The old branch inside
   `GenerateOneRandomCar` is gone, so the ambient model dice no longer decide
   whether the police are asked.

3. **The responders request a destination.** `SSpawnCarRequest::responder`
   carries the role, and the destination is the incident. The request's target
   is the destination too: the planner places on the ring around the incident
   and checks that the site can reach it. The visibility ladder and the
   primitive's birth cull still measure from the player centre
   (`SSpawnCarRequest::centre`), so the no-catchable rule stays player-relative.
   The service copies the destination fields to `SSpawnPlacement` through
   `SpawnPlacementSetDestination`; the primitive keeps the old route builder
   (`JoinCarWithRoadSystemGotoCoors`) and the old mission (`MISSION_GOTOCOORDS`
   / `MISSION_GOTOCOORDS_STRAIGHT`), speed and driving style. The old raw
   collector placement and its five attempts are gone: the planner's four
   placements and its gates replace them.

4. **Event requests step over the ambient target, not the physical caps.**
   `SSpawnCarRequest::priority` makes `SpawnFunctionGate` skip the ambient car
   target (the fill's own line) while the car budget and the ped pool reserve
   still refuse. The police caps and the emergency duty/model caps remain the
   policy owners' own.

5. **One crew table.** `SpawnCrewOf` names the seats of every police model and
   of the two responders; `CCarAI::AddPoliceCarOccupants`,
   `AddAmbulanceOccupants` and `AddFiretruckOccupants` fill the seats from it,
   so the evidence program checks the seats the game creates.

6. **The ledger names the event rows.** The service counts the requests and the
   creations by `SPAWN_PURPOSE_RESPONDER` and `SPAWN_PURPOSE_POLICE`; the
   removal note reads the live duty and law-enforcer flags on a
   `RANDOM_VEHICLE`, so an on-duty ambulance or fire truck and a law enforcer
   leave in their own rows while a mission, parked or permanent car keeps its
   creation class.

## Consequences

- **The ring is around the incident, not the player.** The old emergency
  collector aimed at the player; the accident or fire is within 30 m of the
  player, so the two rings nearly coincide, and the new one also guarantees the
  site can reach the incident.
- **The police no longer wait for the ambient target.** A full ambient fill no
  longer stops a police request; the wanted caps and the car budget do. The
  old branch's cadence is unchanged (every frame at four stars or more, 5 s at
  three, 8 s at two), and a police creation still stamps
  `LastTimeLawEnforcerCreated`.
- **The placement is a planned site.** A responder now starts on a lane with
  the route and lane state of the road, not on the raw collector crossing; its
  forward follows the lane instead of pointing straight at the incident. The
  join that the old path ran after the raw placement still runs, now from the
  planned position.
- **The RNG stream shifts.** The model choice and the ambient dice no longer
  share a call with the police branch, and the emergency path's five
  collector attempts (and their dice) are gone. This is the same class of
  stated shift as ADR 0021; the live soak is the measure.
- **The evidence is offline.** `utils/spawn-service/responders.cpp` drives the
  policy and the service core over a scripted city: a crime creates a police
  car, an accident an ambulance and a fire a fire truck, the caps stop at two
  (emergency) and at the wanted tops (police), the catchable candidate sets
  create nothing, and the occupants and removal rows are checked. The engine
  side (the primitive, the streaming, the Predator fallback and the live
  world) is the build's and the live soak's.
