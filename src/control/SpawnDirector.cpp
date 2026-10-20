#include "common.h"

#include "SpawnDirector.h"

#include "CarCtrl.h"

#include "Camera.h"
#include "Game.h"
#include "PlayerInfo.h"
#include "Population.h"
#include "Pools.h"
#include "Timer.h"
#include "World.h"
#include "Zones.h"

/*
 * The engine side of the population director (VICE-152). The grid, the
 * weights, the apportionment, the maintenance tick and the consistency metric
 * live in SpawnDirector.h (the engine-free core); this file is the frame's
 * half:
 *
 *	the build		the presence facts of every cell, from the site
 *				index (a street, a water lane, the lanes), once
 *				per site build;
 *	the refresh		the live facts of the active cells - the info
 *				zone's densities at the cell centre and the
 *				view - and the apportionment of the engine's own
 *				targets: CCarCtrl::GetRandomCarTarget and
 *				CPopulation's own count target. The refresh runs
 *				when the player's cell, the mode or the cadence
 *				moved, or a slider moved the target by a
 *				twentieth;
 *	the tick		the live ladder arm of every active cell centre
 *				(the frame's own visibility, the same candidate
 *				the planner reads) and the live counts from the
 *				occupancy store;
 *	the two questions	the next cell the ambient requester serves and
 *				the over-target release the near cull asks
 *				about (CarCtrl.cpp).
 *
 * The counters keep one owner: SSpawnDirectorTelemetry has one instance,
 * CSpawnDirector::ms_Telemetry. The live reader (utils/caps-audit/
 * live-counters.py) reads its fields at the offsets pinned by the
 * static_asserts below.
 */

SSpawnDirectorTelemetry CSpawnDirector::ms_Telemetry;
SSpawnDirector CSpawnDirector::ms_Director;

/* The live reader's offsets. The layout is the header's field order; a field
 * move breaks the build here before it can misread the live rows. */
static_assert(offsetof(SSpawnDirectorTelemetry, m_nRefreshes) == 0, "the live reader reads the refresh rows at offset 0");
static_assert(offsetof(SSpawnDirectorTelemetry, m_nCarTargetSum) == 12, "the live reader reads the conservation rows at offset 12");
static_assert(offsetof(SSpawnDirectorTelemetry, m_nPedTargetSum) == 24, "the live reader reads the ped conservation rows at offset 24");
static_assert(offsetof(SSpawnDirectorTelemetry, m_nRequestsIssued) == 36, "the live reader reads the request rows at offset 36");
static_assert(offsetof(SSpawnDirectorTelemetry, m_nReleases) == 44, "the live reader reads the release row at offset 44");
static_assert(offsetof(SSpawnDirectorTelemetry, m_nCarStarvedCells) == 48, "the live reader reads the car consistency rows at offset 48");
static_assert(offsetof(SSpawnDirectorTelemetry, m_nPedStarvedCells) == 60, "the live reader reads the ped consistency rows at offset 60");
static_assert(sizeof(SSpawnDirectorTelemetry) == 72, "the telemetry's size is the live reader's contract");

/* The active set's bound against the ring table's own cap: the cells whose
 * centre is inside the car despawn at the maximum scale. The lattice's worst
 * case (the player at a cell centre) is 61 cells; the bound leaves the margin
 * the harness's larger radii may need. */
static_assert(SPAWN_DIRECTOR_MAX_ACTIVE >= 64, "the active set must hold the worst shipped case");

/* The director's cell grid and the occupancy store's grid share the world's
 * origin, so a cell's presence block is exact (the last row and column reach
 * past the store's own edge and read zero there). */
static_assert(SPAWN_DIRECTOR_GRID_MIN_X == SPAWN_OCCUPANCY_GRID_MIN_X, "the director grid must start at the store's own origin");
static_assert(SPAWN_DIRECTOR_GRID_MIN_Y == SPAWN_OCCUPANCY_GRID_MIN_Y, "the director grid must start at the store's own origin");
static_assert(SPAWN_DIRECTOR_CELL_SIZE == SPAWN_OCCUPANCY_CELL_SIZE * SPAWN_DIRECTOR_CELL_BLOCK, "a director cell must be a whole block of the store's cells");

/* The live facts' user: the player centre the zone lookup and the ladder's
 * candidate are resolved against (one read a refresh, never a per-cell
 * re-fetch). */
struct SDirectorLiveUser
{
	CVector playerCentre;
};

void
CSpawnDirector::ResolveLiveFacts(void *user, int32 cell, float centreX, float centreY, SSpawnDirectorCellFacts *facts)
{
	SDirectorLiveUser *pUser = (SDirectorLiveUser *)user;
	CZoneInfo zone;
	CVector pos(centreX, centreY, pUser->playerCentre.z);
	SNearBubbleCandidate candidate;
	ENearBubbleArm arm;

	CTheZones::GetZoneInfoForTimeOfDay(&pos, &zone);
	facts->zoneCarDensity = zone.carDensity;
	facts->zonePedDensity = zone.pedDensity;
	/* The view term: the arms the player can see the cell under - the
	 * catchable set (refused for a creation, but the player is looking at
	 * it) and the in-view band. The hidden arms (behind the camera, the
	 * occluders, the fade) do not lean the target. */
	CCarCtrl::NearBubbleCandidateFor(pos, pUser->playerCentre, NEAR_BUBBLE_ENTITY_RADIUS, &candidate);
	arm = NearBubbleArmFor(candidate);
	facts->inView = arm == NEAR_BUBBLE_ARM_NONE || arm == NEAR_BUBBLE_ARM_ONSCREEN_BAND;
}

void
CSpawnDirector::Build(void)
{
	SpawnDirectorCollectSiteFacts(CSpawnSites::Index(), ms_Director.aFacts);
}

/* The live facts the frame's tick resolves, and the arms. One candidate per
 * active cell centre, the same call the planner makes per offered site: a
 * frustum test, the occluder hint and a couple of distances, tens of cells a
 * frame. */
static uint8
DirectorArmAt(const CVector &playerCentre, int32 cell)
{
	SNearBubbleCandidate candidate;

	CCarCtrl::NearBubbleCandidateFor(
		CVector(SpawnDirectorCellCentreX(cell), SpawnDirectorCellCentreY(cell), playerCentre.z),
		playerCentre, NEAR_BUBBLE_ENTITY_RADIUS, &candidate);
	return (uint8)NearBubbleArmFor(candidate);
}

void
CSpawnDirector::BeginFrame(void)
{
	const STrafficRings rings = CurrentTrafficRings();
	CPlayerInfo *pPlayer = &CWorld::Players[CWorld::PlayerInFocus];
	CVector playerCentre = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	uint32 nowMs = CTimer::GetTimeInMilliseconds();
	int32 playerCell = SpawnDirectorCellOf(playerCentre.x, playerCentre.y);
	bool interior = CGame::IsInInterior();
	int32 carTarget = interior ? 0 : (int32)ceilf(CCarCtrl::GetRandomCarTarget());
	int32 pedTarget = (int32)ceilf(CPopulation::GetAreaPedCountTarget());
	uint8 mode = interior ? SPAWN_DIRECTOR_MODE_INTERIOR :
		(playerCell >= 0 ? SPAWN_DIRECTOR_MODE_MAP : SPAWN_DIRECTOR_MODE_OFF_MAP);
	bool refresh = ms_Director.lastPlayerCell < 0 ||
		playerCell != ms_Director.lastPlayerCell ||
		mode != ms_Director.mode ||
		(uint32)(nowMs - ms_Director.lastRefreshMs) >= SPAWN_DIRECTOR_REFRESH_MS;

	/* A slider or a transition moved the target materially: the apportionment
	 * follows within a twentieth (the weather's drift of the ped budget is a
	 * per-frame float, so an exact comparison would re-apportion every
	 * frame). */
	if (!refresh) {
		int32 lastCar = ms_Director.target[SPAWN_DIRECTOR_TRACK_CAR];
		int32 lastPed = ms_Director.target[SPAWN_DIRECTOR_TRACK_PED];
		int32 carStep = lastCar / 20 > 1 ? lastCar / 20 : 1;
		int32 pedStep = lastPed / 20 > 1 ? lastPed / 20 : 1;

		if (carTarget - lastCar > carStep || lastCar - carTarget > carStep ||
		    pedTarget - lastPed > pedStep || lastPed - pedTarget > pedStep)
			refresh = true;
	}

	if (refresh) {
		SDirectorLiveUser user;

		user.playerCentre = playerCentre;
		SpawnDirectorRefresh(&ms_Director, nowMs, playerCentre.x, playerCentre.y,
			pPlayer->m_fRoadDensity, rings.carDespawn, rings.pedRemove,
			carTarget, pedTarget, mode, ResolveLiveFacts, &user);
		/* The conservation rows of the service's ledger: the car track's
		 * apportioned sum against the engine's own target. The ped track's
		 * pair is the director's own telemetry. */
		CSpawnService::NoteConservation((uint32)ms_Director.targetSum[SPAWN_DIRECTOR_TRACK_CAR],
			(uint32)ms_Director.target[SPAWN_DIRECTOR_TRACK_CAR]);
		SpawnDirectorNoteRefresh(&ms_Telemetry, &ms_Director);
	}

	/* The frame's arms: the live ladder of every active cell centre. */
	uint8 arms[SPAWN_DIRECTOR_CELLS];
	int32 i;

	for (i = 0; i < ms_Director.numActive[SPAWN_DIRECTOR_TRACK_CAR]; i++) {
		int32 cell = ms_Director.aActive[SPAWN_DIRECTOR_TRACK_CAR][i];

		arms[cell] = DirectorArmAt(playerCentre, cell);
	}
	SpawnDirectorBeginFrame(&ms_Director, CSpawnOccupancy::Store(), nowMs, arms);
	SpawnDirectorNoteTick(&ms_Telemetry, &ms_Director);
}

bool
CSpawnDirector::NextCellTarget(float *pX, float *pY, uint8 *pWater)
{
	CVector playerCentre = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	CVector2D playerSpeed = FindPlayerSpeed();
	float speed = playerSpeed.Magnitude();
	/* The travel direction the request's lead follows: the player's own
	 * velocity, or zero for a standing player (the target is the cell then,
	 * and the ladder's own gates do the rest). */
	float dirX = speed > 0.001f ? playerSpeed.x / speed : 0.0f;
	float dirY = speed > 0.001f ? playerSpeed.y / speed : 0.0f;
	bool served = SpawnDirectorNextRequest(&ms_Director, playerCentre.x, playerCentre.y, dirX, dirY, pX, pY, pWater);

	SpawnDirectorNoteRequest(&ms_Telemetry, served);
	return served;
}

bool
CSpawnDirector::AllowsOverTargetRelease(CVehicle *pVehicle, const CVector &playerPos)
{
	/* The director thins the ambient traffic only: a parked, mission, police
	 * or far car is another requester's and the cull's own rules hold it. */
	if (pVehicle->VehicleCreatedBy != RANDOM_VEHICLE || !pVehicle->bIsTrafficCar)
		return false;

	const CVector &pos = pVehicle->GetPosition();
	int32 cell = SpawnDirectorCellOf(pos.x, pos.y);

	return SpawnDirectorReleaseAllowed(&ms_Director, cell, pos.x, pos.y,
		playerPos.x, playerPos.y, CTimer::GetTimeInMilliseconds());
}

void
CSpawnDirector::NoteOverTargetRelease(CVehicle *pVehicle)
{
	const CVector &pos = pVehicle->GetPosition();
	int32 cell = SpawnDirectorCellOf(pos.x, pos.y);

	SpawnDirectorNoteReleaseAt(&ms_Director, cell, CTimer::GetTimeInMilliseconds());
	SpawnDirectorNoteRelease(&ms_Telemetry);
}

void
CSpawnDirector::ReInit(void)
{
	SpawnDirectorReset(&ms_Director);
	SpawnDirectorTelemetryReset(&ms_Telemetry);
	/* The presence facts are the site index's and survive a restart (the
	 * graph does not move); the live facts refresh on the next tick. */
	SpawnDirectorCollectSiteFacts(CSpawnSites::Index(), ms_Director.aFacts);
}

void
CSpawnDirector::PrintStats(void)
{
#ifndef MASTER
	printf("Spawn director: %d x %d cells of %.0f m; the car set %d cells, the ped set %d; the car target %d (sum %d), the ped target %d (sum %d)\n",
		SPAWN_DIRECTOR_CELLS_X, SPAWN_DIRECTOR_CELLS_Y, SPAWN_DIRECTOR_CELL_SIZE,
		ms_Director.numActive[SPAWN_DIRECTOR_TRACK_CAR], ms_Director.numActive[SPAWN_DIRECTOR_TRACK_PED],
		ms_Director.target[SPAWN_DIRECTOR_TRACK_CAR], ms_Director.targetSum[SPAWN_DIRECTOR_TRACK_CAR],
		ms_Director.target[SPAWN_DIRECTOR_TRACK_PED], ms_Director.targetSum[SPAWN_DIRECTOR_TRACK_PED]);
#endif
}
