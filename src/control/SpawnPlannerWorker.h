#ifndef __GTA_SPAWNPLANNERWORKER_H__
#define __GTA_SPAWNPLANNERWORKER_H__

/*
 * The spawn planner's rank, on a worker thread (VICE-160).
 *
 * VICE-150 wrote the planner (SpawnService.h) with the rank and the deep
 * facts in one call: the cheap gates and the bounded top-K rank over the
 * request's offers, then the deep facts of the ranked prefix and the
 * reservations, then the attempts. The rank is arithmetic on plain records -
 * the site's position, the ladder's candidate, the occupancy store's answers -
 * and it grows with the frame's candidate count; the deep facts (the live zone
 * and CPathFind::TestCoorsCloseness) and every creation are world queries and
 * stay on the main thread. This module is the rank behind the boundary, run by
 * one worker thread, in the far traffic worker's shape (VICE-133).
 *
 * THE CONTRACT (the ticket's items, and where each one holds)
 * -----------------------------------------------------------
 *
 *	1. "The worker reads only its own read-only snapshot and its own
 *	   state."  The cycle (SpawnPlannerDoCycle) reads three blocks and
 *	   nothing else: the site snapshot (the index's sites, copied by the
 *	   main thread), the published occupancy copy (double-buffered, filled
 *	   by the main thread), and the rank commands (the offers and the
 *	   ladder's candidates the main thread built). No pointer to the site
 *	   index, the store, a CVehicle or the world crosses the boundary.
 *	2. "It must not call CGeneral/myrand, CWorld, CStreaming,
 *	   CColmodel/CCollision, CPathFind, the pools, librw or GL."  The module
 *	   includes no engine header beyond the engine-free planner core
 *	   (SpawnService.h), declares none of those names, and calls none. The
 *	   rank is the planner's own function (SpawnRankSites), not a copy: the
 *	   inline path and the worker run the same code. utils/spawn-service/
 *	   worker.cpp compiles this file without the game.
 *	3. "No allocation after init."  The module's storage is static (the site
 *	   snapshot, the two occupancy copies, the command ring, the two output
 *	   blocks); a cycle walks arrays. The thread is created once by Start,
 *	   which the game calls at load (CSpawnService::BuildPlannerWorker after
 *	   CSpawnSites::Build) - not per cycle; the mutex and the two condition
 *	   variables are static like the rest.
 *	4. "It publishes into a double-buffered output block flipped with one
 *	   atomic."  g_aOutput[2] and the atomic flip in DoCycle; the main
 *	   thread reads the published block only after Join, so the flip's
 *	   acquire/release pair is the whole of the publication.
 *	5. "The worker only ranks."  It reserves nothing, resolves no deep fact,
 *	   attempts no creation, writes no counter and no world. The main
 *	   thread's tail does all four: it re-checks the live store, reserves in
 *	   ranked order, resolves the deep facts of the prefix, and runs the
 *	   attempts (SpawnPlanFromRank, SpawnRunPlan). The rank's cheap refusals
 *	   are published as counts and the tail adds them to the telemetry, so
 *	   the counters keep one owner (the main thread). The visibility
 *	   sampling stays on the main thread with its ray budget: the ladder's
 *	   candidate (the frustum, the occluder hint, the fade, the bands) is
 *	   built by CCarCtrl::NearBubbleCandidateFor before the command is
 *	   queued, and the worker only derives the arm from it
 *	   (NearBubbleArmFor, arithmetic); no ray is spent.
 *	6. "The worker never runs during CWorld::Process or the render."  The
 *	   cycle is handed over at the end of the frame (CSpawnService::
 *	   RunPlannerWorker, from Idle after the render) and joined at the top of
 *	   CGame::Process (CSpawnService::JoinPlannerWorker), before
 *	   CWorld::Process. Between the two the worker runs; a debug build
 *	   asserts it (AssertIdle in CWorld::Process and RenderScene) and audits
 *	   the write boundary (the cycle hashes its inputs and the other output
 *	   block before and after the rank and asserts they did not move). The
 *	   window is bounded by the join, so a slow rank costs a wait, never a
 *	   race.
 *	7. "The site snapshot is taken on the main thread after CSpawnSites::
 *	   Build."  CSpawnService::BuildPlannerWorker copies the index's sites
 *	   into the snapshot (SetSites) and starts the thread, right after the
 *	   index is built.
 *	8. "The site flags are refreshed when the index's flags refresh."  The
 *	   script's road toggles and a save load move the graph's node flags and
 *	   CSpawnSites::RefreshFlags re-derives the sites' own flags. The service
 *	   reads the index's refresh count at the end of the frame and copies the
 *	   flags (RefreshSiteFlags) before the hand-over, with the worker idle -
 *	   the FarTrafficRefreshWorldFlags pattern.
 *
 * THE FRAME, IN ORDER (SpawnService.cpp for the engine side)
 * ----------------------------------------------------------
 *
 *	frame N
 *	  top of CGame::Process	CSpawnService::JoinPlannerWorker: the cycle of
 *				frame N-1 is complete and its plans move to the
 *				main thread's pending queue (the contract's
 *				window ends here)
 *	  CWorld::Process	CSpawnService::RequestCar: one pending plan is
 *				applied (the live store re-check, the reserve
 *				in ranked order, the deep facts of the prefix,
 *				the attempts), then the frame's own request is
 *				queued as a rank command
 *	  ...			the render
 *	  end of Idle		CSpawnService::RunPlannerWorker: the occupancy copy
 *				is published, the site flags are refreshed if the
 *				index moved them, and the commands are handed over
 *				(or run inline)
 *
 * THE ONE-FRAME LAG, STATED
 * -------------------------
 *
 * A plan ranked on frame N-1 is applied on frame N: the car of a road-site
 * request appears one frame after the request. The request gates (the budget
 * and the primary model's readiness) ran on the queue frame, so the apply
 * admits at most one creation on a budget that filled in between; the
 * primitive's own checks and the pool reserve stand. The explicit kind keeps
 * the inline planner and the same-frame handle (RequestExplicit creates the
 * car and returns it in the call); a road-site caller that needs the handle
 * (`ppCreated`) also keeps the inline planner. The parked kind, when its
 * requester lands, is synchronous and keeps it too.
 *
 * THE CONTENTION, AND THE PREFIX BOUND
 * ------------------------------------
 *
 * The worker ranks against the occupancy copy as it stood at the top of the
 * applying frame (the publish is at the end of the previous frame, after the
 * world's moves). The store can move between the publish and the apply: a
 * synchronous request (an explicit creation, an inline road-site creation)
 * reserves a site, a car crosses into a candidate's cell, the store's own
 * hooks add a count. The tail resolves every such conflict against the live
 * store: a reserved site, a cell at its capacity and a failed reserve all
 * refuse with OCCUPANCY and the tail moves to the next ranked candidate. The
 * rank and the tail share their code with the inline path, so the plans match
 * the inline planner's for the same store state while the number of tail
 * conflicts and deep refusals stays inside the published prefix
 * (SPAWN_SERVICE_DEEP_CANDIDATES = 8). A reservation conflict does not move
 * the rank's own keys for the other candidates, so the examined prefix and the
 * refusal rows match too. A cell that fills after the publish does move the
 * rank's second key (the cell count) for every candidate in that cell, so the
 * inline planner's rank over the filled store orders the rest differently and
 * may count a different number of cheap drops; the plans still agree (the
 * next ranked candidates are the same), and the harness prints both counts.
 * Past the prefix the worker's plan is empty - the stated bound - where the
 * inline planner could reach a ninth candidate; the plan keeps its first drop
 * reason (OCCUPANCY) and the request is refused for that frame.
 *
 * A site that was closed at the publish and opened before the apply cannot
 * enter the plan (the copy is a filter): the next request picks it up. The
 * reverse (closed after the publish) is the tail's conflict and is resolved.
 *
 * WHAT THE WORKER OWNS, AND WHAT IT DOES NOT
 * ------------------------------------------
 *
 *	owns	the cheap gates (the reservation read, the cell capacity, the
 *		ladder's arm) and the bounded top-K rank, over the snapshot;
 *	does not	the store, the live site index, the deep facts, the
 *		reservations, the attempts, the telemetry, the world.
 */

#include <stdint.h>

#include "SpawnService.h"	/* the planner's core: the offers, the rank, the plan */

/* The occupancy copy (VICE-160): the vehicle counts and the reservation table
 * of the store, as the rank reads them. The main thread fills the inactive
 * copy from the live store and flips; the worker reads the active one. The ped
 * counts and the handle records are never copied: the rank reads vehicles.
 *
 * Two copies are held (the double buffer), so the main thread can fill the
 * next frame's copy while the worker reads the published one. The game fills
 * only after the join (the worker idle), but the contract keeps the two-buffer
 * shape the far worker's output has: one atomic names the read copy. */
struct SSpawnPlannerOccupancy
{
	uint16_t m_aVehicleCount[SPAWN_OCCUPANCY_CELLS];
	uint32_t m_aSiteExpiry[SPAWN_OCCUPANCY_SITES];
	bool m_aSiteInUse[SPAWN_OCCUPANCY_SITES];

	SSpawnOccupancyView View(void) const;
};

/*
 * One rank command: the request's offers and the ladder's candidates, the
 * two inputs the rank reads, plus the request envelope the apply echoes. A
 * frame queues one command per road-site request that takes the worker; the
 * ring holds SPAWN_PLANNER_RING of them (the steady state is one frame's
 * requests, and a full ring falls back to the inline planner and counts the
 * fallback).
 */
struct SSpawnRankCommand
{
	SSpawnCarRequest request;
	int32_t numOffers;
	SSpawnOffer offers[SPAWN_SERVICE_MAX_CANDIDATES];
	SNearBubbleCandidate bubbles[SPAWN_SERVICE_MAX_CANDIDATES];
};

struct SSpawnPlannerInput
{
	uint32_t cycle;		/* the cycle's number (echoed in the output) */
	uint32_t nowMs;		/* the publish's clock: the reservation test's now */
	int32_t numCommands;
	SSpawnRankCommand commands[SPAWN_PLANNER_RING];
};

/*
 * The published plans of one cycle: one ranked plan per command, in the order
 * the commands were queued. `numLost` is a command the cycle could not rank
 * (it must stay 0; the ring makes the command's inputs complete). The cheap
 * counts are the rank's, for the tail's telemetry rows.
 */
struct SSpawnPlannerOutput
{
	uint32_t cycle;
	int32_t numPlans;
	int32_t numRanked;	/* the offers examined over the batch (the cost row) */
	int32_t numLost;	/* commands the cycle could not rank: must stay 0 */
	SSpawnRankedPlan plans[SPAWN_PLANNER_RING];
};

/*
 * The thread, the snapshot and the handshake. The main thread's calls
 * (SetSites, RefreshSiteFlags, QueueRank, PublishOccupancy, Run) all run with
 * the worker idle (after Join and before the hand-over); Join is the frame's
 * barrier and Published is read after it.
 *
 *	Start/Stop		the thread's life (once, at load and shutdown)
 *	SetSites		the site snapshot (a build or a load)
 *	RefreshSiteFlags	the flags of the index's sites (its RefreshFlags
 *				moved); the geometry never changes
 *	Reset			an empty ring and both output blocks zeroed (a
 *				load: nothing stale can be applied)
 *	QueueRank		queue one request's offers and bubbles
 *	PublishOccupancy	the inactive copy of the store's counts and
 *				reservations, then the flip
 *	Run			hand the frame's commands over (or run them inline)
 *	Join			wait for the cycle in flight, if any
 *	Published		the published output block
 *	CommandsPending		the commands waiting for the next hand-over
 *	AssertIdle		the debug window audit (CWorld::Process,
 *				RenderScene)
 *	InFlight		the same state, readable (the evidence program)
 *	SetThreaded		the test seam: the worker thread, or the same
 *				cycle run inline (the game never calls it)
 */
class CSpawnPlannerWorker
{
public:
	static void Start(void);
	static void Stop(void);
	static void SetSites(const SSpawnSite *sites, int32_t numSites);
	static void RefreshSiteFlags(const SSpawnSite *sites, int32_t numSites);
	static void Reset(void);

	static bool QueueRank(const SSpawnCarRequest &request, const SSpawnOffer *offers,
		const SNearBubbleCandidate *bubbles, int32_t numOffers);
	static void PublishOccupancy(const SSpawnOccupancyStore &store);
	static bool Run(uint32_t nowMs);
	static void Join(void);
	static const SSpawnPlannerOutput *Published(void);
	static int32_t CommandsPending(void);

	static void AssertIdle(void);
	static bool InFlight(void);
	static void SetThreaded(bool bThreaded);
	static bool IsEnabled(void) { return SPAWN_PLANNER_WORKER != 0; }
};

/*
 * The cycle itself (engine-free, the evidence program drives it directly):
 * every queued command is ranked (SpawnRankSites) against the published
 * occupancy copy, and the ranked plans are written into `pOutput`. It writes
 * nothing else; a debug build hashes every input and the other output block
 * around the call (SpawnPlannerDoCycle's audit).
 */
void SpawnPlannerRunCycle(const SSpawnSite *sites, int32_t numSites,
	const SSpawnPlannerOccupancy *pOccupancy, const SSpawnPlannerInput *pInput,
	SSpawnPlannerOutput *pOutput);

#endif // __GTA_SPAWNPLANNERWORKER_H__
