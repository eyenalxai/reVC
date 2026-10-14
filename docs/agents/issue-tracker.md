# Issue tracker: Linear

Issues and specs for this repository live in Linear, team **VICE** (`Vice`),
workspace `personal`. Use the `rata` CLI for all operations.

## Conventions

- **Create an issue**: `rata issue create --title "..." --body-file -`. Pipe the
  body with a heredoc.
- **Set the priority**: every issue must carry a priority. Pass `--priority`
  when you create the issue; triage sets it for issues that arrive without one.
  Values: `urgent` (breaks production or blocks a release), `high` (blocks other
  work), `medium` (the fallback), `low` (nice-to-have). Change it with
  `rata issue update <ref> --priority <value>`.
- **Assign the issue**: every issue must be assigned to the authenticated user.
  Pass `--assignee me` when you create the issue; triage assigns `me` to issues
  that arrive without an assignee. Change it with `rata issue assign me <ref>`,
  or clear it with `rata issue unassign <ref>`. Wayfinder children are the
  exception: they stay unassigned until the claim step.
- **Read an issue**: `rata issue show <ref> --comments --json`
- **List issues**: `rata issue list` with the filters you need. Useful filters:
  `--team`, `--project`, `--state`, `--state-type`, `--priority`, `--label`,
  `--assignee`, `--parent`, `--text`, `--unblocked`, `--unassigned`. Every list
  command returns one page: `--limit` is the page size (default 50, maximum
  250), and `--after <cursor>` continues a list. See **Paging lists**.
- **Comment on an issue**: `rata issue comment <ref> --body-file -`
- **Apply / remove labels**: `rata issue label add <ref> <label...>` and
  `rata issue label remove <ref> <label...>`
- **Close**: `rata issue close <ref> --comment "..."`
- **Search**: `rata search "..."`

An issue reference accepts an identifier (`VICE-42`), a UUID, or a linear.app
issue URL. The repository config names the default team (`VICE`) and workspace
profile (`personal`), so most commands need no `--team`. The link lives in
rata's local database, keyed by the repository, so every subdirectory and every
worktree finds it. Restore it in a fresh clone with `rata link --team VICE`.

## Paging lists

Every user-facing list command returns one page per call: `issue list`,
`search`, `team list`, `project list` and `label list`. `--limit` is the page
size (default 50, maximum 250). `--after <cursor>` continues a list. JSON is
`{ <plural>, pageInfo }`: `pageInfo.hasNextPage` says whether a next page
exists, and `pageInfo.endCursor` is the cursor for `--after`. Keep the filters
and the order fixed across pages: a cursor is bound to its query. Lists are
ordered by creation time (`createdAt`); `search` keeps Linear's relevance
ranking, so its paging is best effort. Internal reads drain every page, so
`issue show` is complete.

## Pull requests as a triage surface

**PRs as a request surface: no.** Linear holds issues only. When this repository
treats external pull requests as feature requests, triage them where they live
and mirror the outcome here.

## When a skill says "publish to the issue tracker"

Create a Linear issue with `rata issue create`. A spec is an issue too: see
**Specs and tickets**.

## When a skill says "fetch the relevant ticket"

Run `rata issue show <ref> --comments`.

## Specs and tickets

A **spec** is an issue. Do not create a project for it and do not put `Spec:`
in its title. The spec's **tickets** are its child issues, so the set stays
together under the spec. This repository does not use Linear projects.

- **Publish a spec**: `rata issue create --title "<title>" --label ready-for-agent --priority medium --assignee me --body-file -`.
- **Publish the tickets**: create each ticket as a child of the spec:
  `rata issue create --title "..." --parent <spec-ref> --label ready-for-agent --priority medium --assignee me --body-file -`.
  Wire the blocking edges with
  `rata issue link <ticket-ref> --blocked-by <blocker-ref>`.
- **Implement the spec**: `/implement-spec` fetches the whole set one page at
  a time: `rata issue list --parent <spec-ref> --limit 250 --json`, then
  continue with `--after <endCursor>` while `pageInfo.hasNextPage` is true.
  `rata issue show <spec-ref> --json` also lists the children.
- **Close the spec**: when every ticket is Done, close the spec with a comment
  that points at the result: `rata issue close <spec-ref> --comment "..."`.

## Wayfinding operations

Used by `/wayfinder`. The **map** is a single issue labelled `wayfinder:map`,
with **child** issues as tickets.

- **Map**: `rata issue create --title "..." --label wayfinder:map --assignee me --body-file -`.
- **Child ticket**: `rata issue create --title "..." --parent <map-ref> --label wayfinder:<type> --body-file -`.
  The type labels are `wayfinder:research`, `wayfinder:prototype`,
  `wayfinder:grilling` and `wayfinder:task`. Leave children unassigned: the
  claim step assigns them.
- **Blocking**: native Linear relations. Wire an edge with
  `rata issue link <child-ref> --blocked-by <blocker-ref>`, or the inverse with
  `rata issue link <blocker-ref> --blocks <child-ref>`. A ticket is unblocked
  when every issue that blocks it is closed.
- **Frontier query**: `rata issue list --parent <map-ref> --unblocked --unassigned --sort priority --limit 1 --json`.
  The JSON is `{ issues, pageInfo }`. The first result is the most urgent open,
  unblocked, unclaimed child; ties keep the previous order. The sort reads
  every matching page before it applies the limit, so one call returns the top
  ticket.
- **Claim**: `rata issue assign me <ref>`, the session's first write. The
  assignee is the claim; this is what assigns a wayfinder child.
- **Resolve**: `rata issue comment <ref> --body-file -`, then
  `rata issue close <ref>`, then append a context pointer (gist plus link) to
  the map's Decisions-so-far with `rata issue update <map-ref> --body-file -`.
