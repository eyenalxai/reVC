#ifndef __GTA_CARCTRL_H__
#define __GTA_CARCTRL_H__
#include "PathFind.h"
#include "Boat.h"
#include "Vehicle.h"
/* The car budgets: RANDOM_CARS_PER_INI_UNIT, MAX_RANDOM_CARS, MAX_CARS_IN_USE,
 * the parked target and the fill rate (VICE-123). The fill burst of the traffic
 * lives in FillBurst.h (VICE-126); the near bubble of the spawns and the culls
 * lives in NearBubble.h (VICE-127); the far traffic layer lives in
 * FarTraffic.h (VICE-128). The spawn service (VICE-150) lives in
 * SpawnService.h: the planner, the two creation kinds and the primitive's
 * decisions; CarCtrl.cpp is its ambient requester. */
#include "PopulationBudget.h"
#include "FillBurst.h"
#include "NearBubble.h"
#include "FarTraffic.h"
#include "SpawnService.h"

#define GAME_SPEED_TO_METERS_PER_SECOND 50.0f
#define METERS_PER_SECOND_TO_GAME_SPEED (1.0f / GAME_SPEED_TO_METERS_PER_SECOND)
#define GAME_SPEED_TO_CARAI_SPEED 60.0f
#define TIME_COPS_WAIT_TO_EXIT_AFTER_STOPPING 2500

class CZoneInfo;
class CAutomobile;

enum{
	MAX_CARS_TO_KEEP = 2,
};

#ifdef FIX_BUGS
#define FIX_PATHFIND_BUG
#endif

class CCarCtrl
{
public:
	enum eCarClass {
		NORMAL = 0,
		POOR,
		RICH,
		EXEC,
		WORKER,
		BIG,
		TAXI,
		MOPED,
		MOTORBIKE,

		LEISUREBOAT,
		WORKERBOAT,

		COPS,
		CUBAN,
		HAITIAN,
		STREET,
		DIAZ,
		BIKER,
		SECURITY,
		PLAYER,
		GOLFERS,
		GANG9,
		COPS_BOAT,
		FIRST_CAR_RATING = NORMAL,
		FIRST_BOAT_RATING = LEISUREBOAT,
		FIRST_GANG_CAR_RATING = CUBAN,
		NUM_CAR_CLASSES = MOTORBIKE - FIRST_CAR_RATING + 1,
		NUM_BOAT_CLASSES = WORKERBOAT - FIRST_BOAT_RATING + 1,
		NUM_GANG_CAR_CLASSES = GANG9 - FIRST_GANG_CAR_RATING + 1,
		TOTAL_CUSTOM_CLASSES = NUM_CAR_CLASSES + NUM_BOAT_CLASSES
	};

	static void SwitchVehicleToRealPhysics(CVehicle*);
	static void AddToCarArray(int32 id, int32 vehclass);
	static void UpdateCarCount(CVehicle*, bool);
	static int32 NumCarsInUse(void);
	static int32 ChooseCarModel(int32 vehclass);
	static bool JoinCarWithRoadSystemGotoCoors(CVehicle*, CVector, bool);
	static void JoinCarWithRoadSystem(CVehicle*);
	static void UpdateCarOnRails(CVehicle*);
	static bool MapCouldMoveInThisArea(float x, float y);
	static void ScanForPedDanger(CVehicle *veh);
	static void RemoveFromInterestingVehicleList(CVehicle*);
	static float GetRandomCarTarget(void);
	static void GenerateRandomCars(void);
	static void GenerateOneRandomCar(void);
	static void ArmFillBurst(bool withParkedCars);
	static void SpendFillBurstFrame(void);
	/* The creation primitive (VICE-150): the route and lane state, the
	 * ground and the water, the physics and the status, the occupants with
	 * the caps and reserves, the final validation and the counters. The
	 * dice and the camera decisions are gone (the planner owns them); the
	 * placement carries the planned site (or a raw collector placement, the
	 * far layer's own until VICE-167) and the tier carries the far layer's
	 * birth-cull skip. The return is the refusal (SPAWN_REFUSAL_*). */
	static ESpawnRefusal CreateCarFromPlacement(const SSpawnPlacement& placement, int carClass, int carModel, const CVector& vecTargetPos, const CVector2D& vecPlayerSpeed, bool testForCollision, ESpawnTier tier = SPAWN_TIER_NEAR, CVehicle **ppCreated = nil, ENearBubbleArm *pArm = nil);
	static void GenerateEmergencyServicesCar(void);
	static int32 ChooseModel(CZoneInfo*, int*);
	static int32 ChoosePoliceCarModel(void);
	static int32 ChooseGangCarModel(int32 gang);
	static void RemoveDistantCars(void);
	static void PossiblyRemoveVehicle(CVehicle*, CVector vecPlayerPos);
	static bool IsThisVehicleInteresting(CVehicle*);
	static void RegisterVehicleOfInterest(CVehicle*);
	static int32 CountCarsOfType(int32 mi);
	static void SlowCarOnRailsDownForTrafficAndLights(CVehicle*);
	static bool PickNextNodeAccordingStrategy(CVehicle*);
	static void DragCarToPoint(CVehicle*, CVector*);
	/* The pose of a car on the rail of its two path links: the lane-including
	 * positions, the curve between them and the car's place on it. Shared by
	 * the on-rails simple car (which drags the car onto the pose with the
	 * ground cast) and by the far layer (which writes the pose from the
	 * path's own height; VICE-128). `pPosition` and `pDirection` are the
	 * curve point and its tangent. */
	static void ComputeCarOnRailPose(CVehicle *pVehicle, CVector *pPosition, CVector *pDirection);
	static float FindMaximumSpeedForThisCarInTraffic(CVehicle*);
	static void SlowCarDownForCarsSectorList(CPtrList&, CVehicle*, float, float, float, float, float*, float);
	static void SlowCarDownForPedsSectorList(CPtrList&, CVehicle*, float, float, float, float, float*, float);
	static void SlowCarDownForOtherCar(CEntity*, CVehicle*, float*, float);
	static float TestCollisionBetween2MovingRects(CVehicle*, CVehicle*, float, float, CVector*, CVector*, uint8);
	static float FindAngleToWeaveThroughTraffic(CVehicle*, CPhysical*, float, float);
	static void WeaveThroughCarsSectorList(CPtrList&, CVehicle*, CPhysical*, float, float, float, float, float*, float*);
	static void WeaveForOtherCar(CEntity*, CVehicle*, float*, float*);
	static void WeaveThroughPedsSectorList(CPtrList&, CVehicle*, CPhysical*, float, float, float, float, float*, float*);
	static void WeaveForPed(CEntity*, CVehicle*, float*, float*);
	static void WeaveThroughObjectsSectorList(CPtrList&, CVehicle*, float, float, float, float, float*, float*);
	static void WeaveForObject(CEntity*, CVehicle*, float*, float*);
#ifdef FIX_PATHFIND_BUG
	static void PickNextNodeToChaseCar(CVehicle*, float, float, float, CVehicle*);
#else
	static void PickNextNodeToChaseCar(CVehicle*, float, float, CVehicle*);
#endif
	static bool PickNextNodeToFollowPath(CVehicle*);
	static void PickNextNodeRandomly(CVehicle*);
	/* FindPathDirection moved into CarOnRails.h with the pick (VICE-133): the
	 * car-on-rails rules are shared with the far traffic layer's worker. */
	static void Init(void);
	static void ReInit(void);
	static float FindSpeedMultiplier(float, float, float, float);
	static void SteerAICarWithPhysics(CVehicle*);
	static void SteerAICarWithPhysics_OnlyMission(CVehicle*, float*, float*, float*, bool*);
	static float FindMaxSteerAngle(CVehicle*);
	static void SteerAICarWithPhysicsFollowPath(CVehicle*, float*, float*, float*, bool*);
	static void SteerAICarWithPhysicsHeadingForTarget(CVehicle*, CPhysical*, float, float, float*, float*, float*, bool*);
	static void SteerAICarWithPhysicsTryingToBlockTarget(CVehicle*, float, float, float, float, float*, float*, float*, bool*);
	static void SteerAICarWithPhysicsTryingToBlockTarget_Stop(CVehicle*, float, float, float, float, float*, float*, float*, bool*);
	static bool ThisRoadObjectCouldMove(int16);
	static void ClearInterestingVehicleList();
	static void FindLinksToGoWithTheseNodes(CVehicle*);
	static bool GenerateOneEmergencyServicesCar(uint32, CVector);
	static float FindSpeedMultiplierWithSpeedFromNodes(int8);
	static int32 ChooseBoatModel(int32);
	static int32 ChooseBoatRating(CZoneInfo* pZoneInfo);
	static int32 ChooseCarRating(CZoneInfo* pZoneInfo);
	static void AddToLoadedVehicleArray(int32 mi, int32 rating, int32 freq);
	static void RemoveFromLoadedVehicleArray(int32 mi, int32 rating);
	static int32 ChooseCarModelToLoad(int32 rating);
	static bool BoatWithTallMast(int32 mi);
	static void RemoveCarsIfThePoolGetsFull(void);

	/* The near bubble (VICE-127): the gates of the spawns and the culls, and
	 * the counters the acceptance reads (NearBubble.h, NearBubble.cpp). The
	 * frame begins them once, from GenerateRandomCars. The spawn service
	 * (VICE-150) is the only caller of these: its ladder API forwards here,
	 * so no spawn decision outside SpawnService.cpp asks the gates. */
	static void NearBubbleBeginFrame(void);
	/* The ladder's candidate for a position (the planner's cheap fact).
	 * `occluded` is the trusted hint; the rank derives the arm from the
	 * candidate and never spends a ray. */
	static void NearBubbleCandidateFor(const CVector &pos, const CVector &playerCentre, float radius, SNearBubbleCandidate *pOut);
	static bool NearBubbleAllowsCreation(const CVector &pos, const CVector &playerCentre, float radius, bool rayAllowed, ENearBubbleArm *pArm = nil);
	static bool NearBubbleAllowsRemoval(CVehicle *pVehicle, const CVector &playerCentre, float ringThreshold, bool rayAllowed = true);
	static void NearBubbleCountCreation(const CVector &pos, float radius);
	static void NearBubbleCountRemoval(const CVector &pos, float radius, bool wasInvisible);
	static void NearBubbleCountOtherCreation(const CVector &pos, float radius);
	static void NearBubbleCountValveRemoval(void);
	static void SteerAIBoatWithPhysicsHeadingForTarget(CVehicle*, float, float, float*, float*, float*);
	static void SteerAIHeliTowardsTargetCoors(CAutomobile*);
	static void SteerAIPlaneTowardsTargetCoors(CAutomobile*);
	static void SteerAIBoatWithPhysicsAttackingPlayer(CVehicle*, float*, float*, float*, bool*);
	static void SteerAICarBlockingPlayerForwardAndBack(CVehicle*, float*, float*, float*, bool*);

	static float GetPositionAlongCurrentCurve(CVehicle* pVehicle)
	{
		uint32 timeInCurve = CTimer::GetTimeInMilliseconds() - pVehicle->AutoPilot.m_nTimeEnteredCurve;
		return (float)timeInCurve / pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
	}

	static float LimitRadianAngle(float angle)
	{
		while (angle < -PI)
			angle += TWOPI;
		while (angle > PI)
			angle -= TWOPI;
		return angle;
	}

	static bool bMadDriversCheat;
	static int32 NumLawEnforcerCars;
	static int32 NumAmbulancesOnDuty;
	static int32 NumFiretrucksOnDuty;
	static int32 NumRandomCars;
	static int32 NumMissionCars;
	static int32 NumParkedCars;
	static int32 NumPermanentCars;
	static bool bCarsGeneratedAroundCamera;
	static float CarDensityMultiplier;
	static int8 CountDownToCarsAtStart;
	static int32 MaxNumberOfCarsInUse;
	static uint32 LastTimeLawEnforcerCreated;
	static uint32 LastTimeFireTruckCreated;
	static uint32 LastTimeAmbulanceCreated;
	static int32 TotalNumOfCarsOfRating[TOTAL_CUSTOM_CLASSES];
	static int32 CarArrays[TOTAL_CUSTOM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];

	static int32 MiamiViceCycle;
	static uint32 LastTimeMiamiViceGenerated;
	static int32 NumRequestsOfCarRating[TOTAL_CUSTOM_CLASSES];
	static int32 NumOfLoadedCarsOfRating[TOTAL_CUSTOM_CLASSES];
	static int32 CarFreqArrays[TOTAL_CUSTOM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];
	static int32 LoadedCarsArray[TOTAL_CUSTOM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];

	/* The fill burst (VICE-126): the window of the traffic fill and the
	 * counters of the burst in flight. The counters are live for the
	 * measurement rig (utils/caps-audit/live-counters.py). The "total" pair
	 * accumulates since boot (the last Init or ReInit), so the boot burst
	 * stays readable after a later arming resets the window rows; the four
	 * "ended" counters count the burst runs by the reason they ended
	 * (VICE-146): the target's stop line, the full car budget, the give-up
	 * bound and the spent window. */
	static SFillBurst ms_FillBurst;
	static uint32 ms_nFillBurstAttempts;
	static uint32 ms_nFillBurstPlaced;
	static uint32 ms_nFillBurstTotalAttempts;
	static uint32 ms_nFillBurstTotalPlaced;
	static uint32 ms_nFillBurstEndedLine;
	static uint32 ms_nFillBurstEndedBudget;
	static uint32 ms_nFillBurstEndedStalls;
	static uint32 ms_nFillBurstEndedWindow;
	static float ms_fFillBurstTarget;

	/* The near bubble (VICE-127): the counters of the traffic's churn and the
	 * state of the frame's line-of-sight budget. The two catchable counters
	 * are the acceptance (VICE-146): a car placed or deleted in the frustum
	 * nearer than the band minimum - the catchable zone - is counted there,
	 * and both are zero while the bubble and the band hold. The band
	 * counters are the in-view band's events, at or beyond the minimum; the
	 * fade counters are the catchable events a fade covered (the fill
	 * burst's work); the hidden events subdivide by the arm (the occluded
	 * hint and the close blind spot, both confirmed by the ray); the other
	 * pair counts the cars created outside the traffic spawner, and their
	 * fill is the rest of the car target. */
	static uint32 ms_nNearBubbleCatchableCreations;
	static uint32 ms_nNearBubbleCatchableRemovals;
	static uint32 ms_nNearBubbleBandCreations;
	static uint32 ms_nNearBubbleBandRemovals;
	static uint32 ms_nNearBubbleFadeCreations;
	static uint32 ms_nNearBubbleFadeRemovals;
	static uint32 ms_nNearBubbleHiddenCreations;
	static uint32 ms_nNearBubbleHiddenRemovals;
	static uint32 ms_nNearBubbleOccluderCreations;
	static uint32 ms_nNearBubbleOccluderRemovals;
	static uint32 ms_nNearBubbleRayCreations;
	static uint32 ms_nNearBubbleRefused;
	static uint32 ms_nNearBubbleBypassed;
	static uint32 ms_nNearBubbleRays;
	static uint32 ms_nNearBubbleRaysTotal;
	static uint32 ms_nNearBubbleFill;
	static uint32 ms_nNearBubbleOtherFill;
	static uint32 ms_nNearBubbleOtherCreations;
	static uint32 ms_nNearBubbleOtherCreationsInView;
	static uint32 ms_nNearBubbleValveRemovals;
	static float ms_fNearBubbleTarget;
	static int32 ms_nNearBubbleRaysLeft;
};

extern CVehicle* apCarsToKeep[MAX_CARS_TO_KEEP];

#endif // __GTA_CARCTRL_H__
