#!/bin/bash
# Builds the VICE-121 candidate-list checker (tools/collision_check).
# Run it from the repository root after a cmake configure, so that
# build/compile_commands.json exists (the checker reuses the game's flags):
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   tools/collision_check/build.sh
#   build/collision_check "<gameRoot>" 200
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
OUT="$BUILD/collision_check.obj"

if [ ! -f "$BUILD/compile_commands.json" ]; then
	echo "configure the build first: cmake -S $ROOT -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release" >&2
	exit 1
fi

python3 - > "$OUT.inc" <<EOF
import json
d = json.load(open("$BUILD/compile_commands.json"))
for e in d:
    if e['file'].endswith('collision/Collision.cpp'):
        for p in e['command'].split():
            if p.startswith('-I'):
                print(p)
        break
EOF

DEFS="-DAUDIO_OAL -DCMAKE_BUILD -DCMAKE_NO_AUTOLINK -DLIBRW -DRW_GL3 -DUSE_OUR_VERSIONING"
FLAGS="-O3 -DNDEBUG -std=c++17 -w -ffunction-sections -fdata-sections"
INCS=$(tr '\n' ' ' < "$OUT.inc")
SRCS="src/collision/Collision.cpp src/collision/ColModel.cpp src/collision/ColTriangle.cpp src/collision/ColSphere.cpp src/collision/ColPoint.cpp src/collision/ColBox.cpp src/collision/ColLine.cpp src/math/Vector.cpp src/math/Matrix.cpp"

mkdir -p "$OUT"
c++ $DEFS $FLAGS $INCS -c "$ROOT/tools/collision_check/check.cpp" -o "$OUT/check.o"
for s in $SRCS; do
	c++ $DEFS $FLAGS $INCS -c "$ROOT/$s" -o "$OUT/$(basename "${s%.cpp}").o"
done
c++ -o "$BUILD/collision_check" "$OUT"/*.o -Wl,--gc-sections -lm
echo "built $BUILD/collision_check"
