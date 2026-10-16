#!/bin/bash
# Builds the VICE-130 path-result checker (tools/pathfind_check).
#
# Run it from the repository root after a cmake configure, so that
# build/compile_commands.json exists (the checker reuses the game's flags):
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#
#   tools/pathfind_check/build.sh                 # against the working tree
#   tools/pathfind_check/build.sh miami           # against a git revision
#   tools/pathfind_check/build.sh <rev> <tag>     # revision + output tag
#
# The binary lands in build/pathfind_check[.tag]. A revision build takes
# src/control/PathFind.{cpp,h} from that revision, so the same cases can be
# run through the base code and the changed code and the outputs diffed.
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
OUT="$BUILD/pathfind_check.obj"

REV="$1"
TAG="$2"
if [ -n "$REV" ] && [ -z "$TAG" ]; then
	TAG="$(echo "$REV" | tr '/: ' '___')"
fi
BIN="$BUILD/pathfind_check${TAG:+.$TAG}"
DUMPBIN="$BUILD/pathfind_dump${TAG:+.$TAG}"

if [ ! -f "$BUILD/compile_commands.json" ]; then
	echo "configure the build first: cmake -S $ROOT -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release" >&2
	exit 1
fi

# The sources under test always come from a private directory: a git revision
# is extracted into it, the working tree is copied into it. The trap is only
# allowed to see that directory.
SRCDIR="$(mktemp -d)"
trap 'rm -rf "$SRCDIR"' EXIT
if [ -n "$REV" ]; then
	git -C "$ROOT" show "$REV:src/control/PathFind.cpp" > "$SRCDIR/PathFind.cpp"
	git -C "$ROOT" show "$REV:src/control/PathFind.h" > "$SRCDIR/PathFind.h"
else
	cp "$ROOT/src/control/PathFind.cpp" "$ROOT/src/control/PathFind.h" "$SRCDIR/"
fi
PATHFIND="$SRCDIR/PathFind.cpp"

python3 - > "$OUT.inc" <<EOF
import json
d = json.load(open("$BUILD/compile_commands.json"))
for e in d:
    if e['file'].endswith('control/PathFind.cpp'):
        for p in e['command'].split():
            if p.startswith('-I'):
                print(p)
        break
EOF

DEFS="-DAUDIO_OAL -DCMAKE_BUILD -DCMAKE_NO_AUTOLINK -DLIBRW -DRW_GL3 -DUSE_OUR_VERSIONING"
FLAGS="-O2 -g -DNDEBUG -std=c++17 -w -ffunction-sections -fdata-sections"
INCS=$(tr '\n' ' ' < "$OUT.inc")

mkdir -p "$OUT"
rm -f "$OUT"/*.o
c++ $DEFS $FLAGS -I"$SRCDIR" $INCS -c "$PATHFIND" -o "$OUT/PathFind.o"
c++ $DEFS $FLAGS -I"$SRCDIR" $INCS -c "$ROOT/tools/pathfind_check/check.cpp" -o "$OUT/check.o"
c++ $DEFS $FLAGS -I"$SRCDIR" $INCS -c "$ROOT/tools/pathfind_check/dump.cpp" -o "$OUT/dump.o"
c++ $DEFS $FLAGS -I"$SRCDIR" $INCS -c "$ROOT/tools/pathfind_check/stubs.cpp" -o "$OUT/stubs.o"
c++ -o "$BIN" "$OUT/PathFind.o" "$OUT/check.o" "$OUT/stubs.o" -Wl,--gc-sections -lm
c++ -o "$DUMPBIN" "$OUT/PathFind.o" "$OUT/dump.o" "$OUT/stubs.o" -Wl,--gc-sections -lm
echo "built $BIN and $DUMPBIN"
