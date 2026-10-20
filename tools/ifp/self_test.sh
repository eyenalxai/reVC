#!/bin/bash
# The IFP tool's own test (tools/ifp, VICE-185): the close-out evidence, run
# from one command.
#
#   tools/ifp/build.sh
#   tools/ifp/self_test.sh "<gameRoot>" [workDir]
#
# gameRoot defaults to the Steam Vice City install, workDir to /tmp/opencode/ifp.
# Nothing is written into the game folder.
#
# It checks the shipped file, proves the round trip byte for byte, extracts a
# movement set from the shipped cycles and checks it, builds the crouch movement
# block and checks its four sets, and runs the validator against every planted
# defect, each of which must be caught. When python3 is present it also compares
# the tool's keys with tools/ifp/verify_keys.py, an independent reader of the
# raw bytes.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME="${1:-$HOME/Games/Steam/steamapps/common/Grand Theft Auto Vice City}"
D="${2:-/tmp/opencode/ifp}"
IFP="$ROOT/build/ifp"
FILE="$GAME/anim/ped.ifp"
SET="$D/movement.ifp"

if [ ! -x "$IFP" ]; then
	echo "build the tool first: tools/ifp/build.sh" >&2
	exit 1
fi
if [ ! -f "$FILE" ]; then
	echo "no IFP at $FILE" >&2
	exit 1
fi

mkdir -p "$D"
pass=0
fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }

echo "== the shipped file"
if "$IFP" check "$GAME" > "$D/check.txt" 2>&1; then
	ok "$(cat "$D/check.txt")"
else
	bad "the shipped file does not check out"; cat "$D/check.txt"
fi

echo
echo "== the round trip"
if "$IFP" roundtrip "$GAME" --dir "$D" > "$D/roundtrip.txt" 2>&1 &&
   cmp -s "$FILE" "$D/ped.ifp"; then
	ok "$(tail -1 "$D/roundtrip.txt")"
	ok "the written file is identical to the shipped one (cmp)"
else
	bad "the round trip is not byte-stable"; cat "$D/roundtrip.txt"
fi
if "$IFP" check "$D/ped.ifp" > /dev/null 2>&1; then
	ok "the written file passes the validator under its own stem"
else
	bad "the written file does not pass the validator"
fi

echo
echo "== a movement set from the shipped cycles"
if "$IFP" extract "$GAME" "$SET" WALK_civi run_civi sprint_civi IDLE_stance WALK_start \
   > "$D/extract.txt" 2>&1 && "$IFP" check "$SET" --movement > "$D/set.txt" 2>&1; then
	ok "$(cat "$D/extract.txt")"
	ok "$(head -1 "$D/set.txt")"
else
	bad "the extracted movement set does not check out"; cat "$D/extract.txt" "$D/set.txt"
fi

echo
echo "== the prone block (VICE-187)"
PRONE="$ROOT/gamefiles/anim/prone.ifp"
if "$IFP" check "$PRONE" --movement > "$D/prone-check.txt" 2>&1 &&
   "$IFP" check "$PRONE" --require-root >> "$D/prone-check.txt" 2>&1; then
	ok "$(head -1 "$D/prone-check.txt")"
	ok "$(tail -1 "$D/prone-check.txt")"
else
	bad "the prone block does not check out"; cat "$D/prone-check.txt"
fi

echo
echo "== the crouch block from the shipped cycles (VICE-186)"
CROUCH="$ROOT/gamefiles/anim/crouch.ifp"
if "$IFP" crouch "$GAME" "$D/crouch.ifp" > "$D/crouch.txt" 2>&1; then
	ok "$(head -1 "$D/crouch.txt")"
	if cmp -s "$D/crouch.ifp" "$CROUCH"; then
		ok "the built block is the committed gamefiles/anim/crouch.ifp (cmp)"
	else
		bad "the built block differs from the committed gamefiles/anim/crouch.ifp"
	fi
else
	bad "the crouch step does not build the block"; cat "$D/crouch.txt"
fi
if "$IFP" check "$CROUCH" --movement > "$D/crouch-check.txt" 2>&1 &&
   "$IFP" check "$CROUCH" --require-root >> "$D/crouch-check.txt" 2>&1; then
	ok "$(head -1 "$D/crouch-check.txt")"
	ok "$(tail -1 "$D/crouch-check.txt")"
else
	bad "the crouch block does not check out"; cat "$D/crouch-check.txt"
fi
# Every direction set passes the movement checks on its own. The block's own
# --movement check reads its first five animations only (the forward set), so
# each set is extracted and checked the way a presentation reads it.
CrouchSet() { # <name> <walk> <run> <sprint> <idle> <startwalk>
	local name="$1" set="$D/crouch-$1.ifp"
	shift
	if "$IFP" extract "$CROUCH" "$set" "$@" > /dev/null 2>&1 &&
	   "$IFP" check "$set" --movement > "$D/crouch-set-$name.txt" 2>&1; then
		ok "the $name set: $(tail -1 "$D/crouch-set-$name.txt" | sed "s|^$set: ||")"
	else
		bad "the $name set does not check out"; cat "$D/crouch-set-$name.txt"
	fi
}
CrouchSet forward walk_crouch run_crouch sprint_crouch IDLE_crouch walk_start_crouch
CrouchSet back walk_back_crouch run_back_crouch sprint_back_crouch IDLE_crouch walk_start_back_crouch
CrouchSet left walk_left_crouch run_left_crouch sprint_left_crouch IDLE_crouch walk_start_left_crouch
CrouchSet right walk_right_crouch run_right_crouch sprint_right_crouch IDLE_crouch walk_start_right_crouch

echo
echo "== planted defects, every one must fail"
Plant() { # <defect> <input> <out> <check flags...>
	local defect="$1" input="$2" out="$3"
	shift 3
	if ! "$IFP" plant "$defect" "$input" "$out" > "$D/plant-$defect.txt" 2>&1; then
		bad "cannot plant $defect: $(cat "$D/plant-$defect.txt")"
		return
	fi
	if "$IFP" check "$out" "$@" > "$D/plant-$defect-check.txt" 2>&1; then
		bad "the validator passed a planted $defect"
		return
	fi
	ok "the validator caught $defect ($(tail -1 "$D/plant-$defect-check.txt" | sed "s|^$out: ||")): $(head -1 "$D/plant-$defect-check.txt" | sed "s|^$out: ||")"
}
for defect in blockname unpadded zeroframe ident longname noroot deltatimes; do
	Plant "$defect" "$GAME" "$D/plant-$defect/ped.ifp"
done
Plant missingroot "$SET" "$D/plant-missingroot/movement.ifp" --require-root
for defect in opencycle nospeed slotorder; do
	Plant "$defect" "$SET" "$D/plant-$defect/movement.ifp" --movement
done

echo
echo "== the keys against an independent reader of the raw bytes"
if command -v python3 > /dev/null; then
	"$IFP" dump "$GAME" --keys > "$D/ped.dump" 2> "$D/dump.txt"
	if python3 "$ROOT/tools/ifp/verify_keys.py" "$FILE" "$D/ped.dump" > "$D/verify.txt" 2>&1; then
		ok "$(cat "$D/verify.txt")"
	else
		bad "the independent reader disagrees"; cat "$D/verify.txt"
	fi
	"$IFP" dump "$CROUCH" --keys > "$D/crouch.dump" 2>> "$D/dump.txt"
	if python3 "$ROOT/tools/ifp/verify_keys.py" "$CROUCH" "$D/crouch.dump" >> "$D/verify.txt" 2>&1; then
		ok "the crouch block: $(tail -1 "$D/verify.txt")"
	else
		bad "the independent reader disagrees with the crouch block"; cat "$D/verify.txt"
	fi
else
	echo "skip python3 is not installed"
fi

echo
echo "RESULT: the IFP tool in tools/ifp ($pass checks, $fail failures)"
[ "$fail" -eq 0 ]
