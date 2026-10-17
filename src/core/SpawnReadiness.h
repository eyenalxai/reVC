#ifndef __GTA_SPAWNREADINESS_H__
#define __GTA_SPAWNREADINESS_H__

/*
 * The spawn readiness and prewarm (VICE-151, the design's section 6).
 *
 * The spawn service must never wait for a model: before it plans a car it
 * reads whether the models its planner can choose are resident, and before a
 * transition it prewarms the sets the first planning frames will ask for.
 * This module is that memory: the per-model readiness rule, the whole-set
 * predicate, the prewarm plan (which sets are requested), the pins that keep
 * a prewarmed model from being evicted, and the counters of the whole.
 *
 * The readiness rule
 * ------------------
 *
 * A model is ready when its own stream, its texture dictionary and its
 * animation block are loaded:
 *
 *	HasModelLoaded(model) && HasTxdLoaded(txd) && HasAnimLoaded(anim)
 *
 * A "loaded" model can have an evicted animation block: the streamer's own
 * bookkeeping and its memory eviction (CStreaming::MakeSpaceFor ->
 * RemoveLeastUsedModel) move the txds and the animation blocks without
 * moving the model, so a check of HasModelLoaded alone can lie. A model
 * without an animation block has nothing to evict and its animation read is
 * true: the animation index is -1 when the model's .ide row names no
 * animation file (most pedestrians, most cars) or names one the CD image
 * does not hold (the index resolves against the blocks CStreaming::
 * LoadCdDirectory registered from the image's .ifp entries, so a name with
 * no file, e.g. the peds' "man", resolves to -1). A model without a texture
 * dictionary cannot be ready.
 *
 * The whole-set predicate iterates the arrays the service plans from, never
 * the request mask: the car class arrays (CCarCtrl::LoadedCarsArray, the
 * models a class chooses from) and the pedestrian group
 * (CPopulation::ms_pPedGroups[group].models). The mask
 * (CStreaming::ms_bIsPedFromPedGroupLoaded) is set when a model is
 * *requested*, not when it is loaded, so it cannot answer "is the group in
 * memory" - the group's own model list can. The gang sets are checked the
 * same way (CGangs' two models per gang and the gang's car). An empty set
 * (no stored models: no class row holds one, or every group slot is a -1
 * hole) is never ready: the service has nothing to choose from, so its
 * planner must refuse instead of reading a vacuous truth.
 *
 * The prewarm sets
 * ----------------
 *
 * The car class set is global: every model of the class arrays
 * (CCarCtrl::CarArrays, the shipped data's 56 rows), requested once per load
 * - at the boot, and again after a save load - and pinned for the session.
 * The pedestrian group and the gang sets are per zone: the zone's pedestrian
 * group (the 16 models of the group) and the gangs the zone's thresholds
 * select (the same masks CStreaming::StreamZoneModels loads), requested when
 * the zone changes and released when it changes again.
 *
 * Every model, txd and animation request of a prewarm carries
 * STREAMFLAGS_PRIORITY, so the set is read before the ordinary streaming
 * work. In a cover - the drawn fade, the boot and loading screen, the
 * death/arrest fade, the taxi shortcut, the save load - the prewarm then
 * uses the blocking CStreaming::LoadAllRequestedModels(false) path as
 * CStreaming::LoadScene does, and verifies readiness per model; outside a
 * cover the requests ride the ordinary per-frame streaming and the service
 * waits (the wait is the planner's own refusal, not a block here).
 *
 * The pins
 * --------
 *
 * A pin is the STREAMFLAGS_DONT_REMOVE bit on a prewarmed model, its txd and
 * its animation block: it keeps the streaming's eviction (RemoveLoadedVehicle,
 * RemoveLoadedZoneModel, RemoveLeastUsedModel) away from the set. The pin
 * ledger remembers which flags *this plan* set (the owned bits), so a release
 * never clears a flag another system holds, and the pump re-asserts a pin
 * whose flag was cleared under it.
 *
 * The budget. The pins the plan holds stay inside the vehicle tracker's own
 * slots (MAXVEHICLESLOADED 90; the model store VEHICLEMODELSIZE 110 holds
 * every vehicle model the pins touch): the vehicle pins stop at
 * SPAWN_READY_PIN_BUDGET (the tracker less the reserve of
 * SPAWN_READY_PIN_RESERVE slots). The reserve is the streaming's own room,
 * not the pins': the pins are never removable while held, so the slots the
 * rest of the streaming gets are exactly the reserve, and the reserve's 25
 * is the measured headroom of the non-pin side (the tracker's normal
 * occupancy is 57..65 of 90; the catalogue's 56 are the bulk of it). With
 * the shipped data the widest zone set is 61 vehicle pins, 4 under the
 * budget of 65, and the tracker keeps 29 slots beyond the pins. A pin
 * request that does not fit the budget is counted deferred and not made; the
 * set then reads not resident and the planner refuses, which is the
 * service's own rule. The evidence program prints the counts and the
 * headroom.
 *
 * The hosts
 * ---------
 *
 *	boot / loading screen	CGame::Initialise (the car set and the first
 *				zone set, blocking under the screen)
 *	interior entry		CGame::SetCurrArea: the street's zone set is
 *				released (the interior budget is flat and the
 *				models would hold ~2.6 MB through the stay)
 *	interior exit		CGame::SetCurrArea (the zone set again; the
 *				requests ride the ordinary streaming: the
 *				street appears behind walls, not behind a
 *				fade)
 *	death / arrest fade	CGameLogic::SortOutStreamingAndMemory (the
 *				blocking path, the screen is faded out)
 *	taxi shortcut		CGameLogic's shortcut transition (the blocking
 *				path, the screen is faded out)
 *	save load		GenericGameStorage's successful load (the plan
 *				is reset and rebuilt, blocking under the fade)
 *	drawn fade		CStreaming::Update's per-frame pump: a pending
 *				set whose screen is covered by the drawn fade
 *				(FillBurstIsHidden) takes the blocking path
 *	the pump		CStreaming::Update, once a frame, next to
 *				StreamZoneModels: the zone is re-read, a changed
 *				set is requested, the pins are asserted, the
 *				waited frames are counted. It is the plain
 *				restart's host: CGame::InitialiseWhenRestarting
 *				has no player yet (the script creates it on the
 *				first frames), so the guarded blocking call there
 *				could not prewarm; the pump's requests ride the
 *				restart screen and the fill bursts wait for them.
 *
 * No prewarm runs during a cutscene: the cutscene camera can see and the
 * biggest cutscenes delete the world, so the pump and every host return while
 * CCutsceneMgr::IsCutsceneProcessing(). The cutscene end stays the fill
 * burst's trigger; the fill waits out the streaming on its own.
 *
 * Where the spawn service calls this (the seam, now wired)
 * --------------------------------------------------------
 *
 * The service core (VICE-150) exists and its readiness gate is the real call
 * site: CSpawnService::RequestCar (SpawnService.cpp) reads
 * CSpawnReadiness::IsModelReady(request.carModel) before it plans, so a car
 * whose txd or animation block was evicted defers instead of being created
 * half-loaded. The prewarm's pins hold the class catalogue's own models, txds
 * and anim blocks, so the strict check costs the ambient traffic nothing (its
 * models come from CCarCtrl::ChooseModel's class arrays or the police sets,
 * whose anims are the pinned van block or none). A model outside the prewarm
 * whose anim block was evicted has no healer here yet (the pump heals the
 * pins, not arbitrary models): that is a follow-up decision if a requester
 * ever feeds such a model to the gate.
 *
 * The other reads the planner may take (its own next ticket):
 *
 *	CSpawnReadiness::AreCarClassesReady()	before a car placement (the
 *						whole class set; a per-class
 *						check is a walk of that class's
 *						row with IsModelReady)
 *	CSpawnReadiness::IsPedGroupReady(group)	before a pedestrian placement
 *						(group is the zone info's own)
 *	CSpawnReadiness::IsGangReady(gang)	before a gang placement
 *	CSpawnReadiness::IsModelReady(model)	the single-model form
 *
 * and never waits: a set that is not resident is a refusal (or a deferred
 * request), the service's own decision.
 *
 * No engine header beyond config.h is included: the evidence program in
 * utils/spawn-service drives the plan, the predicates and the budget on its
 * own; SpawnReadiness.cpp is the engine side (the streaming calls, the live
 * arrays, the counters).
 */

#include "config.h"	/* MAXVEHICLESLOADED, VEHICLEMODELSIZE, MODELINFOSIZE, NUMMODELSPERPEDGROUP */
#include <stdint.h>

/*
 * The pin budget: the vehicle pins stop SPAWN_READY_PIN_RESERVE slots below
 * the vehicle tracker's own capacity. The reserve is the streaming's own
 * headroom, taken from the live census: the tracker normally holds 57..65 of
 * its 90 slots (utils/caps-audit/live-counters.py, the recorded rows) and the
 * pins hold the class catalogue permanently, so the reserve is what the rest
 * of the streaming - the wanted sets, the gang cars, the mission cars, the
 * read batch - had beyond the catalogue. 25 leaves that side its measured
 * room; the shipped widest set (61 vehicle pins) stays 4 under the budget of
 * 65. A pin set that filled the tracker would make AddToLoadedVehiclesList
 * fail whenever it found no removable model, and the failing load re-requests
 * itself forever, which is what the reserve rules out.
 */
#define SPAWN_READY_PIN_RESERVE 25
#define SPAWN_READY_PIN_BUDGET (MAXVEHICLESLOADED - SPAWN_READY_PIN_RESERVE)

/* The pin array: the budget's vehicle pins plus the widest zone set (the 16
 * group models and the 9 gangs' 2 peds and 1 car each) with margin. The
 * shipped sets (the 56 class models and the widest zone) fit; a set that
 * does not is deferred, never dropped silently. */
#define SPAWN_READY_MAX_PINS 128

static_assert(SPAWN_READY_PIN_BUDGET > 0 && SPAWN_READY_PIN_BUDGET < MAXVEHICLESLOADED,
	"the pin budget leaves the tracker's reserve free");
static_assert(SPAWN_READY_MAX_PINS > SPAWN_READY_PIN_BUDGET,
	"the pin array holds the budget with the zone sets");

/* The owned bits: the STREAMFLAGS_DONT_REMOVE flags this plan set itself.
 * Only an owned flag is cleared on release, so a model another system holds
 * (a script, a mission, the wanted-level sets) stays held. */
enum {
	SPAWN_READY_OWN_MODEL = 1 << 0,
	SPAWN_READY_OWN_TXD   = 1 << 1,
	SPAWN_READY_OWN_ANIM  = 1 << 2,
};

/* The set bits: which prewarm set a pin belongs to. A model can be in both
 * (a gang car the class catalogue also holds), so the bits are a mask: the
 * zone release drops the zone bit and unpins only a pin with no set left. */
enum {
	SPAWN_READY_SET_CAR  = 1 << 0,
	SPAWN_READY_SET_ZONE = 1 << 1,
};

/* One pinned prewarm model: the model id and the ids of its texture
 * dictionary (the CTxdStore slot, no stream offset) and its animation block,
 * with -1 for "none". `owned` says which flags this plan set, `sets` which
 * prewarm sets hold the pin, `vehicle` whether the model takes a vehicle
 * tracker slot (the budget's kind). */
struct SSpawnReadyPin
{
	int32_t model;
	int32_t txd;
	int32_t anim;
	uint8_t owned;
	uint8_t sets;
	uint8_t vehicle;
};

/*
 * The plan: the sets in force, the pins, and the counters. The engine side
 * builds the sets (the live class arrays, the zone's group and gangs) and
 * makes the streaming calls; this struct owns the bookkeeping, so the
 * evidence program drives the same ledger and budget the game runs.
 *
 * The counters come first, at fixed offsets: utils/caps-audit/live-counters.py
 * reads the running game's CSpawnReadiness::ms_Plan at those offsets (the
 * READINESS table), and SpawnReadiness.cpp static-asserts them. The counters
 * are cumulative since boot and Reset() does not clear them: a save load
 * rebuilds the sets, not the evidence.
 */
struct SSpawnReadyPlan
{
	/* The counters (the live reader's offsets). */
	uint32_t m_nSetsResident;	/* +0:  blocking prewarm verifications that found every pin ready */
	uint32_t m_nRequestsDeferred;	/* +4:  pin requests the budget refused */
	uint32_t m_nFramesWaited;	/* +8:  frames a requested set was not resident */
	uint32_t m_nMissingModels;	/* +12: model checks that failed at a verification */
	int32_t m_nPins;		/* +16: the pins held right now */
	int32_t m_nVehiclePins;		/* +20: of them, the vehicle models (the tracker's slots) */
	uint32_t m_nLastStallMs;	/* +24: the last blocking prewarm's wall time */
	uint32_t m_nStallMsTotal;	/* +28: the blocking prewarms' wall time since boot */

	/* The sets. The car set is global: every model of the class arrays is
	 * pinned once per load. The zone set is the identity in force (the
	 * pedestrian group and the two gang masks) plus whether every model of
	 * it is pinned. */
	bool m_bCarSetRequested;
	int32_t m_nPedGroup;		/* -1: no zone set */
	uint16_t m_nZoneGangs;		/* the gang pedestrian mask */
	uint16_t m_nZoneGangCars;	/* the gang car mask */
	bool m_bZoneSetRequested;

	/* The pump's hint: a pin was requested (or a verification found one
	 * missing), so the next pump verifies before it decides. It keeps the
	 * steady state free of the readiness walk; a verification that finds
	 * every pin ready clears it. The live reader does not read it (it sits
	 * past the counter offsets). */
	bool m_bPinsNeedVerify;

	/* The pins. */
	SSpawnReadyPin m_aPins[SPAWN_READY_MAX_PINS];

	/* A fresh plan: the counters at zero (Reset keeps them afterwards) and
	 * the sets pending. */
	SSpawnReadyPlan(void)
	{
		m_nSetsResident = 0;
		m_nRequestsDeferred = 0;
		m_nFramesWaited = 0;
		m_nMissingModels = 0;
		m_nPins = 0;
		m_nVehiclePins = 0;
		m_nLastStallMs = 0;
		m_nStallMsTotal = 0;
		m_bPinsNeedVerify = false;
		Reset();
	}

	/* The state reset (a boot, a restart, a save load): the sets are
	 * forgotten and every pin is dropped. The caller releases the streaming
	 * flags first; the counters stay (see above). */
	void
	Reset(void)
	{
		m_bCarSetRequested = false;
		m_nPedGroup = -1;
		m_nZoneGangs = 0;
		m_nZoneGangCars = 0;
		m_bZoneSetRequested = false;
		m_bPinsNeedVerify = false;
		m_nPins = 0;
		m_nVehiclePins = 0;
	}

	bool CarSetPending(void) const { return !m_bCarSetRequested; }

	bool
	ZoneSetIs(int32_t pedGroup, uint16_t gangs, uint16_t gangCars) const
	{
		return m_nPedGroup == pedGroup && m_nZoneGangs == gangs && m_nZoneGangCars == gangCars;
	}

	bool
	ZoneSetPending(int32_t pedGroup, uint16_t gangs, uint16_t gangCars) const
	{
		return !m_bZoneSetRequested || !ZoneSetIs(pedGroup, gangs, gangCars);
	}

	/* A new zone set: the identity is in force and incomplete until every
	 * model of it is pinned. The caller releases the old zone's pins first. */
	void
	StartZoneSet(int32_t pedGroup, uint16_t gangs, uint16_t gangCars)
	{
		m_nPedGroup = pedGroup;
		m_nZoneGangs = gangs;
		m_nZoneGangCars = gangCars;
		m_bZoneSetRequested = false;
	}

	/* The zone set is dropped (an interior entry): the identity is
	 * forgotten, so the next RequestZone sees a changed set and pins it.
	 * The caller releases the pins first. */
	void
	EndZoneSet(void)
	{
		m_nPedGroup = -1;
		m_nZoneGangs = 0;
		m_nZoneGangCars = 0;
		m_bZoneSetRequested = false;
	}

	/* The budget: a pin fits when the array has room and, for a vehicle, the
	 * tracker keeps its reserve. */
	bool
	PinFits(bool vehicle) const
	{
		return m_nPins < SPAWN_READY_MAX_PINS &&
		       (!vehicle || m_nVehiclePins < SPAWN_READY_PIN_BUDGET);
	}

	int32_t
	FindPin(int32_t model) const
	{
		for (int32_t i = 0; i < m_nPins; i++)
			if (m_aPins[i].model == model)
				return i;
		return -1;
	}

	/* False when the array is full (the caller counts the request
	 * deferred; PinFits refuses first, so the false path is the guard, not
	 * the policy). */
	bool
	AddPin(const SSpawnReadyPin &pin, bool vehicle)
	{
		if (m_nPins >= SPAWN_READY_MAX_PINS)
			return false;
		m_aPins[m_nPins] = pin;
		m_aPins[m_nPins].vehicle = vehicle ? 1 : 0;
		m_nPins++;
		if (vehicle)
			m_nVehiclePins++;
		m_bPinsNeedVerify = true;
		return true;
	}

	/*
	 * Drop one set's pins (a zone change, an interior entry, a reset): a
	 * pin the other set also holds keeps the other bit; a pin with no set
	 * left leaves the ledger and is written to `released` for the caller to
	 * unpin - the streaming flags are the engine's, not this struct's.
	 * Returns the number written; `released` must hold every pin (the
	 * callers pass SPAWN_READY_MAX_PINS entries).
	 */
	int32_t
	ReleaseSet(uint8_t set, SSpawnReadyPin *released, int32_t maxReleased)
	{
		int32_t count = 0;

		for (int32_t i = 0; i < m_nPins; ) {
			SSpawnReadyPin &pin = m_aPins[i];
			if ((pin.sets & set) == 0) {
				i++;
				continue;
			}
			pin.sets &= ~set;
			if (pin.sets != 0) {
				i++;
				continue;
			}
			if (released && count < maxReleased)
				released[count++] = pin;
			RemovePinAt(i);
		}
		return count;
	}

	/* The entry is dropped; the last pin takes its place (the order is not
	 * read anywhere). */
	void
	RemovePinAt(int32_t index)
	{
		if (index < 0 || index >= m_nPins)
			return;
		if (m_aPins[index].vehicle)
			m_nVehiclePins--;
		m_nPins--;
		m_aPins[index] = m_aPins[m_nPins];
	}
};

class CVector;

/*
 * The engine side: the live predicates the spawn service reads, the hosts
 * and the pump. SpawnReadiness.cpp defines them; the class lives here so the
 * hosts and the evidence reader see one name.
 */
class CSpawnReadiness
{
public:
	/* The planner's reads (the seam the spawn service calls; see the header
	 * comment for the call sites). */
	static bool IsModelReady(int32_t model);
	static int32_t MissingCarModels(int32_t *firstMissing);
	static bool AreCarClassesReady(void);
	static int32_t MissingPedGroupModels(int32_t group, int32_t *firstMissing);
	static bool IsPedGroupReady(int32_t group);
	static bool IsGangReady(int32_t gang);

	/* The sets and the hosts. */
	static void Reset(void);
	static void PrewarmCarClasses(void);
	static void RequestZone(const CVector &pos);
	static void PrewarmInCover(const CVector &pos);
	static void OnInteriorEnter(void);
	static void OnInteriorExit(void);
	static void Update(void);

	/* The plan, the pins and the counters. Public for the live reader
	 * (utils/caps-audit/live-counters.py reads this object's counters at
	 * their fixed offsets) and for the evidence program's shape checks. */
	static SSpawnReadyPlan ms_Plan;
};

/*
 * The per-model readiness rule. The caller resolves the three reads (and the
 * -1 cases) from the engine's own state; the rule itself stays here, where
 * the evidence program can drive every combination.
 */
static inline bool
SpawnReadyModelIsReady(bool modelLoaded, int32_t txdSlot, bool txdLoaded,
                       int32_t animSlot, bool animLoaded)
{
	return modelLoaded && txdSlot >= 0 && txdLoaded && (animSlot < 0 || animLoaded);
}

/*
 * The whole-set predicate over one stored list: `models` holds the ids with
 * -1 holes (the pedestrian group's shape) and `ready` the readiness of each
 * stored model in parallel. Returns the number of stored models that are not
 * ready; the first missing id lands in *firstMissing (-1 when the set is
 * ready, and *firstMissing may be nil).
 */
static inline int32_t
SpawnReadyListMissing(const int32_t *models, const bool *ready, int32_t count, int32_t *firstMissing)
{
	int32_t missing = 0;

	if (firstMissing)
		*firstMissing = -1;
	for (int32_t i = 0; i < count; i++) {
		if (models[i] < 0)
			continue;
		if (ready[i])
			continue;
		if (firstMissing && *firstMissing == -1)
			*firstMissing = models[i];
		missing++;
	}
	return missing;
}

/*
 * The whole-set predicate over the car class arrays: `rows` is the packed
 * model rows (stride `stride`, one row per rating) and `counts` the per-rating
 * loaded counts (the shape of CCarCtrl::LoadedCarsArray and
 * NumOfLoadedCarsOfRating). The readiness of each model is read from
 * `readyByModel`, indexed by model id. Returns the missing total over every
 * row; the first missing id lands in *firstMissing.
 */
static inline int32_t
SpawnReadyClassArraysMissing(const int32_t *rows, int32_t stride, const int32_t *counts,
                             int32_t numClasses, const bool *readyByModel, int32_t *firstMissing)
{
	int32_t missing = 0;

	if (firstMissing)
		*firstMissing = -1;
	for (int32_t c = 0; c < numClasses; c++) {
		int32_t n = counts[c];
		if (n <= 0)
			continue;
		for (int32_t i = 0; i < n; i++) {
			int32_t model = rows[c * stride + i];
			if (model < 0 || readyByModel[model])
				continue;
			if (firstMissing && *firstMissing == -1)
				*firstMissing = model;
			missing++;
		}
	}
	return missing;
}

/* The stored models of one list (the -1 holes are not models) and of the car
 * class arrays: the whole-set verdict needs the count, so an empty set cannot
 * read ready. */
static inline int32_t
SpawnReadyListStored(const int32_t *models, int32_t count)
{
	int32_t stored = 0;

	for (int32_t i = 0; i < count; i++)
		if (models[i] >= 0)
			stored++;
	return stored;
}

static inline int32_t
SpawnReadyClassArraysStored(const int32_t *rows, int32_t stride, const int32_t *counts, int32_t numClasses)
{
	int32_t stored = 0;

	for (int32_t c = 0; c < numClasses; c++) {
		int32_t n = counts[c];
		for (int32_t i = 0; i < n; i++)
			if (rows[c * stride + i] >= 0)
				stored++;
	}
	return stored;
}

/*
 * The whole-set verdict: a set is ready when it holds at least one stored
 * model and none of the stored models is missing. An empty set is never
 * ready - the service has nothing to choose from, so its planner must refuse
 * instead of reading a vacuous truth.
 */
static inline bool
SpawnReadySetIsReady(int32_t stored, int32_t missing)
{
	return stored > 0 && missing == 0;
}

/*
 * The gang mask a zone's own thresholds select (the shipped rule of
 * CStreaming::StreamZoneModels, the mask's only reader; CCarCtrl::ChooseModel
 * reads the same thresholds per gang, not the mask). The mask's bit g is set
 * when the gang's threshold is the first that differs from the cops'
 * threshold, or differs from its predecessor's. Two overloads, because the
 * pedestrian thresholds are uint16 and the car thresholds int16. The rule
 * lives here, not in Zones, so the evidence program can drive it; CTheZones
 * wraps it as GetZoneGangPedMask/GetZoneGangCarMask.
 */
static inline uint16_t
SpawnReadyGangMask(const uint16_t *thresholds, uint16_t copThreshold, int32_t numGangs)
{
	uint16_t mask = 0;

	if (numGangs <= 0)
		return 0;
	if (thresholds[0] != copThreshold)
		mask = 1;
	for (int32_t i = 1; i < numGangs; i++)
		if (thresholds[i] != thresholds[i - 1])
			mask |= (uint16_t)(1 << i);
	return mask;
}

static inline uint16_t
SpawnReadyGangMask(const int16_t *thresholds, int16_t copThreshold, int32_t numGangs)
{
	uint16_t mask = 0;

	if (numGangs <= 0)
		return 0;
	if (thresholds[0] != copThreshold)
		mask = 1;
	for (int32_t i = 1; i < numGangs; i++)
		if (thresholds[i] != thresholds[i - 1])
			mask |= (uint16_t)(1 << i);
	return mask;
}

#endif // __GTA_SPAWNREADINESS_H__
