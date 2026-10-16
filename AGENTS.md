## Engineering

Make technical decisions yourself; do not ask the owner. The owner does not know the code. Backwards compatibility does not matter. Rip and tear if needed. Refactor anything you need; you have free rein over the code. Focus on long-term maintenance. Do not write duplicated code. Re-use existing code when possible.

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
