#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "SpawnPlannerWorker.h"

/*
 * The spawn planner's rank on a worker thread (VICE-160). The contract, the
 * frame's order, the one-frame lag and the contention are in
 * SpawnPlannerWorker.h; this file is the cycle itself, the thread that runs
 * it, and the handshake.
 *
 * Nothing here includes an engine header, and nothing here calls the game: the
 * cycle reads the site snapshot, the occupancy copy and the command ring, and
 * calls the planner's own rank (SpawnRankSites, SpawnService.h). The evidence
 * program (utils/spawn-service/worker.cpp) compiles this translation unit and
 * drives the same cycle offline.
 */

/*
 * The rank's own top-K and the deep prefix are the planner's constants
 * (SPAWN_SERVICE_DEEP_CANDIDATES); the commands and the plans are bounded by
 * SPAWN_PLANNER_RING. A full ring is the main thread's fallback, never a
 * silent drop: QueueRank returns false there.
 */

static std::thread *gp_thread;
static std::mutex g_mutex;
static std::condition_variable g_wake;
static std::condition_variable g_done;
static bool g_bWake;
static bool g_bInFlight;
static bool g_bStop;
static std::atomic<bool> g_bInFlightAtomic;	/* the debug audit's view */
static std::atomic<int> g_active;		/* the published output block */
static std::atomic<int> g_occupancyActive;	/* the published occupancy copy */
static SSpawnPlannerOutput g_aOutput[2];
static SSpawnPlannerInput g_input;
static SSpawnPlannerOccupancy g_aOccupancy[2];
static SSpawnSite g_aSites[NUM_PATHCONNECTIONS];
static int32_t g_numSites;
static bool g_bThreaded = SPAWN_PLANNER_WORKER != 0;

SSpawnOccupancyView
SSpawnPlannerOccupancy::View(void) const
{
	SSpawnOccupancyView view;

	view.m_pVehicleCount = m_aVehicleCount;
	view.m_pSiteExpiry = m_aSiteExpiry;
	view.m_pSiteInUse = m_aSiteInUse;
	return view;
}

void
SpawnPlannerRunCycle(const SSpawnSite *sites, int32_t numSites,
	const SSpawnPlannerOccupancy *pOccupancy, const SSpawnPlannerInput *pInput,
	SSpawnPlannerOutput *pOutput)
{
	int32_t i;

	pOutput->cycle = pInput->cycle;
	pOutput->numPlans = 0;
	pOutput->numRanked = 0;
	pOutput->numLost = 0;

	/* One ranked plan per queued command, in the order the main thread
	 * queued them: the pending plan queue on the main thread relies on the
	 * order, and the request the plan echoes is the one it belongs to. */
	for (i = 0; i < pInput->numCommands; i++) {
		const SSpawnRankCommand *pCommand = &pInput->commands[i];
		SSpawnRank rank;
		SSpawnRankedPlan *pPlan;

		if (pOutput->numPlans >= SPAWN_PLANNER_RING) {
			/* The ring's bound makes this unreachable (the input and the
			 * output hold the same number of commands); it is counted, never
			 * silent. */
			pOutput->numLost++;
			continue;
		}
		SpawnRankSites(sites, pCommand->offers, pCommand->bubbles,
			pCommand->numOffers, pOccupancy->View(), pInput->nowMs, &rank);
		pPlan = &pOutput->plans[pOutput->numPlans];
		memset(pPlan, 0, sizeof(*pPlan));
		pPlan->request = pCommand->request;
		SpawnRankedPlanFromRank(rank, pCommand->offers, pPlan);
		pOutput->numRanked += rank.candidates;
		pOutput->numPlans++;
	}
}

#ifndef NDEBUG

/* The write boundary's audit (VICE-160): the cycle may read the site
 * snapshot, the occupancy copy and the input, and write the output block it
 * was handed. The hash covers every block before and after the rank, so a
 * write anywhere else shows up as an assertion in a debug build. FNV-1a, the
 * far worker harness's hash. */
static uint32_t
SpawnPlannerHash(const void *p, size_t bytes)
{
	const uint8_t *b = (const uint8_t *)p;
	uint32_t hash = 2166136261u;

	while (bytes-- > 0) {
		hash ^= *b++;
		hash *= 16777619u;
	}
	return hash;
}

#endif

static void
SpawnPlannerDoCycle(void)
{
	int next = 1 - g_active.load();
	SSpawnPlannerOutput *pOutput = &g_aOutput[next];

#ifndef NDEBUG
	int32_t auditCommands = g_input.numCommands;
	uint32_t sitesBefore = SpawnPlannerHash(g_aSites, (size_t)g_numSites * sizeof(SSpawnSite));
	uint32_t occupancyBefore = SpawnPlannerHash(&g_aOccupancy[g_occupancyActive.load()], sizeof(SSpawnPlannerOccupancy));
	/* The commands are immutable once queued; the count is the hand-over's
	 * own state (the cycle consumes it), so it is not part of the hash. */
	uint32_t inputBefore = SpawnPlannerHash(g_input.commands,
		(size_t)auditCommands * sizeof(SSpawnRankCommand));
	uint32_t otherOutputBefore = SpawnPlannerHash(&g_aOutput[1 - next], sizeof(SSpawnPlannerOutput));
#endif

	SpawnPlannerRunCycle(g_aSites, g_numSites, &g_aOccupancy[g_occupancyActive.load()], &g_input, pOutput);
	/* The commands are spent: the next frames queue from an empty ring. */
	g_input.numCommands = 0;

#ifndef NDEBUG
	assert(SpawnPlannerHash(g_aSites, (size_t)g_numSites * sizeof(SSpawnSite)) == sitesBefore);
	assert(SpawnPlannerHash(&g_aOccupancy[g_occupancyActive.load()], sizeof(SSpawnPlannerOccupancy)) == occupancyBefore);
	assert(SpawnPlannerHash(g_input.commands,
		(size_t)auditCommands * sizeof(SSpawnRankCommand)) == inputBefore);
	assert(SpawnPlannerHash(&g_aOutput[1 - next], sizeof(SSpawnPlannerOutput)) == otherOutputBefore);
#endif

	g_active.store(next);
}

static void
SpawnPlannerWorkerLoop(void)
{
	std::unique_lock<std::mutex> lock(g_mutex);

	for (;;) {
		g_wake.wait(lock, [] { return g_bWake || g_bStop; });
		if (g_bStop)
			return;
		g_bWake = false;
		lock.unlock();
		SpawnPlannerDoCycle();
		lock.lock();
		g_bInFlight = false;
		g_bInFlightAtomic.store(false);
		g_done.notify_one();
	}
}

void
CSpawnPlannerWorker::Start(void)
{
	if (gp_thread || !g_bThreaded || !IsEnabled())
		return;
	g_bStop = false;
	g_bWake = false;
	g_bInFlight = false;
	g_bInFlightAtomic.store(false);
	gp_thread = new std::thread(SpawnPlannerWorkerLoop);
}

void
CSpawnPlannerWorker::Stop(void)
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
	/* A stop joins whatever cycle was in flight, so the in-flight flags go
	 * with it: the state is startable again and Join can never wait on a
	 * thread that no longer exists. */
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_bInFlight = false;
	}
	g_bInFlightAtomic.store(false);
	g_bWake = false;
}

void
CSpawnPlannerWorker::SetSites(const SSpawnSite *sites, int32_t numSites)
{
	AssertIdle();
	if (sites == 0 || numSites <= 0) {
		g_numSites = 0;
		return;
	}
	if (numSites > NUM_PATHCONNECTIONS)
		numSites = NUM_PATHCONNECTIONS;
	memcpy(g_aSites, sites, (size_t)numSites * sizeof(SSpawnSite));
	g_numSites = numSites;
}

void
CSpawnPlannerWorker::RefreshSiteFlags(const SSpawnSite *sites, int32_t numSites)
{
	int32_t i, n;

	AssertIdle();
	if (sites == 0)
		return;
	/* The index's RefreshFlags re-derives the flags and nothing else: the
	 * geometry, the node pairs, the lanes, the rarities and the order never
	 * change after the build. */
	n = numSites < g_numSites ? numSites : g_numSites;
	for (i = 0; i < n; i++)
		g_aSites[i].flags = sites[i].flags;
}

void
CSpawnPlannerWorker::Reset(void)
{
	AssertIdle();
	g_input.cycle = 0;
	g_input.nowMs = 0;
	g_input.numCommands = 0;
	memset(&g_aOutput[0], 0, sizeof(g_aOutput[0]));
	memset(&g_aOutput[1], 0, sizeof(g_aOutput[1]));
	memset(&g_aOccupancy[0], 0, sizeof(g_aOccupancy[0]));
	memset(&g_aOccupancy[1], 0, sizeof(g_aOccupancy[1]));
	g_active.store(0);
	g_occupancyActive.store(0);
}

bool
CSpawnPlannerWorker::QueueRank(const SSpawnCarRequest &request, const SSpawnOffer *offers,
	const SNearBubbleCandidate *bubbles, int32_t numOffers)
{
	SSpawnRankCommand *pCommand;

	AssertIdle();
	if (numOffers < 0)
		numOffers = 0;
	if (numOffers > SPAWN_SERVICE_MAX_CANDIDATES)
		numOffers = SPAWN_SERVICE_MAX_CANDIDATES;
	if (g_input.numCommands >= SPAWN_PLANNER_RING)
		return false;
	pCommand = &g_input.commands[g_input.numCommands++];
	pCommand->request = request;
	pCommand->numOffers = numOffers;
	if (numOffers > 0) {
		memcpy(pCommand->offers, offers, (size_t)numOffers * sizeof(SSpawnOffer));
		memcpy(pCommand->bubbles, bubbles, (size_t)numOffers * sizeof(SNearBubbleCandidate));
	}
	return true;
}

void
CSpawnPlannerWorker::PublishOccupancy(const SSpawnOccupancyStore &store)
{
	int next;

	AssertIdle();
	next = 1 - g_occupancyActive.load();
	memcpy(g_aOccupancy[next].m_aVehicleCount, store.m_aVehicleCount,
		sizeof(g_aOccupancy[next].m_aVehicleCount));
	memcpy(g_aOccupancy[next].m_aSiteExpiry, store.m_aSiteExpiry,
		sizeof(g_aOccupancy[next].m_aSiteExpiry));
	memcpy(g_aOccupancy[next].m_aSiteInUse, store.m_aSiteInUse,
		sizeof(g_aOccupancy[next].m_aSiteInUse));
	g_occupancyActive.store(next);
}

bool
CSpawnPlannerWorker::Run(uint32_t nowMs)
{
	if (g_input.numCommands == 0)
		return false;
	if (!g_bThreaded) {
		/* The cycle's number is the publication's marker: the main thread
		 * reads a block whose number moved, and a frame with no hand-over
		 * re-reads nothing. */
		g_input.cycle++;
		g_input.nowMs = nowMs;
		SpawnPlannerDoCycle();
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		/* The contract's window is bounded by Join before Run, so no cycle is
		 * in flight here. If one ever were (a missed join), the frame's
		 * commands stay queued rather than run twice. */
		AssertIdle();
		if (g_bInFlight)
			return false;
		if (gp_thread == 0)
			return false;
		/* The publication's marker and the publish's clock: the occupancy copy
		 * and this stamp describe the same instant. */
		g_input.cycle++;
		g_input.nowMs = nowMs;
		g_bWake = true;
		g_bInFlight = true;
	}
	g_bInFlightAtomic.store(true);
	g_wake.notify_one();
	return true;
}

void
CSpawnPlannerWorker::Join(void)
{
	std::unique_lock<std::mutex> lock(g_mutex);

	if (!g_bInFlight)
		return;
	g_done.wait(lock, [] { return !g_bInFlight; });
}

const SSpawnPlannerOutput *
CSpawnPlannerWorker::Published(void)
{
	return &g_aOutput[g_active.load()];
}

int32_t
CSpawnPlannerWorker::CommandsPending(void)
{
	/* The worker is idle whenever the main thread asks (it queues between the
	 * join and the next hand-over), so the count is a plain read. */
	return g_input.numCommands;
}

bool
CSpawnPlannerWorker::InFlight(void)
{
	return g_bInFlightAtomic.load();
}

void
CSpawnPlannerWorker::SetThreaded(bool bThreaded)
{
	/* Only between cycles: the caller is the main thread, with the worker
	 * joined or never started. */
	AssertIdle();
	if (bThreaded == g_bThreaded)
		return;
	if (!bThreaded)
		Stop();			/* the idle thread is joined away */
	g_bThreaded = bThreaded;
}

void
CSpawnPlannerWorker::AssertIdle(void)
{
#ifndef NDEBUG
	/* The worker must be idle: every entry point of the main thread's side of
	 * the boundary joins first. A release build compiles this away. */
	assert(!g_bInFlightAtomic.load());
#endif
}
