#!/bin/bash
# Builds the IFP reader, writer and validator (tools/ifp, VICE-185).
#
# Standalone: the tool carries its own model of the format and uses no engine
# header, so it needs no cmake configure. Run it from the repository root:
#
#   tools/ifp/build.sh
#   build/ifp check     "<gameRoot>/anim/ped.ifp"
#   build/ifp roundtrip "<gameRoot>/anim/ped.ifp" --dir /tmp/opencode/ifp
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"

mkdir -p "$BUILD"
c++ -std=c++17 -O2 -g -Wall -Wextra -o "$BUILD/ifp" \
	"$ROOT/tools/ifp/file.cpp" \
	"$ROOT/tools/ifp/view.cpp" \
	"$ROOT/tools/ifp/pose.cpp" \
	"$ROOT/tools/ifp/check.cpp" \
	"$ROOT/tools/ifp/crouch.cpp" \
	"$ROOT/tools/ifp/dump.cpp" \
	"$ROOT/tools/ifp/plant.cpp" \
	"$ROOT/tools/ifp/main.cpp" \
	-lm
echo "built $BUILD/ifp"
