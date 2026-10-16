#include "common.h"

#include "FarTraffic.h"

#include "CarCtrl.h"

#include "Camera.h"
#include "CutsceneMgr.h"
#include "DMAudio.h"
#include "Game.h"
#include "General.h"
#include "PathFind.h"
#include "Ped.h"
#include "Pools.h"
#include "Streaming.h"
#include "Timer.h"
#include "Vehicle.h"
#include "VisibilityPlugins.h"
#include "World.h"
#include "Zones.h"

/*
 * The engine side of the far traffic layer (VICE-128). The rules, the band, the
 * cap and the switches live in FarTraffic.h; this file fills them from the
 * frame (the ring table, the player, the path network) and owns the cars and
 * the counters the live rig reads (utils/caps-audit/live-counters.py).
 *
 * One frame of the layer is three passes and they are in this order on purpose:
 *
 *	the gate pass	runs every frame over every car of the layer, so a car
 *			that reached the near band is promoted (or given up) in
 *			the frame the player could come to it, and a car beyond
 *			the fog or of a closed band goes. It is a distance and a
 *			switch per car.
 *	the creations	at most FAR_TRAFFIC_CREATE_ATTEMPTS placements, only
 *			while the layer is under its cap: the fog site (a car
 *			forming out of the haze ahead) and the behind site (the
 *			traffic the player passed). The placement is the near
 *			spawner's own (CPathFind::CollectCarCreationCoors) and the
 *			car is the near spawner's own (CCarCtrl::
 *			CreateCarFromPlacement in its far mode), so a far car is
 *			born with the same path state a near car would get and a
 *			promotion changes the flags only.
 *	the stagger	moves the share of the layer the frame owns: the path
 *			curve of the car, the path's own height, the heading, the
 *			alpha reveal and the matrix/RW frame update. No collision
 *			query, no sector scan, no path search, no physics.
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
 *	ms_nFarTrafficRefusedModel	those stopped by the streaming
 *	ms_nFarTrafficRefusedPool	those stopped by a pool reserve (or a driver)
 *	ms_nFarTrafficAttempts		the attempts
 *	ms_nFarTrafficUpdates		the car updates of the stagger
 *
 * The frame's work, exactly:
 *
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
 *	the stagger	a third of the layer, moved.
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
unsigned int CFarTraffic::ms_nFarTrafficRefusedModel;
unsigned int CFarTraffic::ms_nFarTrafficRefusedPool;
unsigned int CFarTraffic::ms_nFarTrafficAttempts;
unsigned int CFarTraffic::ms_nFarTrafficUpdates;

/*
 * The layer. A compact list of the cars it holds, with the pool slot each one
 * lives in. The slot is what makes a list entry safe: a car the layer forgot
 * about (a cutscene's cleanup, the pool's valve, a load) leaves a stale entry
 * behind, and the entry is validated against the slot before it is ever used:
 * the slot must hold the same pointer and the pointer must still be a far car.
 * A slot that was freed and taken by another far car fails the pointer test,
 * and a slot taken by any other car fails the class test. Nothing else is
 * cached, so a stale entry can never be read.
 */
struct SFarTrafficEntry
{
	CVehicle *pVehicle;
	int32 slot;
};

static SFarTrafficEntry ms_aFarCars[FAR_TRAFFIC_MAX];
static int32 ms_nFarCars;

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

/* Drop one entry: the last one takes its place, which keeps the list compact
 * and makes the stagger share a real share of the layer. */
static void
FarTrafficDropEntry(int32 index)
{
	ms_aFarCars[index] = ms_aFarCars[ms_nFarCars - 1];
	ms_nFarCars--;
}

static void
FarTrafficAddEntry(CVehicle *pVehicle)
{
	if (ms_nFarCars >= FAR_TRAFFIC_MAX)
		return;
	ms_aFarCars[ms_nFarCars].pVehicle = pVehicle;
	ms_aFarCars[ms_nFarCars].slot = CPools::GetVehiclePool()->GetJustIndex(pVehicle);
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

/* Give one far car up: the band closed, the cap dropped or the car is beyond
 * the fog. The near bubble's removal rule decides whether the player can see it
 * go; the layer spends at most FAR_TRAFFIC_REMOVE_ATTEMPTS gate answers a
 * frame, and it asks the gate with its ray refused (a removal of the layer's is
 * a fog or a rear-half one; an occluded hint waits a frame instead of spending
 * a two-kilometre ray). */
static bool
FarTrafficRemoveCar(int32 index, const CVector &playerPos)
{
	CVehicle *pVehicle = ms_aFarCars[index].pVehicle;

	if (!CCarCtrl::NearBubbleAllowsRemoval(pVehicle, playerPos, 0.0f, false))
		return false;
	CCarCtrl::NearBubbleCountRemoval(pVehicle->GetPosition(), NEAR_BUBBLE_ENTITY_RADIUS, false);
	CWorld::Remove(pVehicle);
	delete pVehicle;
	CFarTraffic::ms_nFarTrafficRemovals++;
	FarTrafficDropEntry(index);
	return true;
}

/*
 * The movement. The car follows the curve of its two path links - the same
 * arithmetic the on-rails simple car uses, through the shared core in CCarCtrl -
 * and the height comes from the path nodes themselves: the far layer touches no
 * collision query at all, which is the whole reason it is cheap. The car's
 * alpha reveal (VICE-127) runs on the same update, so a car created out of
 * sight holds its invisible frame until the player can see it and then fades in.
 */
static void
FarTrafficUpdateCar(CVehicle *pVehicle)
{
	/* The far state is re-asserted on every update: an explosion un-statices
	 * what it touches (CWorld::TriggerExplosionSectorList) and puts it back on
	 * the moving entity list, and a car that woke up would run the near AI
	 * beside this update. Two flag reads, and the car is a viewed shell
	 * again. */
	if (!pVehicle->GetIsStatic() || pVehicle->bUsesCollision)
		CFarTraffic::ApplyFarState(pVehicle);

	/* The curve is spent: the car picks its next node with the traffic AI's
	 * own pick (CCarCtrl::PickNextNodeRandomly), so a far car drives the
	 * paths the near traffic drives and a promotion lands on a valid rail.
	 * The pick is a random draw from the shared stream, exactly like the
	 * near traffic's own; see the ADR (docs/adr/0026). */
	if ((uint32)(pVehicle->AutoPilot.m_nTimeEnteredCurve + pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve)
			<= CTimer::GetTimeInMilliseconds())
		CCarCtrl::PickNextNodeRandomly(pVehicle);

	CVector position;
	CVector direction;
	CCarCtrl::ComputeCarOnRailPose(pVehicle, &position, &direction);

	/* The height of the rail: the path nodes carry the road's own height, so
	 * the interpolation between them is the road under the car, without a
	 * ground cast (which would be two collision queries per update, the one
	 * thing the layer must not spend). */
	CPathNode *pCurNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nCurrentRouteNode];
	CPathNode *pNextNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode];
	float between = CCarCtrl::GetPositionAlongCurrentCurve(pVehicle);
	position.z = pCurNode->GetZ() * (1.0f - between) + pNextNode->GetZ() * between;
	position.z += pVehicle->GetHeightAboveRoad();

	CVector forward = direction;
	forward.z = 0.0f;
	if (forward.MagnitudeSqr() > 0.0001f)
		forward.Normalise();
	else
		forward = pVehicle->GetForward();
	pVehicle->GetForward() = forward;
	pVehicle->GetRight() = CVector(forward.y, -forward.x, 0.0f);
	pVehicle->GetUp() = CVector(0.0f, 0.0f, 1.0f);
	pVehicle->SetPosition(position);
	pVehicle->SetMoveSpeed(direction / GAME_SPEED_TO_CARAI_SPEED);
	/* The sector lists follow the position (the renderer and the occupancy
	 * tests read them), and the RW frame follows the matrix. */
	pVehicle->RemoveAndAdd();
	pVehicle->GetMatrix().UpdateRW();
	pVehicle->UpdateRwFrame();
	/* The reveal of a creation (VICE-127): an off-screen car holds alpha 0
	 * until the camera can see it, and the ramp is the fade the player sees
	 * instead of a car that appeared from nowhere. The far update is the only
	 * thing stepping it, so the ramp runs at the layer's rate: sixteen steps
	 * of the stagger, which is 1.6 s at 10 Hz. */
	pVehicle->UpdateClumpAlpha();
	CFarTraffic::ms_nFarTrafficUpdates++;
}

/* One creation attempt at one site: the placement is the near spawner's own
 * (the path network's bracket of the site distance), the model comes from the
 * zone's loaded class arrays, and the car is built by the near spawner's own
 * function in its far mode, so the near bubble's creation gate and the
 * acceptance counters are asked exactly once. `forward` and `angleLimit` are
 * the near spawner's own arguments. */
static bool
FarTrafficTrySite(const SFarTrafficBand &band, const CVector &playerPos, const CVector2D &playerSpeed,
	float spawnDist, float dirX, float dirY, float angleLimit, bool forward)
{
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
	for (int32 i = 0; i < numPlacements; i++) {
		/* The water paths are not the layer's: a far car is a road car, and
		 * the boat rings are their own band (TrafficRings.h). */
		if (ThePaths.m_pathNodes[placements[i].node1].bWaterPath)
			continue;
		/* The placement is placed at the site distance, and the bracket's own
		 * length can put it inside or outside the band: the layer's site rule
		 * is the band's (FarTrafficCreateSiteIsInBand), asked with the final
		 * position. */
		float dist = (placements[i].position - playerPos).Magnitude2D();
		if (!FarTrafficCreateSiteIsInBand(band, dist))
			continue;
		CVehicle *pCar = nil;
		if (CCarCtrl::CreateCarFromPlacement(placements[i], carClass, carModel, playerPos,
				playerSpeed, false, false, true, &pCar) && pCar) {
			FarTrafficAddEntry(pCar);
			CFarTraffic::ms_nFarTrafficCreations++;
			return true;
		}
	}
	return false;
}

/* The two sites of one frame: the fog ahead (the oncoming traffic of a long
 * straight road, created in the haze) and the road behind (the same-direction
 * traffic that keeps flowing after the player stops). Both are hidden by the
 * near bubble's own arms - the fade for the first, the off-frustum margin for
 * the second - and the layer asks no line-of-sight ray for either. */
static void
FarTrafficTryCreate(const SFarTrafficBand &band, const CVector &playerPos, const CVector2D &playerSpeed)
{
	CFarTraffic::ms_nFarTrafficAttempts++;
	bool placed = FarTrafficTrySite(band, playerPos, playerSpeed,
		band.createOuter, TheCamera.CamFrontXNorm, TheCamera.CamFrontYNorm, 0.707f, true);
	/* The behind site keeps the rear half of the camera (a direction cosine of
	 * zero): the bubble's soft arm needs the position off the frustum by the
	 * margin, and the rear half is outside it for every FOV the game has. */
	if (!placed)
		placed = FarTrafficTrySite(band, playerPos, playerSpeed,
			band.inner, TheCamera.CamFrontXNorm, TheCamera.CamFrontYNorm, 0.0f, false);
	if (!placed)
		CFarTraffic::ms_nFarTrafficRefused++;
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
	/* The list is pruned first: a car the layer lost (a cutscene, the valve)
	 * leaves a stale entry, and the cap is the live count. */
	FarTrafficPruneEntries();
	if (ms_nFarCars >= FarTrafficCap(band))
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

/*
 * One frame of the layer. The car block of CGame::Process calls it after the
 * near cull, so the frame's pool, bubble budget and fill are already decided.
 */
void
CFarTraffic::Process(void)
{
	if (!FarTrafficIsActive()) {
		/* Paused: a cutscene or an interior owns the world, and a cutscene's
		 * own cleanup deletes the cars (CanBeDeleted is true for a far car).
		 * The list is pruned so the counters stay live; nothing is created,
		 * moved or removed. */
		FarTrafficPruneEntries();
		ms_nFarTrafficCars = ms_nFarCars;
		ms_nFarTrafficCap = 0;
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
	 * land inside the band (280 m or more out) and the staggered move steps a
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

	/* The stagger: the share of the layer this frame owns moves. */
	for (int32 i = 0; i < ms_nFarCars; i++) {
		if (!FarTrafficUpdatesThisFrame(CTimer::GetFrameCounter(), i))
			continue;
		if (FarTrafficEntryIsLive(ms_aFarCars[i]))
			FarTrafficUpdateCar(ms_aFarCars[i].pVehicle);
	}

	ms_nFarTrafficCars = ms_nFarCars;
}
