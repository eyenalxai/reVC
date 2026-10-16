#include "common.h"

#include "Game.h"
#include "General.h"
#include "World.h"
#include "Population.h"
#include "CopPed.h"
#include "Wanted.h"
#include "FileMgr.h"
#include "Gangs.h"
#include "ModelIndices.h"
#include "Zones.h"
#include "PathFind.h"
#include "CivilianPed.h"
#include "EmergencyPed.h"
#include "Replay.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "CarCtrl.h"
#include "IniFile.h"
#include "VisibilityPlugins.h"
#include "PedPlacement.h"
#include "DummyObject.h"
#include "Script.h"
#include "Shadows.h"
#include "SurfaceTable.h"
#include "Weather.h"
#include "Darkel.h"
#include "Streaming.h"
#include "Clock.h"
#include "WaterLevel.h"
#include "TrafficRings.h"
#include "FillBurst.h"

// The pedestrian ring comes from the one table (TrafficRings.h): the creation
// ring, the off-screen band and the removal distances scale with it. The
// on-foot budget, its driver and passenger allowance, the pool reserve and the
// spawn rate come from the cap table (PopulationBudget.h, VICE-123).
#define PED_AREA_DENSITY_SCALE	3.0f // scale the zone density budget for dense on-foot crowds

// The off-network spawns (VICE-122). A bounded fraction of the pedestrian
// spawns skips the node path and takes a free position in the creation ring,
// and the existing wander state walks the pedestrian to the closest ped node
// from there. A node is ground a pedestrian can stand on; a free position needs
// the checks below, and the walk to the network needs a bound and a backstop.
// The constants are the one place the fraction and the checks are tuned.
//
// The fraction of zero is special: no random number is drawn, so the node path
// stays bit-exact with the behaviour before this change.
#define PED_OFF_NETWORK_FRACTION	0.15f	// the fraction of the spawns that tries a free position
#define PED_OFF_NETWORK_ATTEMPTS	5	// free positions tried for one spawn
#define PED_OFF_NETWORK_LEVEL_MARGIN	3.0f	// the ground-to-position tolerance the node path itself allows
#define PED_OFF_NETWORK_ROOF_CAST	50.0f	// the surface cast starts this far above the position
#define PED_OFF_NETWORK_STEP		0.5f	// the surface may stand this far above the ground: a step, not a roof
#define PED_OFF_NETWORK_PROBE_HEIGHT	0.9f	// the clearance probe sits this far above the ground
#define PED_OFF_NETWORK_PROBE_RADIUS	0.5f	// and is this wide: static geometry inside it rejects the spot
#define PED_OFF_NETWORK_WATER_MARGIN	0.1f	// the ground must stand this far above the water line
#define PED_OFF_NETWORK_NODE_RANGE	150.0f	// the closest ped node may not be farther: the walk bound
#define PED_OFF_NETWORK_NODE_LEVEL	5.0f	// and it may not sit on another level: the ground is a roof
#define PED_OFF_NETWORK_JOIN_DISTANCE	8.0f	// a stray this near a node has reached the network
#define PED_OFF_NETWORK_PROGRESS	0.25f	// metres of approach that count for the stray
#define PED_OFF_NETWORK_STALL_TIME	20000	// ms of walking without progress before the stray cull
#define PED_OFF_NETWORK_NO_TARGET_TIME	60000	// ms without a node to head for before the watch is given up
#define PED_OFF_NETWORK_CULL_RADIUS	3.0f	// sphere the camera must not see before a stray is removed
#define PED_OFF_NETWORK_SLOTS		32	// the bounded stray table
#define PED_OFF_NETWORK_CHECKS_PER_FRAME 2	// stray entries checked per frame

PedGroup CPopulation::ms_pPedGroups[NUMPEDGROUPS];
bool CPopulation::ms_bGivePedsWeapons;
int32 CPopulation::m_AllRandomPedsThisType = -1;
float CPopulation::PedDensityMultiplier = 1.0f;
uint32 CPopulation::ms_nTotalMissionPeds;
int32 CPopulation::MaxNumberOfPedsInUse = DEFAULT_MAX_NUMBER_OF_PEDS;
int32 CPopulation::MaxNumberOfPedsInUseInterior = DEFAULT_MAX_NUMBER_OF_PEDS_INTERIOR;
uint32 CPopulation::ms_nNumCivMale;
uint32 CPopulation::ms_nNumCivFemale;
uint32 CPopulation::ms_nNumCop;
bool CPopulation::bZoneChangeHasHappened;
uint32 CPopulation::ms_nNumEmergency;
int8 CPopulation::m_CountDownToPedsAtStart;
uint32 CPopulation::ms_nNumGang1;
uint32 CPopulation::ms_nNumGang2;
uint32 CPopulation::ms_nTotalPeds;
uint32 CPopulation::ms_nNumGang3;
uint32 CPopulation::ms_nTotalGangPeds;
uint32 CPopulation::ms_nNumGang4;
uint32 CPopulation::ms_nTotalCivPeds;
uint32 CPopulation::ms_nNumGang5;
uint32 CPopulation::ms_nNumDummy;
uint32 CPopulation::ms_nNumGang6;
uint32 CPopulation::ms_nNumGang9;
uint32 CPopulation::ms_nNumGang7;
uint32 CPopulation::ms_nNumGang8;

uint32 CPopulation::ms_nTotalCarPassengerPeds;
uint32 CPopulation::NumMiamiViceCops;

uint32 gLastSelectedCivilianIndex;
CEntity *gSunbatheObstacles[2];
CEntity *gCoupleObstacles[3];

uint32 CPopulation::ms_nOffNetworkSpawns;
uint32 CPopulation::ms_nOffNetworkStuck;
uint32 CPopulation::ms_nOffNetworkDrowned;

/* The fill burst (VICE-126). The state of the window and the counters the live
 * rig reads (utils/caps-audit/live-counters.py, the same rig the caps audit
 * uses). */
SFillBurst CPopulation::ms_FillBurst;
uint32 CPopulation::ms_nFillBurstAttempts;
uint32 CPopulation::ms_nFillBurstPlaced;
float CPopulation::ms_fFillBurstTarget;

/* Arm the pedestrian fill burst (VICE-126). The burst of every trigger has the
 * bands of the ring table: the creation ring and the off-screen band of the
 * trickle, so a pedestrian never appears closer than the off-screen band
 * behind the player. (The shipped one-frame boot burst had its own wider band,
 * from 10 m; that band dies with it, because a spawn that close is a pop-in on
 * the first turn of the camera - VICE-127 forbids it.) */
void
CPopulation::ArmFillBurst(void)
{
	FillBurstArm(&ms_FillBurst);
	ms_nFillBurstAttempts = 0;
	ms_nFillBurstPlaced = 0;
	ms_fFillBurstTarget = 0.0f;
}

/* The local on-foot budget of the area this frame: the expression
 * AddToPopulation compares its count against, in one place, so the burst stops
 * at the same line the trickle stops at. */
static float
AreaPedBudget(void)
{
	CPlayerInfo *playerInfo = &CWorld::Players[CWorld::PlayerInFocus];
	CVector playerCentreOfWorld = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	CZoneInfo zoneInfo;
	CTheZones::GetZoneInfoForTimeOfDay(&playerCentreOfWorld, &zoneInfo);
	const STrafficRings rings = CurrentTrafficRings();

	float missionAndWeatherMult = -0.8f * Sqrt(CWeather::Rain) + 1.0f;
	// Taxi side mission
	if (CTheScripts::IsPlayerOnAMission()) {
		CPed *player = FindPlayerPed();
		if (player && player->InVehicle() && player->m_pMyVehicle->IsTaxi())
			missionAndWeatherMult = 1.0f;
	}
	if (CDarkel::FrenzyOnGoing())
		missionAndWeatherMult = 1.0f;
	int selectedMaxPeds = CGame::IsInInterior() ? CPopulation::MaxNumberOfPedsInUseInterior : CPopulation::MaxNumberOfPedsInUse;

	float budget = (zoneInfo.pedDensity + zoneInfo.carDensity) * playerInfo->m_fRoadDensity * CPopulation::PedDensityMultiplier
		* (CDarkel::FrenzyOnGoing() ? 1.f : CIniFile::PedNumberMultiplier) * missionAndWeatherMult * PED_AREA_DENSITY_SCALE
		* SQR(rings.scale);
	return Min(budget, (float)selectedMaxPeds);
}

/* The line the population stops at: the on-foot budget of the area plus the
 * drivers and law enforcers, which count in ms_nTotalPeds and are added back
 * so they do not shrink the crowd (PopulationBudget.h). */
static float
AreaPedCountTarget(void)
{
	return AreaPedBudget() + CCarCtrl::NumRandomCars + CCarCtrl::NumLawEnforcerCars;
}

/* The strays: the off-network pedestrians that have not reached the network yet
 * (VICE-122). The table is bounded and the frame checks a fixed number of
 * entries, so the cost of the cull does not follow the crowd. The references
 * keep the entries valid when a pedestrian dies. */
struct COffNetworkStray
{
	CPed *ped;		// nil when the slot is free or the pedestrian is gone
	CPathNode *node;	// the node the last check measured against
	float fNodeDist;	// the distance to that node at the last check
	uint32 nSpawnTime;	// the spawn, so the oldest watch can be given up
	uint32 nStallTime;	// ms the pedestrian walked without coming closer
	uint32 nNoTargetTime;	// ms the pedestrian had no node to head for
	uint32 nLastCheckTime;	// the time of the last check, for the clocks
};

static COffNetworkStray aOffNetworkStrays[PED_OFF_NETWORK_SLOTS];
static int32 nOffNetworkStrayCursor;

/* The water line of the position's block, without the waves: the placement and
 * the cull both read this line, so neither answer moves with the wave phase. */
static bool
GetWaterLine(const CVector &pos, float *pLevel)
{
	return CWaterLevel::GetWaterLevelNoWaves(pos.x, pos.y, pos.z, pLevel);
}

/* The ground at (x, y) stands above the water line: a free position and the
 * member of a group both need this, or the pedestrian ends up in the water. */
static bool
GroundIsClearOfWater(float x, float y, float groundZ)
{
	float waterLevel;
	if (!GetWaterLine(CVector(x, y, groundZ), &waterLevel))
		return true;
	return groundZ > waterLevel + PED_OFF_NETWORK_WATER_MARGIN;
}

// The pedestrian stands under the water line: the stray reached the water.
static bool
PositionIsInWater(const CVector &pos)
{
	float waterLevel;
	if (!GetWaterLine(pos, &waterLevel))
		return false;
	return pos.z < waterLevel - PED_OFF_NETWORK_WATER_MARGIN;
}

// Stop watching a stray: free the entry and its reference.
static void
UntrackOffNetworkStray(COffNetworkStray *stray)
{
	if (stray->ped)
		stray->ped->CleanUpOldReference((CEntity**)&stray->ped);
	stray->ped = nil;
	stray->node = nil;
}

// Watch a pedestrian that spawned off the network until it reaches a node.
// Bounded: when the table is full the oldest watch is given up.
static void
TrackOffNetworkPed(CPed *ped)
{
	if (ped == nil)
		return;

	COffNetworkStray *stray = &aOffNetworkStrays[0];
	for (int32 i = 0; i < PED_OFF_NETWORK_SLOTS; i++) {
		if (aOffNetworkStrays[i].ped == nil) {
			stray = &aOffNetworkStrays[i];
			break;
		}
		if (aOffNetworkStrays[i].nSpawnTime < stray->nSpawnTime)
			stray = &aOffNetworkStrays[i];
	}
	if (stray->ped)
		stray->ped->CleanUpOldReference((CEntity**)&stray->ped);

	uint32 now = CTimer::GetTimeInMilliseconds();
	stray->ped = ped;
	stray->node = nil;
	stray->fNodeDist = 0.0f;
	stray->nSpawnTime = now;
	stray->nStallTime = 0;
	stray->nNoTargetTime = 0;
	stray->nLastCheckTime = now;
	ped->RegisterReference((CEntity**)&stray->ped);
}

// A pedestrian the placement created on a free position: count it and watch it
// until it reaches the network (VICE-122).
static void
AddOffNetworkPed(CPed *ped, bool offNetwork)
{
	if (!offNetwork || ped == nil)
		return;
	CPopulation::ms_nOffNetworkSpawns++;
	TrackOffNetworkPed(ped);
}

// Remove a stray out of sight. A pedestrian in view stays: the cull never takes
// one away in front of the player. True when the pedestrian is gone.
static bool
CullOffNetworkStray(COffNetworkStray *stray)
{
	if (TheCamera.IsSphereVisible(stray->ped->GetPosition(), PED_OFF_NETWORK_CULL_RADIUS))
		return false;
	CPed *ped = stray->ped;
	UntrackOffNetworkStray(stray);
	CPopulation::RemovePed(ped);
	return true;
}

// The stray cull (VICE-122). A stray that waits on purpose (a traffic light, a
// chat, a hit wall) is not stuck; only a pedestrian that walks towards the
// network and never comes closer is. A stray in the water is a bad placement.
// The counters follow the cull: a removed stray is counted, and a stray the
// camera can see is counted on the frame the cull can take it.
static void
ProcessOffNetworkStrays(void)
{
	uint32 now = CTimer::GetTimeInMilliseconds();

	for (int32 check = 0; check < PED_OFF_NETWORK_CHECKS_PER_FRAME; check++) {
		nOffNetworkStrayCursor = (nOffNetworkStrayCursor + 1) % PED_OFF_NETWORK_SLOTS;
		COffNetworkStray *stray = &aOffNetworkStrays[nOffNetworkStrayCursor];
		CPed *ped = stray->ped;
		if (ped == nil)
			continue;

		// A pedestrian a script claimed, a driver, a dead one, or one that
		// follows another is not the stray this cull is for.
		if (!ped->CanBeDeleted() || ped->bInVehicle || ped->DyingOrDead() ||
			ped->m_objective == OBJECTIVE_FOLLOW_CHAR_IN_FORMATION) {
			UntrackOffNetworkStray(stray);
			continue;
		}

		CVector pos = ped->GetPosition();

		// The wander state walked the pedestrian to its first node: a normal
		// pedestrian from here on.
		if (ped->m_pLastPathNode && Distance2D(ped->m_pLastPathNode->GetPosition(), pos) <= PED_OFF_NETWORK_JOIN_DISTANCE) {
			UntrackOffNetworkStray(stray);
			continue;
		}

		// The pedestrian reached the water: a bad placement. The count follows
		// the removal, so a stray the camera keeps in view stays watched and is
		// counted on the frame the cull can take it.
		if (PositionIsInWater(pos)) {
			if (CullOffNetworkStray(stray))
				CPopulation::ms_nOffNetworkDrowned++;
			continue;
		}

		// The stall clock runs while the stray walks towards the network; it
		// stands while the pedestrian waits (a traffic light, a chat, a hit
		// wall) or stands still, which is what a pedestrian does between two
		// walks. A stray with no node to head for is not a stray the walk clock
		// can measure: it keeps its watch for the no-target time and the watch
		// is then given up, so the slot comes back (a following or fleeing
		// pedestrian is not the stray this cull is for).
		CPathNode *node = ped->m_pNextPathNode ? ped->m_pNextPathNode : ped->m_pLastPathNode;
		if (node == nil) {
			stray->nNoTargetTime += now - stray->nLastCheckTime;
			stray->nLastCheckTime = now;
			if (stray->nNoTargetTime > PED_OFF_NETWORK_NO_TARGET_TIME)
				UntrackOffNetworkStray(stray);
			continue;
		}
		stray->nNoTargetTime = 0;

		bool moving = ped->m_nMoveState == PEDMOVE_WALK || ped->m_nMoveState == PEDMOVE_JOG
			|| ped->m_nMoveState == PEDMOVE_RUN || ped->m_nMoveState == PEDMOVE_SPRINT;
		if (!moving || ped->m_nWaitState != WAITSTATE_FALSE) {
			stray->nLastCheckTime = now;
			continue;
		}

		float dist = Distance2D(node->GetPosition(), pos);
		if (node != stray->node || dist < stray->fNodeDist - PED_OFF_NETWORK_PROGRESS) {
			stray->node = node;
			stray->fNodeDist = dist;
			stray->nStallTime = 0;
		}

		stray->nStallTime += now - stray->nLastCheckTime;
		stray->nLastCheckTime = now;
		if (stray->nStallTime > PED_OFF_NETWORK_STALL_TIME && CullOffNetworkStray(stray))
			CPopulation::ms_nOffNetworkStuck++;
	}
}

void
CPopulation::Initialise()
{
	debug("Initialising CPopulation...\n");

	ms_nNumCivMale = 0;
	ms_nNumCivFemale = 0;
	ms_nNumCop = 0;
	ms_nNumEmergency = 0;
	ms_nNumGang1 = 0;
	ms_nNumGang2 = 0;
	ms_nNumGang3 = 0;
	ms_nNumGang4 = 0;
	ms_nNumGang5 = 0;
	ms_nNumGang6 = 0;
	ms_nNumGang7 = 0;
	ms_nNumGang8 = 0;
	ms_nNumGang9 = 0;
	ms_nNumDummy = 0;

	ms_nTotalCarPassengerPeds = 0;
	ms_nTotalCivPeds = 0;
	ms_nTotalGangPeds = 0;
	ms_nTotalPeds = 0;
	ms_nTotalMissionPeds = 0;
	m_CountDownToPedsAtStart = 2;
	bZoneChangeHasHappened = false; // III leftover

	// The strays and their counters start empty (VICE-122). The pointers are
	// only dropped here: the strays belong to the game that ends with this call.
	memset(aOffNetworkStrays, 0, sizeof(aOffNetworkStrays));
	nOffNetworkStrayCursor = 0;
	ms_nOffNetworkSpawns = 0;
	ms_nOffNetworkStuck = 0;
	ms_nOffNetworkDrowned = 0;

	m_AllRandomPedsThisType = -1;
	PedDensityMultiplier = 1.0f;
	

	LoadPedGroups();

	debug("CPopulation ready\n");
}

void
CPopulation::RemovePed(CPed *ent)
{
	CWorld::Remove((CEntity*)ent);
	delete ent;
}

int32
CPopulation::ChooseCivilianOccupation(int32 group)
{
	if (CWeather::Rain > 0.1f) {
		int32 lastModel;
		for (int i = 0; i < 8; i++) {
			gLastSelectedCivilianIndex = CGeneral::GetRandomNumberInRange(0, NUMMODELSPERPEDGROUP);
			lastModel = ms_pPedGroups[group].models[gLastSelectedCivilianIndex];

			if (!CPopulation::IsSunbather(lastModel))
				break;
		}
		return lastModel;

	} else {
		gLastSelectedCivilianIndex = CGeneral::GetRandomNumberInRange(0, NUMMODELSPERPEDGROUP);
		return ms_pPedGroups[group].models[gLastSelectedCivilianIndex];
	}
}

int32
CPopulation::ChooseNextCivilianOccupation(int32 group)
{
	if (CWeather::Rain > 0.1f) {
		int32 lastModel;
		for (int i = 0; i < NUMMODELSPERPEDGROUP; i++) {
			++gLastSelectedCivilianIndex;
			if (gLastSelectedCivilianIndex >= NUMMODELSPERPEDGROUP)
				gLastSelectedCivilianIndex = 0;
			lastModel = ms_pPedGroups[group].models[gLastSelectedCivilianIndex];

			if (!CPopulation::IsSunbather(ms_pPedGroups[group].models[gLastSelectedCivilianIndex]))
				break;
		}
		return lastModel;

	} else {
		++gLastSelectedCivilianIndex;
		if (gLastSelectedCivilianIndex >= NUMMODELSPERPEDGROUP)
			gLastSelectedCivilianIndex = 0;
		return ms_pPedGroups[group].models[gLastSelectedCivilianIndex];
	}
}

// returns eCopType
int32
CPopulation::ChoosePolicePedOccupation()
{
	CGeneral::GetRandomNumber();
	return COP_STREET;
}

void
CPopulation::LoadPedGroups()
{
	int fd;
	char line[1024];
	int nextPedGroup = 0;
	// char unused[16]; // non-existence of that in mobile kinda verifies that
	char modelName[256];

	CFileMgr::ChangeDir("\\DATA\\");
	fd = CFileMgr::OpenFile("PEDGRP.DAT", "r");
	CFileMgr::ChangeDir("\\");
	while (CFileMgr::ReadLine(fd, line, sizeof(line))) {
		int end;
		// find end of line
		for (end = 0; ; end++) {
			if (line[end] == '\n')
				break;
			if (line[end] == ',' || line[end] == '\r')
				line[end] = ' ';
		}
		line[end] = '\0';
		int cursor = 0;
		int i;
		for (i = 0; i < NUMMODELSPERPEDGROUP; i++) {
			while (line[cursor] <= ' ' && line[cursor] != '\0')
				++cursor;

			if (line[cursor] == '#')
				break;

			// find next whitespace
			int nextWhitespace;
			for (nextWhitespace = cursor; line[nextWhitespace] > ' '; ++nextWhitespace)
				;

			if (cursor == nextWhitespace)
				break;
			
			// read until next whitespace
			strncpy(modelName, &line[cursor], nextWhitespace - cursor);
			modelName[nextWhitespace - cursor] = '\0';
			CModelInfo::GetModelInfo(modelName, &ms_pPedGroups[nextPedGroup].models[i]);
			cursor = nextWhitespace;
		}
		if (i == NUMMODELSPERPEDGROUP)
			nextPedGroup++;
	}
	CFileMgr::CloseFile(fd);
}

void
CPopulation::UpdatePedCount(ePedType pedType, bool decrease)
{
	if (decrease) {
		switch (pedType) {
			case PEDTYPE_PLAYER1:
			case PEDTYPE_PLAYER2:
			case PEDTYPE_PLAYER3:
			case PEDTYPE_PLAYER4:
			case PEDTYPE_UNUSED1:
			case PEDTYPE_SPECIAL:
				return;
			case PEDTYPE_CIVMALE:
				--ms_nNumCivMale;
				break;
			case PEDTYPE_CIVFEMALE:
				--ms_nNumCivFemale;
				break;
			case PEDTYPE_COP:
				--ms_nNumCop;
				break;
			case PEDTYPE_GANG1:
				--ms_nNumGang1;
				break;
			case PEDTYPE_GANG2:
				--ms_nNumGang2;
				break;
			case PEDTYPE_GANG3:
				--ms_nNumGang3;
				break;
			case PEDTYPE_GANG4:
				--ms_nNumGang4;
				break;
			case PEDTYPE_GANG5:
				--ms_nNumGang5;
				break;
			case PEDTYPE_GANG6:
				--ms_nNumGang6;
				break;
			case PEDTYPE_GANG7:
				--ms_nNumGang7;
				break;
			case PEDTYPE_GANG8:
				--ms_nNumGang8;
				break;
			case PEDTYPE_GANG9:
				--ms_nNumGang9;
				break;
			case PEDTYPE_EMERGENCY:
			case PEDTYPE_FIREMAN:
				--ms_nNumEmergency;
				break;
			case PEDTYPE_CRIMINAL:
				--ms_nNumCivMale;
				break;
			case PEDTYPE_PROSTITUTE:
				--ms_nNumCivFemale;
				break;
			case PEDTYPE_UNUSED2:
				--ms_nNumDummy;
				break;
			default:
				Error("Unknown ped type, UpdatePedCount, Population.cpp");
				break;
		}
	} else {
		switch (pedType) {
			case PEDTYPE_PLAYER1:
			case PEDTYPE_PLAYER2:
			case PEDTYPE_PLAYER3:
			case PEDTYPE_PLAYER4:
			case PEDTYPE_UNUSED1:
			case PEDTYPE_SPECIAL:
				return;
			case PEDTYPE_CIVMALE:
				++ms_nNumCivMale;
				break;
			case PEDTYPE_CIVFEMALE:
				++ms_nNumCivFemale;
				break;
			case PEDTYPE_COP:
				++ms_nNumCop;
				break;
			case PEDTYPE_GANG1:
				++ms_nNumGang1;
				break;
			case PEDTYPE_GANG2:
				++ms_nNumGang2;
				break;
			case PEDTYPE_GANG3:
				++ms_nNumGang3;
				break;
			case PEDTYPE_GANG4:
				++ms_nNumGang4;
				break;
			case PEDTYPE_GANG5:
				++ms_nNumGang5;
				break;
			case PEDTYPE_GANG6:
				++ms_nNumGang6;
				break;
			case PEDTYPE_GANG7:
				++ms_nNumGang7;
				break;
			case PEDTYPE_GANG8:
				++ms_nNumGang8;
				break;
			case PEDTYPE_GANG9:
				++ms_nNumGang9;
				break;
			case PEDTYPE_EMERGENCY:
			case PEDTYPE_FIREMAN:
				++ms_nNumEmergency;
				break;
			case PEDTYPE_CRIMINAL:
				++ms_nNumCivMale;
				break;
			case PEDTYPE_PROSTITUTE:
				++ms_nNumCivFemale;
				break;
			case PEDTYPE_UNUSED2:
				++ms_nNumDummy;
				break;
			default:
				Error("Unknown ped type, UpdatePedCount, Population.cpp");
				break;
		}
	}
}

int
CPopulation::ChooseGangOccupation(int gangId)
{
	return CGangs::ChooseGangPedModel(gangId);
}

void
CPopulation::DealWithZoneChange(eLevelName oldLevel, eLevelName newLevel, bool forceIndustrialZone)
{
}

/* The fill burst of the pedestrians, defined below the trickle it replaces. */
static bool PedModelsAreResident(void);
static void SpendFillBurstFrame(void);

void
CPopulation::Update(bool addPeds)
{
	if (CReplay::IsPlayingBack())
		return;

	ManagePopulation();
	RemovePedsIfThePoolGetsFull();
	MoveCarsAndPedsOutOfAbandonedZones();
	// The strays wait out a cutscene: the camera there is not the game view.
	if (!CCutsceneMgr::IsRunning())
		ProcessOffNetworkStrays();

	if (m_CountDownToPedsAtStart != 0) {
		// The countdown lets the world settle after boot and after a restart;
		// its end arms the fill burst (VICE-126).
		if (--m_CountDownToPedsAtStart == 0)
			ArmFillBurst();
		return;
	}

	ms_nTotalCivPeds = ms_nNumCivFemale + ms_nNumCivMale;
	ms_nTotalGangPeds = ms_nNumGang9 + ms_nNumGang8 + ms_nNumGang7
		+ ms_nNumGang6 + ms_nNumGang5 + ms_nNumGang4 + ms_nNumGang3
		+ ms_nNumGang2 + ms_nNumGang1;
	ms_nTotalPeds = ms_nNumDummy + ms_nNumEmergency + ms_nNumCop
		+ ms_nTotalGangPeds + ms_nNumCivFemale + ms_nNumCivMale;
	ms_nTotalPeds -= ms_nTotalCarPassengerPeds;

	// The fill waits out a cutscene (the camera there is not the game view) and
	// a frame the frame-time watchdog skipped, and it stops at the pool: a
	// frame whose free spaces are down to the reserve adds no pedestrian
	// (PopulationBudget.h, PED_POOL_RESERVE). One scan a frame.
	if (CCutsceneMgr::IsRunning() || !addPeds ||
		CPools::GetPedPool()->GetNoOfFreeSpaces() < PED_POOL_RESERVE)
		return;

	if (FillBurstIsRunning(&ms_FillBurst)) {
		SpendFillBurstFrame();
	} else {
		float pcdm = PedCreationDistMultiplier();
		const STrafficRings rings = CurrentTrafficRings();
		// The ring fills from the target and the lifetime, so the rate follows the ring scale.
		for (int i = 0; i < PED_ATTEMPTS_PER_FRAME * rings.scale; i++) {
			// The vehicle-speed multiplier widens the off-screen band's outer
			// edge; the pull-back stays the width of the creation ring.
			AddToPopulation(pcdm * rings.pedCreateMin,
				pcdm * rings.pedCreateMax,
				pcdm * rings.pedOffScreenMax - rings.pedOffScreenPullBack,
				pcdm * rings.pedOffScreenMax);
		}
	}
}

/* The streaming gate of the pedestrian burst (VICE-126): an attempt picks a
 * model of the zone's pedestrian group and stops at HasModelLoaded when it is
 * not resident (AddToPopulation), so a burst that runs before the streaming has
 * the group spends its window for nothing. True while at least one model of the
 * group is resident. */
static bool
PedModelsAreResident(void)
{
	CVector playerCentreOfWorld = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	CZoneInfo zoneInfo;
	CTheZones::GetZoneInfoForTimeOfDay(&playerCentreOfWorld, &zoneInfo);
	for (int i = 0; i < NUMMODELSPERPEDGROUP; i++) {
		int32 model = CPopulation::ms_pPedGroups[zoneInfo.pedGroup].models[i];
		if (model != -1 && CStreaming::HasModelLoaded(model))
			return true;
	}
	return false;
}

/* One frame of the pedestrian fill burst (VICE-126). The attempts come from the
 * burst table: the whole budget while the screen hides the fill, the small one
 * while the player can see it. The stop line and the pool are re-read inside
 * the loop: the reserve holds the burst of a frame (PopulationBudget.h,
 * PED_POOL_RESERVE) and the widest placement is one gang group, so a check
 * every FILL_BURST_POOL_CHECK_ATTEMPTS attempts keeps both the overshoot of the
 * target and the pool's overshoot below the reserve. */
static void
SpendFillBurstFrame(void)
{
	/* The stop line: the on-foot budget of the area plus the drivers, the line
	 * AddToPopulation stops at. A burst never runs past its target. */
	float target = AreaPedCountTarget();
	CPopulation::ms_fFillBurstTarget = target;
	if (FillBurstTargetReached((float)CPopulation::ms_nTotalPeds, target)) {
		FillBurstDisarm(&CPopulation::ms_FillBurst);
		return;
	}
	/* The streaming: the frame waits for the zone's models instead of spending
	 * the window on attempts that stop at the model check. */
	if (!PedModelsAreResident()) {
		FillBurstSpendFrame(&CPopulation::ms_FillBurst, false, false);
		return;
	}

	float pcdm = CPopulation::PedCreationDistMultiplier();
	const STrafficRings rings = CurrentTrafficRings();
	/* The bands of the burst: the creation ring and the off-screen band of the
	 * trickle. No burst gets a band of its own: a spawn closer than the
	 * off-screen band is the pop-in VICE-127 forbids. */
	int attempts = FillBurstAttempts(&CPopulation::ms_FillBurst, FillBurstIsHidden());

	/* What the frame created: the pool used count before and after. The pool is
	 * the one place every placement of the frame shows up, the gang groups and
	 * the couples included. The attempt counter follows the attempts the frame
	 * makes, so a frame the pool or the stop line ends early counts what it
	 * spent, not what it planned. */
	int32 usedBefore = CPools::GetPedPool()->GetNoOfUsedSpaces();
	for (int i = 0; i < attempts; i++) {
		if (i % FILL_BURST_POOL_CHECK_ATTEMPTS == 0) {
			if (CPools::GetPedPool()->GetNoOfFreeSpaces() < PED_POOL_RESERVE)
				break;
			if (FillBurstTargetReached((float)CPopulation::ms_nTotalPeds, target))
				break;
		}
		CPopulation::ms_nFillBurstAttempts++;
		CPopulation::AddToPopulation(pcdm * rings.pedCreateMin, pcdm * rings.pedCreateMax,
			pcdm * rings.pedOffScreenMax - rings.pedOffScreenPullBack, pcdm * rings.pedOffScreenMax);
	}
	int32 placed = CPools::GetPedPool()->GetNoOfUsedSpaces() - usedBefore;

	CPopulation::ms_nFillBurstPlaced += placed;
	FillBurstSpendFrame(&CPopulation::ms_FillBurst, true, placed > 0);
}

// More speed = wider area to spawn peds
float
CPopulation::PedCreationDistMultiplier()
{
	CVehicle *veh = FindPlayerVehicle();
	if (!veh)
		return 1.0f;

	float vehSpeed = veh->m_vecMoveSpeed.Magnitude2D();
	return Clamp(vehSpeed - 0.1f + 1.0f, 1.0f, 1.5f);
}

CPed*
CPopulation::AddPed(ePedType pedType, uint32 miOrCopType, CVector const &coors, int32 modifier)
{
	switch (pedType) {
		case PEDTYPE_CIVMALE:
		case PEDTYPE_CIVFEMALE:
		{
			CCivilianPed *ped = new CCivilianPed(pedType, miOrCopType);
			ped->SetPosition(coors);
			ped->SetOrientation(0.0f, 0.0f, 0.0f);
			CWorld::Add(ped);
			if (ms_bGivePedsWeapons) {
				eWeaponType weapon;

				switch (CGeneral::GetRandomNumber() & 3) {
					case 0:
						weapon = WEAPONTYPE_COLT45;
						break;
					case 1:
						weapon = WEAPONTYPE_NIGHTSTICK;
						break;
					case 2:
						weapon = WEAPONTYPE_GOLFCLUB;
						break;
					case 3:
						weapon = WEAPONTYPE_TEC9;
						break;
					default:
						break;
				}
				if (weapon != WEAPONTYPE_UNARMED) {
					ped->GiveDelayedWeapon(weapon, 25001);
					ped->SetCurrentWeapon(CWeaponInfo::GetWeaponInfo(weapon)->m_nWeaponSlot);
				}
			}
			return ped;
		}
		case PEDTYPE_COP:
		{
			CCopPed *ped = new CCopPed((eCopType)miOrCopType, modifier);
			ped->SetPosition(coors);
			ped->SetOrientation(0.0f, 0.0f, 0.0f);
			CWorld::Add(ped);
			return ped;
		}
		case PEDTYPE_GANG1:
		case PEDTYPE_GANG2:
		case PEDTYPE_GANG3:
		case PEDTYPE_GANG4:
		case PEDTYPE_GANG5:
		case PEDTYPE_GANG6:
		case PEDTYPE_GANG7:
		case PEDTYPE_GANG8:
		case PEDTYPE_GANG9:
		{
			CCivilianPed *ped = new CCivilianPed(pedType, miOrCopType);
			ped->SetPosition(coors);
			ped->SetOrientation(0.0f, 0.0f, 0.0f);
			CWorld::Add(ped);

			eWeaponType weapon;
			if (CGeneral::GetRandomNumberInRange(0, 100) >= 50)
				weapon = (eWeaponType)CGangs::GetGangInfo(pedType - PEDTYPE_GANG1)->m_Weapon2;
			else
				weapon = (eWeaponType)CGangs::GetGangInfo(pedType - PEDTYPE_GANG1)->m_Weapon1;

			ped->GiveDelayedWeapon(weapon, 25001);
			ped->SetCurrentWeapon(CWeaponInfo::GetWeaponInfo(weapon)->m_nWeaponSlot);
			return ped;
		}
		case PEDTYPE_EMERGENCY:
		{
			CEmergencyPed *ped = new CEmergencyPed(PEDTYPE_EMERGENCY);
		    ped->SetPosition(coors);
			ped->SetOrientation(0.0f, 0.0f, 0.0f);
			CWorld::Add(ped);
			return ped;
		}
		case PEDTYPE_FIREMAN:
		{
			CEmergencyPed *ped = new CEmergencyPed(PEDTYPE_FIREMAN);
		    ped->SetPosition(coors);
			ped->SetOrientation(0.0f, 0.0f, 0.0f);
			CWorld::Add(ped);
			return ped;
		}
		case PEDTYPE_CRIMINAL:
		case PEDTYPE_PROSTITUTE:
		{
			CCivilianPed *ped = new CCivilianPed(pedType, miOrCopType);
			ped->SetPosition(coors);
			ped->SetOrientation(0.0f, 0.0f, 0.0f);
			CWorld::Add(ped);
			return ped;
		}
		default:
			Error("Unknown ped type, AddPed, Population.cpp");
			return nil;
	}
}

/* A free position for a pedestrian spawn (VICE-122): a random point in the
 * creation ring, then the checks the node path does not need, because a node is
 * ground a pedestrian can stand on. The wander state walks the pedestrian to
 * the closest node from here, so the position must sit on the pedestrian level,
 * on land and clear of other pedestrians, and the network must be within the
 * walk bound and on the same level: a node far below the position means the
 * position is a roof.
 *
 * What the checks cannot tell: a surface the casts find is a surface, so a low
 * platform whose top is within the level margin of the ring and within the node
 * level of a node (a roof garden, a hillside ledge) still passes. The stray
 * cull takes the pedestrian that cannot leave such a spot, and the group's own
 * level checks keep its members within a step of the seed. */
static bool
FindFreePedCreationCoors(const CVector &centre, float minDist, float maxDist, float minDistOffScreen, float maxDistOffScreen, CVector *pPosition)
{
	bool found;
	for (int attempt = 0; attempt < PED_OFF_NETWORK_ATTEMPTS; attempt++) {
		// The bands of GeneratePedCreationCoors: a position in view takes the
		// creation ring, one out of view takes the off-screen band.
		float angle = CGeneral::GetRandomNumberInRange(0.0f, TWOPI);
		float dist = CGeneral::GetRandomNumberInRange(minDistOffScreen, maxDist);
		CVector pos(centre.x + dist * Sin(angle), centre.y + dist * Cos(angle), centre.z);
		bool visible = TheCamera.IsSphereVisible(pos, 2.0f);
		if (visible && !(minDist < dist && dist < maxDist))
			continue;
		if (!visible && !(minDistOffScreen < dist && dist < maxDistOffScreen))
			continue;

		// The ground. The cast starts on the level itself, so anything above
		// the level is missed and the position lands at or below it.
		float groundZ = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z, &found);
		if (!found)
			continue;
		if (Abs(groundZ - pos.z) > PED_OFF_NETWORK_LEVEL_MARGIN)
			continue;
		pos.z = groundZ;

		// The surface: the top surface over the position may stand no more than
		// a step above the ground. A roof, a canopy or a bridge deck stands
		// higher, and a pedestrian there cannot leave it.
		float surfaceZ = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + PED_OFF_NETWORK_ROOF_CAST, &found);
		if (!found || surfaceZ > pos.z + PED_OFF_NETWORK_STEP)
			continue;

		// The water: the ground must stand above the water line. The line is the
		// level without the waves, so the shoreline does not move with the wave
		// phase.
		if (!GroundIsClearOfWater(pos.x, pos.y, pos.z))
			continue;

		// The clearance over the feet: a sphere against the static world. The
		// pedestrian position check below reads vehicles and pedestrians only,
		// so this is what catches a spot inside a wall, a fence or a low roof.
		if (CWorld::TestSphereAgainstWorld(pos + CVector(0.0f, 0.0f, PED_OFF_NETWORK_PROBE_HEIGHT), PED_OFF_NETWORK_PROBE_RADIUS,
			nil, true, false, false, false, false, false))
			continue;

		// The spot must be free for a pedestrian.
		if (!CPedPlacement::IsPositionClearForPed(pos))
			continue;

		// The network: the walk to it is bounded, and it must be on the level of
		// the ground, or the ground is a roof above the network.
		int32 node = ThePaths.FindNodeClosestToCoors(pos, PATH_PED, PED_OFF_NETWORK_NODE_RANGE);
		if (node < 0)
			continue;
		if (Abs(ThePaths.m_pathNodes[node].GetPosition().z - pos.z) > PED_OFF_NETWORK_NODE_LEVEL)
			continue;

		*pPosition = pos;
		return true;
	}
	return false;
}

void
CPopulation::AddToPopulation(float minDist, float maxDist, float minDistOffScreen, float maxDistOffScreen)
{
	uint32 pedTypeToAdd;
	int32 modelToAdd;
	int pedAmount;

	CZoneInfo zoneInfo;
	int32 man = -1, woman = -1;
	CPed *gangLeader = nil;
	bool addCop = false;
	bool isSecurityGuard = false;
	bool forceAddingCop = false;
	CPlayerInfo *playerInfo = &CWorld::Players[CWorld::PlayerInFocus];
	CVector playerCentreOfWorld = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	/* The cop placement and the per-area budget read the ring table, the same
	 * one the caller built its distances from. */
	const STrafficRings rings = CurrentTrafficRings();
	CTheZones::GetZoneInfoForTimeOfDay(&playerCentreOfWorld, &zoneInfo);
	CWanted *wantedInfo = playerInfo->m_pPed->m_pWanted;

	if (wantedInfo->GetWantedLevel() > 2) {
		if (!CGame::IsInInterior() && (CGeneral::GetRandomNumber() % 32 == 0) && FindPlayerVehicle())
			forceAddingCop = true;

		uint32 maxCops = CGame::IsInInterior() ? wantedInfo->m_MaxCops * 1.6f : wantedInfo->m_MaxCops;
		if ((ms_nNumCop < maxCops || forceAddingCop) &&
			(!playerInfo->m_pPed->bInVehicle &&
				(CCarCtrl::NumLawEnforcerCars >= wantedInfo->m_MaximumLawEnforcerVehicles
				|| CCarCtrl::NumRandomCars >= CCarCtrl::GetRandomCarTarget()
				|| CCarCtrl::NumCarsInUse() >= CCarCtrl::MaxNumberOfCarsInUse) || forceAddingCop)) {
			addCop = true;
			minDist = PedCreationDistMultiplier() * rings.pedCreateMin;
			maxDist = PedCreationDistMultiplier() * rings.pedCreateMax;
		}
	}
	// Yeah, float
	// The on-foot budget of the area, and the hard top of the count on foot:
	// MaxNumberOfPedsInUse is an on-foot cap, sized by the pool arithmetic of
	// PopulationBudget.h without the drivers, because the drivers are added on top
	// of it below and the passengers stay out of ms_nTotalPeds entirely. The
	// expression lives in AreaPedBudget, which the fill burst reads too, so the
	// burst stops at the line this call stops at.
	float maxPossiblePedsForArea = AreaPedBudget();
	// Car and law-enforcer drivers count in ms_nTotalPeds: add them back so the
	// on-foot budget does not shrink with the car count.
	float maxPossibleTotalPeds = maxPossiblePedsForArea + CCarCtrl::NumRandomCars + CCarCtrl::NumLawEnforcerCars;

	if (ms_nTotalPeds < maxPossibleTotalPeds || addCop) {
		int decisionThreshold = CGeneral::GetRandomNumberInRange(0, 1000);
		if (decisionThreshold < zoneInfo.copPedThreshold || addCop) {
			pedTypeToAdd = PEDTYPE_COP;
			modelToAdd = ChoosePolicePedOccupation();
		} else {
			int i = 0;
			for (i = 0; i < NUM_GANGS; i++) {
				if (decisionThreshold < zoneInfo.gangPedThreshold[i]) {
					break;
				}
			}

			if (i == NUM_GANGS) {
				if (CGeneral::GetRandomNumberInRange(0.0f, 1.0f) <= 0.95f) {
					modelToAdd = ChooseCivilianOccupation(zoneInfo.pedGroup);

					if (modelToAdd == -1)
						return;
					pedTypeToAdd = ((CPedModelInfo*)CModelInfo::GetModelInfo(modelToAdd))->m_pedType;

				} else {
					ChooseCivilianCoupleOccupations(zoneInfo.pedGroup, man, woman);
					if (man == -1 || woman == -1)
						return;
					pedTypeToAdd = ((CPedModelInfo*)CModelInfo::GetModelInfo(woman))->m_pedType;
				}
			} else {
				pedTypeToAdd = PEDTYPE_GANG1 + i;

				if (IsSecurityGuard((ePedType)pedTypeToAdd)) {
					isSecurityGuard = true;
					modelToAdd = ChooseGangOccupation(pedTypeToAdd - PEDTYPE_GANG1);

					if (modelToAdd == -1)
						return;
					pedTypeToAdd = ((CPedModelInfo*)CModelInfo::GetModelInfo(modelToAdd))->m_pedType;

				}
			}
		}
		if (!addCop && m_AllRandomPedsThisType > PEDTYPE_PLAYER1)
			pedTypeToAdd = m_AllRandomPedsThisType;

		if (pedTypeToAdd >= PEDTYPE_GANG1 && pedTypeToAdd <= PEDTYPE_GANG9 && !isSecurityGuard) {
			minDist += 30.0f;
			maxDist += 30.0f;
			pedAmount = ComputeRandomisedGangSize();
		} else
			pedAmount = 1;

		CVector generatedCoors;
		/* Off the network (VICE-122): a bounded fraction of the spawns takes a
		 * free position in the creation ring instead of a node pair. On-network
		 * spawning is untouched: the fraction is small, a free position that
		 * fails a check falls back to the node path below, and a fraction of
		 * zero draws no random number at all, so the node path is bit-exact. */
		bool offNetwork = false;
		if (!CGame::IsInInterior() && PED_OFF_NETWORK_FRACTION > 0.0f &&
			CGeneral::GetRandomNumberInRange(0.0f, 1.0f) < PED_OFF_NETWORK_FRACTION)
			offNetwork = FindFreePedCreationCoors(playerCentreOfWorld, minDist, maxDist, minDistOffScreen, maxDistOffScreen, &generatedCoors);

		if (!offNetwork) {
			int node1, node2;
			float randomPos;
			bool foundCoors = !!ThePaths.GeneratePedCreationCoors(playerCentreOfWorld.x, playerCentreOfWorld.y, minDist, maxDist, minDistOffScreen, maxDistOffScreen,
				&generatedCoors, &node1, &node2, &randomPos, nil);

			if (!foundCoors)
				return;

			uint8 nodeSpawnRate = Min(ThePaths.m_pathNodes[node1].spawnRate, ThePaths.m_pathNodes[node2].spawnRate);
			int randomRate = CGeneral::GetRandomNumber() & 0xF;
			if (randomRate > nodeSpawnRate)
				return;

			CPathFind::TakeWidthIntoAccountForCoors(&ThePaths.m_pathNodes[node1], &ThePaths.m_pathNodes[node2], CGeneral::GetRandomNumber(), &generatedCoors.x, &generatedCoors.y);
		}
		if (CGame::currArea == AREA_MALL && (pedTypeToAdd == PEDTYPE_CIVMALE || pedTypeToAdd == PEDTYPE_CIVFEMALE || pedTypeToAdd == PEDTYPE_CRIMINAL) &&
			CGeneral::GetRandomNumberInRange(0.f, 1.f) > 0.5f) {

			PlaceMallPedsAsStationaryGroup(generatedCoors, zoneInfo.pedGroup);
			return;
		}

		if (pedTypeToAdd >= PEDTYPE_GANG1 && pedTypeToAdd <= PEDTYPE_GANG9 && !isSecurityGuard) {
			PlaceGangMembers((ePedType)pedTypeToAdd, pedAmount, generatedCoors, offNetwork);
			return;
		}

		if (man > -1 && woman > -1) {
			PlaceCouple(PEDTYPE_CIVMALE, man, PEDTYPE_CIVFEMALE, woman, generatedCoors, offNetwork);
			return;
		}

		for (int i = 0; i < pedAmount; ++i) {

			if (pedTypeToAdd == PEDTYPE_COP) {
				// Unused code, ChoosePolicePedOccupation returns COP_STREET. Spawning FBI/SWAT/Army done in somewhere else.
				if (modelToAdd == COP_STREET) {
					if (!CStreaming::HasModelLoaded(MI_COP))
						return;

				} else if (modelToAdd == COP_FBI) {
					if (!CStreaming::HasModelLoaded(MI_COP) || !CStreaming::HasModelLoaded(CWeaponInfo::GetWeaponInfo(WEAPONTYPE_MP5)->m_nModelId))
						return;

				} else if (modelToAdd == COP_SWAT) {
					if (!CStreaming::HasModelLoaded(MI_SWAT) || !CStreaming::HasModelLoaded(CWeaponInfo::GetWeaponInfo(WEAPONTYPE_UZI)->m_nModelId))
						return;

				} else if (modelToAdd == COP_ARMY) {
					if (!CStreaming::HasModelLoaded(MI_ARMY) ||
						!CStreaming::HasModelLoaded(CWeaponInfo::GetWeaponInfo(WEAPONTYPE_MP5)->m_nModelId) || !CStreaming::HasModelLoaded(CWeaponInfo::GetWeaponInfo(WEAPONTYPE_GRENADE)->m_nModelId))
						return;
				}
			} else if (!CStreaming::HasModelLoaded(modelToAdd)) {
				return;
			}
			generatedCoors.z += 0.7f;

			// What? How can this not be met?
			if (i < pedAmount) {
				// rand()
				// III leftover, unused
				if (gangLeader) {
					// Align gang members in formation. (btw i can't be 0 in here)
					float offsetMin = i * 0.75f;
					float offsetMax = (i + 1.0f) * 0.75f - offsetMin;
					float xOffset = CGeneral::GetRandomNumberInRange(offsetMin, offsetMin + offsetMax);
					float yOffset = CGeneral::GetRandomNumberInRange(offsetMin, offsetMin + offsetMax);
					if (CGeneral::GetRandomNumber() & 1)
						xOffset = -xOffset;
					if (CGeneral::GetRandomNumber() & 1)
						yOffset = -yOffset;
					generatedCoors.x = xOffset + gangLeader->GetPosition().x;
					generatedCoors.y = yOffset + gangLeader->GetPosition().y;
				}
			}
			if (!CPedPlacement::IsPositionClearForPed(generatedCoors))
				break;

			// Why no love for last gang member?!
			if (i + 1 < pedAmount) {
				bool foundGround;
				float groundZ = CWorld::FindGroundZFor3DCoord(generatedCoors.x, generatedCoors.y, 2.0f + generatedCoors.z, &foundGround) + 0.7f;
				if (!foundGround)
					return;

				generatedCoors.z = Max(generatedCoors.z, groundZ);
			}
			bool distIsOk = true;
			if (TheCamera.IsSphereVisible(generatedCoors, 2.0f)) {
				if (PedCreationDistMultiplier() * rings.pedCreateMin > (generatedCoors - playerCentreOfWorld).Magnitude2D())
					distIsOk = false;
			}

			// Place skaters if only they're on tarmac.
			bool surfaceIsOk = true;
			if (((CPedModelInfo*)CModelInfo::GetModelInfo(modelToAdd))->m_pedStatType == PEDSTAT_SKATER) {
				CEntity* foundEnt = nil;
				CColPoint foundCol;
				CWorld::ProcessVerticalLine(generatedCoors + CVector(0.f, 0.f, 2.f), generatedCoors.z - 2.0f, foundCol, foundEnt, true, false, false, false, false, false, nil);
				if (foundEnt) {
					if (foundCol.surfaceB == SURFACE_TARMAC || foundCol.surfaceB == SURFACE_PAVEMENT)
						surfaceIsOk = true;
					else
						surfaceIsOk = false;

				} else {
					surfaceIsOk = false;
				}
			}
			if (!distIsOk || !surfaceIsOk)
				break;
			CPed *newPed = AddPed((ePedType)pedTypeToAdd, modelToAdd, generatedCoors);
			AddOffNetworkPed(newPed, offNetwork);
			if (forceAddingCop && newPed->m_nPedType == PEDTYPE_COP)
				((CCopPed*)newPed)->m_bThrowsSpikeTrap = true;

			bool gonnaSunbathe = false;
			if (CPopulation::IsSunbather(modelToAdd)) {
				CEntity* foundEnt = nil;
				CColPoint foundCol;
				CWorld::ProcessVerticalLine(generatedCoors + CVector(0.f, 0.f, 2.f), generatedCoors.z - 2.0f, foundCol, foundEnt, true, false, false, false, false, false, nil);
				if (foundEnt) {
					if ((foundCol.surfaceB == SURFACE_CONCRETE_BEACH || foundCol.surfaceB == SURFACE_SAND)
						&& CClock::GetHours() >= 10 && CClock::GetHours() <= 18 && 0.0f == CWeather::Rain) {
						gonnaSunbathe = true;
						if (CPedPlacement::IsPositionClearForPed(generatedCoors, 3.0f, ARRAY_SIZE(gSunbatheObstacles), gSunbatheObstacles)) {
							for (int j = 0; j < ARRAY_SIZE(gSunbatheObstacles); j++) {
								if (gSunbatheObstacles[j] && gSunbatheObstacles[j] != newPed)
									gonnaSunbathe = false;
							}
						}
					}
				}
			}
			if (gonnaSunbathe) {
				float heading = CGeneral::GetRandomNumberInRange(0.f, 1.f) * TWOPI;
				newPed->m_fRotationDest = heading;
				newPed->m_fRotationCur = heading;
				// unused
				// v61 = CGeneral::GetRandomTrueFalse();
				newPed->SetWaitState(WAITSTATE_SUN_BATHE_IDLE, nil);
				CVector toyPos(newPed->GetPosition());
				float waterLevel;
				if (CWaterLevel::GetGroundLevel(toyPos, &waterLevel, nil, 30.0f)) {
					toyPos.z = 0.04f + waterLevel;
					CEntity *toy = CWaterLevel::CreateBeachToy(toyPos, BEACHTOY_ANY_TOWEL);
					if (toy)
						toy->SetHeading(heading);

					if (!(CGeneral::GetRandomNumber() & 3)) {
						CWaterLevel::CreateBeachToy(toyPos + CVector(CGeneral::GetRandomNumberInRange(-2.f, 2.f), CGeneral::GetRandomNumberInRange(-2.f, 2.f), 0.f), BEACHTOY_LOTION);
					}
				}
			} else {
				newPed->SetWanderPath(CGeneral::GetRandomNumberInRange(0, 8));
			}
			
			if (i != 0) {
				// Gang member
				newPed->SetLeader(gangLeader);

				newPed->SetPedState(PED_UNKNOWN);
				gangLeader->SetPedState(PED_UNKNOWN);

			} else {
				gangLeader = newPed;
			}
			CVisibilityPlugins::SetClumpAlpha(newPed->GetClump(), 0);
			/*
			// Pointless, this is already a for loop
			if (i + 1 > pedAmount)
				break;
			if (pedAmount <= 1)
				break; */
		}
	}
}

CPed*
CPopulation::AddPedInCar(CVehicle* car, bool isDriver)
{
	const int defaultModel = MI_MALE01;
	int miamiViceIndex = 0;
	bool imSureThatModelIsLoaded = true;
	CVector coors = FindPlayerCoors();
	CZoneInfo zoneInfo;
	int pedType;

	// May be eCopType, model index or non-sense(for medic), AddPed knows that by looking to ped type.
	int preferredModel;

	CTheZones::GetZoneInfoForTimeOfDay(&coors, &zoneInfo);
	switch (car->GetModelIndex()) {
		case MI_FIRETRUCK:
			preferredModel = 0;
			pedType = PEDTYPE_FIREMAN;
			break;
		case MI_AMBULAN:
			preferredModel = 0;
			pedType = PEDTYPE_EMERGENCY;
			break;
		case MI_POLICE:
		case MI_PREDATOR:
			preferredModel = COP_STREET;
			pedType = PEDTYPE_COP;
			break;
		case MI_ENFORCER:
			preferredModel = COP_SWAT;
			pedType = PEDTYPE_COP;
			break;
		case MI_RHINO:
		case MI_BARRACKS:
			preferredModel = COP_ARMY;
			pedType = PEDTYPE_COP;
			break;
		case MI_FBIRANCH:
			preferredModel = COP_FBI;
			pedType = PEDTYPE_COP;
			break;
		default:
			if (car->IsTaxi()) {
				if (isDriver) {
					pedType = PEDTYPE_CIVMALE;
					preferredModel = MI_TAXI_D;
					break;
				}
				// fall through if not
			} else if (car->GetModelIndex() == MI_VICECHEE) {
				if (car->bIsLawEnforcer) {
					preferredModel = COP_MIAMIVICE;
					pedType = PEDTYPE_COP;
					miamiViceIndex = (isDriver ? 2 * CCarCtrl::MiamiViceCycle : 2 * CCarCtrl::MiamiViceCycle + 1);
					break;
				}
				// fall through if not
			}

			int gangOfPed = 0;
			imSureThatModelIsLoaded = false;

			while (gangOfPed < NUM_GANGS && CGangs::GetGangInfo(gangOfPed)->m_nVehicleMI != car->GetModelIndex())
				gangOfPed++;

			if (gangOfPed < NUM_GANGS) {
				pedType = gangOfPed + PEDTYPE_GANG1;
				preferredModel = ChooseGangOccupation(gangOfPed);
			} else if (gangOfPed == NUM_GANGS) {
			    CVehicleModelInfo *carModel = ((CVehicleModelInfo *)CModelInfo::GetModelInfo(car->GetModelIndex()));
				preferredModel = ChooseCivilianOccupation(zoneInfo.pedGroup);
				int i = 15;
				for(; i >= 0; i--) {
					CPedModelInfo* pedModel = (CPedModelInfo*)CModelInfo::GetModelInfo(preferredModel);

					if (pedModel->GetRwObject()) {
						if (!car->IsPassenger(preferredModel) && !car->IsDriver(preferredModel)) {
							if (((CPedModelInfo*)CModelInfo::GetModelInfo(preferredModel))->m_carsCanDrive & (1 << carModel->m_vehicleClass))
								break;
						}
					}

					preferredModel = ChooseNextCivilianOccupation(zoneInfo.pedGroup);
				}
				if (i == -1)
					preferredModel = defaultModel;

				pedType = ((CPedModelInfo*)CModelInfo::GetModelInfo(preferredModel))->m_pedType;
			}
			break;
	}
	if (!imSureThatModelIsLoaded && !((CPedModelInfo*)CModelInfo::GetModelInfo(preferredModel))->GetRwObject()) {
		preferredModel = defaultModel;
		pedType = ((CPedModelInfo*)CModelInfo::GetModelInfo(defaultModel))->m_pedType;
	}

	CPed *newPed = CPopulation::AddPed((ePedType)pedType, preferredModel, car->GetPosition(), miamiViceIndex);
	newPed->bUsesCollision = false;

	if (newPed->GetWeapon()->m_eWeaponType != WEAPONTYPE_UNARMED) {
		newPed->RemoveWeaponModel(CWeaponInfo::GetWeaponInfo(newPed->GetWeapon()->m_eWeaponType)->m_nModelId);
	}

	newPed->AddInCarAnims(car, isDriver);
	return newPed;
}

void
CPopulation::MoveCarsAndPedsOutOfAbandonedZones()
{
}

void
CPopulation::ConvertAllObjectsToDummyObjects()
{
	uint32 i = CPools::GetObjectPool()->GetSize();
	while(i--) {
		CObject *obj = CPools::GetObjectPool()->GetSlot(i);
		if (obj) {
			if (obj->CanBeDeleted())
				ConvertToDummyObject(obj);
		}
	}
}

void
CPopulation::ConvertToRealObject(CDummyObject *dummy)
{
	if (!TestSafeForRealObject(dummy))
		return;

	CObject *obj = new CObject(dummy);
	if (!obj)
		return;

	CWorld::Remove(dummy);
	delete dummy;
	CWorld::Add(obj);

	CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(obj->GetModelIndex());
	if (IsGlass(obj->GetModelIndex()) && !mi->m_isArtistGlass) {
		obj->bIsVisible = false;
	} else if (obj->GetModelIndex() == MI_BUOY) {
		obj->SetIsStatic(false);
		obj->m_vecMoveSpeed = CVector(0.0f, 0.0f, -0.001f);
		obj->bTouchingWater = true;
		obj->AddToMovingList();
	}
}

void
CPopulation::ConvertToDummyObject(CObject *obj)
{
	CDummyObject *dummy = new CDummyObject(obj);
	// The dummy pool holds the whole map's dummies plus every object that is
	// far enough to be one, so it can be full. The object then stays real and
	// the scan tries the conversion again later (VICE-134).
	if (!dummy)
		return;

	dummy->GetMatrix() = obj->m_objectMatrix;
	dummy->GetMatrix().UpdateRW();
	dummy->UpdateRwFrame();

	CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(obj->GetModelIndex());
	if (IsGlass(obj->GetModelIndex()) && !mi->m_isArtistGlass)
		dummy->bIsVisible = false;

	CWorld::Remove(obj);
	delete obj;
	CWorld::Add(dummy);
}

bool
CPopulation::TestRoomForDummyObject(CObject *obj)
{
	int16 collidingObjs;
	CWorld::FindObjectsKindaColliding(obj->m_objectMatrix.GetPosition(), obj->GetBoundRadius(),
		false, &collidingObjs, 2, nil, false, true, true, false, false);

	return collidingObjs == 0;
}

bool
CPopulation::TestSafeForRealObject(CDummyObject *dummy)
{
	CPtrNode *ptrNode;
	CColModel *dummyCol = dummy->GetColModel();

	float radius = dummyCol->boundingSphere.radius;
	int minX = CWorld::GetSectorIndexX(dummy->GetPosition().x - radius);
	if (minX < 0) minX = 0;
	int minY = CWorld::GetSectorIndexY(dummy->GetPosition().y - radius);
	if (minY < 0) minY = 0;
	int maxX = CWorld::GetSectorIndexX(dummy->GetPosition().x + radius);
#ifdef FIX_BUGS
	if (maxX >= NUMSECTORS_X) maxX = NUMSECTORS_X - 1;
#else
	if (maxX >= NUMSECTORS_X) maxX = NUMSECTORS_X;
#endif

	int maxY = CWorld::GetSectorIndexY(dummy->GetPosition().y + radius);
#ifdef FIX_BUGS
	if (maxY >= NUMSECTORS_Y) maxY = NUMSECTORS_Y - 1;
#else
	if (maxY >= NUMSECTORS_Y) maxY = NUMSECTORS_Y;
#endif

	float colRadius = dummy->GetBoundRadius();
	CVUVECTOR colCentre;
	dummy->GetBoundCentre(colCentre);

	static CColPoint aTempColPoints[MAX_COLLISION_POINTS];

	for (int curY = minY; curY <= maxY; curY++) {
		for (int curX = minX; curX <= maxX; curX++) {
			CSector *sector = CWorld::GetSector(curX, curY);

			for (ptrNode = sector->m_lists[ENTITYLIST_VEHICLES].first; ptrNode; ptrNode = ptrNode->next) {
				CVehicle *veh = (CVehicle*)ptrNode->item;
				if (veh->m_scanCode != CWorld::GetCurrentScanCode()) {
					if (veh->GetIsTouching(colCentre, colRadius)) {
						veh->m_scanCode = CWorld::GetCurrentScanCode();
						if (CCollision::ProcessColModels(dummy->GetMatrix(), *dummyCol, veh->GetMatrix(), *veh->GetColModel(), aTempColPoints, nil, nil) > 0)
							return false;
					}
				}
			}

			for (ptrNode = sector->m_lists[ENTITYLIST_VEHICLES_OVERLAP].first; ptrNode; ptrNode = ptrNode->next) {
				CVehicle *veh = (CVehicle*)ptrNode->item;
				if (veh->m_scanCode != CWorld::GetCurrentScanCode()) {
					if (veh->GetIsTouching(colCentre, colRadius)) {
						veh->m_scanCode = CWorld::GetCurrentScanCode();
						if (CCollision::ProcessColModels(dummy->GetMatrix(), *dummyCol, veh->GetMatrix(), *veh->GetColModel(), aTempColPoints, nil, nil) > 0)
							return false;
					}
				}
			}
		}
	}
	return true;
}

void
CPopulation::ManagePopulation(void)
{
	int frameMod32 = CTimer::GetFrameCounter() & 31;
	CVector playerPos = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);

	// Why this code is here?! Delete temporary objects when they got too far, and convert others to "dummy" objects. (like lamp posts)
	int objectPoolSize = CPools::GetObjectPool()->GetSize();
	for (int i = objectPoolSize * frameMod32 / 32; i < objectPoolSize * (frameMod32 + 1) / 32; i++) {
		CObject *obj = CPools::GetObjectPool()->GetSlot(i);
		if (obj && obj->CanBeDeleted()) {
			float objPlayerDist = (obj->GetPosition() - playerPos).Magnitude();
			if (obj->ObjectCreatedBy == TEMP_OBJECT) {
				if (obj->GetModelIndex() != MI_ROADWORKBARRIER1 && obj->GetModelIndex() != MI_BEACHBALL) {
					if (objPlayerDist > 51.0f || objPlayerDist > 25.0f && !obj->GetIsOnScreen() || CTimer::GetTimeInMilliseconds() > obj->m_nEndOfLifeTime) {
						CWorld::Remove(obj);
						delete obj;
					}
				} else if (objPlayerDist > 120.0f) {
					CWorld::Remove(obj);
					delete obj;
				}

			} else if (objPlayerDist > 80.0f && (obj->m_objectMatrix.GetPosition() - playerPos).Magnitude() > 80.0f) {
				if (obj->ObjectCreatedBy != CUTSCENE_OBJECT && TestRoomForDummyObject(obj)) {
					ConvertToDummyObject(obj);
				}
			}
		}
	}

	// Convert them back to real objects. Dummy objects don't have collisions, so they need to be converted.
	int dummyPoolSize = CPools::GetDummyPool()->GetSize();
	for (int i = dummyPoolSize * frameMod32 / 32; i < dummyPoolSize * (frameMod32 + 1) / 32; i++) {
		CDummy *dummy = CPools::GetDummyPool()->GetSlot(i);
		if (dummy && (dummy->m_area == CGame::currArea || dummy->m_area == AREA_EVERYWHERE)) {
			if ((dummy->GetPosition() - playerPos).Magnitude() < 80.0f)
				ConvertToRealObject((CDummyObject*)dummy);
		}
	}

	int pedPoolSize = CPools::GetPedPool()->GetSize();
	// The multiplier only follows the player's vehicle speed and nothing in the
	// loop below writes the player's vehicle, so one read covers the whole scan.
	float pedCreationDistMultiplier = PedCreationDistMultiplier();
	// The removal ring comes from the one table, like the creation ring above.
	const STrafficRings rings = CurrentTrafficRings();
#ifndef SQUEEZE_PERFORMANCE
	for (int poolIndex = pedPoolSize-1; poolIndex >= 0; poolIndex--) {
#else
	for (int poolIndex = (pedPoolSize * (frameMod32 + 1) / 32) - 1; poolIndex >= pedPoolSize * frameMod32 / 32; poolIndex--) {
#endif
		CPed *ped = CPools::GetPedPool()->GetSlot(poolIndex);

		if (ped && !ped->IsPlayer() && ped->CanBeDeleted() && !ped->bInVehicle) {
			uint32 timeSinceDeath = CTimer::GetTimeInMilliseconds() - ped->m_bloodyFootprintCountOrDeathTime;
			if (ped->m_nPedState == PED_DEAD && (timeSinceDeath > 30000 || CDarkel::FrenzyOnGoing() && timeSinceDeath > 15000))
				ped->bFadeOut = true;

			if (ped->bFadeOut && CVisibilityPlugins::GetClumpAlpha(ped->GetClump()) == 0) {
				RemovePed(ped);
				continue;
			}

			float dist = (ped->GetPosition() - playerPos).Magnitude2D();

			bool pedIsFarAway = false;

			if (ped->IsGangMember())
				dist -= 30.0f;
			else if (ped->bDeadPedInFrontOfCar && ped->m_vehicleInAccident)
				dist = 0.0f;

			if (pedCreationDistMultiplier * rings.pedRemoveSpecial < dist ||
				(!ped->bCullExtraFarAway && pedCreationDistMultiplier * rings.pedRemove < dist)) {
				pedIsFarAway = true;
			}
#ifndef EXTENDED_OFFSCREEN_DESPAWN_RANGE
			else if (pedCreationDistMultiplier * rings.pedOffScreenMax < dist) {
				if (CTimer::GetTimeInMilliseconds() > ped->m_nExtendedRangeTimer && !ped->GetIsOnScreen()) {
					if (TheCamera.Cams[TheCamera.ActiveCam].Mode != CCam::MODE_SNIPER
						&& TheCamera.Cams[TheCamera.ActiveCam].Mode != CCam::MODE_SNIPER_RUNABOUT
						&& TheCamera.Cams[TheCamera.ActiveCam].Mode != CCam::MODE_CAMERA
						&& !TheCamera.Cams[TheCamera.ActiveCam].LookingLeft
						&& !TheCamera.Cams[TheCamera.ActiveCam].LookingRight
						&& !TheCamera.Cams[TheCamera.ActiveCam].LookingBehind) {
						pedIsFarAway = true;
					}
				}
						
			}
#endif
			else {
				ped->m_nExtendedRangeTimer = ped->m_nPedType == PEDTYPE_COP ? CTimer::GetTimeInMilliseconds() + 10000 : CTimer::GetTimeInMilliseconds() + 4000;
			}

			if (!pedIsFarAway)
				continue;

			if (ped->m_nPedState == PED_DEAD && !ped->bFadeOut) {
				CVector pedPos = ped->GetPosition();

				float randAngle = (uint8) CGeneral::GetRandomNumber() * (3.14f / 128.0f); // Not PI, 3.14
				switch (CGeneral::GetRandomNumber() % 3) {
					case 0:
						CShadows::AddPermanentShadow(SHADOWTYPE_DARK, gpOutline1Tex, &pedPos,
							0.9f * Cos(randAngle), 0.9f * Sin(randAngle), 0.9f * Sin(randAngle), -0.9f * Cos(randAngle),
							255, 255, 255, 255, 4.0f, 40000, 1.0f);
						break;
					case 1:
						CShadows::AddPermanentShadow(SHADOWTYPE_DARK, gpOutline2Tex, &pedPos,
							0.9f * Cos(randAngle), 0.9f * Sin(randAngle), 0.9f * Sin(randAngle), -0.9f * Cos(randAngle),
							255, 255, 255, 255, 4.0f, 40000, 1.0f);
						break;
					case 2:
						CShadows::AddPermanentShadow(SHADOWTYPE_DARK, gpOutline3Tex, &pedPos,
							0.9f * Cos(randAngle), 0.9f * Sin(randAngle), 0.9f * Sin(randAngle), -0.9f * Cos(randAngle),
							255, 255, 255, 255, 4.0f, 40000, 1.0f);
						break;
					default:
						break;
				}
			}
			if (ped->GetIsOnScreen())
				ped->bFadeOut = true;
			else
				RemovePed(ped);
		}
	}
}

CPed* 
CPopulation::AddDeadPedInFrontOfCar(const CVector& pos, CVehicle* pCulprit)
{
	if (TheCamera.IsSphereVisible(pos, 2.0f) && CurrentTrafficRings().pedCreateMin * PedCreationDistMultiplier() > (pos - FindPlayerPed()->GetPosition()).Magnitude2D()) {
			return nil;
	}

	bool found;
	float z = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z, &found) + 1.0f;
	if (!found)
		return nil;
	z = Max(z, pos.z);
	if (!CModelInfo::GetModelInfo(MI_MALE01)->GetRwObject())
		return nil;
	CPed* pPed = CPopulation::AddPed(PEDTYPE_CIVMALE, MI_MALE01, pos);
	pPed->SetDie();
	pPed->m_nPedMoney = 0;
	pPed->bDeadPedInFrontOfCar = true;
	pPed->m_vehicleInAccident = pCulprit;
	pCulprit->RegisterReference((CEntity**)&pPed->m_vehicleInAccident);
	CEntity* pEntities[3] = { 0 };
	if (!CPedPlacement::IsPositionClearForPed(pos, 2.0f, 3, pEntities)) {
		for (int i = 0; i < 3; i++) {
			if (pEntities[i] && pEntities[i] != pCulprit && pEntities[i] != pPed) {
				RemovePed(pPed);
				return nil;
			}
		}
	}
	CColPoint colpts[MAX_COLLISION_POINTS];
	if (CCollision::ProcessColModels(pCulprit->GetMatrix(), *pCulprit->GetColModel(), pPed->GetMatrix(), *pPed->GetColModel(), colpts, nil, nil)) {
		RemovePed(pPed);
		return nil;
	}
	CVisibilityPlugins::SetClumpAlpha(pPed->GetClump(), 0);
	return pPed;
}

bool
CPopulation::IsSkateable(CVector const& pos)
{
	CColPoint foundCol;
	CEntity* foundEnt = nil;
	CWorld::ProcessVerticalLine(pos + CVector(0.f, 0.f, 2.f), pos.z - 2.0f, foundCol, foundEnt, true, false, false, false, false, false, nil);
	if (!foundEnt)
		return false;

	return foundCol.surfaceB == SURFACE_TARMAC || foundCol.surfaceB == SURFACE_PAVEMENT;
}

bool
CPopulation::CanJeerAtStripper(int32 model)
{
	return model == MI_WMOBE || model == MI_WMYBE || model == MI_WMOST || model == MI_BMYBB;
}

void
CPopulation::RemovePedsIfThePoolGetsFull(void)
{
	if ((CTimer::GetFrameCounter() & 7) == 5) {
		if (CPools::GetPedPool()->GetNoOfFreeSpaces() < 8) {
			CPed *closestPed = nil;
			float closestDist = 10000000.0;
			int poolSize = CPools::GetPedPool()->GetSize();
			for (int i = poolSize - 1; i >= 0; i--) {
				CPed* ped = CPools::GetPedPool()->GetSlot(i);
				if (ped && ped->CanBeDeleted()) {
					float dist = (TheCamera.GetPosition() - ped->GetPosition()).Magnitude();
					if (dist < closestDist) {
						closestDist = dist;
						closestPed = ped;
					}
				}
			}
			if (closestPed) {
				RemovePed(closestPed);
			}
		}
	}
}

bool
CPopulation::IsMale(int32 model)
{
	switch (model) {
		case MI_HMYST:
		case MI_HMOST:
		case MI_HMYRI:
		case MI_HMORI:
		case MI_HMYBE:
		case MI_HMOBE:
		case MI_HMOTR:
		case MI_HMYAP:
		case MI_HMOCA:
		case MI_BMODK:
		case MI_BMYKR:
		case MI_BMYST:
		case MI_BMOST:
		case MI_BMYRI:
		case MI_BMYBE:
		case MI_BMOBE:
		case MI_BMYBU:
		case MI_BMOTR:
		case MI_BMYPI:
		case MI_BMYBB:
		case MI_WMYCR:
		case MI_WMYST:
		case MI_WMOST:
		case MI_WMYRI:
		case MI_WMORI:
		case MI_WMYBE:
		case MI_WMOBE:
		case MI_WMYCW:
		case MI_WMYGO:
		case MI_WMOGO:
		case MI_WMYLG:
		case MI_WMYBU:
		case MI_WMOBU:
		case MI_WMOTR:
		case MI_WMYPI:
		case MI_WMOCA:
		case MI_WMYJG:
		case MI_WMYSK:

		// BUG? Why no JMOTO?
			return true;
		default:
			return false;
	}
}

bool
CPopulation::IsFemale(int32 model)
{
	switch (model) {
		case MI_HFYST:
		case MI_HFOST:
		case MI_HFYRI:
		case MI_HFORI:
		case MI_HFYBE:
		case MI_HFOBE:
		case MI_HFYBU:
		case MI_HFYMD:
		case MI_HFYCG:
		case MI_HFYPR:
		case MI_HFOTR:
		case MI_BFYST:
		case MI_BFOST:
		case MI_BFYRI:
		case MI_BFORI:
		case MI_BFYBE:
		case MI_BFOBE:
		case MI_BFYPR:
		case MI_BFOTR:
		case MI_WFYST:
		case MI_WFOST:
		case MI_WFYRI:
		case MI_WFORI:
		case MI_WFYBE:
		case MI_WFOBE:
		case MI_WFOGO:
		case MI_WFYLG:
		case MI_WFYBU:
		case MI_WFYPR:
		case MI_WFOTR:
		case MI_WFYJG:
		case MI_WFYSK:
		case MI_WFYSH:
		case MI_WFOSH:
		case MI_JFOTO:
			return true;
		default:
			return false;
	}
}

bool
CPopulation::IsSunbather(int32 model)
{
	switch (model) {
		case MI_HFYBE:
		case MI_HFOBE:
		case MI_HMYBE:
		case MI_HMOBE:
		case MI_BFYBE:
		case MI_BMYBE:
		case MI_BFOBE:
		case MI_BMOBE:
		case MI_WFYBE:
		case MI_WMYBE:
		case MI_WFOBE:
		case MI_WMOBE:
			return true;
		default:
			return false;
	}
}

int32
CPopulation::ComputeRandomisedGangSize(void)
{
	return CGeneral::GetRandomNumberInRange(3, 6);
}

bool
CPopulation::CanSolicitPlayerInCar(int32 model)
{
	return model == MI_HFYPR || model == MI_BFYPR || model == MI_WFYPR;
}

bool
CPopulation::CanSolicitPlayerOnFoot(int32 model)
{
	return model == MI_HFYMD || model == MI_HFYCG || model == MI_BFOTR || model == MI_BMOTR || model == MI_WFOTR || model == MI_WMOTR;
}

bool
CPopulation::IsSecurityGuard(ePedType pedType)
{
	return pedType == PEDTYPE_GANG5;
}

void
CPopulation::ChooseCivilianCoupleOccupations(int32 group, int32& man, int32& woman)
{
	man = -1;
	woman = -1;
	
	for (int i = 0; i < 8; i++) {
		if (man > -1)
			break;

		int32 model = ms_pPedGroups[group].models[CGeneral::GetRandomNumberInRange(0, NUMMODELSPERPEDGROUP)];
		if (man == -1 && IsMale(model) && ((CPedModelInfo*)CModelInfo::GetModelInfo(model))->m_pedType == PEDTYPE_CIVMALE) {
			man = model;
		}
	}

	if (man != -1) {
		int32 model;
		for (int i = 0; i < NUMMODELSPERPEDGROUP; i++) {
			model = ms_pPedGroups[group].models[i];
			if (IsFemale(model)) {
				CPedModelInfo* womanModelInfo = (CPedModelInfo*)CModelInfo::GetModelInfo(model);
				if (womanModelInfo->m_pedType == PEDTYPE_CIVFEMALE) {
					CPedModelInfo* manModelInfo = (CPedModelInfo*)CModelInfo::GetModelInfo(man);

					// If both are skater or not, finalize the decision
					if (manModelInfo && womanModelInfo) {
						if (manModelInfo->m_animGroup == womanModelInfo->m_animGroup) {
							if (manModelInfo->m_pedStatType != PEDSTAT_SKATER && womanModelInfo->m_pedStatType != PEDSTAT_SKATER)
								break;

							if (manModelInfo->m_pedStatType == PEDSTAT_SKATER && womanModelInfo->m_pedStatType == PEDSTAT_SKATER)
								break;
						}
					}
				}
			}
		}
		woman = model;
	}
}

void
CPopulation::PlaceGangMembers(ePedType pedType, int pedAmount, CVector const& coors, bool offNetwork)
{
	if (CGeneral::GetRandomNumberInRange(0.f, 1.f) < 0.333f) {
		PlaceGangMembersInFormation(pedType, pedAmount, coors, offNetwork);
	} else {
		PlaceGangMembersInCircle(pedType, pedAmount, coors, offNetwork);
	}
}

void
CPopulation::PlaceGangMembersInFormation(ePedType pedType, int pedAmount, CVector const& coors, bool offNetwork)
{
	CPed *createdPeds[5];

	if (!TheCamera.IsSphereVisible(coors, 3.0f) || CurrentTrafficRings().pedCreateMin * PedCreationDistMultiplier() <= (coors - FindPlayerPed()->GetPosition()).Magnitude2D()) {
		if (CPedPlacement::IsPositionClearForPed(coors, 3.0f, -1, nil)) {
			bool leaderFoundGround;
			float leaderGroundZ = CWorld::FindGroundZFor3DCoord(coors.x, coors.y, coors.z, &leaderFoundGround) + 1.0f;
			if (leaderFoundGround) {
				float finalZ = coors.z > leaderGroundZ ? coors.z : leaderGroundZ;
				int leaderModel = ChooseGangOccupation(pedType - PEDTYPE_GANG1);
				if (((CPedModelInfo*)CModelInfo::GetModelInfo(leaderModel))->GetRwObject()) {
					CPed *leader = AddPed(pedType, leaderModel, CVector(coors.x, coors.y, finalZ));
					if (leader) {
						AddOffNetworkPed(leader, offNetwork);
						leader->SetObjective(OBJECTIVE_NONE);
						leader->SetWanderPath(CGeneral::GetRandomNumberInRange(0, 8));
						leader->bIsLeader = true;
						if (CGangs::GetWillAttackPlayerWithCops(pedType))
							leader->bCanAttackPlayerWithCops = true;

						int pedIdx = 1;
						createdPeds[0] = leader;
						for (int i = 1; i < pedAmount; ++i) {
							int memberModel = ChooseGangOccupation(pedType - PEDTYPE_GANG1);
							if (!((CPedModelInfo*)CModelInfo::GetModelInfo(memberModel))->GetRwObject())
								continue;

							CPed* memberPed = AddPed(pedType, memberModel, CVector(coors.x, coors.y, finalZ));
							if (!memberPed)
								continue;

							AddOffNetworkPed(memberPed, offNetwork);
							memberPed->SetObjective(OBJECTIVE_FOLLOW_CHAR_IN_FORMATION, leader);
							memberPed->SetFormation((eFormation)i);
							CVector formationPos = memberPed->GetFormationPosition();
							CVector finalFormationPos = formationPos;
							bool formationFoundGround;
							float memberGroundZ = CWorld::FindGroundZFor3DCoord(formationPos.x, formationPos.y, 1.0f + formationPos.z, &formationFoundGround);
							float formationGroundZ = memberGroundZ + 1.0f;

							finalFormationPos.z = Max(finalFormationPos.z, formationGroundZ);
							if (formationFoundGround && (!offNetwork || GroundIsClearOfWater(formationPos.x, formationPos.y, memberGroundZ))) {
								if (Abs(finalFormationPos.z - leader->GetPosition().z) <= 1.0f) {
									if (CWorld::GetIsLineOfSightClear(finalFormationPos, leader->GetPosition(), true, false, false, false, false, false, false)) {
										memberPed->SetPosition(finalFormationPos);
										createdPeds[pedIdx++] = memberPed;
										if (CGangs::GetWillAttackPlayerWithCops(pedType))
											leader->bCanAttackPlayerWithCops = true;

										CVisibilityPlugins::SetClumpAlpha(memberPed->GetClump(), 0);
										continue;
									}
								}
							}
							RemovePed(memberPed);
						}
						if (pedIdx >= 3) {
							for (int j = 1; j < pedIdx; ++j)
								createdPeds[j]->SetLeader(createdPeds[0]);

						} else {
							for (int k = 0; k < pedIdx; ++k) {
								RemovePed(createdPeds[k]);
							}
						}
					}
				}
			}
		}
	}
}

void
CPopulation::PlaceGangMembersInCircle(ePedType pedType, int pedAmount, CVector const& coors, bool offNetwork)
{
	CPed *createdPeds[5];

	if (pedAmount < 2)
		return;

	float circleSector = TWOPI / pedAmount;

	float circleR = Sqrt(0.5f / (1.0f - Cos(circleSector)));

	if (!TheCamera.IsSphereVisible(coors, circleR) ||
		CurrentTrafficRings().pedCreateMin * PedCreationDistMultiplier() <= (coors - FindPlayerPed()->GetPosition()).Magnitude2D()) {

		if (CPedPlacement::IsPositionClearForPed(coors, circleR, -1, nil)) {
			int pedIdx = 0;
			CVector leaderPos;
#ifdef FIX_BUGS
			bool createLeader = true;
#endif

			for (int i = 0; i < pedAmount; i++) {
				float angleMult = i + CGeneral::GetRandomNumberInRange(-0.2f, 0.2f);
				float randomR = circleR + CGeneral::GetRandomNumberInRange(-0.2f, 0.2f) * circleR;
				float xOffset = randomR * Cos(angleMult * circleSector);
				float yOffset = randomR * Sin(angleMult * circleSector);
				bool foundGround;
				float memberGroundZ = CWorld::FindGroundZFor3DCoord(xOffset + coors.x, yOffset + coors.y, coors.z + 1.0, &foundGround);
				float groundZ = memberGroundZ + 1.0f;
				if (foundGround) {
					CVector finalPos(coors.x + xOffset, coors.y + yOffset, coors.z > groundZ ? coors.z : groundZ);
					if (offNetwork && !GroundIsClearOfWater(xOffset + coors.x, yOffset + coors.y, memberGroundZ))
						continue;
#ifndef FIX_BUGS
					const bool createLeader = i == 0;
#endif
					if (createLeader)
						leaderPos = finalPos;

					int gangModel = ChooseGangOccupation(pedType - PEDTYPE_GANG1);
					if (((CPedModelInfo*)CModelInfo::GetModelInfo(gangModel))->GetRwObject()) {
						CEntity* obstacles[6] = { nil, nil, nil, nil, nil, nil };
						CPedPlacement::IsPositionClearForPed(finalPos, CModelInfo::GetColModel(gangModel)->boundingSphere.radius, ARRAY_SIZE(obstacles), obstacles);
						bool foundObstacle = false;
						for (int m = 0; m < ARRAY_SIZE(obstacles); m++) {
							CEntity* obstacle = obstacles[m];
							if (obstacle) {
								int n = 0;
								bool obstacleIsHarmless = false;
								for (int n = 0; n < pedIdx; n++) {
									if (obstacle == createdPeds[n])
										obstacleIsHarmless = true;
								}
								if (!obstacleIsHarmless) {
									foundObstacle = true;
									break;
								}
							}
						}
						bool memberCanSeeLeader = createLeader ? true : CWorld::GetIsLineOfSightClear(finalPos, leaderPos, true, false, false, false, false, false, false);

						bool notTooHighFromLeader = createLeader ? true : !(Abs(finalPos.z - leaderPos.z) >= 1.0f);

						if (!foundObstacle && memberCanSeeLeader && notTooHighFromLeader) {
							CPed* newPed = AddPed(pedType, gangModel, finalPos);
							if (newPed) {
								AddOffNetworkPed(newPed, offNetwork);
								createdPeds[pedIdx++] = newPed;
								float angle = CGeneral::GetRadianAngleBetweenPoints(
									coors.x, coors.y,
									finalPos.x, finalPos.y);
								newPed->m_fRotationDest = angle;
								newPed->m_fRotationCur = angle;
								if (CGangs::GetWillAttackPlayerWithCops(pedType))
									newPed->bCanAttackPlayerWithCops = true;

								CVisibilityPlugins::SetClumpAlpha(newPed->GetClump(), 0);
#ifdef FIX_BUGS
								createLeader = false;
#endif
							}
							// No.
#ifndef FIX_BUGS
							else
								CWorld::Remove(nil);
#endif
						}
					}
				}
			}
			if (pedIdx >= 3) {
				for (int j = 0; j < pedIdx / 2; ++j) {
					createdPeds[j]->SetChat(createdPeds[pedIdx - 1 - j], 100000);
					createdPeds[pedIdx - 1 - j]->SetChat(createdPeds[j], 100000);
				}

				// Make that extra guy in the middle stand there(PED_UNKNOWN locks him) and do nothing :lmao:
				if (pedIdx % 2 != 0) {
					CPed *tmim = createdPeds[(pedIdx - 1) / 2];
					float angle = CGeneral::GetRadianAngleBetweenPoints(
						tmim->GetPosition().x, tmim->GetPosition().y,
						createdPeds[0]->GetPosition().x, createdPeds[0]->GetPosition().y);
					tmim->SetHeading(angle);
					tmim->SetPedState(PED_UNKNOWN);
				}
				createdPeds[0]->bIsLeader = true;

				for (int l = 1; l < pedIdx; ++l)
					createdPeds[l]->SetLeader(createdPeds[0]);

			} else {
				for (int k = 0; k < pedIdx; ++k) {
					RemovePed(createdPeds[k]);
				}
			}
		}
	}
}

void
CPopulation::PlaceCouple(ePedType manType, int32 manModel, ePedType womanType, int32 womanModel, CVector coors, bool offNetwork)
{
	// Homosexuality filter!!!! Homophobic R* >>>:(
	if (manType != PEDTYPE_CIVMALE || womanType != PEDTYPE_CIVFEMALE)
		return;

	if (!TheCamera.IsSphereVisible(coors, 1.5f) || CurrentTrafficRings().pedCreateMin * PedCreationDistMultiplier() <= (coors - FindPlayerPed()->GetPosition()).Magnitude2D()) {
		if (CPedPlacement::IsPositionClearForPed(coors, CModelInfo::GetColModel(manModel)->boundingSphere.radius, -1, nil)) {
			bool manFoundGround;
			float manGroundZ = CWorld::FindGroundZFor3DCoord(coors.x, coors.y, coors.z, &manFoundGround) + 1.0f;
			if (manFoundGround) {
				CVector correctedManPos = coors;
				correctedManPos.z = Max(coors.z, manGroundZ);
				if (((CPedModelInfo*)CModelInfo::GetModelInfo(manModel))->GetRwObject()) {
					CPed *man = AddPed(PEDTYPE_CIVMALE, manModel, correctedManPos);
					if (man) {
						AddOffNetworkPed(man, offNetwork);
						man->SetObjective(OBJECTIVE_NONE);
						man->SetWanderPath(CGeneral::GetRandomNumberInRange(0, 8));
						man->bIsLeader = true;
						CVisibilityPlugins::SetClumpAlpha(man->GetClump(), 0);

						if (((CPedModelInfo*)CModelInfo::GetModelInfo(womanModel))->GetRwObject()) {
							CPed* woman = AddPed(PEDTYPE_CIVFEMALE, womanModel, correctedManPos); // will set the correct position later
							if (woman) {
								AddOffNetworkPed(woman, offNetwork);
								woman->SetObjective(OBJECTIVE_FOLLOW_CHAR_IN_FORMATION, man);
								woman->SetFormation(FORMATION_RIGHT);

								CVector formationPos = woman->GetFormationPosition();
								CVector womanPos = formationPos;
								bool womanFoundGround;
								float memberGroundZ = CWorld::FindGroundZFor3DCoord(formationPos.x, formationPos.y, 1.0f + formationPos.z, &womanFoundGround);
								float formationGroundZ = memberGroundZ + 1.0f;

								if (womanFoundGround && (!offNetwork || GroundIsClearOfWater(formationPos.x, formationPos.y, memberGroundZ))) {
									CVector correctedWomanPos = womanPos;
									correctedWomanPos.z = Max(womanPos.z, formationGroundZ);
									woman->SetPosition(correctedWomanPos);

									// What's the point of this??
									CEntity* obstacles[3];
									memcpy(obstacles, gCoupleObstacles, sizeof(gCoupleObstacles));

									CPedPlacement::IsPositionClearForPed(womanPos, CModelInfo::GetColModel(womanModel)->boundingSphere.radius, ARRAY_SIZE(obstacles), obstacles);
									for (int i = 0; i < ARRAY_SIZE(obstacles); i++) {
										CEntity *obstacle = obstacles[i];
										if (obstacle) {

											// We found a real obstacle, so let's break and we can delete them...
											if (obstacle != man && obstacle != woman)
												break;
										}
										if (i == ARRAY_SIZE(obstacles) - 1) {
											CVisibilityPlugins::SetClumpAlpha(woman->GetClump(), 0);
											return;
										}
									}
								}
								RemovePed(woman);
								RemovePed(man);
							}
						}
					}
				}
			}
		}
	}
}

// Mostly copy paste of PlaceGangMembersInFormation.
void
CPopulation::PlaceMallPedsAsStationaryGroup(CVector const& coors, int32 group)
{
#ifdef FIX_BUGS
	CPed *createdPeds[6];
#else
	CPed *createdPeds[5];
#endif

	if (CGame::currArea != AREA_MALL)
		return;

	int pedAmount = CGeneral::GetRandomNumberInRange(0, 4) + 3;

	float circleSector = TWOPI / pedAmount;

	float circleR = Sqrt(0.5f / (1.0f - Cos(circleSector)));

	if (!TheCamera.IsSphereVisible(coors, circleR) ||
		CurrentTrafficRings().pedCreateMin * PedCreationDistMultiplier() <= (coors - FindPlayerPed()->GetPosition()).Magnitude2D()) {

		if (CPedPlacement::IsPositionClearForPed(coors, circleR, -1, nil)) {
			int pedIdx = 0;
			CVector leaderPos;
#ifdef FIX_BUGS
			bool createLeader = true;
#endif

			for (int i = 0; i < pedAmount; i++) {	
				float angleMult = i + CGeneral::GetRandomNumberInRange(-0.2f, 0.2f);
				float randomR = circleR + CGeneral::GetRandomNumberInRange(-0.2f, 0.2f) * circleR;
				float xOffset = randomR * Cos(angleMult * circleSector);
				float yOffset = randomR * Sin(angleMult * circleSector);
				bool foundGround;
				float groundZ = CWorld::FindGroundZFor3DCoord(xOffset + coors.x, yOffset + coors.y, coors.z + 1.0, &foundGround) + 1.0f;
				if (foundGround) {
					CVector finalPos(coors.x + xOffset, coors.y + yOffset, coors.z > groundZ ? coors.z : groundZ);

#ifndef FIX_BUGS
					const bool createLeader = i == 0;
#endif
					if (createLeader)
						leaderPos = finalPos;

					int pedModel = ChooseCivilianOccupation(group);
					CPedModelInfo *pedModelInfo = (CPedModelInfo*)CModelInfo::GetModelInfo(pedModel);

					if (pedModelInfo->GetRwObject()) {
						CEntity* obstacles[6] = { nil, nil, nil, nil, nil, nil };
						CPedPlacement::IsPositionClearForPed(finalPos, CModelInfo::GetColModel(pedModel)->boundingSphere.radius, ARRAY_SIZE(obstacles), obstacles);
						bool foundObstacle = false;
						for (int m = 0; m < ARRAY_SIZE(obstacles); m++) {
							CEntity* obstacle = obstacles[m];
							if (obstacle) {
								int n = 0;
								bool obstacleIsHarmless = false;
								for (int n = 0; n < pedIdx; n++) {
									if (obstacle == createdPeds[n])
										obstacleIsHarmless = true;
								}
								if (!obstacleIsHarmless) {
									foundObstacle = true;
									break;
								}
							}
						}
						bool memberCanSeeLeader = createLeader ? true : CWorld::GetIsLineOfSightClear(finalPos, leaderPos, true, false, false, false, false, false, false);

						bool notTooHighFromLeader = createLeader ? true : !(Abs(finalPos.z - leaderPos.z) >= 1.0f);

						if (!foundObstacle && memberCanSeeLeader && notTooHighFromLeader) {
							CPed *newPed = AddPed(pedModelInfo->m_pedType, pedModel, finalPos);
							if (newPed) {
								createdPeds[pedIdx++] = newPed;
								float angle = CGeneral::GetRadianAngleBetweenPoints(
									coors.x, coors.y,
									finalPos.x, finalPos.y);
								newPed->m_fRotationDest = angle;
								newPed->m_fRotationCur = angle;
								newPed->m_fearFlags = 0;
								CVisibilityPlugins::SetClumpAlpha(newPed->GetClump(), 0);
#ifdef FIX_BUGS
								createLeader = false;
#endif
							}
							// No.
#ifndef FIX_BUGS
							else
								CWorld::Remove(nil);
#endif
						}
					}
				}
			}
			if (pedIdx >= 3) {
				for (int j = 0; j < pedIdx / 2; ++j) {
					createdPeds[j]->SetChat(createdPeds[pedIdx - 1 - j], 100000);
					createdPeds[pedIdx - 1 - j]->SetChat(createdPeds[j], 100000);
				}

				// Make that extra guy in the middle stand there(PED_UNKNOWN locks him) and do nothing :lmao:
				if (pedIdx % 2 != 0) {
					CPed *tmim = createdPeds[(pedIdx - 1) / 2];
					float angle = CGeneral::GetRadianAngleBetweenPoints(
						tmim->GetPosition().x, tmim->GetPosition().y,
						createdPeds[0]->GetPosition().x, createdPeds[0]->GetPosition().y);
					tmim->SetHeading(angle);
					tmim->SetPedState(PED_UNKNOWN);
				}
				createdPeds[0]->bIsLeader = true;

				for (int l = 1; l < pedIdx; ++l)
					createdPeds[l]->SetLeader(createdPeds[0]);

			} else {
				for (int k = 0; k < pedIdx; ++k) {
					RemovePed(createdPeds[k]);
				}
			}
		}
	}
}
