#!/usr/bin/env python3
"""Read the population and traffic counters of a running reVC, read-only.

The caps audit (VICE-123) needs live values while the owner plays. The game is a
PIE, so the value of a static symbol lives at

    (the base of the executable in /proc/<pid>/maps) + (the nm offset of <exe>)

and the exe is read from /proc/<pid>/exe, which still works after the binary on
disk was replaced (the running image is kept as a deleted file).

Usage (from the repository root):
    utils/caps-audit/live-counters.py [pid] [exe]

With no pid, the first process named reVC is used. With no exe, the binary the
process actually runs is used (the right one for the offsets). The program only
reads: /proc/<pid>/mem is opened read-only and nothing is written or signalled.

The pool sizes are not mirrored here: every CPool carries its own size, and the
program checks that size against the enum in src/core/config.h (the pool
arithmetic of PopulationBudget.h is built on those numbers), so a drift shows up
in the output instead of silently mis-sizing the pool scan.

Measured values (VICE-123, 2026-10-16, max sliders in sunny weather, 2400 car
budget, ped cap 5500 before the cap change of this branch):
    cars 149..201 of the 2400 budget, peds 2251..5500 of the 5500 cap,
    parked 0..12, vehicles loaded 57..65 of 90, ped pool 5417/8192,
    vehicle pool 153/2560, dummy pool 2338/2800, visible entities 361..1233.

The fill burst rows (VICE-126) are the transition timeline: the frames left of
each window read 0 while the burst is idle, and fall from 60 while it spends
itself after a boot, an interior exit or a cutscene end. The attempts and the
placements of the rows above them are the burst's totals since it was armed, and
the target is the local line it stopped at (the 90 % stop line sits on it). The
orchestrator reads those rows live across a transition. The "since boot" totals
accumulate over every run of the session (they are reset by the boot of the
game only), so the boot burst stays readable after a later arming reset the
window rows, and the "ended" rows count the runs by the reason they ended
(VICE-146): the target's stop line, the full car budget (cars only), the
give-up bound, or the window spent. A burst waits, without spending its window,
for a target that is not known yet at boot, so a boot run is not burned on a
zero target.

The near bubble rows (VICE-127) are the acceptance of the pop-in ticket: the two
catchable-zone counters (renamed from the frustum pair at VICE-146) count a car
created or removed where the player could catch it and must stay 0. Since
VICE-146 they read the catchable zone only - in the frustum nearer than the
in-view band minimum - because the band at or beyond the minimum is allowed (it
is the stock game's on-screen spawn band: a creation is covered by the car's own
alpha ramp, a removal by the fade-out of the despawn), and the band's own
creations and removals have their two rows. The fade rows are the fill burst's
work, exempt by the screen; the fill and the target are the traffic inside the
player's 90 m disc and the share of the car target that area asks for (the other
fill is the rest of the target: the cars outside the traffic spawner, whose
creations the "other" rows carry apart). The counters are cumulative, so the
orchestrator notes them before a drive or a camera turn and reads them after.
"""

import os
import re
import struct
import subprocess
import sys

# The symbols of the audit, with their mangled name and format. A missing
# symbol is reported, not fatal: the table is worth reading even when one name
# moved.
SYMBOLS = [
    ("cars: NumRandomCars",          "_ZN8CCarCtrl13NumRandomCarsE",                  "i"),
    ("cars: NumLawEnforcerCars",     "_ZN8CCarCtrl18NumLawEnforcerCarsE",              "i"),
    ("cars: NumParkedCars",          "_ZN8CCarCtrl13NumParkedCarsE",                   "i"),
    ("cars: NumMissionCars",         "_ZN8CCarCtrl14NumMissionCarsE",                  "i"),
    ("cars: NumPermanentCars",       "_ZN8CCarCtrl16NumPermanentCarsE",                "i"),
    ("cars: NumAmbulancesOnDuty",    "_ZN8CCarCtrl19NumAmbulancesOnDutyE",             "i"),
    ("cars: NumFiretrucksOnDuty",    "_ZN8CCarCtrl19NumFiretrucksOnDutyE",             "i"),
    ("cars: MaxNumberOfCarsInUse",   "_ZN8CCarCtrl20MaxNumberOfCarsInUseE",            "i"),
    ("cars: CarDensityMultiplier",   "_ZN8CCarCtrl20CarDensityMultiplierE",            "f"),
    ("cars: CarNumberMultiplier",    "_ZN8CIniFile19CarNumberMultiplierE",             "f"),
    ("peds: MaxNumberOfPedsInUse",   "_ZN11CPopulation20MaxNumberOfPedsInUseE",        "i"),
    ("peds: MaxNumberOfPedsInUseInterior", "_ZN11CPopulation28MaxNumberOfPedsInUseInteriorE", "i"),
    ("peds: PedDensityMultiplier",   "_ZN11CPopulation20PedDensityMultiplierE",        "f"),
    ("peds: PedNumberMultiplier",    "_ZN8CIniFile19PedNumberMultiplierE",             "f"),
    ("peds: ms_nTotalPeds",          "_ZN11CPopulation13ms_nTotalPedsE",               "I"),
    ("peds: ms_nTotalCarPassengerPeds", "_ZN11CPopulation25ms_nTotalCarPassengerPedsE", "I"),
    ("peds: ms_nNumDummy",           "_ZN11CPopulation12ms_nNumDummyE",                "I"),
    ("peds: ms_nNumCop",             "_ZN11CPopulation10ms_nNumCopE",                  "I"),
    ("peds: ms_nNumCivMale",         "_ZN11CPopulation14ms_nNumCivMaleE",              "I"),
    # The fill burst (VICE-126): the countdown that arms the boot burst, the
    # target the last burst frame aimed at, and what the burst has spent so far.
    # The "since boot" pair accumulates over every run of the session; the
    # "ended" rows count the runs by the reason they ended (VICE-146): the
    # target's stop line, the full car budget (cars only: the pedestrian frame
    # has no budget line), the give-up bound (a run of frames without a
    # placement), or the window spent.
    ("peds: burst countdown",        "_ZN11CPopulation24m_CountDownToPedsAtStartE",    "b"),
    ("peds: burst target",           "_ZN11CPopulation19ms_fFillBurstTargetE",         "f"),
    ("peds: burst attempts",         "_ZN11CPopulation21ms_nFillBurstAttemptsE",       "I"),
    ("peds: burst placed",           "_ZN11CPopulation19ms_nFillBurstPlacedE",         "I"),
    ("peds: burst attempts since boot", "_ZN11CPopulation26ms_nFillBurstTotalAttemptsE", "I"),
    ("peds: burst placed since boot", "_ZN11CPopulation24ms_nFillBurstTotalPlacedE",   "I"),
    ("peds: burst ended by line",    "_ZN11CPopulation22ms_nFillBurstEndedLineE",      "I"),
    ("peds: burst ended by stalls",  "_ZN11CPopulation24ms_nFillBurstEndedStallsE",    "I"),
    ("peds: burst ended by window",  "_ZN11CPopulation24ms_nFillBurstEndedWindowE",    "I"),
    ("cars: burst countdown",        "_ZN8CCarCtrl22CountDownToCarsAtStartE",          "b"),
    ("cars: burst target",           "_ZN8CCarCtrl19ms_fFillBurstTargetE",             "f"),
    ("cars: burst attempts",         "_ZN8CCarCtrl21ms_nFillBurstAttemptsE",           "I"),
    ("cars: burst placed",           "_ZN8CCarCtrl19ms_nFillBurstPlacedE",             "I"),
    ("cars: burst attempts since boot", "_ZN8CCarCtrl26ms_nFillBurstTotalAttemptsE",   "I"),
    ("cars: burst placed since boot", "_ZN8CCarCtrl24ms_nFillBurstTotalPlacedE",       "I"),
    ("cars: burst ended by line",    "_ZN8CCarCtrl22ms_nFillBurstEndedLineE",          "I"),
    ("cars: burst ended by budget",  "_ZN8CCarCtrl24ms_nFillBurstEndedBudgetE",        "I"),
    ("cars: burst ended by stalls",  "_ZN8CCarCtrl24ms_nFillBurstEndedStallsE",        "I"),
    ("cars: burst ended by window",  "_ZN8CCarCtrl24ms_nFillBurstEndedWindowE",        "I"),
    ("stream: ms_numVehiclesLoaded", "_ZN10CStreaming20ms_numVehiclesLoadedE",        "i"),
    ("stream: ms_numPedsLoaded",     "_ZN10CStreaming16ms_numPedsLoadedE",             "i"),
    ("stream: desiredNumVehiclesLoaded", "desiredNumVehiclesLoaded",                   "i"),
    ("generators: CurrentActiveCount", "_ZN17CTheCarGenerators18CurrentActiveCountE",  "I"),
    ("generators: NumOfCarGenerators", "_ZN17CTheCarGenerators18NumOfCarGeneratorsE",  "I"),
    ("render: ms_lodDistScale",      "_ZN9CRenderer15ms_lodDistScaleE",                "f"),
    ("render: ms_nNoOfVisibleEntities", "_ZN9CRenderer23ms_nNoOfVisibleEntitiesE",     "i"),
    ("weather: m_fCurrentFarClip",   "_ZN10CTimeCycle17m_fCurrentFarClipE",            "f"),
    ("weather: gbNewRenderer",       "gbNewRenderer",                                  "B"),
]

# The pools (CPool<T>: entries*, flags*, size, allocPtr). The used count is the
# number of flags without POOLFLAG_ISFREE; the flags are read in one go. The
# second column is the enum name of the size in src/core/config.h, checked
# against the size the pool object carries.
POOLS = [
    ("pool: Peds",            "_ZN6CPools11ms_pPedPoolE",           "NUMPEDS"),
    ("pool: Vehicles",        "_ZN6CPools15ms_pVehiclePoolE",       "NUMVEHICLES"),
    ("pool: Dummies",         "_ZN6CPools13ms_pDummyPoolE",         "NUMDUMMIES"),
    ("pool: Objects",         "_ZN6CPools14ms_pObjectPoolE",        "NUMOBJECTS"),
    ("pool: PtrNodes",        "_ZN6CPools15ms_pPtrNodePoolE",       "NUMPTRNODES"),
    ("pool: EntryInfoNodes",  "_ZN6CPools21ms_pEntryInfoNodePoolE", "NUMENTRYINFOS"),
    ("pool: Buildings",       "_ZN6CPools16ms_pBuildingPoolE",      "NUMBUILDINGS"),
]

# The fill burst (VICE-126): the state of the two work windows, SFillBurst
# (two ints: the frames left and the stalls). Both fields read 0 while the burst
# is idle; across an interior exit or a cutscene end they show the window
# spending itself (the frames fall) and the counters above show what it placed.
BURSTS = [
    ("burst peds: frames left",  "_ZN11CPopulation12ms_FillBurstE", 0),
    ("burst peds: stalls",       "_ZN11CPopulation12ms_FillBurstE", 4),
    ("burst cars: frames left",  "_ZN8CCarCtrl12ms_FillBurstE",     0),
    ("burst cars: stalls",       "_ZN8CCarCtrl12ms_FillBurstE",     4),
]

# The near bubble (VICE-127). The two catchable-zone counters are the acceptance
# of the pop-in ticket and must stay 0. They were renamed from the frustum pair
# at VICE-146, when they took the narrower meaning: the catchable zone - in the
# frustum nearer than the in-view band minimum - because the band at or beyond
# the minimum is allowed (the stock game's on-screen spawn band; a creation is
# covered by the car's own alpha ramp, a removal by the fade-out of the
# despawn); the band rows are its creations and removals, expected to rise with
# the traffic. The fade rows are the catchable events a fade covered (the fill
# burst's work, exempt by the screen); the hidden and refused rows show the rule
# working, and the hidden ones subdivide by the arm (the occluded hint and the
# close blind spot, both confirmed by the line-of-sight ray: the "ray creations"
# row is the close blind spot's, the occluder rows are the hint's); the rays row
# is the frame's budget (4) and the total follows it; the fill is the traffic
# inside the player's disc, the other fill is the rest of the car target (the
# cars outside the traffic spawner), and the target is the share of the car
# budget the disc asks for. The other creations (and the in-view ones of them)
# are the scripted, set-piece, road block, car park, garage and cheat cars: the
# ticket's counters carry them apart so a scripted car that appears in view is
# visible here and never inside the traffic's rows; its in-view row is the wider
# catchable set (the band included), informational by design. The valve row is
# the pool's own last line.
NEAR_BUBBLE = [
    ("bubble: catchable-zone creations", "_ZN8CCarCtrl32ms_nNearBubbleCatchableCreationsE", "I"),
    ("bubble: catchable-zone removals",  "_ZN8CCarCtrl31ms_nNearBubbleCatchableRemovalsE",  "I"),
    ("bubble: band creations",       "_ZN8CCarCtrl27ms_nNearBubbleBandCreationsE",      "I"),
    ("bubble: band removals",        "_ZN8CCarCtrl26ms_nNearBubbleBandRemovalsE",       "I"),
    ("bubble: fade creations",       "_ZN8CCarCtrl27ms_nNearBubbleFadeCreationsE",      "I"),
    ("bubble: fade removals",        "_ZN8CCarCtrl26ms_nNearBubbleFadeRemovalsE",       "I"),
    ("bubble: hidden creations",     "_ZN8CCarCtrl29ms_nNearBubbleHiddenCreationsE",    "I"),
    ("bubble: hidden removals",      "_ZN8CCarCtrl28ms_nNearBubbleHiddenRemovalsE",     "I"),
    ("bubble: occluder creations",   "_ZN8CCarCtrl31ms_nNearBubbleOccluderCreationsE",   "I"),
    ("bubble: occluder removals",    "_ZN8CCarCtrl30ms_nNearBubbleOccluderRemovalsE",    "I"),
    ("bubble: ray creations",        "_ZN8CCarCtrl26ms_nNearBubbleRayCreationsE",        "I"),
    ("bubble: refused",              "_ZN8CCarCtrl21ms_nNearBubbleRefusedE",            "I"),
    ("bubble: bypassed",             "_ZN8CCarCtrl22ms_nNearBubbleBypassedE",           "I"),
    ("bubble: rays this frame",      "_ZN8CCarCtrl18ms_nNearBubbleRaysE",               "I"),
    ("bubble: rays total",           "_ZN8CCarCtrl23ms_nNearBubbleRaysTotalE",          "I"),
    ("bubble: rays left this frame", "_ZN8CCarCtrl22ms_nNearBubbleRaysLeftE",           "i"),
    ("bubble: fill (traffic in 90 m)", "_ZN8CCarCtrl18ms_nNearBubbleFillE",             "I"),
    ("bubble: other fill (rest)",    "_ZN8CCarCtrl23ms_nNearBubbleOtherFillE",          "I"),
    ("bubble: fill target",          "_ZN8CCarCtrl20ms_fNearBubbleTargetE",             "f"),
    ("bubble: other creations",      "_ZN8CCarCtrl28ms_nNearBubbleOtherCreationsE",     "I"),
    ("bubble: other creations in view", "_ZN8CCarCtrl34ms_nNearBubbleOtherCreationsInViewE", "I"),
    ("bubble: valve removals",       "_ZN8CCarCtrl27ms_nNearBubbleValveRemovalsE",      "I"),
]

CONFIG = "src/core/config.h"


def find_pid():
    out = subprocess.run(["pgrep", "-x", "reVC"], capture_output=True, text=True).stdout.split()
    if not out:
        raise SystemExit("no reVC process found (start one, or pass a pid)")
    return int(out[0])


def exe_base(pid):
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            parts = line.split(None, 5)
            if len(parts) >= 6 and parts[2] == "00000000" and parts[5].rstrip().split("/")[-1].startswith("reVC"):
                return int(parts[0].split("-")[0], 16)
    raise SystemExit("the executable base is not in /proc/<pid>/maps")


def nm_offsets(exe):
    out = subprocess.run(["nm", exe], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("nm failed: " + out.stderr)
    table = {}
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] != "U":
            table[parts[2]] = int(parts[0], 16)
    return table


def config_sizes():
    """The pool sizes of src/core/config.h, so the scan cannot mis-size a pool."""
    sizes = {}
    try:
        text = open(CONFIG).read()
    except OSError:
        return sizes
    for match in re.finditer(r"\b([A-Z][A-Z0-9_]*)\s*=\s*(\d+)", text):
        sizes[match.group(1)] = int(match.group(2))
    return sizes


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else find_pid()
    exe = sys.argv[2] if len(sys.argv) > 2 else f"/proc/{pid}/exe"
    base = exe_base(pid)
    sym = nm_offsets(exe)
    mem = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
    sizes = config_sizes()

    def read(off, fmt):
        return struct.unpack(fmt, os.pread(mem, struct.calcsize(fmt), base + off))[0]

    print(f"reVC pid {pid}, base 0x{base:x}, image {exe}")
    if not sizes:
        print(f"  {CONFIG} is not readable from here (run from the repository root):")
        print("  the pool sizes come from the pool objects themselves, unchecked")
    values = {}
    for label, mangled, fmt in SYMBOLS:
        off = sym.get(mangled)
        if off is None:
            print(f"  {label:44s} symbol missing")
            continue
        v = read(off, fmt)
        values[label.split(': ')[1]] = v
        if fmt == "f":
            print(f"  {label:44s} {v:.4f}")
        else:
            print(f"  {label:44s} {v}")

    print()
    for label, mangled, offset in BURSTS:
        off = sym.get(mangled)
        if off is None:
            print(f"  {label:44s} symbol missing")
            continue
        v = read(off + offset, "i")
        print(f"  {label:44s} {v}")

    print()
    for label, mangled, fmt in NEAR_BUBBLE:
        off = sym.get(mangled)
        if off is None:
            print(f"  {label:44s} symbol missing")
            continue
        v = read(off, fmt)
        values[label.split(': ')[1]] = v
        if fmt == "f":
            print(f"  {label:44s} {v:.4f}")
        else:
            print(f"  {label:44s} {v}")

    print()
    for label, mangled, enum_name in POOLS:
        off = sym.get(mangled)
        if off is None:
            print(f"  {label:44s} symbol missing")
            continue
        ptr = read(off, "Q")
        if ptr == 0:
            print(f"  {label:44s} not allocated")
            continue
        entries, flags, psize, allocptr = struct.unpack("<QQii", os.pread(mem, 24, ptr))
        data = os.pread(mem, psize, flags)
        used = sum(1 for b in data if not (b & 0x80))
        note = ""
        if enum_name in sizes and sizes[enum_name] != psize:
            note = f"  DRIFT: config.h says {enum_name} = {sizes[enum_name]}"
        elif enum_name in sizes:
            note = f"  {enum_name} = {psize} in config.h"
        print(f"  {label:44s} used {used:6d} / {psize:6d}  free {psize - used}{note}")

    # The ring scale: ApplyPopulationCaps gives min(45 x the car density x
    # scale^2, 2400), so before the top binds, scale = sqrt(budget / (45 x the
    # density). The traffic uses the same scale, so one derivation covers both.
    print()
    car_budget = values.get("MaxNumberOfCarsInUse", 0)
    density = values.get("CarNumberMultiplier", 0.0)
    if car_budget >= 2400:
        print("  the ring scale: at or above 3.96 (the car budget is at its top)")
    elif car_budget > 0 and density > 0:
        print(f"  the ring scale: about {(car_budget / (45.0 * density)) ** 0.5:.3f}")
    else:
        print("  the ring scale: not derivable (a symbol is missing)")

    # The acceptance of the near bubble (VICE-127), as one line. The two
    # catchable-zone counters read the zone of VICE-146: in the frustum nearer
    # than the in-view band minimum, where the player could catch the event.
    # The band counters are reported next to them: they are allowed and expected
    # to rise as the traffic fills; they do not fail the verdict.
    print()
    creations = values.get("catchable-zone creations")
    removals = values.get("catchable-zone removals")
    valve = values.get("valve removals")
    band_creations = values.get("band creations")
    band_removals = values.get("band removals")
    fill = values.get("fill (traffic in 90 m)")
    target = values.get("fill target")
    if creations is None or removals is None or valve is None:
        print("  the near bubble: a symbol is missing (the acceptance cannot be read)")
    else:
        # The verdict of the traffic: a catchable-zone creation or deletion, or
        # an in-view deletion from the pool's own last line, fails it.
        verdict = "PASS" if creations == 0 and removals == 0 and valve == 0 else "FAIL"
        band = ""
        if band_creations is not None and band_removals is not None:
            band = f", band creations {band_creations}, band removals {band_removals}"
        print(f"  the near bubble: catchable-zone creations {creations}, "
              f"catchable-zone removals {removals}, valve removals {valve}{band} -> {verdict}")
    other = values.get("other fill (rest)")
    if fill is not None and target is not None:
        if other is None:
            print(f"  the near bubble: fill {fill} against the target {target:.1f}")
        else:
            print(f"  the near bubble: fill {fill} + other {other} against the target {target:.1f}")
    occluder = values.get("occluder creations")
    ray = values.get("ray creations")
    if occluder is not None and ray is not None:
        print(f"  the near bubble: the hidden creations by arm: occluded hint {occluder}, "
              f"close blind spot {ray}")
    other_creations = values.get("other creations")
    other_in_view = values.get("other creations in view")
    if other_creations is not None and other_in_view is not None:
        # The scripted cars are their own category: a road block or a cheat car
        # in view is expected, so this row informs and does not fail the verdict.
        print(f"  the near bubble: the cars outside the spawner {other_creations}, "
              f"of them in view {other_in_view} (their own row, not the traffic's)")


if __name__ == "__main__":
    main()
