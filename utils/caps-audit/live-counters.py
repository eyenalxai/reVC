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
give-up bound (a run of frames without a placement), the window spent, or the
boot wait bound (the world never became ready: no target, or no models of the
first seconds). A burst waits for its world on that own bound (the boot-wait
state row), without spending its window and without burning the stall bound,
so a boot run is neither burned on a zero target nor disarmed silently.

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

The far layer rows (VICE-128) are the band between the near traffic's despawn
and the fog: the count against the cap of the frame (the cap is the band's, so
it moves with the draw distance and the weather; zero is a closed band, not a
fault), the layer's churn (creations and removals), the three switches
(promotions - with the driverless safeties the reach's seal forces - and
hand-overs) and the refusals by cause (an absent model is the streaming; a pool
reserve is the layer's own promise to the rest of the game). The "in reach" row
is the layer's own acceptance gauge and must stay 0. The updates row is the
stagger's work: about a third of the layer a frame, so it grows by the cap each
three frames.

The standable cell rows (VICE-154) are the off-network pedestrian placement's
own counters: the cells the lazy build spent and queued, the queue's and the
cache's drops (a nearer cell displaced a farther one, or the cache evicted the
farthest entry), the positions the spawns requested and how they were answered
(a validated spot, or a built cell with nothing on it), and the probes the
builds ran with their verdicts - the rejects by reason (no ground, water, the
level, a roof, the clearance, the slope). The counters are cumulative since the
boot, so a soak reads the growth of the builds and the mix of the reasons.
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
    # The off-network spawns (VICE-122, the standable cells of VICE-154): the
    # created pedestrians that stood on a cell and the ones the node path
    # took (the realised share the target ratio is held against), and the
    # strays the cull took by their reason.
    ("peds: off-network spawns",     "_ZN11CPopulation20ms_nOffNetworkSpawnsE",        "I"),
    ("peds: network spawns",         "_ZN11CPopulation17ms_nNetworkSpawnsE",           "I"),
    ("peds: off-network stuck",      "_ZN11CPopulation19ms_nOffNetworkStuckE",         "I"),
    ("peds: off-network drowned",    "_ZN11CPopulation21ms_nOffNetworkDrownedE",       "I"),
    # The fill burst (VICE-126): the countdown that arms the boot burst, the
    # target the last burst frame aimed at, and what the burst has spent so far.
    # The "since boot" pair accumulates over every run of the session; the
    # "ended" rows count the runs by the reason they ended (VICE-146): the
    # target's stop line, the full car budget (cars only: the pedestrian frame
    # has no budget line), the give-up bound (a run of frames without a
    # placement), the window spent, or - pedestrian only - the boot wait
    # bound (a boot whose world never became ready).
    ("peds: burst countdown",        "_ZN11CPopulation24m_CountDownToPedsAtStartE",    "b"),
    ("peds: burst target",           "_ZN11CPopulation19ms_fFillBurstTargetE",         "f"),
    ("peds: burst attempts",         "_ZN11CPopulation21ms_nFillBurstAttemptsE",       "I"),
    ("peds: burst placed",           "_ZN11CPopulation19ms_nFillBurstPlacedE",         "I"),
    ("peds: burst attempts since boot", "_ZN11CPopulation26ms_nFillBurstTotalAttemptsE", "I"),
    ("peds: burst placed since boot", "_ZN11CPopulation24ms_nFillBurstTotalPlacedE",   "I"),
    ("peds: burst ended by line",    "_ZN11CPopulation22ms_nFillBurstEndedLineE",      "I"),
    ("peds: burst ended by stalls",  "_ZN11CPopulation24ms_nFillBurstEndedStallsE",    "I"),
    ("peds: burst ended by window",  "_ZN11CPopulation24ms_nFillBurstEndedWindowE",    "I"),
    ("peds: burst ended by boot wait", "_ZN11CPopulation26ms_nFillBurstEndedBootWaitE", "I"),
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
# (three ints: the frames left, the stalls, and the boot waits of the fix
# round). The fields read 0 while the burst is idle; across an interior exit or
# a cutscene end they show the window spending itself (the frames fall) and the
# counters above show what it placed. The boot wait row is non-zero only while
# a pedestrian burst waits for its world (the zero target, or the models of the
# first seconds; the car target and budget have positive floors and its caller
# does not use the boot wait, so the row is the pedestrian burst's).
BURSTS = [
    ("burst peds: frames left",  "_ZN11CPopulation12ms_FillBurstE", 0),
    ("burst peds: stalls",       "_ZN11CPopulation12ms_FillBurstE", 4),
    ("burst peds: boot waits",   "_ZN11CPopulation12ms_FillBurstE", 8),
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

# The far traffic layer (VICE-128). The layer holds the cars of the band between
# the near traffic's despawn and the fog; the count and the cap are the live
# pair to read (the cap is the band's, so it moves with the draw distance and
# the weather), the creations/removals are its churn, the promotions, the safety
# promotions and the hand-overs are the three switches (a far car that the
# player came near becomes traffic - with its driver, or without one when the
# ped reserve refused it and the player is inside the reach - and a traffic car
# the cull would delete keeps driving as a far one), and the refusals split by
# cause: a refused model is the streaming (the layer builds a car only with a
# model the streamer holds), a refused pool is a reserve (the layer leaves the
# pool room for the rest of the game, and a promotion's driver comes out of it).
# The "in reach" row is the acceptance gauge: the far cars inside the player's
# reach after a frame, which must stay 0. The updates are the cars the stagger
# moved (about a third of the layer a frame); the attempts are the frames that
# tried to create (each attempt tries up to two sites). Since VICE-133 the
# stagger's simulation runs on the layer's worker thread: the worker cycles row
# is its cycles (the rate is the row over the elapsed wall time), the worker lost
# row counts every time the worker's list and the layer's disagreed about a car
# (a command it could not apply, or a pose whose slot did not match) and must
# stay 0, the short curves row counts the pick's clamped curves (the engine's
# old "fout" case, which the worker counts instead of printing), and the
# catch-ups row counts the pending cycles the applicator had to run inline
# because many frames in a row never handed their cycle over (the command ring's
# water mark): 0 in normal play. The refused poses row counts the poses the
# world's own rule rejected (a position or a direction that is not finite, or an
# x/y outside the sector grid; VICE-168): the pose is never written and the car
# leaves the layer, so a healthy layer reads 0.
FAR_TRAFFIC = [
    ("far: cars",                    "_ZN11CFarTraffic18ms_nFarTrafficCarsE",           "I"),
    ("far: cap",                     "_ZN11CFarTraffic17ms_nFarTrafficCapE",            "I"),
    ("far: creations",               "_ZN11CFarTraffic23ms_nFarTrafficCreationsE",      "I"),
    ("far: removals",                "_ZN11CFarTraffic22ms_nFarTrafficRemovalsE",       "I"),
    ("far: promotions",              "_ZN11CFarTraffic24ms_nFarTrafficPromotionsE",     "I"),
    ("far: safety promotions",       "_ZN11CFarTraffic30ms_nFarTrafficSafetyPromotionsE", "I"),
    ("far: demotions (hand-overs)",  "_ZN11CFarTraffic23ms_nFarTrafficDemotionsE",      "I"),
    ("far: in reach (must be 0)",    "_ZN11CFarTraffic21ms_nFarTrafficInReachE",        "I"),
    ("far: refused",                 "_ZN11CFarTraffic21ms_nFarTrafficRefusedE",        "I"),
    ("far: refused no site",         "_ZN11CFarTraffic27ms_nFarTrafficRefusedNoSiteE",  "I"),
    ("far: refused band",            "_ZN11CFarTraffic25ms_nFarTrafficRefusedBandE",    "I"),
    ("far: refused gate",            "_ZN11CFarTraffic25ms_nFarTrafficRefusedGateE",    "I"),
    ("far: refused model",           "_ZN11CFarTraffic26ms_nFarTrafficRefusedModelE",   "I"),
    ("far: refused pool",            "_ZN11CFarTraffic25ms_nFarTrafficRefusedPoolE",    "I"),
    ("far: attempts",                "_ZN11CFarTraffic22ms_nFarTrafficAttemptsE",       "I"),
    ("far: updates (stagger)",       "_ZN11CFarTraffic21ms_nFarTrafficUpdatesE",        "I"),
    ("far: worker cycles",           "_ZN11CFarTraffic26ms_nFarTrafficWorkerCyclesE",   "I"),
    ("far: worker lost (must be 0)", "_ZN11CFarTraffic24ms_nFarTrafficWorkerLostE",     "I"),
    ("far: short curves",            "_ZN11CFarTraffic25ms_nFarTrafficShortCurvesE",    "I"),
    ("far: worker catch-ups",        "_ZN11CFarTraffic22ms_nFarTrafficCatchUpsE",       "I"),
    ("far: refused poses (must be 0)", "_ZN11CFarTraffic26ms_nFarTrafficRefusedPosesE", "I"),
]

# The pedestrian standable cells (VICE-154). The store holds the truth
# (CPedSites::ms_cells); the facade mirrors its stats into these plain statics
# after every pump and every request, so the rig can name them. The rows are
# cumulative since the last reset (a boot, a restart or a save load:
# CPopulation::Initialise resets the store): the cells built and queued, the
# queue's and the cache's drops (the queue gives up its farthest cell for a
# nearer one; the cache evicts the farthest entry for a nearer build, or
# drops an entry a request found stale at another level), the positions the
# off-network spawns requested and how they were answered (a spot, a dead
# cell), the probes the builds ran and their verdicts - the rejects by their
# reason, the ticket's telemetry. The off-network counters of the population
# above them are the other side of the same story: the spawns that stood on a
# cell (ms_nOffNetworkSpawns) against the ones the node path took
# (ms_nNetworkSpawns).
PED_SITES = [
    ("ped sites: cells built",       "_ZN9CPedSites14ms_nCellsBuiltE",       "I"),
    ("ped sites: cells queued",      "_ZN9CPedSites15ms_nCellsQueuedE",      "I"),
    ("ped sites: cells dropped",     "_ZN9CPedSites16ms_nCellsDroppedE",     "I"),
    ("ped sites: cells evicted",     "_ZN9CPedSites16ms_nCellsEvictedE",     "I"),
    ("ped sites: requests",          "_ZN9CPedSites12ms_nRequestsE",         "I"),
    ("ped sites: requests answered", "_ZN9CPedSites20ms_nRequestsAnsweredE", "I"),
    ("ped sites: requests dead",     "_ZN9CPedSites16ms_nRequestsDeadE",     "I"),
    ("ped sites: probes",            "_ZN9CPedSites10ms_nProbesE",           "I"),
    ("ped sites: rejects no ground", "_ZN9CPedSites19ms_nRejectsNoGroundE",  "I"),
    ("ped sites: rejects water",     "_ZN9CPedSites16ms_nRejectsWaterE",     "I"),
    ("ped sites: rejects level",     "_ZN9CPedSites16ms_nRejectsLevelE",     "I"),
    ("ped sites: rejects roof",      "_ZN9CPedSites15ms_nRejectsRoofE",      "I"),
    ("ped sites: rejects clearance", "_ZN9CPedSites20ms_nRejectsClearanceE", "I"),
    ("ped sites: rejects slope",     "_ZN9CPedSites16ms_nRejectsSlopeE",     "I"),
]
# The spawn service (VICE-150): the one SSpawnServiceTelemetry instance the
# engine fills. The symbol is the struct; the rows read its fields at the
# offsets pinned by the static_asserts in SpawnService.cpp, so a field move
# breaks the build before it can misread a live row. The arrays are in the
# purpose / kind / reason enumeration order.
#
# The requests are the service's intake, by purpose (ambient, parked,
# responder, police, mission, scenario, special, debug) and by kind (road-site,
# explicit). The refusals are every refusal event by reason (no site,
# occupancy, visibility, zone, readiness, budget, pool, reachability): a
# request's own gate, a candidate the plan dropped, or an attempt the
# primitive refused - refusals after planning are counted, not hidden. The
# created and removed rows are the placements by purpose; the covered-fill row
# is the fade's own creations. The readiness rows are the requests deferred
# for models and the milliseconds they spent waiting. The plan rows are the
# planner's cost: the calls, the candidates examined, the placements returned
# and the total microseconds (us/call is the column to read against a frame's
# budget; the measured harness rate is about 5 us a call over 197 candidates).
# The conservation rows are the director's check (the cell target sum against
# the global target) and its breach row; the defect rows are the primitive's
# ground casts that found nothing, the air offsets over its 7 m guard and the
# water casts that failed. The rows are cumulative since CCarCtrl's Init or
# ReInit (the service restarts its counters with the world).
SPAWN_SERVICE_STRUCT = "_ZN13CSpawnService12ms_TelemetryE"
SPAWN_SERVICE = [
    ("spawn: requests ambient",         0,                          "I"),
    ("spawn: requests parked",          4,                          "I"),
    ("spawn: requests responder",       8,                          "I"),
    ("spawn: requests police",          12,                         "I"),
    ("spawn: requests mission",         16,                         "I"),
    ("spawn: requests scenario",        20,                         "I"),
    ("spawn: requests special",         24,                         "I"),
    ("spawn: requests debug",           28,                         "I"),
    ("spawn: requests road-site",       32,                         "I"),
    ("spawn: requests explicit",        36,                         "I"),
    ("spawn: refused no site",          40 + 1 * 4,                 "I"),
    ("spawn: refused occupancy",        40 + 2 * 4,                 "I"),
    ("spawn: refused visibility",       40 + 3 * 4,                 "I"),
    ("spawn: refused zone",             40 + 4 * 4,                 "I"),
    ("spawn: refused readiness",        40 + 5 * 4,                 "I"),
    ("spawn: refused budget",           40 + 6 * 4,                 "I"),
    ("spawn: refused pool",             40 + 7 * 4,                 "I"),
    ("spawn: refused reachability",     40 + 8 * 4,                 "I"),
    ("spawn: created ambient",          76,                         "I"),
    ("spawn: created mission",          76 + 4 * 4,                 "I"),
    ("spawn: removed ambient",          108,                        "I"),
    ("spawn: covered-fill creations",   140,                        "I"),
    ("spawn: readiness waits",          144,                        "I"),
    ("spawn: readiness wait ms",        148,                        "I"),
    ("spawn: plan calls",               184,                        "I"),
    ("spawn: plan candidates",          188,                        "I"),
    ("spawn: plan placements",          192,                        "I"),
    ("spawn: plan micros total",        200,                        "Q"),
    ("spawn: conservation cell sum",    208,                        "I"),
    ("spawn: conservation global",      212,                        "I"),
    ("spawn: conservation breaches",    216,                        "I"),
    ("spawn: ground rejections",        220,                        "I"),
    ("spawn: air offset rejections",    224,                        "I"),
    ("spawn: water rejections",         228,                        "I"),
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
    for label, mangled, fmt in PED_SITES:
        off = sym.get(mangled)
        if off is None:
            print(f"  {label:44s} symbol missing")
            continue
        print(f"  {label:44s} {read(off, fmt)}")

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
    for label, mangled, fmt in FAR_TRAFFIC:
        off = sym.get(mangled)
        if off is None:
            print(f"  {label:44s} symbol missing")
            continue
        v = read(off, fmt)
        # The far rows live under their own key space: the near bubble has a
        # "refused" row too, and the two must not collide in `values`.
        values["far " + label.split(': ')[1]] = v
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

    # The spawn service (VICE-150): the one telemetry struct's fields, at the
    # offsets SpawnService.cpp pins. The counters restart with CCarCtrl's Init
    # and ReInit, so the rows are the session's own (a save load starts them
    # over with the world it restores).
    print()
    spawn_base = sym.get(SPAWN_SERVICE_STRUCT)
    if spawn_base is None:
        print("  the spawn service: symbol missing")
    else:
        for label, offset, fmt in SPAWN_SERVICE:
            v = read(spawn_base + offset, fmt)
            values["spawn " + label.split(': ')[1]] = v
            print(f"  {label:44s} {v}")

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

    # The spawn service's intake, as one line: what the service was asked for,
    # what it refused (by reason) and what it placed. The plan rows are the
    # cost: the last column is the average microseconds a plan call spent.
    spawn_requests = values.get("spawn requests ambient")
    spawn_created = values.get("spawn created ambient")
    spawn_calls = values.get("spawn plan calls")
    if spawn_requests is None or spawn_created is None or spawn_calls is None:
        print("  the spawn service: a symbol is missing (the intake cannot be read)")
    else:
        refusals = sum(values.get("spawn refused " + r, 0) for r in (
            "no site", "occupancy", "visibility", "zone", "readiness",
            "budget", "pool", "reachability"))
        per_call = (values.get("spawn plan micros total", 0) / spawn_calls) if spawn_calls else 0.0
        print(f"  the spawn service: ambient requests {spawn_requests}, "
              f"created {spawn_created}, refusals {refusals}, "
              f"plan {per_call:.1f} us a call over "
              f"{values.get('spawn plan candidates', 0)} candidates")

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

    # The far traffic layer (VICE-128), as a few lines. The layer fills the band
    # between the near traffic and the fog; a cap of zero is a closed band (the
    # stock draw distance or a storm), not a fault.
    print()
    far_cars = values.get("far cars")
    far_cap = values.get("far cap")
    far_creations = values.get("far creations")
    far_removals = values.get("far removals")
    far_promotions = values.get("far promotions")
    far_safety = values.get("far safety promotions")
    far_demotions = values.get("far demotions (hand-overs)")
    far_in_reach = values.get("far in reach (must be 0)")
    far_refused = values.get("far refused")
    far_updates = values.get("far updates (stagger)")
    if far_cars is None or far_cap is None:
        print("  the far layer: a symbol is missing (the layer cannot be read)")
    else:
        note = "closed band" if far_cap == 0 else "open band"
        if far_cap and far_cars > far_cap:
            # A count above the cap is the intended drain after a drop of the
            # band (a draw distance move or a weather change): the creations
            # stop at the cap, the removals take FAR_TRAFFIC_REMOVE_ATTEMPTS a
            # frame, so the count falls back to the cap over the next frames.
            note = f"over the cap by {far_cars - far_cap}, draining the drop"
        print(f"  the far layer: {far_cars} cars against the cap {far_cap} ({note})")
    if far_in_reach is not None:
        verdict = "PASS" if far_in_reach == 0 else "FAIL"
        print(f"  the far layer: far cars inside the player's reach {far_in_reach} -> {verdict}")
    if far_creations is not None:
        print(f"  the far layer: creations {far_creations}, removals {far_removals}, "
              f"promotions {far_promotions} (of them without a driver {far_safety}), "
              f"hand-overs {far_demotions}, refused {far_refused}")
    far_no_site = values.get("far refused no site")
    far_band = values.get("far refused band")
    far_gate = values.get("far refused gate")
    if far_no_site is not None:
        # The causes of a refused attempt: the path network could not place at
        # any site (the player over water or at the map's edge), every placement
        # fell outside the band, or the near bubble's gate and the car build
        # refused the in-band ones (a lane already taken is the normal case).
        print(f"  the far layer: the refusals by cause: no site {far_no_site}, "
              f"band {far_band}, gate {far_gate}")
    if far_updates is not None:
        print(f"  the far layer: the stagger moved {far_updates} cars so far "
              f"(about a third of the layer a frame)")
    far_cycles = values.get("far worker cycles")
    far_catchups = values.get("far worker catch-ups")
    far_lost = values.get("far worker lost (must be 0)")
    if far_cycles is not None:
        print(f"  the far layer: the worker ran {far_cycles} cycles "
              f"(the rate is the row over the elapsed time)")
    if far_lost is not None:
        verdict = "PASS" if far_lost == 0 else "FAIL"
        print(f"  the far layer: the worker and the layer disagreed about {far_lost} "
              f"cars -> {verdict}")
    if far_catchups is not None:
        print(f"  the far layer: the applicator ran {far_catchups} catch-up cycles "
              f"(0 in normal play)")


if __name__ == "__main__":
    main()
