## Engineering

Make technical decisions yourself; do not ask the owner. The owner does not know the code. Backwards compatibility does not matter. Rip and tear if needed. Refactor anything you need; you have free rein over the code. Focus on long-term maintenance. Do not write duplicated code. Re-use existing code when possible.

## Architecture decision records

The ADRs in `docs/adr/` are the agent's own notes about what the code does and
why. The owner does not read them and does not care about them.

- They are not the source of truth. The code is.
- Never cite an ADR as the reason for a decision, and never let an ADR stop a
  refactor, a rewrite or a deletion. Refactoring takes priority.
- Keep them up to date as the code moves. A stale ADR is worse than no ADR.
- An ADR that names an external interface (a value a shipped mission script
  passes, or a file another program writes) is a note about that interface, not
  a rule the agent may not break. Check the interface itself, then update the
  ADR.

## Commits

All new commits must be written in conventional commit style.

## Builds

After every build, install the binary. Copy `build/src/reVC` into the game folder
(`~/Games/Steam/steamapps/common/Grand Theft Auto Vice City/`). Write a temporary file
first, then move it over `reVC`. Do not start the game or kill running instances.

## Agent skills

### Issue tracker

Issues live in Linear (team VICE) via the `rata` CLI; specs and tickets are issues and child issues, no projects. See `docs/agents/issue-tracker.md`.

### Triage labels

Default vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`, plus categories `bug` and `enhancement`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.
