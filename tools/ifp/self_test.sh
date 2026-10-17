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
# movement set from the shipped cycles and checks it, and runs the validator
# against every planted defect, each of which must be caught. When python3 is
# present it also compares the tool's keys with tools/ifp/verify_keys.py, an
# independent reader of the raw bytes.
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
else
	echo "skip python3 is not installed"
fi

echo
echo "RESULT: the IFP tool in tools/ifp ($pass checks, $fail failures)"
[ "$fail" -eq 0 ]
