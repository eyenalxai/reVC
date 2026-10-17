#include "common.h"

#include "SpawnReadiness.h"

#include "CarCtrl.h"
#include "CutsceneMgr.h"
#include "FillBurst.h"
#include "Gangs.h"
#include "Game.h"
#include "ModelInfo.h"
#include "PlayerInfo.h"
#include "Population.h"
#include "Streaming.h"
#include "Timer.h"
#include "Zones.h"

#include <stddef.h>

/*
 * The engine side of the spawn readiness (VICE-151). SpawnReadiness.h owns
 * the rule, the plan and the counters; this file owns the live reads and the
 * streaming calls: the predicates over CStreaming's own state, the set
 * builders over CCarCtrl, CPopulation, CGangs and CTheZones, the pins as
 * STREAMFLAGS_DONT_REMOVE requests, and the pump of CStreaming::Update.
 *
 * The whole module is main-thread only, like the streaming it drives.
 */

SSpawnReadyPlan CSpawnReadiness::ms_Plan;

/* The counters' offsets are the live reader's contract (the READINESS table
 * of utils/caps-audit/live-counters.py). A field that moves fails the build
 * here, not the read of the running game. */
static_assert(offsetof(SSpawnReadyPlan, m_nSetsResident) == 0, "the live reader's offset of the sets-resident counter");
static_assert(offsetof(SSpawnReadyPlan, m_nRequestsDeferred) == 4, "the live reader's offset of the deferred counter");
static_assert(offsetof(SSpawnReadyPlan, m_nFramesWaited) == 8, "the live reader's offset of the frames-waited counter");
static_assert(offsetof(SSpawnReadyPlan, m_nMissingModels) == 12, "the live reader's offset of the missing counter");
static_assert(offsetof(SSpawnReadyPlan, m_nPins) == 16, "the live reader's offset of the pins counter");
static_assert(offsetof(SSpawnReadyPlan, m_nVehiclePins) == 20, "the live reader's offset of the vehicle-pins counter");
static_assert(offsetof(SSpawnReadyPlan, m_nLastStallMs) == 24, "the live reader's offset of the last-stall counter");
static_assert(offsetof(SSpawnReadyPlan, m_nStallMsTotal) == 28, "the live reader's offset of the stall-total counter");

/*
 * The per-model rule (the header's SpawnReadyModelIsReady) over the engine's
 * own state: the model's stream, its texture dictionary (the model info's
 * txd slot) and its animation block (the model info's animation index). A
 * model without an animation block needs none; a model without a txd slot
 * cannot be ready.
 */
bool
CSpawnReadiness::IsModelReady(int32 model)
{
	if (model < 0 || model >= MODELINFOSIZE)
		return false;
	CBaseModelInfo *mi = CModelInfo::GetModelInfo(model);
	if (mi == nil)
		return false;

	int32 txd = mi->GetTxdSlot();
	int32 anim = mi->GetAnimFileIndex();
	return SpawnReadyModelIsReady(
		CStreaming::HasModelLoaded(model),
		txd, txd < 0 ? false : CStreaming::HasTxdLoaded(txd),
		anim, anim < 0 ? true : CStreaming::HasAnimLoaded(anim));
}

/*
 * The whole car class set: every model the loaded class rows hold. The rows
 * are CCarCtrl::LoadedCarsArray, packed from each row's head with
 * NumOfLoadedCarsOfRating's count - the models the class chooses from
 * (CCarCtrl::ChooseCarModel), never the catalogue. The readiness of the
 * models is resolved once into a scratch indexed by model id; only the ids
 * the rows carry are written, so the scratch never needs clearing.
 */
int32
CSpawnReadiness::MissingCarModels(int32 *firstMissing)
{
	static bool readyByModel[MODELINFOSIZE];

	for (int32 c = 0; c < CCarCtrl::TOTAL_CUSTOM_CLASSES; c++) {
		int32 n = CCarCtrl::NumOfLoadedCarsOfRating[c];
		for (int32 i = 0; i < n; i++) {
			int32 model = CCarCtrl::LoadedCarsArray[c][i];
			readyByModel[model] = IsModelReady(model);
		}
	}
	return SpawnReadyClassArraysMissing(&CCarCtrl::LoadedCarsArray[0][0], MAX_CAR_MODELS_IN_ARRAY,
	                                    CCarCtrl::NumOfLoadedCarsOfRating, CCarCtrl::TOTAL_CUSTOM_CLASSES,
	                                    readyByModel, firstMissing);
}

bool
CSpawnReadiness::AreCarClassesReady(void)
{
	return MissingCarModels(nil) == 0;
}

/*
 * The whole pedestrian group: every model of CPopulation::ms_pPedGroups
 * (the -1 holes are skipped). The group's request mask
 * (CStreaming::ms_bIsPedFromPedGroupLoaded) is deliberately not read: it is
 * set when a model is requested, not when it is loaded.
 */
int32
CSpawnReadiness::MissingPedGroupModels(int32 group, int32 *firstMissing)
{
	if (group < 0 || group >= NUMPEDGROUPS) {
		if (firstMissing)
			*firstMissing = -1;
		return 0;
	}

	const int32 *models = CPopulation::ms_pPedGroups[group].models;
	bool ready[NUMMODELSPERPEDGROUP];
	for (int32 i = 0; i < NUMMODELSPERPEDGROUP; i++)
		ready[i] = models[i] >= 0 ? IsModelReady(models[i]) : false;
	return SpawnReadyListMissing(models, ready, NUMMODELSPERPEDGROUP, firstMissing);
}

bool
CSpawnReadiness::IsPedGroupReady(int32 group)
{
	return group >= 0 && group < NUMPEDGROUPS && MissingPedGroupModels(group, nil) == 0;
}

/*
 * One gang's set: its two pedestrian models and its car, where the data has
 * them (CGangs::Initialise leaves a model -1 when the gang has none).
 */
bool
CSpawnReadiness::IsGangReady(int32 gang)
{
	if (gang < 0 || gang >= NUM_GANGS)
		return false;

	int32 models[3];
	models[0] = CGangs::GetGangPedModel1(gang);
	models[1] = CGangs::GetGangPedModel2(gang);
	models[2] = CGangs::GetGangVehicleModel(gang);
	for (int32 i = 0; i < 3; i++)
		if (models[i] >= 0 && !IsModelReady(models[i]))
			return false;
	return true;
}

/*
 * The pins.
 *
 * PinModel makes one prewarm request: the model, its txd and its animation
 * block, every request carrying STREAMFLAGS_PRIORITY and the DONT_REMOVE pin
 * this plan owns. A model that already has a pin only gets the set bit (a
 * gang car the class set already holds is one pin, not two); a flag that is
 * not held any more - cleared under the pin by another system's
 * SetModelIsDeletable, or never ours to begin with - is taken and re-set, so
 * a pin heals itself and an evicted model is re-requested.
 *
 * Returns false when the budget refused the pin (counted deferred), so the
 * caller can leave its set incomplete and retry on the next pump.
 */
static void
AssertPin(SSpawnReadyPin &pin)
{
	if (CStreaming::CanRemoveModel(pin.model)) {
		pin.owned |= SPAWN_READY_OWN_MODEL;
		CStreaming::RequestModel(pin.model, STREAMFLAGS_PRIORITY | STREAMFLAGS_DONT_REMOVE);
	}
	if (pin.txd >= 0 && CStreaming::CanRemoveTxd(pin.txd)) {
		pin.owned |= SPAWN_READY_OWN_TXD;
		CStreaming::RequestTxd(pin.txd, STREAMFLAGS_PRIORITY | STREAMFLAGS_DONT_REMOVE);
	}
	if (pin.anim >= 0 && CStreaming::CanRemoveAnim(pin.anim)) {
		pin.owned |= SPAWN_READY_OWN_ANIM;
		CStreaming::RequestAnim(pin.anim, STREAMFLAGS_PRIORITY | STREAMFLAGS_DONT_REMOVE);
	}
}

static bool
PinModel(int32 model, uint8 setBit)
{
	SSpawnReadyPlan &plan = CSpawnReadiness::ms_Plan;

	int32 index = plan.FindPin(model);
	if (index >= 0) {
		SSpawnReadyPin &pin = plan.m_aPins[index];
		pin.sets |= setBit;
		AssertPin(pin);
		return true;
	}

	CBaseModelInfo *mi = CModelInfo::GetModelInfo(model);
	if (mi == nil)
		return false;

	bool vehicle = mi->GetModelType() == MITYPE_VEHICLE;
	if (!plan.PinFits(vehicle)) {
		plan.m_nRequestsDeferred++;
		return false;
	}

	SSpawnReadyPin pin;
	pin.model = model;
	pin.txd = mi->GetTxdSlot();
	pin.anim = mi->GetAnimFileIndex();
	pin.owned = 0;
	pin.sets = setBit;
	AssertPin(pin);

	plan.AddPin(pin, vehicle);
	return true;
}

/* Release one pin: the owned flags only, so a model another system holds
 * stays held. */
static void
ReleasePin(const SSpawnReadyPin &pin)
{
	if (pin.owned & SPAWN_READY_OWN_MODEL)
		CStreaming::SetModelIsDeletable(pin.model);
	if (pin.txd >= 0 && (pin.owned & SPAWN_READY_OWN_TXD))
		CStreaming::SetModelIsDeletable(pin.txd + STREAM_OFFSET_TXD);
	if (pin.anim >= 0 && (pin.owned & SPAWN_READY_OWN_ANIM))
		CStreaming::SetModelIsDeletable(pin.anim + STREAM_OFFSET_ANIM);
}

/*
 * Release the zone set's pins: the zone bit goes first; a pin the car set
 * also holds (or another zone set, impossible today) stays. The engine's
 * flags are cleared only for a pin with no set left.
 */
static void
ReleaseZonePins(void)
{
	SSpawnReadyPlan &plan = CSpawnReadiness::ms_Plan;

	for (int32 i = 0; i < plan.m_nPins; ) {
		SSpawnReadyPin &pin = plan.m_aPins[i];
		if ((pin.sets & SPAWN_READY_SET_ZONE) == 0) {
			i++;
			continue;
		}
		pin.sets &= ~SPAWN_READY_SET_ZONE;
		if (pin.sets != 0) {
			i++;
			continue;
		}
		ReleasePin(pin);
		plan.RemovePinAt(i);
	}
}

/* Re-assert every pin whose flag was cleared under it. The pump runs this
 * once a frame; the cost is three flag reads per pin. */
static void
AssertPins(void)
{
	SSpawnReadyPlan &plan = CSpawnReadiness::ms_Plan;

	for (int32 i = 0; i < plan.m_nPins; i++)
		AssertPin(plan.m_aPins[i]);
}

/* The readiness of every pin: the verification the prewarm counters read. */
static int32
CountMissingPins(void)
{
	int32 missing = 0;

	for (int32 i = 0; i < CSpawnReadiness::ms_Plan.m_nPins; i++)
		if (!CSpawnReadiness::IsModelReady(CSpawnReadiness::ms_Plan.m_aPins[i].model))
			missing++;
	return missing;
}

/*
 * The blocking prewarm: the path CStreaming::LoadScene uses. The requests
 * were made with priority; the call reads them all, then the pins are
 * verified per model. The wall time is measured with the cycle counter (the
 * same clock CStreaming's own debug timings use), so the live reader has the
 * real stall of the last cover.
 */
static void
LoadPinsBlocking(void)
{
	SSpawnReadyPlan &plan = CSpawnReadiness::ms_Plan;

	if (plan.m_nPins == 0)
		return;

	uint32 start = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();
	CStreaming::LoadAllRequestedModels(false);
	uint32 elapsed = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond() - start;

	plan.m_nLastStallMs = elapsed;
	plan.m_nStallMsTotal += elapsed;

	int32 missing = CountMissingPins();
	if (missing == 0)
		plan.m_nSetsResident++;
	else
		plan.m_nMissingModels += missing;
}

/*
 * The global car class set: every model of CCarCtrl::CarArrays (the shipped
 * data's class rows), pinned once per load. A model the budget defers leaves
 * the set pending, so the next pump retries it.
 */
void
CSpawnReadiness::PrewarmCarClasses(void)
{
	if (!ms_Plan.CarSetPending())
		return;

	bool complete = true;
	for (int32 c = 0; c < CCarCtrl::TOTAL_CUSTOM_CLASSES; c++)
		for (int32 i = 0; i < CCarCtrl::TotalNumOfCarsOfRating[c]; i++)
			if (!PinModel(CCarCtrl::CarArrays[c][i], SPAWN_READY_SET_CAR))
				complete = false;
	if (complete)
		ms_Plan.m_bCarSetRequested = true;
}

/*
 * The zone set: the zone's pedestrian group and the gangs the zone's
 * thresholds select (CTheZones' masks, the rule StreamZoneModels loads by).
 * A zone change releases the old zone's pins first; a gang car the class set
 * already holds is one pin with both set bits. The zone set is skipped
 * inside an interior (its budget is flat and the street's zone set is not
 * the room's); the interior exit requests the street's.
 */
void
CSpawnReadiness::RequestZone(const CVector &pos)
{
	if (CGame::IsInInterior())
		return;

	CZoneInfo info;
	CTheZones::GetZoneInfoForTimeOfDay(&pos, &info);
	uint16 gangs = CTheZones::GetZoneGangPedMask(info);
	uint16 gangCars = CTheZones::GetZoneGangCarMask(info);

	if (!ms_Plan.ZoneSetIs(info.pedGroup, gangs, gangCars)) {
		ReleaseZonePins();
		ms_Plan.StartZoneSet(info.pedGroup, gangs, gangCars);
	}
	if (ms_Plan.m_bZoneSetRequested)
		return;

	bool complete = true;
	if (info.pedGroup >= 0 && info.pedGroup < NUMPEDGROUPS)
		for (int32 i = 0; i < NUMMODELSPERPEDGROUP; i++) {
			int32 model = CPopulation::ms_pPedGroups[info.pedGroup].models[i];
			if (model >= 0 && !PinModel(model, SPAWN_READY_SET_ZONE))
				complete = false;
		}
	for (int32 g = 0; g < NUM_GANGS; g++) {
		if (gangs & (1 << g)) {
			int32 m1 = CGangs::GetGangPedModel1(g);
			int32 m2 = CGangs::GetGangPedModel2(g);
			if (m1 >= 0 && !PinModel(m1, SPAWN_READY_SET_ZONE))
				complete = false;
			if (m2 >= 0 && !PinModel(m2, SPAWN_READY_SET_ZONE))
				complete = false;
		}
		if (gangCars & (1 << g)) {
			int32 car = CGangs::GetGangVehicleModel(g);
			if (car >= 0 && !PinModel(car, SPAWN_READY_SET_ZONE))
				complete = false;
		}
	}
	if (complete)
		ms_Plan.m_bZoneSetRequested = true;
}

/*
 * The blocking prewarm of a cover (the boot and loading screen, the
 * death/arrest fade, the taxi shortcut, the save load): the sets are
 * requested, the request list is read with the blocking path, and the pins
 * are verified. The screen is covered, so the stall is invisible; the
 * counters carry it for the live reader. (The drawn fade's cover goes
 * through the pump's own blocking path, Update -> LoadPinsBlocking.)
 */
void
CSpawnReadiness::PrewarmInCover(const CVector &pos)
{
	if (CCutsceneMgr::IsCutsceneProcessing())
		return;

	AssertPins();
	PrewarmCarClasses();
	RequestZone(pos);
	LoadPinsBlocking();
}

/*
 * The interior exit: the street's zone set is requested and one pump starts
 * the reads. The street appears behind walls, not behind a fade, so this
 * host does not block; the fill burst's own gate waits for the models. The
 * next pump re-reads the position anyway, so a script that teleports the
 * player after the area switch is corrected within the frame.
 */
void
CSpawnReadiness::OnInteriorExit(void)
{
	if (CCutsceneMgr::IsCutsceneProcessing())
		return;
	if (FindPlayerPed() == nil)
		return;

	AssertPins();
	PrewarmCarClasses();
	RequestZone(FindPlayerCoors());
	CStreaming::LoadRequestedModels();
}

/*
 * The per-frame pump (CStreaming::Update, next to StreamZoneModels): the
 * zone follows the player, the pins are asserted, and a set that is still
 * not resident counts a waited frame. A waited set whose screen is covered
 * by the drawn fade (FillBurstIsHidden: the black frames of a transition)
 * takes the blocking path - the drawn fade is a cover, so the stall is
 * invisible there too.
 */
void
CSpawnReadiness::Update(void)
{
	if (CCutsceneMgr::IsCutsceneProcessing())
		return;

	AssertPins();
	PrewarmCarClasses();
	if (FindPlayerPed())
		RequestZone(FindPlayerCoors());

	if (ms_Plan.m_nPins == 0)
		return;
	if (CountMissingPins() != 0) {
		ms_Plan.m_nFramesWaited++;
		if (FillBurstIsHidden())
			LoadPinsBlocking();
	}
}

/*
 * The rebuild (a boot, a restart, a save load): every pin is released and
 * the sets are forgotten. The caller releases before CStreaming::ReInit
 * (the restart), so the streamer's own sweep sees the flags already gone.
 */
void
CSpawnReadiness::Reset(void)
{
	for (int32 i = 0; i < ms_Plan.m_nPins; i++)
		ReleasePin(ms_Plan.m_aPins[i]);
	ms_Plan.Reset();
}
