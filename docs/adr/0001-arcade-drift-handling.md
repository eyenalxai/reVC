# ADR 0001: Arcade drift handling is a player-only layer in its own module

Date: 2026-10-14
Status: Accepted

## Context

The owner wants arcade drift handling: handbrake-initiated slides where the
player controls the drift angle with counter-steer (see `GLOSSARY.md`). The feel
reference is Trackmania Canyon. Realistic tire simulation is out of scope.

reVC simulates every car with one shared physics step: `CAutomobile::ProcessControl`
plus `CVehicle::ProcessWheel`. AI cars, mission cars, races and traffic all share
that step. A change there touches the whole game.

## Decision

1. The drift logic lives in a new module, `src/vehicles/DriftControl.*`. The
   module holds the drift state machine and the tuning for one car.
2. Only the player's car steps the module. AI cars and every other vehicle keep
   the original path.
3. The feature is off by default. `reVC.ini` gets a `[Drift]` section, and the
   Controller setup menu gets an on/off option.
4. The model is arcade, not realistic: a rear grip cutoff starts and holds the
   slide, a yaw controller holds the target angle, the slide keeps forward
   speed, and a long drift gives a small boost.
5. The original handling path stays for normal driving. The drift layer is only
   active between drift entry and drift exit.

## Alternatives considered

- **Tune `handling.cfg` only.** Too coarse: no control over the angle, no boost,
  and every owner of the game must edit their own data file.
- **Rework the shared grip model.** Changes AI, traffic and normal driving at
  the same time. It also conflicts with the rule "never slower than default".
- **Port a kart model as-is.** The ground truth differs from GTA physics; a
  port would fight the existing collision, surface and damage systems.

## Consequences

- Missions, races against AI and traffic keep the behaviour they have today.
- The drift feel needs the owner's play-test; the tuning values live in one
  module and one ini section, so a change of feel is cheap.
- A later extension can give bikes or AI cars their own drift behaviour. That is
  out of scope now.
