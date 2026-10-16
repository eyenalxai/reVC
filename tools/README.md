# tools

Verification programs that link the game's own translation units and drive
them, built by each subdirectory's `build.sh` with the game's compile flags and
small stubs for the services the unit does not need. `utils/` holds the
opposite kind: standalone evidence programs that read the sources or the game
data and re-derive a computation on their own, without linking game code.

* `collision_check` (VICE-121): the pruned triangle candidate list against the
  plain scan, over the real collision models.
* `pathfind_check` (VICE-130): the path searches and the closest-node lookup
  against the base revision, over a graph snapshot from a running game.
