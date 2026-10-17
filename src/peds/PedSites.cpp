#include "common.h"

#include "PedSites.h"
#include "World.h"
#include "WaterLevel.h"
#include "Camera.h"
#include "General.h"
#include "PlayerInfo.h"

/*
 * The engine side of the pedestrian standable cells (VICE-154). The model,
 * the predicate and the store are in PedSites.h/PedSiteCells.cpp, which are
 * engine-free; this file is the world queries of one probe, the frame's slice
 * of the lazy build, and the population's entry point.
 *
 * Every query here runs on the game's own thread. The design's worker
 * boundary (section 9, section 11) is kept: the planner worker will read the
 * store's plain answers, never the world, and nothing in the store calls back
 * into the game.
 */

/* The grid mirrors World.h's own numbers; a mismatch is a cell at the wrong
 * place, so the build checks the mirror. */
static_assert(PED_SITE_GRID_MIN_X == WORLD_MIN_X, "the ped site grid's west edge is the world's");
static_assert(PED_SITE_GRID_MIN_Y == WORLD_MIN_Y, "the ped site grid's south edge is the world's");
static_assert((int)(PED_SITE_CELLS_X * PED_SITE_CELL_SIZE) == (int)WORLD_SIZE_X, "the ped site grid spans the world's width");
static_assert((int)(PED_SITE_CELLS_Y * PED_SITE_CELL_SIZE) == (int)WORLD_SIZE_Y, "the ped site grid spans the world's height");

/* The one store of the game. */
CPedSiteCells CPedSites::ms_cells;

/* The live rig's counters (see the header). */
uint32 CPedSites::ms_nCellsBuilt;
uint32 CPedSites::ms_nCellsQueued;
uint32 CPedSites::ms_nCellsDropped;
uint32 CPedSites::ms_nCellsEvicted;
uint32 CPedSites::ms_nRequests;
uint32 CPedSites::ms_nRequestsAnswered;
uint32 CPedSites::ms_nRequestsDead;
uint32 CPedSites::ms_nProbes;
uint32 CPedSites::ms_nRejectsNoGround;
uint32 CPedSites::ms_nRejectsWater;
uint32 CPedSites::ms_nRejectsLevel;
uint32 CPedSites::ms_nRejectsRoof;
uint32 CPedSites::ms_nRejectsClearance;
uint32 CPedSites::ms_nRejectsSlope;

/* The one probe of one lattice point: the world queries the predicate reads.
 * The flags are the VICE-122 placement's own casts and sphere: the buildings
 * only (a sidewalk's ground is a building's collision), and the cast pair the
 * old free position used, so the cells validate what that path validated, with
 * the slope added. */
static void
PedSiteProbeWorld(float x, float y, float levelZ, SPedSiteProbe &probe)
{
	CColPoint col;
	CEntity *ent = nil;
	bool found;
	float surfaceZ;

	/* The ground: the topmost surface at or below the level of the request
	 * (the ring level the node path's own ground check used). The normal is
	 * the slope predicate's input. */
	probe.groundFound = CWorld::ProcessVerticalLine(CVector(x, y, levelZ), -1000.0f, col, ent,
		true, false, false, false, false, false, nil);
	probe.groundZ = probe.groundFound ? col.point.z : 0.0f;
	probe.groundNormalZ = probe.groundFound ? col.normal.z : 0.0f;

	/* The surface over the point: the topmost surface below the cast, the
	 * VICE-122 roof test. The cast starts at the ground (the old path's own
	 * start: it set its position to the ground before the cast); a ground
	 * that was not found leaves the cast at the level (the verdict is the
	 * no-ground one, so the surface is never read). A roof, a canopy or a
	 * bridge deck more than a step above the ground rejects the point. */
	surfaceZ = CWorld::FindGroundZFor3DCoord(x, y,
		(probe.groundFound ? probe.groundZ : levelZ) + PED_SITE_ROOF_CAST, &found);
	probe.surfaceFound = found;
	probe.surfaceZ = surfaceZ;

	/* The water line of the block, without the waves: the same call the
	 * placement and the stray cull read (the z argument is the call's shape;
	 * the line ignores it). */
	probe.waterFound = CWaterLevel::GetWaterLevelNoWaves(x, y, probe.groundFound ? probe.groundZ : levelZ, &probe.waterZ);

	/* The clearance over the feet: a sphere against the static world. The
	 * pedestrian position check (CPedPlacement::IsPositionClearForPed) reads
	 * vehicles and pedestrians only, so this is what catches a spot inside a
	 * wall, a fence or a thin roof; that dynamic check stays in the creation
	 * loop, where it is fresh. */
	probe.clearance = !CWorld::TestSphereAgainstWorld(CVector(x, y, probe.groundZ + PED_SITE_CLEARANCE_HEIGHT),
		PED_SITE_CLEARANCE_RADIUS, nil, true, false, false, false, false, false);
}

/* One build: the lattice's probes of the cell, then the store's report (the
 * predicate, the spot selection and the counters). */
void
CPedSites::ProbeCell(uint16 cell, float levelZ)
{
	SPedSiteProbe aProbes[PED_SITE_PROBES];

	for (int32 i = 0; i < PED_SITE_PROBES; i++) {
		float x, y;
		CPedSiteCells::ProbePoint(cell, i, &x, &y);
		PedSiteProbeWorld(x, y, levelZ, aProbes[i]);
	}
	ms_cells.ReportCellProbed(cell, levelZ, aProbes);
}

void
CPedSites::Reset(void)
{
	ms_cells.Reset();
	PublishStats();
}

void
CPedSites::Pump(void)
{
	const CVector &playerCentre = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);

	ms_cells.SetAnchor(playerCentre.x, playerCentre.y);

	/* The frame's slice: PED_SITE_CELLS_PER_FRAME builds, nearest first. A
	 * frame with an empty queue spends nothing, and no frame can spend more
	 * than the cap: the build cannot spike a frame (the evidence program
	 * prices the arithmetic; the probes are a constant per build). */
	for (int32 i = 0; i < PED_SITE_CELLS_PER_FRAME; i++) {
		uint16 cell;
		float levelZ;
		if (!ms_cells.PopNearestQueued(cell, levelZ))
			break;
		ProbeCell(cell, levelZ);
	}
	PublishStats();
}

bool
CPedSites::FindOffNetworkCoors(const SPedSitePoint &centre, float minDist, float maxDist,
	float minDistOffScreen, float maxDistOffScreen, SPedSitePoint *pPosition)
{
	for (int attempt = 0; attempt < PED_SITE_ATTEMPTS; attempt++) {
		/* The draw is the VICE-122 one: an angle and a distance over the
		 * union of the two bands (the creation ring in view, the wider
		 * off-screen band around it). The camera decides which band the
		 * answer must fall in; the draw only picks the cell. */
		float angle = CGeneral::GetRandomNumberInRange(0.0f, TWOPI);
		float dist = CGeneral::GetRandomNumberInRange(minDistOffScreen, maxDist);
		float x = centre.x + dist * Sin(angle);
		float y = centre.y + dist * Cos(angle);
		SPedSitePoint spot;

		if (ms_cells.RequestSpot(x, y, centre.z, &spot) != PED_SITE_ANSWER_SPOT)
			continue;

		/* The answer is a stored point of the drawn cell, up to a cell away
		 * from the draw: the caller's rules are read on the point that is
		 * returned, not on the point that was drawn. */
		float spotDist = (CVector(spot.x, spot.y, spot.z) - CVector(centre.x, centre.y, centre.z)).Magnitude2D();
		bool visible = TheCamera.IsSphereVisible(CVector(spot.x, spot.y, spot.z), 2.0f);
		if (visible && !(minDist < spotDist && spotDist < maxDist))
			continue;
		if (!visible && !(minDistOffScreen < spotDist && spotDist < maxDistOffScreen))
			continue;
		/* The level of the ring: the point's ground against the centre's,
		 * the same margin the build's level rules read. */
		if (Abs(spot.z - centre.z) > PED_SITE_LEVEL_MARGIN)
			continue;

		*pPosition = spot;
		PublishStats();
		return true;
	}
	PublishStats();
	return false;
}

void
CPedSites::PublishStats(void)
{
	const SPedSiteStats &stats = ms_cells.GetStats();

	ms_nCellsBuilt = stats.cellsBuilt;
	ms_nCellsQueued = stats.cellsQueued;
	ms_nCellsDropped = stats.cellsDropped;
	ms_nCellsEvicted = stats.cellsEvicted;
	ms_nRequests = stats.requests;
	ms_nRequestsAnswered = stats.answered;
	ms_nRequestsDead = stats.deadAnswers;
	ms_nProbes = stats.probes;
	ms_nRejectsNoGround = stats.verdicts[PED_SITE_NO_GROUND];
	ms_nRejectsWater = stats.verdicts[PED_SITE_WATER];
	ms_nRejectsLevel = stats.verdicts[PED_SITE_LEVEL];
	ms_nRejectsRoof = stats.verdicts[PED_SITE_ROOF];
	ms_nRejectsClearance = stats.verdicts[PED_SITE_CLEARANCE];
	ms_nRejectsSlope = stats.verdicts[PED_SITE_SLOPE];
}
