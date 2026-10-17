#include "common.h"

#include "FarTraffic.h"

#include "CarCtrl.h"
#include "FarTrafficWorker.h"

#include "Camera.h"
#include "CutsceneMgr.h"
#include "DMAudio.h"
#include "Game.h"
#include "General.h"
#include "PathFind.h"
#include "Ped.h"
#include "Pools.h"
#include "SpawnOccupancy.h"
#include "Streaming.h"
#include "Timer.h"
#include "Vehicle.h"
#include "VisibilityPlugins.h"
#include "World.h"
#include "Zones.h"

/*
 * The engine side of the far traffic layer (VICE-128): the applicator, and -
 * since VICE-133 - the main thread's half of the worker's boundary.
 *
 * The rules, the band, the cap and the switches live in FarTraffic.h; the
 * simulation and the worker live in FarTrafficWorker.h/.cpp. This file fills
 * the rules from the frame (the ring table, the player, the pool), owns the
 * cars and the counters the live rig reads (utils/caps-audit/live-counters.py),
 * and is the **only** place a far car or the world is written.
 *
 * The frame, with the worker (the contract is in FarTrafficWorker.h):
 *
 *	the path snapshot	BuildWorldSnapshot, once, at load, right after
 *				ThePaths.PreparePathData (and BuildNodeGrid).
 *				The script's road toggles and a save load are
 *				copied over on the main thread when the graph's
 *				flag version moves (FarTrafficRefreshWorldFlags).
 *	the top of CGame::Process	CFarTraffic::JoinWorker: the previous
 *				frame's cycle is done and its output is
 *				published. This is the end of the worker's
 *				window: from here to the end of the frame the
 *				worker is idle, so it never runs during
 *				CWorld::Process or the render (a debug build
 *				asserts it).
 *	the car block		CFarTraffic::Process, the applicator:
 *				(a) the published poses are written into the
 *				cars - position, heading, speed, the sector
 *				re-insert, the RW frame, the alpha reveal, the
 *				u-turn (the one decision of the pick that is a
 *				world action);
 *				(b) the gate pass, the reach gauge and the
 *				creation attempts run exactly as VICE-128 wrote
 *				them, and every list change queues its command
 *				(ADD/DROP) for the worker;
 *				(c) the frame's cycle is stamped (the game time
 *				and the frame counter) and marked ready.
 *	the end of Idle		CFarTraffic::RunWorker: the cycle is handed to
 *				the worker (or, in the inline mode, run on the
 *				main thread right there). A frame whose cycle
 *				would do nothing (no commands, no car of the
 *				stagger's share) does not wake the worker at
 *				all.
 *
 * The cars' own facts (the pool slot) stay here: the worker's state is a mirror
 * of the route state, keyed by the slot, and the published poses are checked
 * against the slot before anything is written (a mismatch is counted, and must
 * stay 0 - the acceptance row of the boundary).
 *
 * The switches are the layer's only writes to a car's state, and both of them
 * go through CFarTraffic::ApplyFarState and CFarTraffic::ApplyTrafficState:
 *
 *	promotion	the car reached the band's inner edge: its class becomes
 *			RANDOM_VEHICLE (so it counts in the car budget again and the
 *			spawner can see it), it gets a driver (CVehicle::SetUpDriver,
 *			the spawner's own path), collision, the moving list and its
 *			audio. The matrix, the model, the colours and the clump are
 *			untouched, so the player sees the same car.
 *	hand-over	the near cull is about to delete a traffic car beyond the
 *			despawn and hidden; the layer takes it instead: the driver
 *			is flagged away (the ped is removed on the next CWorld::
 *			Process, while the car is already a viewed shell), the class
 *			becomes FAR_TRAFFIC_VEHICLE, the budget count is given back,
 *			and the far state is applied. The car keeps driving, so the
 *			player sees a car recede into the fog instead of vanishing.
 *
 * The counters (the live rig):
 *
 *	ms_nFarTrafficCars		the layer of this frame
 *	ms_nFarTrafficCap		the cap of this frame, from the band
 *	ms_nFarTrafficCreations		creations (the ticket's count)
 *	ms_nFarTrafficRemovals		removals (the ticket's count)
 *	ms_nFarTrafficPromotions	the switches to a full traffic car
 *	ms_nFarTrafficSafetyPromotions	the switches without a driver (the pool refused)
 *	ms_nFarTrafficDemotions		the cull's hand-overs
 *	ms_nFarTrafficInReach		the acceptance: far cars inside the reach, must stay 0
 *	ms_nFarTrafficRefused		the creation attempts that placed nothing
 *	ms_nFarTrafficRefusedNoSite	those the network could not place at all
 *	ms_nFarTrafficRefusedBand	those whose placements all fell outside the band
 *	ms_nFarTrafficRefusedGate	those whose in-band placements the gate refused
 *	ms_nFarTrafficRefusedModel	those stopped by the streaming
 *	ms_nFarTrafficRefusedPool	those stopped by a pool reserve (or a driver)
 *	ms_nFarTrafficAttempts		the attempts
 *	ms_nFarTrafficUpdates		the car updates the stagger applied
 *	ms_nFarTrafficWorkerCycles	the cycles the worker ran (VICE-133; the
 *					rate is this over the elapsed time)
 *	ms_nFarTrafficWorkerLost	the cycle and the layer's list disagreed
 *					(a command the worker could not apply, or a
 *					pose whose slot did not match): must stay 0
 *	ms_nFarTrafficShortCurves	the pick's clamped curves (the engine's own
 *					"fout" diagnostic, counted instead of printed)
 *	ms_nFarTrafficRefusedPoses	the poses the world could not hold (VICE-168):
 *					the simulation's refusals, plus any pose the
 *					apply refused that the simulation published.
 *					The pose is never written, and the car leaves
 *					the layer without a driver; a healthy layer
 *					reads 0
 *	ms_nFarTrafficCatchUps		the pending cycle the applicator ran inline
 *					because the window between two cycles grew
 *					the command ring towards its water mark (0 in
 *					normal play)
 *
 * The frame's work, exactly:
 *
 *	the apply	the cars of the previous cycle's stagger share: the world
 *			writes of one rail pose each, plus the alpha reveal and the
 *			u-turn.
 *	the gate pass	every car, every frame: a distance, a switch, and - when a
 *			car must go - one gate answer from the frame's own bound of
 *			FAR_TRAFFIC_REMOVE_ATTEMPTS answers. A refused answer spends
 *			the frame's attempt all the same, so the bound is on the
 *			gates the layer asks, not on the cars it removes. A promotion
 *			and a safety promotion are not bounded: a car inside the band
 *			may not wait a frame in which the player could reach it.
 *	the creations	at most FAR_TRAFFIC_CREATE_ATTEMPTS placement scans, only
 *			while the layer is under its cap and the pool holds its
 *			reserves. A refused reserve is counted.
 *	the worker	everything else: the path pick, the curve, the heading and
 *			the stagger's index, off the main thread.
 */

/* The counters. Cumulative, like the fill burst's and the near bubble's; the
 * orchestrator notes them before a drive and reads them after. */
unsigned int CFarTraffic::ms_nFarTrafficCars;
unsigned int CFarTraffic::ms_nFarTrafficCap;
unsigned int CFarTraffic::ms_nFarTrafficCreations;
unsigned int CFarTraffic::ms_nFarTrafficRemovals;
unsigned int CFarTraffic::ms_nFarTrafficPromotions;
unsigned int CFarTraffic::ms_nFarTrafficSafetyPromotions;
unsigned int CFarTraffic::ms_nFarTrafficDemotions;
unsigned int CFarTraffic::ms_nFarTrafficInReach;
unsigned int CFarTraffic::ms_nFarTrafficRefused;
unsigned int CFarTraffic::ms_nFarTrafficRefusedNoSite;
unsigned int CFarTraffic::ms_nFarTrafficRefusedBand;
unsigned int CFarTraffic::ms_nFarTrafficRefusedGate;
unsigned int CFarTraffic::ms_nFarTrafficRefusedModel;
unsigned int CFarTraffic::ms_nFarTrafficRefusedPool;
unsigned int CFarTraffic::ms_nFarTrafficAttempts;
unsigned int CFarTraffic::ms_nFarTrafficUpdates;
unsigned int CFarTraffic::ms_nFarTrafficWorkerCycles;
unsigned int CFarTraffic::ms_nFarTrafficWorkerLost;
unsigned int CFarTraffic::ms_nFarTrafficShortCurves;
unsigned int CFarTraffic::ms_nFarTrafficCatchUps;
unsigned int CFarTraffic::ms_nFarTrafficRefusedPoses;

/*
 * The layer. A compact list of the cars it holds, with the pool slot each one
 * lives in. The slot is what makes a list entry safe: a car the layer forgot
 * about (a cutscene's cleanup, the pool's valve, a load) leaves a stale entry
 * behind, and the entry is validated against the slot before it is ever used:
 * the slot must hold the same pointer and the pointer must still be a far car.
 * A slot that was freed and taken by another far car fails the pointer test,
 * and a slot taken by any other car fails the class test. Nothing else is
 * cached, so a stale entry can never be read.
 *
 * Since VICE-133 the slot is also the key the worker's sim state is matched
 * with: every entry the list gains or loses queues its ADD/DROP for the
 * worker's own array, which is what keeps the two lists in lockstep and makes
 * the published poses' indices mean the same cars on both sides.
 */
struct SFarTrafficEntry
{
	CVehicle *pVehicle;
	int32 slot;
};

static SFarTrafficEntry ms_aFarCars[FAR_TRAFFIC_MAX];
static int32 ms_nFarCars;

/*
 * The path snapshot (VICE-133): the parts of the path graph the worker's
 * simulation reads, copied out of ThePaths on the main thread. It is built once
 * by BuildWorldSnapshot and never written again, except for the node flags,
 * which FarTrafficRefreshWorldFlags copies when the graph's own version moves.
 */
static SFarTrafficWorld ms_farWorld;
static uint32 ms_nFarWorldFlagsVersion;

/* The cycle number of the output block the applicator last applied. A frame
 * whose cycle did nothing (no commands, no car of the stagger's share) is
 * skipped by CFarTrafficWorker::Run and leaves the published block in place, so
 * the block is read out exactly once, by its own cycle number. (The same guard
 * makes a re-entrant Process - a resize callback's extra Idle - harmless.)
 * CFarTrafficWorker::ResetCars zeroes the blocks and this marker together, so a
 * load never reads a stale cycle. */
static uint32 ms_nLastAppliedCycle;

static void FarTrafficRefreshWorldFlags(void);

/* The layer's own gate: the same world state the near spawner refuses to work
 * in. A cutscene owns the camera and the world and clears the cars; an interior
 * has no road around it. */
static bool
FarTrafficIsActive(void)
{
	return !CCutsceneMgr::IsRunning() && !CGame::IsInInterior();
}

static bool
FarTrafficEntryIsLive(const SFarTrafficEntry &entry)
{
	if (entry.slot < 0 || entry.slot >= CPools::GetVehiclePool()->GetSize())
		return false;
	CVehicle *pAtSlot = CPools::GetVehiclePool()->GetSlot(entry.slot);
	return pAtSlot == entry.pVehicle &&
		pAtSlot->VehicleCreatedBy == FAR_TRAFFIC_VEHICLE;
}

/* The live count (the entries whose cars are still there): what the cap reads,
 * without moving the list. A stale entry (a cutscene, the valve, a delete) is
 * dropped at the applicator, where the drop's swap cannot move an entry under
 * the poses the worker published. */
static int32
FarTrafficCountLive(void)
{
	int32 live = 0;
	for (int32 i = 0; i < ms_nFarCars; i++)
		if (FarTrafficEntryIsLive(ms_aFarCars[i]))
			live++;
	return live;
}

/* The ring's water mark: before the layer queues past this, the pending cycle
 * runs inline here and its output is applied, so the window between two cycles
 * (the frames whose cycle the main loop never handed over - an early return
 * from Idle) cannot grow the command queue past the ring. Half the ring is far
 * beyond a real frame: one frame's worst case is a drop of every entry the
 * layer holds plus the create attempts. */
#define FAR_TRAFFIC_QUEUE_WATER (FAR_TRAFFIC_SIM_COMMANDS / 2)

static void FarTrafficApplyOutput(void);

static void
FarTrafficCatchUpIfNeeded(void)
{
	if (CFarTrafficWorker::CommandsPending() < FAR_TRAFFIC_QUEUE_WATER)
		return;
	/* The pending cycle runs now, on the main thread (or on the worker, with
	 * this thread waiting: the catch-up is the same cycle either way), and its
	 * output is applied here. The worker's list and the layer's stay in
	 * lockstep and no command is ever dropped. The one price: the frame's own
	 * cycle runs again at the end of the frame, so the stagger's share of this
	 * one frame moves twice. */
	CFarTraffic::ms_nFarTrafficCatchUps++;
	CFarTrafficWorker::EndCycle();
	if (CFarTrafficWorker::Run()) {
		CFarTrafficWorker::Join();
		FarTrafficApplyOutput();
	}
	CFarTrafficWorker::BeginCycle(CTimer::GetTimeInMilliseconds(), CTimer::GetFrameCounter());
}

/* Drop one entry: the last one takes its place, which keeps the list compact
 * and makes the stagger share a real share of the layer. The worker's own array
 * drops by the same rule (FarTrafficSimRunCycle), so its entry indices stay the
 * layer's. The catch-up first, so the command queue cannot grow past the ring. */
static void
FarTrafficDropEntry(int32 index)
{
	FarTrafficCatchUpIfNeeded();
	CFarTrafficWorker::DropCar(ms_aFarCars[index].slot);
	ms_aFarCars[index] = ms_aFarCars[ms_nFarCars - 1];
	ms_nFarCars--;
}

/* The worker's seed for one car (VICE-133): the route state the simulation must
 * carry on from, read here, on the main thread, and copied - never referenced.
 * The worker's own random stream is seeded by the cycle from the car's identity
 * (the slot and its two seeds). */
static void
FarTrafficSeedSimCar(CVehicle *pVehicle, int32 slot, SFarSimCar *pSim)
{
	memset(pSim, 0, sizeof(*pSim));
	pSim->slot = slot;
	pSim->currentRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	pSim->nextRouteNode = pVehicle->AutoPilot.m_nNextRouteNode;
	pSim->prevRouteNode = pVehicle->AutoPilot.m_nPrevRouteNode;
	pSim->timeEnteredCurve = pVehicle->AutoPilot.m_nTimeEnteredCurve;
	pSim->timeToSpendOnCurrentCurve = pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
	pSim->currentPathNodeInfo = pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	pSim->nextPathNodeInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	pSim->previousPathNodeInfo = pVehicle->AutoPilot.m_nPreviousPathNodeInfo;
	pSim->currentDirection = pVehicle->AutoPilot.m_nCurrentDirection;
	pSim->nextDirection = pVehicle->AutoPilot.m_nNextDirection;
	pSim->previousDirection = pVehicle->AutoPilot.m_nPreviousDirection;
	pSim->currentLane = pVehicle->AutoPilot.m_nCurrentLane;
	pSim->nextLane = pVehicle->AutoPilot.m_nNextLane;
	pSim->routeSeed = pVehicle->m_nRouteSeed;
	pSim->randomSeed = pVehicle->m_randomSeed;
	pSim->bStayInCurrentLevel = pVehicle->AutoPilot.m_bStayInCurrentLevel;
	pSim->bStayInFastLane = pVehicle->AutoPilot.m_bStayInFastLane;
	pSim->maxTrafficSpeed = pVehicle->AutoPilot.m_fMaxTrafficSpeed;
	pSim->heightAboveRoad = pVehicle->GetHeightAboveRoad();
	pSim->forwardX = pVehicle->GetForward().x;
	pSim->forwardY = pVehicle->GetForward().y;
}

/* The write-back of a promotion (VICE-168): the inverse of
 * FarTrafficSeedSimCar. While a car is far the simulation owns its route state
 * (the two links of the curve, the lanes, the directions, the path-node infos,
 * the curve times and the heading), so a car that returns to the near rails
 * hands the near side the state the simulation advanced, not the state the car
 * was seeded with when it became far. The clock is the one that matters: the
 * near rails compile the curve progress from m_nTimeEnteredCurve against
 * CTimer::GetTimeInMilliseconds (CCarRailsEngineView::CurveProgress,
 * CarCtrl.cpp:1471) and the simulation's own clock is the same one (the cycle's
 * `now`), so the copied times are consistent across the switch. A stale clock
 * read a progress in the hundreds and CCarCtrl::DragCarToPoint dragged the car
 * out of the world - the ScanForPedDanger assert of the crash report. */
static void
FarTrafficWriteBackSimCar(CVehicle *pVehicle, const SFarSimCar &sim)
{
	pVehicle->AutoPilot.m_nCurrentRouteNode = sim.currentRouteNode;
	pVehicle->AutoPilot.m_nNextRouteNode = sim.nextRouteNode;
	pVehicle->AutoPilot.m_nPrevRouteNode = sim.prevRouteNode;
	pVehicle->AutoPilot.m_nTimeEnteredCurve = sim.timeEnteredCurve;
	/* The picks floor the curve at 10 ms (CarOnRails.h:419); the same floor on
	 * the way back, so the near rails can never divide by a curve time of
	 * zero. */
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve =
		sim.timeToSpendOnCurrentCurve > 10 ? sim.timeToSpendOnCurrentCurve : 10;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = (uint32)sim.currentPathNodeInfo;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = (uint32)sim.nextPathNodeInfo;
	pVehicle->AutoPilot.m_nPreviousPathNodeInfo = (uint32)sim.previousPathNodeInfo;
	pVehicle->AutoPilot.m_nPreviousDirection = sim.previousDirection;
	pVehicle->AutoPilot.m_nCurrentDirection = sim.currentDirection;
	pVehicle->AutoPilot.m_nNextDirection = sim.nextDirection;
	pVehicle->AutoPilot.m_nCurrentLane = sim.currentLane;
	pVehicle->AutoPilot.m_nNextLane = sim.nextLane;
	/* The heading the simulation last applied. A normal promotion keeps the
	 * matrix, and the last applied pose already wrote this value; a refused
	 * pose never wrote its heading, so this writes one only when it is
	 * finite. */
	if (sim.forwardX >= -FLT_MAX && sim.forwardX <= FLT_MAX &&
	    sim.forwardY >= -FLT_MAX && sim.forwardY <= FLT_MAX) {
		pVehicle->GetForward() = CVector(sim.forwardX, sim.forwardY, 0.0f);
		pVehicle->GetRight() = CVector(sim.forwardY, -sim.forwardX, 0.0f);
	}
}

static void
FarTrafficAddEntry(CVehicle *pVehicle)
{
	if (ms_nFarCars >= FAR_TRAFFIC_MAX)
		return;
	FarTrafficCatchUpIfNeeded();
	SFarSimCar sim;

	ms_aFarCars[ms_nFarCars].pVehicle = pVehicle;
	ms_aFarCars[ms_nFarCars].slot = CPools::GetVehiclePool()->GetJustIndex(pVehicle);
	FarTrafficSeedSimCar(pVehicle, ms_aFarCars[ms_nFarCars].slot, &sim);
	CFarTrafficWorker::AddCar(sim);
	ms_nFarCars++;
}

/* Drop the entries of cars that are gone (a cutscene's cleanup, the pool's
 * valve, a load). Called every frame, and on its own while the layer is
 * paused, so the count the rig reads is always the live one. */
static void
FarTrafficPruneEntries(void)
{
	for (int32 i = 0; i < ms_nFarCars; ) {
		if (FarTrafficEntryIsLive(ms_aFarCars[i]))
			i++;
		else
			FarTrafficDropEntry(i);
	}
}

/*
 * The far state and its inverse. These two functions are the whole of a switch;
 * nothing else in the game writes the flags of a far car.
 */
void
CFarTraffic::ApplyFarState(CVehicle *pVehicle)
{
	/* The static flag first: it is what keeps the car off the moving entity
	 * list, so CWorld::Process never runs its control (no car AI, no physics,
	 * no wheels, no damage), and what takes it off the list when it was on it
	 * (a hand-over of a car that was just driving). GetIsStatic also reads
	 * bIsStaticWaitingForCollision, which belongs to the scripts; a far car is
	 * never one of those. */
	pVehicle->SetIsStatic(true);
	pVehicle->RemoveFromMovingList();
	/* Collision off both ways: ProcessCollisionSectorList skips a body whose
	 * flag is clear, so nothing can hit the car and the car can hit nothing -
	 * the player cannot touch it and the near AI cannot even brake for it
	 * (SlowCarDownForCarsSectorList and the weave scans test the flag). */
	pVehicle->bUsesCollision = false;
	/* The audio entity of a car kilometres away: off. ProcessEntity skips an
	 * entity whose status is false, and the engine sound of a far car is not
	 * part of the layer's price. */
	if (pVehicle->m_audioEntityId >= 0)
		DMAudio.SetEntityStatus(pVehicle->m_audioEntityId, FALSE);
}

void
CFarTraffic::ApplyTrafficState(CVehicle *pVehicle)
{
	pVehicle->SetIsStatic(false);
	pVehicle->bUsesCollision = true;
	if (pVehicle->m_audioEntityId >= 0)
		DMAudio.SetEntityStatus(pVehicle->m_audioEntityId, TRUE);
	/* The car is controlled again: the moving list is what CWorld::Process
	 * walks. AddToMovingList is idempotent. */
	pVehicle->AddToMovingList();
}

/* Turn one far car into a full traffic car, in the frame the gate pass found it
 * at the near band's edge. The switch writes nothing the renderer draws of the
 * car: the matrix, the model, the colours, the status and the clump alpha are
 * the car's own and stay. `bWithDriver` is the normal path; a safety promotion
 * (the pool refused the driver and the player is inside the reach) makes the car
 * real without one, because a far car is a shell the player can drive through
 * and that is not allowed where the player can be. */
static void
FarTrafficPromoteCar(int32 index, bool bWithDriver)
{
	CVehicle *pVehicle = ms_aFarCars[index].pVehicle;

	/* The car joins the traffic budget again. The order is the one
	 * UpdateCarCount reads: the count is given from the new class. */
	pVehicle->VehicleCreatedBy = RANDOM_VEHICLE;
	CCarCtrl::UpdateCarCount(pVehicle, false);
	CFarTraffic::ApplyTrafficState(pVehicle);
	/* The simulation owned the car's route state while it was far: the near
	 * rails must carry on from the state and the clock the simulation left
	 * (VICE-168), not from the state the car was seeded with when it became
	 * far. The worker is idle here - the applicator's window, between the join
	 * and the next hand-over (FarTrafficWorker.h) - and the entry is dropped
	 * below, after the write-back. A failed export is a boundary fault (the
	 * two lists disagreed): it is counted, and the promotion carries on,
	 * because a car at the band's edge may not wait a frame. */
	SFarSimCar sim;
	if (CFarTrafficWorker::ExportCar(index, sim))
		FarTrafficWriteBackSimCar(pVehicle, sim);
	else
		CFarTraffic::ms_nFarTrafficWorkerLost++;
	if (bWithDriver) {
		/* A driver, from the population's own path (CPopulation::AddPedInCar
		 * through CVehicle::SetUpDriver): the pool was checked this frame. The
		 * driver rides the car's reveal: if the car still holds its invisible
		 * frame, the driver's rendering waits for it (CPed::Render tests
		 * bRenderPedInCar; CVehicle::UpdateClumpAlpha releases it at the last
		 * fade step), so a promotion cannot show a driver before its car. */
		CPed *pDriver = pVehicle->SetUpDriver();
		if (pDriver && pDriver->bRenderPedInCar &&
			CVisibilityPlugins::GetClumpAlpha(pVehicle->GetClump()) < 255) {
			pDriver->bRenderPedInCar = false;
			pVehicle->bDriverWaitsForReveal = true;
		}
	}
	/* The car is a fresh traffic car from this frame: the stuck cull may not
	 * chase it before it has driven, exactly as a fresh spawn. */
	pVehicle->AutoPilot.m_nTimeToStartMission = CTimer::GetTimeInMilliseconds();
	if (bWithDriver)
		CFarTraffic::ms_nFarTrafficPromotions++;
	else
		CFarTraffic::ms_nFarTrafficSafetyPromotions++;
	FarTrafficDropEntry(index);
}

/* Delete one car of the layer and its entry. The near bubble's removal counter
 * is told the car was invisible: a deletion of the layer's happens at alpha 0
 * or off screen (the in-view band's cars fade first, VICE-146), so it can never
 * move the catchable-zone acceptance pair. */
static void
FarTrafficDeleteCar(int32 index)
{
	CVehicle *pVehicle = ms_aFarCars[index].pVehicle;

	CSpawnService::CountRemoval(pVehicle->GetPosition(), NEAR_BUBBLE_ENTITY_RADIUS, true);
	CWorld::Remove(pVehicle);
	delete pVehicle;
	CFarTraffic::ms_nFarTrafficRemovals++;
	FarTrafficDropEntry(index);
}

/* The layer's removal of one car: the near bubble's rule first (with the gate's
 * ray refused - a removal of the layer's is a fog or a rear-half one), then the
 * in-view band's own way out (VICE-146). A car the player can see goes the way
 * the near traffic's despawn goes: the fade-out (bFadeOut), and the faded tail
 * is deleted at alpha 0 by the gate pass, exactly where the near cull deletes
 * its own. An invisible or off-screen car goes at once. Returns true when the
 * car is gone (the entry dropped). */
static bool
FarTrafficRemoveCar(int32 index, const CVector &playerPos)
{
	CVehicle *pVehicle = ms_aFarCars[index].pVehicle;

	if (!CSpawnService::AllowsRemoval(pVehicle, playerPos, 0.0f, false))
		return false;
	if (pVehicle->GetIsOnScreen() &&
		CVisibilityPlugins::GetClumpAlpha(pVehicle->GetClump()) > 0) {
		/* The band's removal is the despawn's fade-out (VICE-146): the car
		 * leaves the way the near traffic leaves in view, never by a
		 * deletion. */
		pVehicle->bFadeOut = true;
		return false;
	}
	FarTrafficDeleteCar(index);
	return true;
}

/*
 * The apply (VICE-133): write one published pose into its car. Everything here
 * is a world write and is therefore on the main thread; the arithmetic behind
 * the pose was the worker's. The order is the one FarTrafficUpdateCar had in
 * VICE-128: the far state first (an explosion can un-static a car), then the
 * pick's own world action (the u-turn), then the transform, the speed, the
 * sector re-insert, the RW frame and the alpha reveal.
 *
 * The world's rule comes before all of it (VICE-168): a pose the sector grid
 * cannot hold is refused, whatever producer wrote it (the inline mode, a stale
 * block, the catch-up path), and the return value tells the caller to recover
 * the car. The recovery runs after the block's loop (FarTrafficApplyOutput):
 * it promotes the car, and the promotion's drop swaps the last entry into the
 * hole, which must not happen under a pose the loop has not applied yet.
 */
static bool
FarTrafficApplyPose(const SFarSimPose &pose)
{
	if (pose.index >= ms_nFarCars || ms_aFarCars[pose.index].slot != pose.slot) {
		/* The cycle and the layer's list disagreed: a boundary fault. A
		 * mismatch is counted, and the pose is dropped (never written into
		 * the wrong car). */
		CFarTraffic::ms_nFarTrafficWorkerLost++;
		return false;
	}
	SFarTrafficEntry &entry = ms_aFarCars[pose.index];
	if (!FarTrafficEntryIsLive(entry))
		return false;
	CVehicle *pVehicle = entry.pVehicle;

	/* The pose rule, the same one the simulation asks before it publishes
	 * (FarTrafficPoseIsInWorld): a position or a direction that is not finite,
	 * or an x/y outside the world's sector grid, walks CPhysical::RemoveAndAdd
	 * out of CWorld::ms_aSectors. The pose is refused, never written: the car
	 * keeps the position of its last good pose. A refusal the simulation
	 * already counted is only picked up here (the block's numRefused carries
	 * it to the row); a pose the simulation published but the grid cannot hold
	 * is counted here. */
	if (pose.bRefused ||
	    !FarTrafficPoseIsInWorld(ms_farWorld.bounds, pose.x, pose.y, pose.z, pose.dirX, pose.dirY)) {
		if (!pose.bRefused)
			CFarTraffic::ms_nFarTrafficRefusedPoses++;
		return true;
	}

	/* The far state is re-asserted on every update: an explosion un-statices
	 * what it touches (CWorld::TriggerExplosionSectorList) and puts it back on
	 * the moving entity list, and a car that woke up would run the near AI
	 * beside this update. Two flag reads, and the car is a viewed shell
	 * again. */
	if (!pVehicle->GetIsStatic() || pVehicle->bUsesCollision)
		CFarTraffic::ApplyFarState(pVehicle);

	/* The pick of the worker's cycle gave up and turns the car around: the
	 * engine's own two calls, made here (the pick's only world action). */
	if (pose.bUTurn) {
		pVehicle->SetStatus(STATUS_PHYSICS);
		CCarCtrl::SwitchVehicleToRealPhysics(pVehicle);
	}

	CVector forward(pose.forwardX, pose.forwardY, 0.0f);
	pVehicle->GetForward() = forward;
	pVehicle->GetRight() = CVector(forward.y, -forward.x, 0.0f);
	pVehicle->GetUp() = CVector(0.0f, 0.0f, 1.0f);
	pVehicle->SetPosition(CVector(pose.x, pose.y, pose.z));
	pVehicle->SetMoveSpeed(CVector(pose.dirX, pose.dirY, 0.0f) / GAME_SPEED_TO_CARAI_SPEED);
	/* The sector lists follow the position (the renderer and the occupancy
	 * tests read them), and the RW frame follows the matrix. */
	pVehicle->RemoveAndAdd();
	/* The occupancy store follows the layer's own movement (VICE-149): the
	 * far car is static and off the moving list, so the physics step never
	 * re-homes it, and its re-pose does not pass CWorld::Add. */
	CSpawnOccupancy::EntityMoved(pVehicle);
	pVehicle->GetMatrix().UpdateRW();
	pVehicle->UpdateRwFrame();
	/* The reveal of a creation (VICE-127): an off-screen car holds alpha 0
	 * until the camera can see it, and the ramp is the fade the player sees
	 * instead of a car that appeared from nowhere. The stagger's share is
	 * stepped here, so the ramp runs at the layer's rate: sixteen steps of
	 * the stagger, which is 1.6 s at 10 Hz. */
	pVehicle->UpdateClumpAlpha();
	CFarTraffic::ms_nFarTrafficUpdates++;
	return false;
}

/*
 * The path snapshot (VICE-133): the nodes, the links and the connections of the
 * graph, copied out of ThePaths. Nothing here changes after PreparePathData
 * except the two node flags, which FarTrafficRefreshWorldFlags follows. The
 * worker is stopped for the copy, so no cycle can read a half-built snapshot.
 */
void
CFarTraffic::BuildWorldSnapshot(void)
{
	CFarTrafficWorker::Stop();
	ms_nFarCars = 0;
	/* The blocks are zeroed by ResetCars, cycle number included: the marker
	 * goes with them, so the fresh (empty) block is not even read out. */
	ms_nLastAppliedCycle = 0;
	CFarTrafficWorker::ResetCars();

	/* The world's sector grid (VICE-168): the bounds of the pose rule, from
	 * the engine's own macros (World.h), so the worker's simulation can
	 * refuse a pose it cannot hold without including an engine header. */
	ms_farWorld.bounds.minX = WORLD_MIN_X;
	ms_farWorld.bounds.maxX = WORLD_MAX_X;
	ms_farWorld.bounds.minY = WORLD_MIN_Y;
	ms_farWorld.bounds.maxY = WORLD_MAX_Y;
	ms_farWorld.numNodes = ThePaths.m_numPathNodes;
	ms_farWorld.numLinks = ThePaths.m_numCarPathLinks;
	ms_farWorld.numConnections = ThePaths.m_numConnections;
	for (int32 i = 0; i < ThePaths.m_numPathNodes; i++) {
		CPathNode *pNode = &ThePaths.m_pathNodes[i];
		ms_farWorld.nodes[i].x = pNode->GetX();
		ms_farWorld.nodes[i].y = pNode->GetY();
		ms_farWorld.nodes[i].z = pNode->GetZ();
		ms_farWorld.nodes[i].firstLink = pNode->firstLink;
		ms_farWorld.nodes[i].numLinks = pNode->numLinks;
		ms_farWorld.nodes[i].flags = 0;
	}
	for (int32 i = 0; i < ThePaths.m_numCarPathLinks; i++) {
		CCarPathLink *pLink = &ThePaths.m_carPathLinks[i];
		ms_farWorld.links[i].x = pLink->GetX();
		ms_farWorld.links[i].y = pLink->GetY();
		ms_farWorld.links[i].dirX = pLink->GetDirX();
		ms_farWorld.links[i].dirY = pLink->GetDirY();
		ms_farWorld.links[i].laneOffset = pLink->OneWayLaneOffset();
		ms_farWorld.links[i].pathNodeIndex = pLink->pathNodeIndex;
		ms_farWorld.links[i].numLeftLanes = pLink->numLeftLanes;
		ms_farWorld.links[i].numRightLanes = pLink->numRightLanes;
	}
	for (int32 i = 0; i < ThePaths.m_numConnections; i++) {
		ms_farWorld.connectionNode[i] = ThePaths.ConnectedNode(i);
		ms_farWorld.connectionLink[i] = ThePaths.m_carPathConnections[i];
	}
	/* The flags of the nodes the graph has not written yet (the tail of the
	 * arrays) stay zero: the simulation only walks the nodes the links reach. */
	ms_nFarWorldFlagsVersion = ~0u;
	FarTrafficRefreshWorldFlags();
	CFarTrafficWorker::SetWorld(&ms_farWorld);
	CFarTrafficWorker::Start();
}

/* The node flags of the snapshot, re-read when the graph's own flag version
 * moved: the script's road toggles (CPathFind::SwitchRoadsOffInArea and its
 * relatives, and the between-levels marks) and a save load are the only part of
 * the graph that moves after PreparePathData, and all of them bump the version.
 * Called with the worker joined, from the applicator. */
static void
FarTrafficRefreshWorldFlags(void)
{
	if (ms_nFarWorldFlagsVersion == ThePaths.m_nodeFlagsVersion)
		return;
	ms_nFarWorldFlagsVersion = ThePaths.m_nodeFlagsVersion;
	for (int32 i = 0; i < ThePaths.m_numCarPathNodes; i++) {
		CPathNode *pNode = &ThePaths.m_pathNodes[i];
		uint8 flags = 0;
		if (pNode->bDeadEnd)
			flags |= FAR_SIM_NODE_DEADEND;
		if (pNode->bDisabled)
			flags |= FAR_SIM_NODE_DISABLED;
		if (pNode->bBetweenLevels)
			flags |= FAR_SIM_NODE_BETWEENLEVELS;
		ms_farWorld.nodes[i].flags = flags;
	}
}

/* What one site call found. The attempt's counters subdivide by the furthest
 * stage the placements reached, so a live read names the cause: the network
 * could not place at any site of the attempt (`placements == 0` everywhere),
 * the placements all fell outside the band (the site distance against the
 * band's edges), or the near bubble's gate and the car build refused every
 * in-band placement. */
struct SFarTrafficSiteResult
{
	int32 placements;	/* the collector's answer: 0 = the network does not reach the site */
	bool inBand;		/* a placement passed the band's own check */
	bool gated;		/* an in-band placement reached the creation gate */
};

/* One creation attempt at one site: the placement is the near spawner's own
 * (the path network's bracket of the site distance), the model comes from the
 * zone's loaded class arrays, and the car is built by the near spawner's own
 * function in its far mode, so the near bubble's creation gate and the
 * acceptance counters are asked exactly once. `forward` and `angleLimit` are
 * the near spawner's own arguments. */
static bool
FarTrafficTrySite(const SFarTrafficBand &band, const CVector &playerPos, const CVector2D &playerSpeed,
	float spawnDist, float dirX, float dirY, float angleLimit, bool forward, SFarTrafficSiteResult *pResult)
{
	pResult->placements = 0;
	pResult->inBand = false;
	pResult->gated = false;

	/* The residency rule: a far car is only built with a model the streamer
	 * already holds (`CStreaming::HasModelLoaded`, the engine's own check).
	 * The model comes from the population's loaded class arrays, so it is a
	 * model the near traffic of the area would use too - the far layer never
	 * spends a streaming slot of its own. */
	CZoneInfo zone;
	CTheZones::GetZoneInfoForTimeOfDay(&playerPos, &zone);
	int carClass = CCarCtrl::ChooseCarRating(&zone);
	int carModel = CCarCtrl::ChooseCarModel(carClass);
	if (carModel == -1 || !CStreaming::HasModelLoaded(carModel)) {
		CFarTraffic::ms_nFarTrafficRefusedModel++;
		return false;
	}

	CCarCreationPlacement placements[CAR_SPAWN_ATTEMPTS];
	int32 numPlacements = ThePaths.CollectCarCreationCoors(playerPos.x, playerPos.y, dirX, dirY,
		spawnDist, angleLimit, forward, false, placements, CAR_SPAWN_ATTEMPTS);
	pResult->placements = numPlacements;
	for (int32 i = 0; i < numPlacements; i++) {
		/* The water paths are not the layer's: a far car is a road car, and
		 * the boat rings are their own band (TrafficRings.h). */
		if (ThePaths.m_pathNodes[placements[i].node1].bWaterPath)
			continue;
		/* The placement lands at or inside the site distance (the collector's
		 * interpolation), and the bracket's own geometry can put it anywhere
		 * in the band or outside it: the layer's site rule is the band's
		 * (FarTrafficCreateSiteIsInBand), asked with the final position. */
		float dist = (placements[i].position - playerPos).Magnitude2D();
		if (!FarTrafficCreateSiteIsInBand(band, dist))
			continue;
		pResult->inBand = true;
		CVehicle *pCar = nil;
		/* The far layer's own placement until VICE-190 (its migration to a
		 * service request, where it becomes a first-class requester): the
		 * raw node pair, no planned site, the far tier's birth-cull skip.
		 * The old primitive's rarity dice is gone with VICE-150 (the
		 * planner's weight replaces it); the far layer's own rarity weight
		 * arrives with its request. */
		SSpawnPlacement placement;
		placement.site = -1;
		placement.node1 = placements[i].node1;
		placement.node2 = placements[i].node2;
		placement.link = -1;
		placement.x = placements[i].position.x;
		placement.y = placements[i].position.y;
		placement.z = placements[i].position.z;
		placement.between = placements[i].positionBetweenNodes;
		if (CCarCtrl::CreateCarFromPlacement(placement, carClass, carModel, playerPos,
				playerSpeed, false, SPAWN_TIER_FAR, &pCar) == SPAWN_REFUSAL_NONE && pCar) {
			FarTrafficAddEntry(pCar);
			CFarTraffic::ms_nFarTrafficCreations++;
			return true;
		}
		pResult->gated = true;
	}
	return false;
}

/* One attempt's sites, farthest first: the fog (the haze of a long sight line),
 * then the near site ahead (the fallback when the network does not reach the
 * fog - the city is 3.6 km across and the visible range reaches 2 km), then the
 * near site behind (the traffic the player passed). The attempt is over when a
 * car is built; the stage the last placement reached names the refusal. */
static void
FarTrafficTryCreate(const SFarTrafficBand &band, const CVector &playerPos, const CVector2D &playerSpeed)
{
	SFarTrafficSiteResult fog, ahead, behind;
	float nearDist = FarTrafficNearSiteDistance(band);

	CFarTraffic::ms_nFarTrafficAttempts++;
	if (FarTrafficTrySite(band, playerPos, playerSpeed,
			FarTrafficFogSiteDistance(band), TheCamera.CamFrontXNorm, TheCamera.CamFrontYNorm,
			0.707f, true, &fog))
		return;
	/* The near site ahead keeps the front cone (within 45 degrees of the
	 * camera's forward), where the fog site's own filter sat. */
	if (FarTrafficTrySite(band, playerPos, playerSpeed,
			nearDist, TheCamera.CamFrontXNorm, TheCamera.CamFrontYNorm, 0.707f, true, &ahead))
		return;
	/* The rear half: off the frustum by the bubble's margin for every FOV the
	 * game has, so the soft arm is this site's own. */
	if (FarTrafficTrySite(band, playerPos, playerSpeed,
			nearDist, TheCamera.CamFrontXNorm, TheCamera.CamFrontYNorm, 0.0f, false, &behind))
		return;

	/* Nothing was built: the counters say which stage refused every site of
	 * the attempt, so a live read names the cause instead of one lump. */
	CFarTraffic::ms_nFarTrafficRefused++;
	if (fog.placements == 0 && ahead.placements == 0 && behind.placements == 0)
		CFarTraffic::ms_nFarTrafficRefusedNoSite++;
	else if (!fog.inBand && !ahead.inBand && !behind.inBand)
		CFarTraffic::ms_nFarTrafficRefusedBand++;
	else
		CFarTraffic::ms_nFarTrafficRefusedGate++;
}

/*
 * The near cull's hand-over (VICE-128): CCarCtrl::PossiblyRemoveVehicle has
 * decided a traffic car is out of the bubble, past its fresh-spawn grace and
 * hidden, and is about to delete it. The layer takes it instead when the band
 * and the cap want it, and the car keeps driving into the fog.
 */
bool
CFarTraffic::AdoptOnCull(CVehicle *pVehicle, const CVector &playerPos)
{
	if (!FarTrafficIsActive())
		return false;
	/* The layer's own cars only, and never a mission or script car: a car the
	 * cull is about to delete may keep driving as a viewed shell, but the
	 * decision is the traffic's (VICE-128). */
	if (pVehicle->VehicleCreatedBy != RANDOM_VEHICLE || !pVehicle->bIsTrafficCar)
		return false;
	if (pVehicle->bExtendedRange)
		/* A boat's or a set piece's own ring: not the far band's. */
		return false;

	const STrafficRings rings = CurrentTrafficRings();
	const SFarTrafficBand band = ComputeFarTrafficBand(rings);
	if (!FarTrafficAdoptSiteIsInBand(band, (pVehicle->GetPosition() - playerPos).Magnitude2D()))
		return false;
	/* The cap reads the live count, without pruning: a stale entry (a car the
	 * layer lost to a cutscene or the valve) is dropped at the applicator,
	 * where the drop cannot reorder the list under the worker's published
	 * poses. A stale entry therefore reserves a place for at most the rest of
	 * this frame. */
	if (FarTrafficCountLive() >= FarTrafficCap(band))
		return false;

	/* The occupants go: the car is a viewed shell from here, and the driver
	 * ped of a car kilometres away is a pool slot with an AI of its own. The
	 * flag removes each ped on the next CWorld::Process, exactly as the
	 * deletion of the car would; the car is hidden at this point (the cull
	 * asked the near bubble), so nothing of it can be seen. */
	if (pVehicle->pDriver)
		pVehicle->pDriver->FlagToDestroyWhenNextProcessed();
	for (int i = 0; i < pVehicle->m_nNumMaxPassengers; i++)
		if (pVehicle->pPassengers[i])
			pVehicle->pPassengers[i]->FlagToDestroyWhenNextProcessed();

	/* The budget count goes back before the class changes (UpdateCarCount
	 * reads the class), then the far state. */
	CCarCtrl::UpdateCarCount(pVehicle, true);
	pVehicle->VehicleCreatedBy = FAR_TRAFFIC_VEHICLE;
	ApplyFarState(pVehicle);
	FarTrafficAddEntry(pVehicle);
	CFarTraffic::ms_nFarTrafficDemotions++;
	return true;
}

/* The worker thread's end (VICE-133): a shutdown joins it (a cycle in flight is
 * joined with it) so the next load starts a fresh thread with a fresh snapshot.
 * The cars are gone with the world; the lists are reset by
 * CFarTraffic::BuildWorldSnapshot. */
void
CFarTraffic::ShutDown(void)
{
	CFarTrafficWorker::Stop();
}

/*
 * The main thread's join (VICE-133): called at the top of CGame::Process, before
 * CWorld::Process. The worker's cycle of the previous frame is complete here,
 * and its output is published; from this point to the end of the frame the
 * worker is idle. A debug build's CWorld::Process and RenderScene assert it.
 */
void
CFarTraffic::JoinWorker(void)
{
	CFarTrafficWorker::Join();
}

/* The debug build's audit of the window (VICE-133): the worker is not inside a
 * cycle. The assertion itself lives in the worker module (FarTrafficWorker.cpp)
 * and compiles away in a release build, so this is a call to an empty function
 * there. */
void
CFarTraffic::AssertWorkerIdle(void)
{
	CFarTrafficWorker::AssertIdle();
}

/*
 * The hand-over (VICE-133): called from Idle at the end of the frame, after the
 * render. The cycle the applicator stamped is handed to the worker, or run
 * inline (the escape hatch's mode). A frame whose cycle would do nothing - no
 * commands and no car of the stagger's share - wakes the worker not at all, so
 * a layer that is off (the stock draw distance) costs the handshake nothing.
 */
void
CFarTraffic::RunWorker(void)
{
	if (CFarTrafficWorker::Run())
		ms_nFarTrafficWorkerCycles++;
}

/*
 * The published cycle of the worker, applied to the cars: the stagger's share,
 * every pose checked against its car first (the index, the slot and the live
 * test), so a stale output can never touch a car that is not the one it
 * describes. The block is read out once, by its cycle number. A command the
 * cycle could not apply (the ring's bound) is reported into the lost row here
 * too, so a divergence is never silent. A pose the world cannot hold (VICE-168)
 * is refused by FarTrafficApplyPose and its car is handed to the near side
 * without a driver after the block's loop.
 */
static void
FarTrafficApplyOutput(void)
{
	const SFarTrafficOutput *pOutput = CFarTrafficWorker::Published();

	if (CFarTrafficWorker::TakeOverflow())
		CFarTraffic::ms_nFarTrafficWorkerLost++;
	if (pOutput->cycle == ms_nLastAppliedCycle)
		return;
	ms_nLastAppliedCycle = pOutput->cycle;
	/* The refused poses (VICE-168) are recovered after the block has been
	 * walked: the recovery promotes the car (the layer must not hold a car it
	 * cannot place), the promotion drops an entry, and a drop swaps the last
	 * entry into the hole - in the middle of the loop that would move a car
	 * under a pose the loop has not applied yet. The slot is the car's
	 * identity across the drops, so the recovery finds it by slot and
	 * re-validates it (an earlier recovery, or a catch-up's own apply, may
	 * already have taken the car out of the layer). */
	uint16 refusedSlots[FAR_TRAFFIC_MAX];
	int32 numRefused = 0;
	for (int32 i = 0; i < pOutput->numPoses; i++) {
		const SFarSimPose &pose = pOutput->poses[i];
		if (FarTrafficApplyPose(pose))
			refusedSlots[numRefused++] = pose.slot;
	}
	for (int32 i = 0; i < numRefused; i++) {
		for (int32 j = 0; j < ms_nFarCars; j++) {
			if (ms_aFarCars[j].slot == refusedSlots[i] && FarTrafficEntryIsLive(ms_aFarCars[j])) {
				FarTrafficPromoteCar(j, false);
				break;
			}
		}
	}
	CFarTraffic::ms_nFarTrafficShortCurves += pOutput->numShortCurves;
	CFarTraffic::ms_nFarTrafficRefusedPoses += pOutput->numRefused;
	CFarTraffic::ms_nFarTrafficWorkerLost += pOutput->numLost;
}

/*
 * One frame of the layer. The car block of CGame::Process calls it after the
 * near cull, so the frame's pool, bubble budget and fill are already decided.
 */
void
CFarTraffic::Process(void)
{
	/* The contract's barrier: the worker is idle here (JoinWorker ran before
	 * CWorld::Process) and stays idle until RunWorker. The reads below are
	 * therefore plain reads of a published block. */
	CFarTrafficWorker::Join();

	/* The previous cycle's poses, first, while the layer's list is exactly
	 * what the worker's simulation left it as: only an append may have
	 * happened since (a cull hand-over, which moves no entry), because the
	 * stale entries below are pruned after this and every other drop of the
	 * frame is later still. */
	FarTrafficApplyOutput();

	/* The graph's flags moved (a script's road toggle, a save load): copy them
	 * into the snapshot before this frame's cycle, with the worker idle. */
	FarTrafficRefreshWorldFlags();

	/* The stale entries (a cutscene's cleanup, the valve, a delete) go now,
	 * with the apply already behind them: the drop's swap reorders the list,
	 * and the worker's poses are read against the order its own list has. */
	FarTrafficPruneEntries();

	/* The frame's cycle is stamped; the events below queue their commands into
	 * it. The stamp is the game time of this frame, so the poses the worker
	 * computes from it land one frame later (the double buffer's own lag). */
	CFarTrafficWorker::BeginCycle(CTimer::GetTimeInMilliseconds(), CTimer::GetFrameCounter());

	if (!FarTrafficIsActive()) {
		/* Paused: a cutscene or an interior owns the world, and a cutscene's
		 * own cleanup deletes the cars (CanBeDeleted is true for a far car).
		 * The list was pruned above so the counters stay live (the drops
		 * queued for the worker's own list); nothing is created, moved or
		 * removed here. */
		ms_nFarTrafficCars = ms_nFarCars;
		ms_nFarTrafficCap = 0;
		CFarTrafficWorker::EndCycle();
		return;
	}

	const STrafficRings rings = CurrentTrafficRings();
	const SFarTrafficBand band = ComputeFarTrafficBand(rings);
	const CVector playerPos = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	int cap = FarTrafficCap(band);
	ms_nFarTrafficCap = cap;

	/* The gate pass: every car, every frame. */
	int removalGates = FAR_TRAFFIC_REMOVE_ATTEMPTS;
	/* The ped pool is read once for the whole pass (the read is a scan of the
	 * pool): a promotion spends its car's driver, and the pass follows the
	 * count locally. */
	int pedFree = CPools::GetPedPool()->GetNoOfFreeSpaces();
	for (int32 i = 0; i < ms_nFarCars; ) {
		if (!FarTrafficEntryIsLive(ms_aFarCars[i])) {
			FarTrafficDropEntry(i);
			continue;
		}
		CVehicle *pVehicle = ms_aFarCars[i].pVehicle;
		float dist = (pVehicle->GetPosition() - playerPos).Magnitude2D();
		EFarTrafficSwitch what = FarTrafficSwitchFor(band, dist, pedFree >= PED_POOL_RESERVE);
		if (what == FAR_TRAFFIC_PROMOTE || what == FAR_TRAFFIC_SAFETY_PROMOTE) {
			/* The promotions are not bounded by a per-frame allowance: a car
			 * that reached the band's edge may not wait where the player
			 * could come to it. The normal one takes its driver from the
			 * pool's reserve (a car and a driver); the safety one has no
			 * driver to take, and the refusal is counted. */
			bool bWithDriver = what == FAR_TRAFFIC_PROMOTE;
			if (bWithDriver)
				pedFree -= FAR_TRAFFIC_PROMOTION_PEDS;
			else
				CFarTraffic::ms_nFarTrafficRefusedPool++;
			FarTrafficPromoteCar(i, bWithDriver);
			continue;
		}
		/* A wreck is no longer traffic: an explosion can still reach a far car
		 * (the explosion loop does not test the collision flag), and a burning
		 * shell driving through the fog is worse than a car the fog hides. */
		if (pVehicle->GetStatus() == STATUS_WRECKED)
			what = FAR_TRAFFIC_REMOVE;
		/* A car that is already fading out (the in-view band's removal,
		 * VICE-146) is on its way and asks no gate again; the switches above
		 * have already promoted it if the player came near. Its tail is
		 * deleted where the near cull deletes its own: at alpha 0. */
		if (pVehicle->bFadeOut) {
			if (CVisibilityPlugins::GetClumpAlpha(pVehicle->GetClump()) == 0) {
				FarTrafficDeleteCar(i);
				continue;
			}
			i++;
			continue;
		}
		if (what == FAR_TRAFFIC_REMOVE || ms_nFarCars > cap) {
			/* The removals are the layer's own churn and each one asks the
			 * near bubble's removal rule. The frame's allowance counts the
			 * gates the layer asks, not the cars it removes: a refused answer
			 * spends the allowance too, so one frame cannot spend more than
			 * FAR_TRAFFIC_REMOVE_ATTEMPTS answers (and, with the gate's ray
			 * refused, not one of the frame's rays). A car that must go waits
			 * for the next frame; a car beyond the fog is hidden-ish and
			 * leaves a frame later, exactly as the near cull's own removals
			 * do. */
			if (removalGates > 0) {
				removalGates--;
				if (FarTrafficRemoveCar(i, playerPos))
					continue;
			}
			i++;
			continue;
		}
		i++;
	}

	/* The acceptance gauge: after a frame of the gate pass, no car of the layer
	 * is inside the player's reach - every car that reached it was promoted (or
	 * made real without a driver). The gauge is read from the state the pass
	 * decided on, which is the state the rule answers for: the frame's creations
	 * land inside the band (280 m or more out) and the worker's move steps a
	 * car by under two metres between its updates, so a car that drifts across
	 * the 120 m line is promoted by the next frame's pass. It must stay 0. */
	CFarTraffic::ms_nFarTrafficInReach = 0;
	for (int32 i = 0; i < ms_nFarCars; i++) {
		if (!FarTrafficEntryIsLive(ms_aFarCars[i]))
			continue;
		if ((ms_aFarCars[i].pVehicle->GetPosition() - playerPos).Magnitude2D() <= NEAR_BUBBLE_REMOVE_DISTANCE)
			CFarTraffic::ms_nFarTrafficInReach++;
	}

	/* The creation attempts: only while the layer is under its cap and the
	 * band is open. The pool reserves are the layer's promise to the rest of
	 * the game: a creation never takes the last slots, and a refused reserve is
	 * counted so a live reading tells it from the rule refusing. */
	if (band.active && ms_nFarCars < cap) {
		if (CPools::GetVehiclePool()->GetNoOfFreeSpaces() < FAR_TRAFFIC_POOL_RESERVE ||
			pedFree < PED_POOL_RESERVE) {
			CFarTraffic::ms_nFarTrafficRefusedPool++;
		} else {
			CVector2D playerSpeed = FindPlayerSpeed();
			for (int i = 0; i < FAR_TRAFFIC_CREATE_ATTEMPTS && ms_nFarCars < cap; i++)
				FarTrafficTryCreate(band, playerPos, playerSpeed);
		}
	}

	ms_nFarTrafficCars = ms_nFarCars;
	CFarTrafficWorker::EndCycle();
}
