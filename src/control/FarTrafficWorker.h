#ifndef __GTA_FARTRAFFICWORKER_H__
#define __GTA_FARTRAFFICWORKER_H__

/*
 * The far traffic layer's simulation, on a worker thread (VICE-133).
 *
 * VICE-128 wrote the layer single-threaded with the boundary the VICE-131 spike
 * fixed (/tmp/opencode/vice-131-spike.md section 4): the whole of it is
 * arithmetic on the layer's own state, and every touch of the world goes
 * through the layer's own applicator. This module is that simulation, moved
 * behind the boundary and run by one worker thread.
 *
 * THE CONTRACT (the ticket's items, and where each one holds)
 * -----------------------------------------------------------
 *
 *	1. "The worker reads only the FarTrafficWorld snapshot and its own
 *	   state."  The worker's cycle (FarTrafficSimRunCycle) takes the world
 *	   snapshot and the sim cars and nothing else: no pointer to a CVehicle,
 *	   no engine call. The sim's two views (SFarSimGraphView,
 *	   SFarSimCarView, FarTrafficWorker.cpp) read the snapshot and the
 *	   per-car state; the rules they serve are the engine-free core of
 *	   CarOnRails.h.
 *	2. "It must not call CGeneral/myrand, CWorld, CStreaming,
 *	   CColModel/CCollision, CPathFind, the pools, librw or GL."  The module
 *	   includes no engine header at all (this file compiles on its own, and
 *	   utils/far-traffic-worker compiles it without the game). There is
 *	   therefore no call to make: the names are not even declared. The
 *	   random draws are the car's own stream (FarTrafficSimRngNext, a
 *	   private 32 bit LCG), the path graph is the snapshot, the world is the
 *	   applicator's.
 *	3. "No allocation after init."  The module's storage is static (the sim
 *	   cars, the input, the two output blocks); a cycle walks arrays and
 *	   multiplies. The thread itself and its two condition variables are
 *	   created once, by Start, which the game calls at load (CGame::Initialise
 *	   after PreparePathData) - not per cycle.
 *	4. "It publishes into a double-buffered output block flipped with one
 *	   atomic."  g_aOutput[2] and the atomic flip in DoCycle; the main thread
 *	   reads the published block only after Join, so the flip's acquire
 *	   release pair is the whole of the publication.
 *	5. "Creation, removal, promotion and demotion happen only in the
 *	   applicator."  The worker's commands (SFarTrafficCommand) are only
 *	   ADD (a car the applicator has already built and seeded) and DROP (a
 *	   car the applicator has already promoted, removed or lost); the worker
 *	   never creates, deletes or switches anything, and it writes no car and
 *	   no world at all. The gates (CCarCtrl::NearBubbleAllowsCreation /
 *	   NearBubbleAllowsRemoval, the pool reserves) stay in the applicator.
 *	6. "The worker never runs during CWorld::Process or the render."  The
 *	   cycle is handed over at the end of the frame (CFarTraffic::RunWorker,
 *	   called from Idle after the render) and joined before CWorld::Process
 *	   (CFarTraffic::JoinWorker, the top of CGame::Process). Between the two
 *	   the worker runs; a debug build asserts it (AssertIdle in
 *	   CWorld::Process and RenderScene). The window is bounded by the join,
 *	   so a slow cycle costs a wait, never a race.
 *	7. "The path snapshot is taken on the main thread after
 *	   PreparePathData/BuildNodeGrid."  CFarTraffic::BuildWorldSnapshot
 *	   (FarTraffic.cpp) copies the node positions, links and connections
 *	   from ThePaths into the snapshot right after CGame::Initialise's
 *	   ThePaths.PreparePathData() and ThePaths.BuildNodeGrid().
 *	8. "Scripted road toggles are applied to it on the main thread."  The
 *	   path graph's bDisabled/bBetweenLevels flags carry a version
 *	   (CPathFind::m_nodeFlagsVersion, bumped by the toggles and by a save
 *	   load); the applicator refreshes the snapshot's flag bytes when the
 *	   version moved, with the worker joined, before it builds the frame's
 *	   cycle.
 *
 * THE FRAME, IN ORDER (FarTraffic.cpp for the engine side)
 * --------------------------------------------------------
 *
 *	frame N
 *	  top of CGame::Process	CFarTraffic::JoinWorker: the cycle of frame
 *				N-1 is complete and its output is published
 *				(the contract's window ends here)
 *	  ...			CWorld::Process, the camera, the car block
 *	  CFarTraffic::Process	the applicator: the published poses are applied
 *				to the cars (position, heading, speed, the sector
 *				re-insert, the RW frame, the alpha reveal), then the
 *				gate pass, the reach gauge and the creations run, and
 *				the frame's commands are queued
 *	  ...			the render
 *	  end of Idle		CFarTraffic::RunWorker: the queued cycle is handed
 *				to the worker (or run inline)
 *
 * The cycle the frame N applicator queues is the one the frame N+1 applicator
 * applies: the poses are one frame old when they land. That is the price of the
 * double buffer, and it is bounded by the stagger's own rate (a far car moves a
 * third as often as the near ones, and the pose lags one frame on top of that);
 * the layer's rules - the band, the cap, the switches, the gates - are untouched
 * by it.
 *
 * WHAT THE WORKER OWNS, AND WHAT IT DOES NOT
 * ------------------------------------------
 *
 *	owns	per car: the route (the two nodes and their links, the lanes and
 *		directions, the curve times), the heading, the car's own random
 *		stream, the u-turn decision;
 *		per frame: which share of the stagger moves.
 *	does not	the car itself (the applicator writes it), the counters,
 *		the band, the cap, the switches, the gates, the creations, the
 *		removals, the path snapshot's build.
 *
 * The u-turn is the one decision of the rules that is a world action
 * (CCarCtrl::SwitchVehicleToRealPhysics); the sim flags it in the pose
 * (SFarSimPose::bUTurn) and the applicator makes the call, on the main thread,
 * where every other world write already is.
 */

#include <stdint.h>

/* The shared car-on-rails rules (CarOnRails.h) are what the sim's views serve;
 * FarTraffic.h holds the layer's band, cap and stagger arithmetic. */
#include "CarOnRails.h"
#include "FarTraffic.h"
#include "config.h"		/* NUM_PATHNODES, NUM_CARPATHLINKS, NUM_PATHCONNECTIONS */

/*
 * The path snapshot (FarTrafficWorld in the spike's words): the parts of the
 * path graph the simulation reads, as plain arrays. It is built once by
 * CFarTraffic::BuildWorldSnapshot and never written again except for the node
 * flags (bDisabled/bBetweenLevels), which the applicator refreshes on the main
 * thread when the graph's own flag version moves (CPathFind::m_nodeFlagsVersion).
 *
 * The node positions are metres (the graph stores eighths); the link direction
 * is the unit vector (the graph stores hundredths); the lane offset is
 * CCarPathLink::OneWayLaneOffset, precomputed, so the worker does not repeat
 * it per car per update.
 */
struct SFarSimNode
{
	float x, y, z;
	int16_t firstLink;
	uint8_t numLinks;
	uint8_t flags;
};

enum {
	FAR_SIM_NODE_DEADEND = 1,
	FAR_SIM_NODE_DISABLED = 2,
	FAR_SIM_NODE_BETWEENLEVELS = 4,
};

struct SFarSimLink
{
	float x, y;
	float dirX, dirY;
	float laneOffset;
	int16_t pathNodeIndex;
	int8_t numLeftLanes, numRightLanes;
};

struct SFarTrafficWorld
{
	int32_t numNodes;
	int32_t numLinks;
	int32_t numConnections;
	SFarSimNode nodes[NUM_PATHNODES];
	SFarSimLink links[NUM_CARPATHLINKS];
	uint16_t connectionNode[NUM_PATHCONNECTIONS];	/* ConnectedNode, masked */
	int16_t connectionLink[NUM_PATHCONNECTIONS];	/* m_carPathConnections */
};

/*
 * One far car's simulation state (the "own state" of the spike's seam). It is
 * the route state of CAutoPilot plus the two per-car constants the rail
 * arithmetic reads; the car itself (the pool slot) is the applicator's, and the
 * only shared thing between the two is the slot.
 */
struct SFarSimCar
{
	uint16_t slot;		/* the vehicle pool slot: the car's identity */
	uint8_t bUTurn;		/* the pick gave up: the applicator turns it around */
	uint8_t pad;
	int32_t currentRouteNode, nextRouteNode, prevRouteNode;
	int32_t timeEnteredCurve, timeToSpendOnCurrentCurve;
	int32_t currentPathNodeInfo, nextPathNodeInfo, previousPathNodeInfo;
	int8_t currentDirection, nextDirection, previousDirection;
	int8_t currentLane, nextLane;
	int16_t routeSeed;	/* CVehicle::m_nRouteSeed (the stream's seed) */
	uint16_t randomSeed;	/* CEntity::m_randomSeed (the pose's own offset) */
	uint8_t bStayInCurrentLevel, bStayInFastLane;	/* AutoPilot's two flags */
	uint8_t pad2, pad3;
	uint32_t rng;		/* set by the cycle: the car's own stream */
	float maxTrafficSpeed;	/* AutoPilot::m_fMaxTrafficSpeed */
	float heightAboveRoad;	/* CVehicle::GetHeightAboveRoad */
	float forwardX, forwardY;	/* the heading the last applied pose wrote */
};

/*
 * The published pose of one car: what the applicator writes into the car. `x`,
 * `y`, `z` are the world position (the curve plus the path's own height plus
 * the car's height above the road), `dirX/dirY` the curve's tangent (the move
 * speed is this divided by GAME_SPEED_TO_CARAI_SPEED) and `forwardX/forwardY`
 * the flattened, normalised heading.
 */
struct SFarSimPose
{
	uint16_t index;		/* the entry index in the layer's list */
	uint16_t slot;		/* the same car's pool slot (the applicator checks) */
	uint8_t bUTurn;
	uint8_t bShortCurve;	/* the pick's clamped curve (the engine's "fout") */
	float x, y, z;
	float dirX, dirY;
	float forwardX, forwardY;
};

/*
 * The commands of the frames between two cycles: what the applicator did to its
 * own list. The worker applies them in the same order to its own array, which
 * is what keeps the two lists in lockstep (the stagger's index is the entry
 * index of both). A frame queues what it changed; a frame whose cycle the
 * main loop never handed over (an early return from Idle) leaves its commands
 * queued, and the next cycle applies them all, in order.
 */
enum {
	FAR_SIM_OP_ADD = 1,	/* `car` is the seed (the applicator built it) */
	FAR_SIM_OP_DROP = 2,	/* only `car.slot` is read */
};

struct SFarTrafficCommand
{
	uint8_t op;
	uint8_t pad;
	int16_t pad2;
	SFarSimCar car;
};

/* The command bound: every command is an add of a car the count had room for or
 * a drop of a car that was in the list, so a run of frames between two cycles
 * cannot produce more than an add and a drop per slot, and the creations of one
 * frame are FAR_TRAFFIC_CREATE_ATTEMPTS on top. */
#define FAR_TRAFFIC_SIM_COMMANDS (2 * FAR_TRAFFIC_MAX + FAR_TRAFFIC_CREATE_ATTEMPTS)

struct SFarTrafficInput
{
	uint32_t cycle;		/* the cycle's number (echoed in the output) */
	uint32_t now;		/* the game time the arithmetic runs at */
	uint32_t frameCounter;	/* the stagger's frame */
	int32_t numCommands;
	SFarTrafficCommand commands[FAR_TRAFFIC_SIM_COMMANDS];
};

struct SFarTrafficOutput
{
	uint32_t cycle;
	int32_t numCars;	/* the layer after the commands */
	int32_t numPoses;	/* the cars this cycle moved (the stagger's share) */
	int32_t numShortCurves;	/* the pick's clamped curves (the "fout" case) */
	int32_t numLost;	/* commands that could not be applied: must stay 0 */
	SFarSimPose poses[FAR_TRAFFIC_MAX];
};

/*
 * The simulation. These are the rules of the far car's update, in the worker's
 * own terms:
 *
 *	FarTrafficSimUpdateCar	one car's frame: the pick when the curve is
 *				spent, the rail pose (CarOnRails.h), the path's
 *				own height, the heading. No world, no engine.
 *	FarTrafficSimRunCycle	the commands of the frames since the last
 *				cycle, then the share of the stagger, then the
 *				publication.
 *	FarTrafficSimRngSeed	the car's own stream's seed.
 *
 * They are free functions (not methods of the thread class) so the evidence
 * program can drive them directly, with its own world and cars.
 */
void FarTrafficSimUpdateCar(const SFarTrafficWorld *pWorld, SFarSimCar *pCar, uint32_t now, SFarSimPose *pPose);
void FarTrafficSimRunCycle(const SFarTrafficWorld *pWorld, SFarSimCar *pCars, int32_t *pNumCars,
	const SFarTrafficInput *pInput, SFarTrafficOutput *pOutput);
uint32_t FarTrafficSimRngSeed(uint16_t slot, uint16_t randomSeed, int16_t routeSeed);

/* True when the frame's share of the stagger over `numCars` is not empty (the
 * cycle would move a car). The main thread uses it to skip the handshake on the
 * frames that have nothing to do. */
bool FarTrafficFrameHasShare(uint32_t frameCounter, int32_t numCars);

/*
 * The thread, the input and the double-buffered output. The cycle's four calls
 * (BeginCycle, AddCar/DropCar, EndCycle, Run) are the applicator's; Join is the
 * frame's and the applicator's barrier; Published is read after Join.
 *
 *	Start/Stop		the thread's life (once, at load and shutdown)
 *	SetWorld/ResetCars	the snapshot and an empty layer (a load)
 *	BeginCycle(now, frame)	stamp this frame's cycle (the pending commands
 *				of an unhanded-over frame are kept)
 *	AddCar/DropCar		queue one command
 *	EndCycle		the frame's commands are complete
 *	Run			hand the cycle over (or run it inline)
 *	Join			wait for the cycle in flight, if any
 *	Published		the published output block
 */
class CFarTrafficWorker
{
public:
	static void Start(void);
	static void Stop(void);
	static void SetWorld(const SFarTrafficWorld *pWorld);
	static void ResetCars(void);

	static void BeginCycle(uint32_t now, uint32_t frameCounter);
	static void AddCar(const SFarSimCar &car);
	static void DropCar(uint16_t slot);
	static void EndCycle(void);
	/* Returns true when the cycle ran (or was handed over), false when it
	 * would have done nothing and was dropped. */
	static bool Run(void);
	static void Join(void);
	static const SFarTrafficOutput *Published(void);

	/* The cycle is handed over (Run) and the output is published. The main
	 * thread reads it after Join. */

	/* A debug build's audit of the contract's window: true when the worker is
	 * inside a cycle. CWorld::Process and RenderScene assert it is false (the
	 * join at the top of CGame::Process precedes both); a release build
	 * compiles the calls away. */
	static void AssertIdle(void);

	/* The mode, for the measurement (and the escape hatch of the ticket): a
	 * threaded worker, or the same cycle run inline on the main thread. The
	 * two paths run the same simulation; only the thread differs. */
	static void SetThreaded(bool bThreaded);
	static bool IsThreaded(void);
};

#endif // __GTA_FARTRAFFICWORKER_H__
