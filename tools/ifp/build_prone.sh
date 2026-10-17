#!/bin/bash
# Builds the prone content (VICE-187) from the shipped Vice City animation and
# writes gamefiles/anim/prone.ifp. The generator carries the parameters and the
# reasoning; see its header (tools/ifp/prone.cpp).
#
#   tools/ifp/build_prone.sh [<gameRoot>] [<out.ifp>]
#
# gameRoot defaults to the Steam Vice City install, the output to
# gamefiles/anim/prone.ifp. Nothing is ever written into the game folder.
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME="${1:-$HOME/Games/Steam/steamapps/common/Grand Theft Auto Vice City}"
OUT="${2:-$ROOT/gamefiles/anim/prone.ifp}"

mkdir -p "$ROOT/build"
c++ -std=c++17 -O2 -g -Wall -Wextra -o "$ROOT/build/prone" \
	"$ROOT/tools/ifp/prone.cpp" \
	"$ROOT/tools/ifp/file.cpp" \
	"$ROOT/tools/ifp/view.cpp" \
	"$ROOT/tools/ifp/check.cpp" \
	-lm
mkdir -p "$(dirname "$OUT")"
"$ROOT/build/prone" "$GAME" "$OUT"
