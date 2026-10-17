#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "FarTrafficWorker.h"

/*
 * The far traffic layer's simulation and its worker thread (VICE-133). The
 * contract, the frame's order and the boundary are in FarTrafficWorker.h; this
 * file is the simulation itself, the thread that runs it, and the handshake.
 *
 * Nothing here includes an engine header, and nothing here calls the game: the
 * simulation reads the FarTrafficWorld snapshot and the sim cars, and the
 * handshake uses plain C++ (std::thread, a mutex and two condition variables,
 * allocated once by Start). The evidence program (utils/far-traffic-worker)
 * compiles this translation unit and drives the same cycle offline.
 */

/*
 * The car's own random stream: a private 32 bit LCG. The far cars must not draw
 * from the game's stream (VICE-131 section 2.1: the shared cursor would
 * interleave with the frame's draws), and a per-car stream has the second
 * property the layer wants: a car's route sequence does not depend on the order
 * in which the other cars drew. The step is the shipped PC myrand's
 * (seed * 1103515245 + 12345, the top bits read as a 15 bit draw), so the
 * probabilities the path pick expects (MYRAND_MAX) hold.
 */
static inline uint32_t
FarSimRngNext(SFarSimCar *pCar)
{
	pCar->rng = pCar->rng * 1103515245u + 12345u;
	return (pCar->rng >> 16) & 0x7FFF;
}

uint32_t
FarTrafficSimRngSeed(uint16_t slot, uint16_t randomSeed, int16_t routeSeed)
{
	/* Deterministic, distinct per car, and carrying the car's own seeds: a
	 * scripted route seed still gives the car a sequence of its own. */
	uint32_t seed = (uint32_t)slot * 2654435761u + 0x9E3779B9u;
	seed ^= (uint32_t)randomSeed * 40503u;
	seed ^= (uint32_t)(uint16_t)routeSeed * 2246822519u;
	return seed;
}

/*
 * The sim's views of the shared car-on-rails core (CarOnRails.h): the graph
 * view reads the snapshot, the car view reads one SFarSimCar. They are the
 * worker's half of the "one copy of the rules" arrangement; the engine's half
 * is CCarRailsEngineView in CarCtrl.cpp.
 */
struct SFarSimGraphView
{
	const SFarTrafficWorld *pWorld;

	float NodeX(int32_t node) { return pWorld->nodes[node].x; }
	float NodeY(int32_t node) { return pWorld->nodes[node].y; }
	int32_t NumLinks(int32_t node) { return pWorld->nodes[node].numLinks; }
	int32_t NodeFirstLink(int32_t node) { return pWorld->nodes[node].firstLink; }
	bool NodeDeadEnd(int32_t node) { return (pWorld->nodes[node].flags & FAR_SIM_NODE_DEADEND) != 0; }
	bool NodeDisabled(int32_t node) { return (pWorld->nodes[node].flags & FAR_SIM_NODE_DISABLED) != 0; }
	bool NodeBetweenLevels(int32_t node) { return (pWorld->nodes[node].flags & FAR_SIM_NODE_BETWEENLEVELS) != 0; }
	int32_t ConnectedNode(int32_t connection) { return pWorld->connectionNode[connection]; }
	int32_t LinkOfConnection(int32_t connection) { return pWorld->connectionLink[connection]; }
	float LinkX(int32_t link) { return pWorld->links[link].x; }
	float LinkY(int32_t link) { return pWorld->links[link].y; }
	float LinkDirX(int32_t link) { return pWorld->links[link].dirX; }
	float LinkDirY(int32_t link) { return pWorld->links[link].dirY; }
	int32_t LinkLeftLanes(int32_t link) { return pWorld->links[link].numLeftLanes; }
	int32_t LinkRightLanes(int32_t link) { return pWorld->links[link].numRightLanes; }
	float LinkLaneOffset(int32_t link) { return pWorld->links[link].laneOffset; }
	int32_t LinkPathNode(int32_t link) { return pWorld->links[link].pathNodeIndex; }
};

struct SFarSimCarView
{
	SFarSimCar *pCar;
	uint32_t now;
	int32_t *pShortCurves;

	int32_t CurrentRouteNode() { return pCar->currentRouteNode; }
	int32_t NextRouteNode() { return pCar->nextRouteNode; }
	int32_t PrevRouteNode() { return pCar->prevRouteNode; }
	void SetCurrentRouteNode(int32_t v) { pCar->currentRouteNode = v; }
	void SetNextRouteNode(int32_t v) { pCar->nextRouteNode = v; }
	void SetPrevRouteNode(int32_t v) { pCar->prevRouteNode = v; }
	int32_t TimeEnteredCurve() { return pCar->timeEnteredCurve; }
	int32_t TimeOnCurve() { return pCar->timeToSpendOnCurrentCurve; }
	void SetTimeEnteredCurve(int32_t v) { pCar->timeEnteredCurve = v; }
	void SetTimeOnCurve(int32_t v) { pCar->timeToSpendOnCurrentCurve = v; }
	int32_t CurrentPathNodeInfo() { return pCar->currentPathNodeInfo; }
	int32_t NextPathNodeInfo() { return pCar->nextPathNodeInfo; }
	int32_t PreviousPathNodeInfo() { return pCar->previousPathNodeInfo; }
	void SetCurrentPathNodeInfo(int32_t v) { pCar->currentPathNodeInfo = v; }
	void SetNextPathNodeInfo(int32_t v) { pCar->nextPathNodeInfo = v; }
	void SetPreviousPathNodeInfo(int32_t v) { pCar->previousPathNodeInfo = v; }
	int32_t CurrentDirection() { return pCar->currentDirection; }
	int32_t NextDirection() { return pCar->nextDirection; }
	void SetCurrentDirection(int32_t v) { pCar->currentDirection = v; }
	void SetNextDirection(int32_t v) { pCar->nextDirection = v; }
	void SetPreviousDirection(int32_t v) { pCar->previousDirection = v; }
	int32_t CurrentLane() { return pCar->currentLane; }
	int32_t NextLane() { return pCar->nextLane; }
	void SetCurrentLane(int32_t v) { pCar->currentLane = v; }
	void SetNextLane(int32_t v) { pCar->nextLane = v; }
	uint32_t RandomSeed() { return pCar->randomSeed; }
	float MaxTrafficSpeed() { return pCar->maxTrafficSpeed; }
	bool StayInCurrentLevel() { return pCar->bStayInCurrentLevel != 0; }
	bool StayInFastLane() { return pCar->bStayInFastLane != 0; }

	float CurveProgress()
	{
		uint32_t timeInCurve = now - (uint32_t)pCar->timeEnteredCurve;
		return (float)timeInCurve / pCar->timeToSpendOnCurrentCurve;
	}
	uint32_t Random() { return FarSimRngNext(pCar); }
	bool RandomTrueFalse() { return Random() < 0x7FFF / 2; }
	void UTurn() { pCar->bUTurn = 1; }
	void ShortCurve() { (*pShortCurves)++; }
};

void
FarTrafficSimUpdateCar(const SFarTrafficWorld *pWorld, SFarSimCar *pCar, uint32_t now, SFarSimPose *pPose)
{
	SFarSimGraphView g;
	SFarSimCarView c;
	int32_t shortCurves = 0;

	g.pWorld = pWorld;
	c.pCar = pCar;
	c.now = now;
	c.pShortCurves = &shortCurves;
	pCar->bUTurn = 0;

	/* The pick when the curve is spent (the engine's FarTrafficUpdateCar): a
	 * far car follows the same path links the near traffic drives, with the
	 * same rules and the same lane arithmetic. */
	if ((uint32_t)(pCar->timeEnteredCurve + pCar->timeToSpendOnCurrentCurve) <= now)
		CarRailsPickNextNode(g, c);

	/* The rail pose (the near rails' own arithmetic, shared). */
	float x, y, dirX, dirY;
	CarRailsPose(g, c, &x, &y, &dirX, &dirY);

	/* The height of the rail: the path nodes carry the road's own height, so
	 * the interpolation between them is the road under the car, without a
	 * ground cast (which would be two collision queries per update, the one
	 * thing the layer must not spend). The node z is the far layer's own rule
	 * (the near rails drag the car onto the ground instead), so it is not part
	 * of the shared pose. The progress is the curve's own span, as the pose
	 * reads it (CarRailsClampCurveProgress): a stale clock must not
	 * extrapolate the height either. */
	float between = CarRailsClampCurveProgress(c.CurveProgress());
	float z = pWorld->nodes[pCar->currentRouteNode].z * (1.0f - between)
		+ pWorld->nodes[pCar->nextRouteNode].z * between;
	z += pCar->heightAboveRoad;

	/* The heading: the curve's tangent with the slope removed and the length
	 * normalised; a degenerate tangent keeps the car's last heading. */
	float forwardX = dirX;
	float forwardY = dirY;
	float sq = forwardX*forwardX + forwardY*forwardY;
	if (sq > 0.0001f) {
		float invsqrt = 1.0f / sqrtf(sq);
		forwardX *= invsqrt;
		forwardY *= invsqrt;
	} else {
		forwardX = pCar->forwardX;
		forwardY = pCar->forwardY;
	}
	pCar->forwardX = forwardX;
	pCar->forwardY = forwardY;

	pPose->slot = pCar->slot;
	pPose->bUTurn = pCar->bUTurn;
	pPose->bShortCurve = shortCurves > 0;
	/* The world's own rule over the pose (VICE-168), one predicate shared with
	 * the applicator (FarTraffic.h): a position or a direction that is not
	 * finite, or x/y outside the sector grid, is refused. The pose still
	 * carries the arithmetic's numbers - the evidence program compares them -
	 * but the applicator will not write it: the car keeps its last good
	 * position and leaves the layer, so the sector insert of
	 * CPhysical::RemoveAndAdd can never see the point. */
	pPose->bRefused = !FarTrafficPoseIsInWorld(pWorld->bounds, x, y, z, dirX, dirY);
	pPose->x = x;
	pPose->y = y;
	pPose->z = z;
	pPose->dirX = dirX;
	pPose->dirY = dirY;
	pPose->forwardX = forwardX;
	pPose->forwardY = forwardY;
}

void
FarTrafficSimRunCycle(const SFarTrafficWorld *pWorld, SFarSimCar *pCars, int32_t *pNumCars,
	const SFarTrafficInput *pInput, SFarTrafficOutput *pOutput)
{
	int32_t numCars = *pNumCars;

	pOutput->cycle = pInput->cycle;
	pOutput->numPoses = 0;
	pOutput->numShortCurves = 0;
	pOutput->numRefused = 0;
	pOutput->numLost = 0;

	/* The commands of every frame since the last cycle, in the order the
	 * applicator made them. The drop takes the last entry's place, exactly as
	 * the applicator's own list does, so the two arrays stay in lockstep and
	 * the stagger's index means the same car on both sides. */
	for (int32_t i = 0; i < pInput->numCommands; i++) {
		const SFarTrafficCommand *pCommand = &pInput->commands[i];
		if (pCommand->op == FAR_SIM_OP_ADD) {
			if (numCars >= FAR_TRAFFIC_MAX) {
				pOutput->numLost++;
				continue;
			}
			pCars[numCars] = pCommand->car;
			pCars[numCars].bUTurn = 0;
			pCars[numCars].rng = FarTrafficSimRngSeed(pCommand->car.slot,
				pCommand->car.randomSeed, pCommand->car.routeSeed);
			numCars++;
		} else {
			int32_t at = -1;
			for (int32_t j = 0; j < numCars; j++) {
				if (pCars[j].slot == pCommand->car.slot) {
					at = j;
					break;
				}
			}
			if (at < 0) {
				pOutput->numLost++;
				continue;
			}
			pCars[at] = pCars[numCars - 1];
			numCars--;
		}
	}

	/* The share of the stagger this frame owns. */
	for (int32_t i = 0; i < numCars; i++) {
		if (!FarTrafficUpdatesThisFrame(pInput->frameCounter, i))
			continue;
		SFarSimPose *pPose = &pOutput->poses[pOutput->numPoses];
		FarTrafficSimUpdateCar(pWorld, &pCars[i], pInput->now, pPose);
		pPose->index = (uint16_t)i;
		if (pPose->bShortCurve)
			pOutput->numShortCurves++;
		if (pPose->bRefused)
			pOutput->numRefused++;
		pOutput->numPoses++;
	}

	*pNumCars = numCars;
	pOutput->numCars = numCars;
}

bool
FarTrafficFrameHasShare(uint32_t frameCounter, int32_t numCars)
{
	for (int32_t i = 0; i < numCars && i < FAR_TRAFFIC_STAGGER; i++)
		if (FarTrafficUpdatesThisFrame(frameCounter, i))
			return true;
	return false;
}

/*
 * The thread and the handshake. The state is the module's, and the mutex
 * guards the handshake flags and the worker's own list count; the output blocks
 * need no lock (the main thread reads the published one only after Join).
 */
static std::thread *gp_thread;
static std::mutex g_mutex;
static std::condition_variable g_wake;
static std::condition_variable g_done;
static bool g_bWake;
static bool g_bInFlight;
static bool g_bStop;
static bool g_bReady;		/* EndCycle ran: the input may be handed over */
static bool g_bQueueFull;	/* a command did not fit the ring (reported) */
static std::atomic<bool> g_bInFlightAtomic;	/* the debug audit's view */
static std::atomic<int> g_active;	/* the published output block */
static SFarTrafficOutput g_aOutput[2];
static SFarTrafficInput g_input;
static SFarSimCar g_aCars[FAR_TRAFFIC_MAX];
static int32_t g_numCars;
static const SFarTrafficWorld *gp_world;
static bool g_bThreaded = FAR_TRAFFIC_WORKER != 0;

static void
FarTrafficWorkerDoCycle(void)
{
	int next = 1 - g_active.load();

	FarTrafficSimRunCycle(gp_world, g_aCars, &g_numCars, &g_input, &g_aOutput[next]);
	/* The commands are spent: the next frames queue from an empty list. */
	g_input.numCommands = 0;
	g_active.store(next);
}

static void
FarTrafficWorkerLoop(void)
{
	std::unique_lock<std::mutex> lock(g_mutex);

	for (;;) {
		g_wake.wait(lock, [] { return g_bWake || g_bStop; });
		if (g_bStop)
			return;
		g_bWake = false;
		lock.unlock();
		FarTrafficWorkerDoCycle();
		lock.lock();
		g_bInFlight = false;
		g_bInFlightAtomic.store(false);
		g_done.notify_one();
	}
}

void
CFarTrafficWorker::Start(void)
{
	if (gp_thread || !g_bThreaded)
		return;
	g_bStop = false;
	g_bWake = false;
	g_bInFlight = false;
	g_bInFlightAtomic.store(false);
	g_bReady = false;
	gp_thread = new std::thread(FarTrafficWorkerLoop);
}

void
CFarTrafficWorker::Stop(void)
{
	if (gp_thread == 0)
		return;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_bStop = true;
		g_bWake = true;
	}
	g_wake.notify_one();
	gp_thread->join();
	delete gp_thread;
	gp_thread = 0;
	/* A stop joins whatever cycle was in flight (the worker's own loop checks
	 * the stop flag on its way out), so the in-flight flags must go with it:
	 * the state is startable again, and Join can never wait on a thread that
	 * no longer exists. */
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_bInFlight = false;
	}
	g_bInFlightAtomic.store(false);
	g_bWake = false;
	g_bReady = false;
}

void
CFarTrafficWorker::SetWorld(const SFarTrafficWorld *pWorld)
{
	gp_world = pWorld;
}

void
CFarTrafficWorker::ResetCars(void)
{
	/* A fresh layer: no cars, no commands, no published cycle. The whole of
	 * both output blocks goes (a block's cycle number, its poses, its short
	 * curves and its lost count), so nothing of the world the last load
	 * simulated can be read out of them again. The applicator's marker is
	 * zero with them (CFarTraffic::BuildWorldSnapshot), so the zeroed block is
	 * not applied either. */
	g_numCars = 0;
	g_input.cycle = 0;
	g_input.now = 0;
	g_input.frameCounter = 0;
	g_input.numCommands = 0;
	g_bReady = false;
	g_bQueueFull = false;
	memset(&g_aOutput[0], 0, sizeof(g_aOutput[0]));
	memset(&g_aOutput[1], 0, sizeof(g_aOutput[1]));
	g_active.store(0);
}

void
CFarTrafficWorker::BeginCycle(uint32_t now, uint32_t frameCounter)
{
	/* The contract's own assertion: the input is only built while the worker
	 * is idle (Join ran at the top of the frame, and the applicator joins
	 * again before it builds). */
	AssertIdle();
	g_input.cycle++;
	g_input.now = now;
	g_input.frameCounter = frameCounter;
}

void
CFarTrafficWorker::AddCar(const SFarSimCar &car)
{
	if (g_input.numCommands >= FAR_TRAFFIC_SIM_COMMANDS) {
		/* The ring is full (the applicator's catch-up did not run, or ran too
		 * late): the command cannot be applied, so the worker's list and the
		 * layer's diverge here. It is reported, never silent
		 * (CFarTrafficWorker::TakeOverflow), and the layer's lost row paints
		 * it. */
		g_bQueueFull = true;
		return;
	}
	SFarTrafficCommand *pCommand = &g_input.commands[g_input.numCommands++];
	memset(pCommand, 0, sizeof(*pCommand));
	pCommand->op = FAR_SIM_OP_ADD;
	pCommand->car = car;
}

void
CFarTrafficWorker::DropCar(uint16_t slot)
{
	if (g_input.numCommands >= FAR_TRAFFIC_SIM_COMMANDS) {
		g_bQueueFull = true;
		return;
	}
	SFarTrafficCommand *pCommand = &g_input.commands[g_input.numCommands++];
	memset(pCommand, 0, sizeof(*pCommand));
	pCommand->op = FAR_SIM_OP_DROP;
	pCommand->car.slot = slot;
}

void
CFarTrafficWorker::EndCycle(void)
{
	g_bReady = true;
}

int32_t
CFarTrafficWorker::CommandsPending(void)
{
	/* The worker is idle whenever the applicator asks (it queues between the
	 * join and the next signal), so the count is a plain read. */
	return g_input.numCommands;
}

bool
CFarTrafficWorker::TakeOverflow(void)
{
	if (!g_bQueueFull)
		return false;
	g_bQueueFull = false;
	return true;
}

bool
CFarTrafficWorker::Run(void)
{
	if (!g_bReady)
		return false;
	g_bReady = false;
	/* A cycle with nothing to do is dropped rather than woken for: no commands
	 * and not one car of the frame's share. A frame that skips its cycle loses
	 * nothing (the share would not have moved, and the commands stay queued). */
	if (g_input.numCommands == 0 && !FarTrafficFrameHasShare(g_input.frameCounter, g_numCars))
		return false;
	if (!g_bThreaded) {
		FarTrafficWorkerDoCycle();
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		/* The contract's window is bounded by Join before Run, so no cycle
		 * is in flight here. If one ever were (a missed join), the frame's
		 * cycle is dropped rather than run twice: its commands stay queued
		 * for the next cycle, which is lossless. */
		AssertIdle();
		if (g_bInFlight)
			return false;
		if (gp_thread == 0) {
			/* No worker thread (a caller that never Start()ed; the game
			 * starts it at load, CGame::Initialise). The cycle is not run and
			 * nothing is set in flight - the commands stay queued, the
			 * applicator's catch-up keeps asking, and its water mark reports
			 * the state rather than hanging the next join. */
			AssertIdle();
			return false;
		}
		g_bWake = true;
		g_bInFlight = true;
	}
	g_bInFlightAtomic.store(true);
	g_wake.notify_one();
	return true;
}

void
CFarTrafficWorker::Join(void)
{
	std::unique_lock<std::mutex> lock(g_mutex);
	if (!g_bInFlight)
		return;
	g_done.wait(lock, [] { return !g_bInFlight; });
}

const SFarTrafficOutput *
CFarTrafficWorker::Published(void)
{
	return &g_aOutput[g_active.load()];
}

bool
CFarTrafficWorker::ExportCar(int32_t index, SFarSimCar &out)
{
	/* The contract's window (FarTrafficWorker.h): the caller is the applicator
	 * on the main thread, the worker is joined, and no cycle is handed over
	 * before the end of the frame (Run). The sim array is quiescent, so the
	 * copy is the whole of it. */
	AssertIdle();
	if (index < 0 || index >= g_numCars)
		return false;
	out = g_aCars[index];
	return true;
}

void
CFarTrafficWorker::SetThreaded(bool bThreaded)
{
	/* Only between cycles: the caller (the measurement, or a restart) is the
	 * main thread, with the worker joined or never started. */
	AssertIdle();
	if (bThreaded == g_bThreaded)
		return;
	if (!bThreaded)
		Stop();			/* the idle thread is joined away */
	g_bThreaded = bThreaded;
}

void
CFarTrafficWorker::AssertIdle(void)
{
#ifndef NDEBUG
	/* The worker must be idle: every entry point of the main thread's side of
	 * the boundary joins first. A release build compiles this away. */
	assert(!g_bInFlightAtomic.load());
#endif
}
