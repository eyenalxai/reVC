# ADR 0028: The movement solver owns velocity, position and stance

Date: 2026-10-18
Status: Accepted
Relates to: VICE-169 (the spec), VICE-170..VICE-188 (the tickets), ADR 0012 (the drift snapshot pattern), ADR 0015 (the aim pose)

## Context

The on-foot movement is animation locked. The chooser
(`CPlayerPed::SetRealMoveAnim`) picks the walk, run and sprint blend from
`m_fMoveSpeed`, a pad ramp, and the controller then targets the *animation's*
reference speed (`GetMoveAnimReferenceSpeed`, the only reader of
`hierarchy->rootSpeed`). The velocity follows the animation, and the animation
follows the pad, in one frame. Every reported movement defect of the last
rebuild comes from that loop: the ped that does not move while the aim holds a
walk animation, the sprint jump that stops, the legs that slide when the ped
strafes because the controller runs at the forward reference speed.

Three more limits sit in the same place:

- Crouch is a flag pair whose partial animations key the whole body, the legs
  included, so a crouched player who moves freezes his legs.
- There is no slide and no prone, and no content for either.
- The jump impulse lives in a finish callback of the jump animation
  (`FinishLaunchCB`), so the launch waits for the animation to reach it.

The fork also wants omnidirectional movement in every view, effortless chaining
between the states, and later a wall run and a double jump.

## Decision

1. **Four layers, one authority each.** The **intent** is one struct the adapter
   builds from the pad, the camera and the ped: the camera relative heading, the
   speed tier, the stance request and the action edges. The **solver** owns the
   state machine and the stance axis, and it owns the player's
   `m_vecMoveSpeed.x/y` and `m_moved`. The **snapshot** is one read of the
   solver's state for every consumer. The **presentation** (the animation
   chooser) reads the snapshot, and writes no velocity.
2. **The solver is a module with a small interface.** `src/peds/PlayerMoveSolver.*`
   is engine free: `Step(intent)` returns the snapshot, the snapshot is the only
   read surface, and the state stays private. One instance per player. The
   adapter in `CPlayerPed::UpdatePlayerMovement` keeps the pad block, the gates
   and the write back, and the three on-foot control paths keep calling it.
3. **The invariant.** No presentation state may gate the velocity or the input
   response on animation progress. The exception list is explicit and closed:
   car entry and exit, the get-up, the cutscenes, the mission control flags,
   being attached, the replay, and the fight path, which keeps its
   animation-driven movement. A harness check in `utils/` fails when a movement
   write reads an animation association.
   The input response is the same in every view: the pad always moves the ped.
   The only gates are the scripted control flags (`PLAYERCONTROL_*`) and the
   stationary sights (the sniper, the rocket launcher and the camera). The stock
   rule that zeroed the movement while a smooth spray turned the body is gone;
   it also read the duck flag, so it held a crouched walk in place in the third
   person view.
4. **The stride is data.** The state rows (acceleration, friction, turn rate),
   the stance rows (speed scale, turn scale, capsule), the speed ladder and the
   transition table hold the feel. A state or a transition is a row, not an `if`.
   The slide's entry boost, its friction curve and the slope acceleration are
   row fields.
5. **`m_nMoveState` stays a reported value.** `eMoveState` has 84 read lines in
   11 files, several of them comparisons rather than equalities
   (`Ped.cpp` `SetStoredState`, the flee gate; `Script5.cpp` `IsPedStopped`;
   the camera's rotation gate; the footstep sounds). The solver derives it from
   its own state and the speed tier; nothing repurposes it and nothing inserts a
   value into it.
6. **The animation runtime stays.** Blending, the partial associations, the
   velocity extraction, the frame callbacks, the association budget, the IFP
   loader and the group registration are kept as they are. Only the chooser
   becomes presentation.
7. **The aim layer sits on top.** `CPedIK` keeps writing after the blend and
   survives on the partial aim pose. Because its reference reads assume an
   upright body, the prone, dive and slide aim is the solver's yaw plus pose
   variants plus the correction, not the stock bone-derived reference. (ADR 0015
   holds the pose rule.)

## Considered alternatives

- **Keep the controller and tune the numbers.** Rejected: the lock is the loop
  itself, not the constants. Every tuning pass would keep the legs as the speed
  reference.
- **Take the SA structure (a task owns crouch, the duck flag drives it).**
  Rejected: SA's movement core is not reversed anywhere available
  (`CalculateNewVelocity`, `UpdatePosition`, `ProcessControl` and
  `SetRealMoveAnim` are stubs in gta-reversed), SA has no crouch or prone move
  state either, and a task-shaped crouch would put the stance in the AI layer
  instead of the movement layer.
- **Gate the movement on `bIsStanding`.** Rejected: it is a physics-owned bit
  written by the collision pass, one frame behind the solver's own decision, and
  it cannot express a stance.
- **Go straight to pose matching.** Deferred, not rejected: the motion matching
  chooser is the presentation end target, on the same snapshot interface. The
  state-machine chooser is its first implementation.

## Consequences

- The solver can be driven in a `utils/` harness with no engine: the intent and
  the snapshot are plain data, and the harness replays an input stream over the
  transitions.
- A new state (the wall run) or a new air move (the double jump) is a table row,
  a step function and an event. No control path changes.
- The presentation sees the solver's state one frame late, the same latency the
  chooser has today. The camera, the HUD and the telemetry all read the same
  snapshot, so they cannot disagree with each other.
- The old vocabulary goes: there is no "movement controller" any more.
- A later change that reads an animation association inside the solver fails the
  invariant check, which is the point.
