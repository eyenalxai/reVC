#ifndef __GTA_SPAWNDIRECTOR_H__
#define __GTA_SPAWNDIRECTOR_H__

/*
 * The population director (VICE-152, the design's section 8): the traffic and
 * the crowd keep their global budgets, and the director spreads them over the
 * map so no area starves and none packs.
 *
 * The problem it answers is the owner's: a packed street, then a completely
 * empty road. The global target (CCarCtrl::GetRandomCarTarget, the one number
 * the density sliders and the ring set) is a *total*; the old trickle spent it
 * wherever the ring's dice fell, so the areas the dice missed stayed dry for as
 * long as the player was there. The director makes the total a *distribution*:
 * every street cell carries a target, the maintenance tick serves the deficits
 * by proximity and visibility, and a cell that stays dry past the fill bound is
 * a counted defect.
 *
 * The conservation rule
 * ---------------------
 *
 * The cells hold **weights** (the live zone density, the player's
 * m_fRoadDensity and the visibility of the cell), and the weights are
 * apportioned to the global targets by integer arithmetic whose sum is the
 * target itself: the cell targets always add up to CCarCtrl::
 * GetRandomCarTarget (the car track) and to the pedestrian count target
 * CPopulation stops at (the ped track). No sum exceeds the caps, because the
 * total the director apportions is the same capped number the gates use
 * (MAX_RANDOM_CARS behind GetRandomCarTarget, MAX_PEDS_IN_USE behind the ped
 * budget). The engine passes the targets in - it never recomputes them - so
 * this module cannot drift from the one range control (ADR 0009, ADR 0013,
 * ADR 0017).
 *
 * The grid
 * --------
 *
 * One cell is SPAWN_DIRECTOR_CELL_SIZE (128 m), the grid covers World.h's
 * 4000 x 4000 m from (-2400, -2000) as 32 x 32 cells, and the cell's presence
 * is the occupancy store's own 16 m counts summed over the 8 x 8 block
 * (SpawnOccupancy.h, VICE-149). A cell is *active* for a track when its centre
 * is inside that track's own radius: the car track uses the ring's despawn
 * (rings.carDespawn: 280 m stock, 560 m at the cap - the traffic the near ring
 * owns), the ped track the pedestrian removal ring (rings.pedRemove: 131 m
 * stock, 262 m at the cap - the crowd lives inside the ped ring, not over the
 * car band). The active set is at most SPAWN_DIRECTOR_MAX_ACTIVE cells; the
 * shipped worst case (the player at a cell centre, the 560 m despawn) is 61.
 *
 * The target math
 * ---------------
 *
 *	total_cars	= 30 x CarDensityMultiplier x CarNumberMultiplier x scale^2
 *			  (topped by MAX_RANDOM_CARS; 408 at the owner's sliders),
 *			  the engine's own GetRandomCarTarget
 *	target_cell	= total x weight_cell / sum(weights), clamped to the
 *			  cell's ceiling and rounded so the sum is exact
 *
 * The ceiling is the cell's **lane capacity**: the sum of the travelled-side
 * lanes of the sites the cell holds (the site index's own field), times
 * SPAWN_DIRECTOR_CARS_PER_LANE. It stops one dense zone weight from asking a
 * couple of country lanes for the traffic of an avenue; the shipped city's
 * ceilings never bind at the owner's sliders (a 128 m cell holds some twenty
 * directed connections of two to four lanes each, so the ceiling is in the
 * hundreds against a share of a few cars). The floor is the "no street goes
 * dry" line: SPAWN_DIRECTOR_CELL_FLOOR (1) for every cell that holds a street
 * (or a water lane, for the boat cells) and has a target to carry. The floor is
 * the consistency metric's threshold, not a second budget: the sum stays the
 * total.
 *
 * The apportionment is the largest-remainder method over the weights, with the
 * ceiling as a hard clamp and the remainder redistributed over the cells that
 * still have room, in bounded passes. A total below the sum of the ceilings
 * always lands exactly; a pathological graph whose lanes cannot hold the
 * budget leaves the sum short and the shortfall is counted (the breach row),
 * never hidden.
 *
 * The tracks
 * ----------
 *
 * The car track spreads the random-car target over the land cells and the
 * **water cells** (the boat sites' own cells) together: a boat is a random car,
 * so the water cells carry their own share of the same total, and a request the
 * director aims at a water cell asks the service for a site that may be water
 * (SSpawnCarRequest::allowBoats). The ped track spreads the pedestrian count
 * target (the on-foot budget plus the car crews, the line CPopulation::
 * Update compares its totals against) over the cells with streets. The ped
 * track's *requests* stay the population's own trickle until the pedestrian
 * requester migrates (the design's later ticket); the director owns the
 * targets, the deficits and the consistency metric of the crowd now, and the
 * occupancy store's ped counts are its current.
 *
 * The maintenance tick
 * --------------------
 *
 * SpawnDirectorBeginFrame runs once a frame, before the ambient calls: the
 * live ladder arm of every active cell centre (NearBubble.h) and the cell's
 * live counts from the occupancy store. SpawnDirectorNextRequest hands the
 * ambient requester the next cell to serve: the deficient cells (target above
 * the count and the requests already handed out), ordered by visibility first
 * (the arms a creation is possible under, the hidden ones ahead of the in-view
 * band, the refused cells never), then by proximity to the player. A frame with
 * no deficient cell left hands nothing out, so the trickle stops at the
 * targets; the rate itself is still the ring's (RANDOM_CAR_ATTEMPTS_PER_FRAME x
 * scale, ADR 0013), which bounds the fill.
 *
 * The cell's own frame asks the service for a site near the cell: the target of
 * the request is the cell centre and the ring is SPAWN_DIRECTOR_REQUEST_RING
 * (half a cell), so the band around the target covers the cell and its
 * neighbours. The creation still passes every service gate (the plan, the
 * occupancy, the visibility, the reachability) - the director only chooses
 * where the traffic is needed.
 *
 * The over-target cells release removals at their **far edge**:
 * SpawnDirectorReleaseAllowed answers the near cull's per-car question
 * (CarCtrl.cpp), true for an ambient car in a cell that holds more than its
 * target plus SPAWN_DIRECTOR_OVERTARGET_SLACK, in the half of the cell the
 * player is not in, while the cell's release clock is free. The cull's own gate
 * (CSpawnService::AllowsRemoval: hidden, past the grace, outside the bubble)
 * still decides the deletion, so nothing the player can see vanishes.
 *
 * The consistency metric
 * ----------------------
 *
 * Every active cell keeps the minimum of its count over the drive (the
 * per-cell minimum, reset with the world) and the clock of its stay below the
 * floor: a cell below its floor longer than SPAWN_DIRECTOR_FILL_BOUND_MS is a
 * defect, counted once per stay. The per-cell minimum is the before/after
 * number the design asks for (the dry cell of the old dice shows a minimum of
 * 0); the harness prints its distribution (minimum, median, maximum over the
 * cells) and the defect check is the acceptance. The bound is derived from the
 * frame's own rate, not guessed: the ring cap asks 6 cars a frame (30 fps, 180
 * a second), so the whole 408-car budget of the owner's sliders refills in
 * 2.3 s; the bound is four times that, and a cell starved longer than it is not
 * waiting for the rate.
 *
 * The modes
 * ---------
 *
 * The director has one mode per frame: MAP (the street grid), INTERIOR (the
 * player is in an interior: the car target is zero and no road planning runs -
 * the interior's roads do not exist in the graph - while the ped target is the
 * flat interior budget the population's own AreaPedBudget returns) and OFF_MAP
 * (the player is off the grid: no active set, no requests). The area tag is
 * the mode plus the cell's own street/water tags; the site index carries no
 * per-area field yet, so the mode gates the whole tick rather than a subset of
 * the sites.
 *
 * The far layer's ledger
 * ----------------------
 *
 * The far layer (VICE-128) is not a requester yet (that is VICE-190); its
 * creations and removals are accounted here, in the service's ledger, under
 * SPAWN_PURPOSE_FAR (SpawnService.h). The director itself counts nothing of
 * the far layer: the layer's own paths note the rows.
 *
 * The watchdog
 * ------------
 *
 * The slow-frame pedestrian gate of the frame (Game.cpp: a frame whose
 * streaming update overran its budget skips the ped trickle) stays where it
 * is: it is the director's own guard against filling the crowd on a frame that
 * cannot afford the work, and this ticket does not move or weaken it. The
 * director's own tick is arithmetic over the active cells (tens of them), and
 * the zone lookups of a refresh are bounded by the active set, so the director
 * adds no per-frame world query of its own.
 *
 * The module is engine-free: it reads the plain site records and the occupancy
 * store and never touches CPathFind, CTheZones or the camera. The engine side
 * (SpawnDirector.cpp) fills the live facts (the zone densities, the view, the
 * arms); the evidence program (utils/spawn-service/director.cpp) drives the
 * same functions over a synthetic city and a scripted drive.
 */

#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include "config.h"
#include "PopulationBudget.h"	/* RANDOM_CARS_PER_INI_UNIT, MAX_RANDOM_CARS */
#include "SpawnSites.h"		/* CSpawnSiteIndex, SSpawnSite */
#include "SpawnOccupancy.h"	/* SSpawnOccupancyStore */
#include "SpawnService.h"	/* SpawnVisibilityRank, SSpawnServiceTelemetry */
#include "NearBubble.h"		/* ENearBubbleArm, NearBubbleArmFor */

class CVehicle;
class CVector;

/* --- The grid ------------------------------------------------------------ */

/* 128 m: big enough that a cell holds a street corner and a share of the
 * target (a few cars at the owner's sliders), small enough that the empty-road
 * case is a cell and not a district. The grid is the world's own rectangle
 * (World.h: 4000 x 4000 m from (-2400, -2000)); 32 x 128 m = 4096 covers it
 * with the last cell partial, and the presence block of a cell is 8 x 8 of the
 * store's 16 m cells. */
#define SPAWN_DIRECTOR_CELL_SIZE	128.0f
#define SPAWN_DIRECTOR_GRID_MIN_X	(-2400.0f)	/* WORLD_MIN_X */
#define SPAWN_DIRECTOR_GRID_MIN_Y	(-2000.0f)	/* WORLD_MIN_Y */
#define SPAWN_DIRECTOR_CELLS_X		32
#define SPAWN_DIRECTOR_CELLS_Y		32
#define SPAWN_DIRECTOR_CELLS		(SPAWN_DIRECTOR_CELLS_X * SPAWN_DIRECTOR_CELLS_Y)

/* The presence block of one director cell, in the store's 16 m cells. */
#define SPAWN_DIRECTOR_CELL_BLOCK	8

/* --- The weights --------------------------------------------------------- */

/* The visibility term of a weight: the cells the player can see carry the
 * traffic the player judges the street by, so an in-view cell weighs half again
 * a hidden one. The live ladder arm decides it (the engine fills the flag from
 * the camera's own frustum and the near bubble); the term never gates a
 * creation - the ladder does that - it only leans the target toward what the
 * player sees. */
#define SPAWN_DIRECTOR_VISIBLE_WEIGHT	1.5f

/* The ceiling's unit: the cars one travelled lane of the cell's streets may be
 * asked to hold. Two is deliberately generous against the shipped graph (the
 * ceiling exists for the degenerate cell, not to ration the avenues). */
#define SPAWN_DIRECTOR_CARS_PER_LANE	2

/* The ped ceiling's unit: the crowd a lane's sidewalks may be asked to hold.
 * The crowd's own cap is the area's density (MAX_PEDS_IN_USE 4900 over the ped
 * ring's 215,000 m2 is 0.023 peds an m2, some 370 for a 128 m cell), and a cell
 * of the shipped grid holds a handful of lanes, so 32 a lane puts the ceiling
 * above the densest cell the ring can ask for and below a pathological graph's
 * one-lane cell; the ped budget's own cap (MAX_PEDS_IN_USE) stays the hard
 * top. */
#define SPAWN_DIRECTOR_PEDS_PER_LANE	32

/* --- The maintenance ----------------------------------------------------- */

/* The active set's bound: the worst shipped case is 61 cells (the player at a
 * cell centre, the 560 m despawn of the ring cap). The static assert below
 * pins the bound against the ring table's own cap. */
#define SPAWN_DIRECTOR_MAX_ACTIVE	96

/* The fill bound: the time a cell may stay below its floor. The ring cap asks
 * RANDOM_CAR_ATTEMPTS_PER_FRAME x RING_MAX_SCALE = 6 cars a frame (180 a
 * second), so the 408-car budget of the owner's sliders refills in about 2.3 s
 * at 30 fps; the bound is four times that. */
#define SPAWN_DIRECTOR_FILL_BOUND_MS	10000

/* The floor: the least a street cell should hold ("no street goes dry"). The
 * floor is the defect threshold of the consistency metric; the target sum stays
 * the global target. */
#define SPAWN_DIRECTOR_CELL_FLOOR	1

/* The refresh cadence: the weights and the apportionment follow the player's
 * cell, the mode and the sliders. A slider move lands within a second; a cell
 * crossing refreshes at once. */
#define SPAWN_DIRECTOR_REFRESH_MS	1000

/* The over-target release: a cell above its target by this slack may hand a car
 * at its far edge to the cull, at most once per cooldown. The slack keeps the
 * release from churning the traffic around the target's own jitter. */
#define SPAWN_DIRECTOR_OVERTARGET_SLACK	2
#define SPAWN_DIRECTOR_RELEASE_COOLDOWN_MS 2000

/* The ring of a directed request: one cell. The request's target sits one ring
 * behind the served cell along the player's travel (SpawnDirectorRequestTarget
 * below), so the cell's sites are at the ring's distance from the target and
 * the service's band around the target (max(dist + 70, dist x 1.7),
 * SpawnService.h: [58, 198] here) covers the cell's whole block. The lead is
 * not decoration: the primitive's speed test (CarCtrl.cpp) refuses a car that
 * drives away from the request's target *relative to the player's own speed*,
 * so the sites ahead of the target along the travel are exactly the sites the
 * test can accept while the player is faster than the traffic; the cell's
 * sites must all sit there. */
#define SPAWN_DIRECTOR_REQUEST_RING	128.0f
#define SPAWN_DIRECTOR_REQUEST_LEAD	SPAWN_DIRECTOR_REQUEST_RING

/* The two tracks. The car track carries the random cars and the boats (one
 * total); the ped track the crowd. */
enum ESpawnDirectorTrack
{
	SPAWN_DIRECTOR_TRACK_CAR = 0,
	SPAWN_DIRECTOR_TRACK_PED,
	SPAWN_DIRECTOR_TRACKS
};

/* The per-frame modes. */
enum ESpawnDirectorMode
{
	SPAWN_DIRECTOR_MODE_MAP = 0,
	SPAWN_DIRECTOR_MODE_INTERIOR,
	SPAWN_DIRECTOR_MODE_OFF_MAP
};

/* --- The grid arithmetic ------------------------------------------------- */

static inline bool
SpawnDirectorCellAt(float x, float y, int32_t *pCell)
{
	float fx = (x - SPAWN_DIRECTOR_GRID_MIN_X) / SPAWN_DIRECTOR_CELL_SIZE;
	float fy = (y - SPAWN_DIRECTOR_GRID_MIN_Y) / SPAWN_DIRECTOR_CELL_SIZE;

	if (!(fx >= 0.0f && fx < (float)SPAWN_DIRECTOR_CELLS_X &&
	      fy >= 0.0f && fy < (float)SPAWN_DIRECTOR_CELLS_Y))
		return false;
	*pCell = (int32_t)fy * SPAWN_DIRECTOR_CELLS_X + (int32_t)fx;
	return true;
}

static inline int32_t
SpawnDirectorCellOf(float x, float y)
{
	int32_t cell;

	if (!SpawnDirectorCellAt(x, y, &cell))
		return -1;
	return cell;
}

static inline float
SpawnDirectorCellCentreX(int32_t cell)
{
	return SPAWN_DIRECTOR_GRID_MIN_X + ((float)(cell % SPAWN_DIRECTOR_CELLS_X) + 0.5f) * SPAWN_DIRECTOR_CELL_SIZE;
}

static inline float
SpawnDirectorCellCentreY(int32_t cell)
{
	return SPAWN_DIRECTOR_GRID_MIN_Y + ((float)(cell / SPAWN_DIRECTOR_CELLS_X) + 0.5f) * SPAWN_DIRECTOR_CELL_SIZE;
}

/* The centre's distance from a point, squared. */
static inline float
SpawnDirectorCellDistSqr(int32_t cell, float x, float y)
{
	float dx = SpawnDirectorCellCentreX(cell) - x;
	float dy = SpawnDirectorCellCentreY(cell) - y;

	return dx * dx + dy * dy;
}

/* --- The cell facts ------------------------------------------------------ */

/*
 * What the director knows of one cell. The presence facts (a street, a water
 * lane, the lanes) are the site index's and never move after the build; the
 * live facts (the zone densities, the view) are resolved by the engine at each
 * refresh; the arm is the frame's own (the ladder).
 */
struct SSpawnDirectorCellFacts
{
	float zoneCarDensity;	/* the live info zone's car density at the centre */
	float zonePedDensity;	/* the live info zone's ped density at the centre */
	uint16_t lanesLand;	/* the travelled lanes of the cell's road sites */
	uint16_t lanesWater;	/* the travelled lanes of the cell's boat sites */
	uint8_t street;		/* the cell holds a road site */
	uint8_t water;		/* the cell holds a boat site */
	uint8_t inView;		/* the camera sees the cell (the weight's term) */
	uint8_t arm;		/* ENearBubbleArm of the centre (the live order) */
};

/* The weight of one cell for a track: the live zone density, the player's road
 * density, the view and the presence. The density floors at a small positive
 * value, so a zone that reports zero still carries its street (a cell whose
 * weight is zero would never be apportioned, and the street would go dry by
 * arithmetic). */
#define SPAWN_DIRECTOR_DENSITY_FLOOR	0.05f

static inline float
SpawnDirectorWeight(const SSpawnDirectorCellFacts &facts, float roadDensity, bool pedTrack)
{
	float density = pedTrack ? facts.zonePedDensity + facts.zoneCarDensity : facts.zoneCarDensity;
	float weight;

	if (pedTrack) {
		if (!facts.street)
			return 0.0f;
	} else {
		if (!facts.street && !facts.water)
			return 0.0f;
	}
	if (density < SPAWN_DIRECTOR_DENSITY_FLOOR)
		density = SPAWN_DIRECTOR_DENSITY_FLOOR;
	weight = density * roadDensity;
	if (facts.inView)
		weight *= SPAWN_DIRECTOR_VISIBLE_WEIGHT;
	return weight;
}

/* The ceiling of one cell for a track: the lane capacity. */
static inline uint16_t
SpawnDirectorCeiling(const SSpawnDirectorCellFacts &facts, bool pedTrack)
{
	int32_t lanes = facts.lanesLand;
	int32_t perLane = pedTrack ? SPAWN_DIRECTOR_PEDS_PER_LANE : SPAWN_DIRECTOR_CARS_PER_LANE;
	int32_t ceiling;

	if (!pedTrack)
		lanes += facts.lanesWater;
	if (lanes <= 0)
		return 0;
	ceiling = lanes * perLane;
	return ceiling > 0xFFFF ? 0xFFFF : (uint16_t)ceiling;
}

/* The floor of one cell for a track: one, where the cell carries the track and
 * a target exists. */
static inline uint8_t
SpawnDirectorFloor(const SSpawnDirectorCellFacts &facts, bool pedTrack, uint16_t target)
{
	if (target == 0)
		return 0;
	if (pedTrack)
		return facts.street ? SPAWN_DIRECTOR_CELL_FLOOR : 0;
	return (facts.street || facts.water) ? SPAWN_DIRECTOR_CELL_FLOOR : 0;
}

/* --- The site facts' collection ------------------------------------------ */

/*
 * The presence facts of every cell, from the site index (the build): a land
 * site marks the cell's street, a boat site its water lane, and each site's
 * travelled lanes add to its track's capacity. The sites outside the grid
 * (there are none on the shipped map) are dropped. The engine calls this once
 * per site build; the harness after its own build.
 */
static inline void
SpawnDirectorCollectSiteFacts(const CSpawnSiteIndex &index, SSpawnDirectorCellFacts *facts)
{
	int32_t i, numSites = index.GetNumSites();
	const SSpawnSite *sites = index.GetSites();

	for (i = 0; i < SPAWN_DIRECTOR_CELLS; i++) {
		facts[i].lanesLand = 0;
		facts[i].lanesWater = 0;
		facts[i].street = 0;
		facts[i].water = 0;
	}
	for (i = 0; i < numSites; i++) {
		const SSpawnSite &site = sites[i];
		int32_t cell = SpawnDirectorCellOf(site.x, site.y);
		if (cell < 0)
			continue;
		if (site.flags & SPAWN_SITE_BOAT) {
			facts[cell].water = 1;
			facts[cell].lanesWater += site.lanes;
		} else {
			facts[cell].street = 1;
			facts[cell].lanesLand += site.lanes;
		}
	}
}

/* --- The apportionment --------------------------------------------------- */

/*
 * The largest-remainder apportionment: the base shares (the integer part of
 * total x weight / sum(weights), clamped to the ceiling), then the remainder
 * one unit at a time to the cell with the largest remaining fraction among
 * those with room. The result's sum is `total` whenever the ceilings allow it;
 * a shortfall (the sum of the ceilings below the total) is returned by the
 * caller's own sum and counted as a breach. The weights and the ceilings are
 * the caller's arrays; `targets` is written. Returns the sum of the targets.
 */
static inline int32_t
SpawnDirectorApportion(const float *weights, const uint16_t *ceilings, int32_t count, int32_t total, uint16_t *targets)
{
	int32_t i, assigned = 0, remainder;
	float sum = 0.0f;

	for (i = 0; i < count; i++)
		targets[i] = 0;
	if (total <= 0 || count <= 0)
		return 0;
	for (i = 0; i < count; i++)
		if (weights[i] > 0.0f && ceilings[i] > 0)
			sum += weights[i];
	if (sum <= 0.0f)
		return 0;

	for (i = 0; i < count; i++) {
		float share;
		int32_t base;

		if (weights[i] <= 0.0f || ceilings[i] <= 0)
			continue;
		share = (float)total * weights[i] / sum;
		base = (int32_t)share;
		if (base > (int32_t)ceilings[i])
			base = (int32_t)ceilings[i];
		targets[i] = (uint16_t)base;
		assigned += base;
	}
	remainder = total - assigned;
	/* The remainder passes: each unit goes to the largest remaining fraction.
	 * A pass that finds no room stops the loop (the ceilings bind). */
	while (remainder > 0) {
		int32_t best = -1;
		float bestFraction = -1.0f;

		for (i = 0; i < count; i++) {
			float share, fraction;

			if (weights[i] <= 0.0f || targets[i] >= ceilings[i])
				continue;
			share = (float)total * weights[i] / sum;
			fraction = share - (float)targets[i];
			if (fraction > bestFraction) {
				bestFraction = fraction;
				best = i;
			}
		}
		if (best < 0)
			break;
		targets[best]++;
		remainder--;
	}
	assigned = 0;
	for (i = 0; i < count; i++)
		assigned += targets[i];
	return assigned;
}

/* --- The director -------------------------------------------------------- */

/* One track's state of one cell. */
struct SSpawnDirectorTrackState
{
	uint16_t target;	/* the apportioned share of the global target */
	uint16_t ceiling;	/* the lane capacity (the apportionment's clamp) */
	uint16_t current;	/* the count at the tick (the occupancy store) */
	uint16_t minCount;	/* the per-cell minimum since the reset (the metric) */
	uint8_t floor;		/* the least the cell should hold (0: no floor) */
	uint8_t asked;		/* the requests handed out since the tick */
	uint8_t minSeen;	/* minCount holds a reading */
	uint8_t belowNoted;	/* the stay below the floor is already a defect */
	uint8_t defectEvents;	/* the stays that crossed the fill bound (this cell) */
	uint32_t belowSince;	/* the ms the cell went below its floor (0: not below) */
};

/* One cell's state. */
struct SSpawnDirectorCellState
{
	uint8_t active;		/* in an active set of the last refresh (a track mask) */
	uint8_t street;
	uint8_t water;
	uint8_t arm;		/* the live ladder arm of the centre */
	uint32_t releaseUntil;	/* the over-target release clock (0: free) */
	SSpawnDirectorTrackState track[SPAWN_DIRECTOR_TRACKS];
};

/*
 * The director. One instance per world: the presence and live facts, the cell
 * state, the active sets and the apportionment's own numbers. Engine-free; the
 * engine's one static instance lives in SpawnDirector.cpp.
 */
struct SSpawnDirector
{
	SSpawnDirectorCellFacts aFacts[SPAWN_DIRECTOR_CELLS];
	SSpawnDirectorCellState aState[SPAWN_DIRECTOR_CELLS];
	int32_t aActive[SPAWN_DIRECTOR_TRACKS][SPAWN_DIRECTOR_MAX_ACTIVE];
	int32_t numActive[SPAWN_DIRECTOR_TRACKS];
	int32_t target[SPAWN_DIRECTOR_TRACKS];		/* the globals of the last refresh */
	int32_t targetSum[SPAWN_DIRECTOR_TRACKS];	/* the apportioned sums (the conservation) */
	int32_t currentSum[SPAWN_DIRECTOR_TRACKS];	/* the active cells' live counts (the tick) */
	uint32_t nDefects[SPAWN_DIRECTOR_TRACKS];	/* the stays that crossed the fill bound (since the reset) */
	uint32_t lastRefreshMs;
	int32_t lastPlayerCell;
	uint8_t lastMode;
	uint8_t mode;
};

static inline void
SpawnDirectorReset(SSpawnDirector *d)
{
	int32_t i, t;

	for (i = 0; i < SPAWN_DIRECTOR_CELLS; i++) {
		d->aFacts[i].zoneCarDensity = 0.0f;
		d->aFacts[i].zonePedDensity = 0.0f;
		d->aFacts[i].lanesLand = 0;
		d->aFacts[i].lanesWater = 0;
		d->aFacts[i].street = 0;
		d->aFacts[i].water = 0;
		d->aFacts[i].inView = 0;
		d->aFacts[i].arm = NEAR_BUBBLE_ARM_NONE;
		d->aState[i].active = 0;
		d->aState[i].street = 0;
		d->aState[i].water = 0;
		d->aState[i].arm = NEAR_BUBBLE_ARM_NONE;
		d->aState[i].releaseUntil = 0;
		for (t = 0; t < SPAWN_DIRECTOR_TRACKS; t++) {
			SSpawnDirectorTrackState &track = d->aState[i].track[t];

			track.target = 0;
			track.ceiling = 0;
			track.current = 0;
			track.minCount = 0;
			track.floor = 0;
			track.asked = 0;
			track.minSeen = 0;
			track.belowNoted = 0;
			track.defectEvents = 0;
			track.belowSince = 0;
		}
	}
	for (t = 0; t < SPAWN_DIRECTOR_TRACKS; t++) {
		d->numActive[t] = 0;
		d->target[t] = 0;
		d->targetSum[t] = 0;
		d->currentSum[t] = 0;
		d->nDefects[t] = 0;
	}
	d->lastRefreshMs = 0;
	d->lastPlayerCell = -1;
	d->lastMode = SPAWN_DIRECTOR_MODE_OFF_MAP;
	d->mode = SPAWN_DIRECTOR_MODE_OFF_MAP;
}

/* The store's count over one director cell: the 8 x 8 block of the presence
 * grid, clipped to the store's own 250 x 250 (the director's last row and
 * column reach past it). */
static inline int32_t
SpawnDirectorCountAtCell(const SSpawnOccupancyStore &store, ESpawnOccupancyClass type, int32_t cell)
{
	int32_t blockX = (cell % SPAWN_DIRECTOR_CELLS_X) * SPAWN_DIRECTOR_CELL_BLOCK;
	int32_t blockY = (cell / SPAWN_DIRECTOR_CELLS_X) * SPAWN_DIRECTOR_CELL_BLOCK;
	int32_t x, y, total = 0;

	for (y = 0; y < SPAWN_DIRECTOR_CELL_BLOCK; y++)
		for (x = 0; x < SPAWN_DIRECTOR_CELL_BLOCK; x++)
			total += store.CountAtCell(type, blockX + x, blockY + y);
	return total;
}

/* The live facts of one active cell, resolved by the engine at a refresh. The
 * callback writes the cell's densities and its view into the facts the
 * director passes it (the director's own array). */
typedef void (*SpawnDirectorLiveFactsFn)(void *user, int32_t cell, float centreX, float centreY,
	SSpawnDirectorCellFacts *facts);

/*
 * The refresh: the active sets, the weights, the ceilings and the
 * apportionment. The engine calls it when the player's cell, the mode or the
 * sliders moved (or the cadence passed); the callback fills the live facts of
 * the cells that entered the active set (the zone lookups stay bounded by the
 * set, not by the grid). The targets are the engine's own globals: the car
 * target (CCarCtrl::GetRandomCarTarget) and the ped count target
 * (CPopulation's own line). In the interior mode the car target is the
 * engine's zero and no car apportionment runs.
 */
static inline void
SpawnDirectorRefresh(SSpawnDirector *d, uint32_t nowMs, float playerX, float playerY,
	float roadDensity, float carRadius, float pedRadius,
	int32_t carTarget, int32_t pedTarget, uint8_t mode,
	SpawnDirectorLiveFactsFn liveFacts, void *liveUser)
{
	int32_t i, cell, t;
	int32_t playerCell = SpawnDirectorCellOf(playerX, playerY);

	d->mode = mode;
	d->lastRefreshMs = nowMs;
	d->lastPlayerCell = playerCell;

	for (t = 0; t < SPAWN_DIRECTOR_TRACKS; t++)
		d->numActive[t] = 0;

	if (playerCell < 0 || mode == SPAWN_DIRECTOR_MODE_OFF_MAP) {
		for (i = 0; i < SPAWN_DIRECTOR_CELLS; i++)
			d->aState[i].active = 0;
		d->target[SPAWN_DIRECTOR_TRACK_CAR] = 0;
		d->target[SPAWN_DIRECTOR_TRACK_PED] = 0;
		d->targetSum[SPAWN_DIRECTOR_TRACK_CAR] = 0;
		d->targetSum[SPAWN_DIRECTOR_TRACK_PED] = 0;
		return;
	}

	/* The active sets: the cell centres inside each track's radius. The cell
	 * state's mask is rebuilt here, so a cell that left the set keeps its
	 * metric and its clocks (the drive's memory) but stops being ticked. */
	for (i = 0; i < SPAWN_DIRECTOR_CELLS; i++)
		d->aState[i].active = 0;
	for (cell = 0; cell < SPAWN_DIRECTOR_CELLS; cell++) {
		float distSqr = SpawnDirectorCellDistSqr(cell, playerX, playerY);
		uint8_t mask = 0;

		if (distSqr <= carRadius * carRadius) {
			mask |= 1 << SPAWN_DIRECTOR_TRACK_CAR;
			if (d->numActive[SPAWN_DIRECTOR_TRACK_CAR] < SPAWN_DIRECTOR_MAX_ACTIVE)
				d->aActive[SPAWN_DIRECTOR_TRACK_CAR][d->numActive[SPAWN_DIRECTOR_TRACK_CAR]++] = cell;
		}
		if (distSqr <= pedRadius * pedRadius) {
			mask |= 1 << SPAWN_DIRECTOR_TRACK_PED;
			if (d->numActive[SPAWN_DIRECTOR_TRACK_PED] < SPAWN_DIRECTOR_MAX_ACTIVE)
				d->aActive[SPAWN_DIRECTOR_TRACK_PED][d->numActive[SPAWN_DIRECTOR_TRACK_PED]++] = cell;
		}
		d->aState[cell].active = mask;
		d->aState[cell].street = d->aFacts[cell].street;
		d->aState[cell].water = d->aFacts[cell].water;
	}

	/* The live facts of the active cells, then the apportionment per track. */
	for (i = 0; i < d->numActive[SPAWN_DIRECTOR_TRACK_CAR]; i++) {
		cell = d->aActive[SPAWN_DIRECTOR_TRACK_CAR][i];
		if (liveFacts)
			liveFacts(liveUser, cell, SpawnDirectorCellCentreX(cell), SpawnDirectorCellCentreY(cell), &d->aFacts[cell]);
	}

	d->target[SPAWN_DIRECTOR_TRACK_CAR] = mode == SPAWN_DIRECTOR_MODE_INTERIOR ? 0 : carTarget;
	d->target[SPAWN_DIRECTOR_TRACK_PED] = pedTarget;
	for (t = 0; t < SPAWN_DIRECTOR_TRACKS; t++) {
		float weights[SPAWN_DIRECTOR_MAX_ACTIVE];
		uint16_t ceilings[SPAWN_DIRECTOR_MAX_ACTIVE];
		uint16_t targets[SPAWN_DIRECTOR_MAX_ACTIVE];
		bool pedTrack = t == SPAWN_DIRECTOR_TRACK_PED;
		int32_t count = d->numActive[t];
		int32_t sum;

		for (i = 0; i < count; i++) {
			cell = d->aActive[t][i];
			weights[i] = SpawnDirectorWeight(d->aFacts[cell], roadDensity, pedTrack);
			ceilings[i] = SpawnDirectorCeiling(d->aFacts[cell], pedTrack);
			d->aState[cell].track[t].ceiling = ceilings[i];
		}
		sum = SpawnDirectorApportion(weights, ceilings, count, d->target[t], targets);
		for (i = 0; i < count; i++) {
			cell = d->aActive[t][i];
			d->aState[cell].track[t].target = targets[i];
			d->aState[cell].track[t].floor = SpawnDirectorFloor(d->aFacts[cell], pedTrack, targets[i]);
			d->aState[cell].track[t].asked = 0;
		}
		d->targetSum[t] = sum;
	}
}

/*
 * The tick: the live arms, the counts, the per-cell minimum and the below-floor
 * clocks. `arms` is indexed by cell id (the engine fills the active cells; a
 * cell with no entry reads the hidden soft arm, which is what an unknown view
 * means for the order). The counts come from the occupancy store; the tracks
 * the cell is not active in are left alone.
 */
static inline void
SpawnDirectorBeginFrame(SSpawnDirector *d, const SSpawnOccupancyStore &store, uint32_t nowMs, const uint8_t *arms)
{
	int32_t i, cell, t;

	for (i = 0; i < d->numActive[SPAWN_DIRECTOR_TRACK_CAR]; i++) {
		cell = d->aActive[SPAWN_DIRECTOR_TRACK_CAR][i];
		d->aState[cell].arm = arms ? arms[cell] : (uint8_t)NEAR_BUBBLE_ARM_SOFT;
	}
	for (t = 0; t < SPAWN_DIRECTOR_TRACKS; t++) {
		ESpawnOccupancyClass type = t == SPAWN_DIRECTOR_TRACK_CAR ? SPAWN_OCCUPANCY_VEHICLE : SPAWN_OCCUPANCY_PED;
		int32_t total = 0;

		for (i = 0; i < d->numActive[t]; i++) {
			SSpawnDirectorTrackState &track = d->aState[cell = d->aActive[t][i]].track[t];
			int32_t current = SpawnDirectorCountAtCell(store, type, cell);

			if (current > 0xFFFF)
				current = 0xFFFF;
			total += current;
			track.current = (uint16_t)current;
			track.asked = 0;
			if (!track.minSeen || (uint16_t)current < track.minCount) {
				track.minCount = (uint16_t)current;
				track.minSeen = 1;
			}
			if ((uint16_t)current < track.floor) {
				if (track.belowSince == 0)
					track.belowSince = nowMs ? nowMs : 1;
				else if (!track.belowNoted &&
					(int32_t)(nowMs - track.belowSince) > SPAWN_DIRECTOR_FILL_BOUND_MS) {
					/* The stay crossed the fill bound: the defect is
					 * counted once, and the cell keeps its clock until it
					 * recovers. */
					track.belowNoted = 1;
					if (track.defectEvents < 255)
						track.defectEvents++;
					d->nDefects[t]++;
				}
			} else {
				track.belowSince = 0;
				track.belowNoted = 0;
			}
		}
		d->currentSum[t] = total;
	}
}

/* The cells below their floor right now, per track (the live row). */
static inline int32_t
SpawnDirectorStarvedCells(const SSpawnDirector *d, int32_t track)
{
	int32_t i, cell, count = 0;

	for (i = 0; i < d->numActive[track]; i++) {
		cell = d->aActive[track][i];
		if (d->aState[cell].track[track].belowSince != 0)
			count++;
	}
	return count;
}

/* The defect events a track recorded: the stays below the floor that crossed
 * the fill bound since the last reset. */
static inline int32_t
SpawnDirectorDefects(const SSpawnDirector *d, int32_t track)
{
	return (int32_t)d->nDefects[track];
}

/* The worst per-cell minimum of a track's active cells (the drive's number). */
static inline int32_t
SpawnDirectorWorstMin(const SSpawnDirector *d, int32_t track)
{
	int32_t i, cell, worst = -1;

	for (i = 0; i < d->numActive[track]; i++) {
		cell = d->aActive[track][i];
		if (!d->aState[cell].track[track].minSeen)
			continue;
		if (worst < 0 || d->aState[cell].track[track].minCount < worst)
			worst = d->aState[cell].track[track].minCount;
	}
	return worst < 0 ? 0 : worst;
}

/* The target of a served cell's request: the cell's centre one lead behind
 * along the player's travel (dirX, dirY: a unit vector, or zero for a standing
 * player). The engine passes the player's own velocity direction, the harness
 * its scripted one; the returned target is the request's reachability target
 * and the centre of its site band. */
static inline void
SpawnDirectorRequestTarget(int32_t cell, float dirX, float dirY, float *pX, float *pY)
{
	*pX = SpawnDirectorCellCentreX(cell) - dirX * SPAWN_DIRECTOR_REQUEST_LEAD;
	*pY = SpawnDirectorCellCentreY(cell) - dirY * SPAWN_DIRECTOR_REQUEST_LEAD;
}

/*
 * The next cell the ambient requester should serve: the deficient cells of the
 * car track (the target above the count and the requests handed out), the
 * largest deficit fraction first (the fair share: a cell at nothing against a
 * small target outranks a cell only a little short of a large one, so the far
 * cells of the active set are not starved by the near ones the flow keeps
 * re-opening), then the nearest to the player, then the larger deficit. The
 * visibility is the gate, not the order: a cell whose centre the ladder
 * refuses (the bubble, the catchable zone) is never served - the service's own
 * plan ranks the sites inside a served cell by the same arms, hidden first,
 * so the visibility still decides where inside the cell the car may appear.
 * `dirX, dirY` is the player's travel direction (a unit vector, or zero for a
 * standing player): the handed-out target sits one lead behind the cell along
 * it (SpawnDirectorRequestTarget). False when no cell has a deficit the ladder
 * can serve. The handed-out cell's `asked` grows, so a frame of calls spreads
 * over the deficits instead of hammering one cell.
 */
static inline bool
SpawnDirectorNextRequest(SSpawnDirector *d, float playerX, float playerY, float dirX, float dirY, float *pX, float *pY, uint8_t *pWater)
{
	int32_t i, best = -1, bestDeficit = 0, bestTarget = 1;
	float bestDist = 0.0f;

	for (i = 0; i < d->numActive[SPAWN_DIRECTOR_TRACK_CAR]; i++) {
		int32_t cell = d->aActive[SPAWN_DIRECTOR_TRACK_CAR][i];
		SSpawnDirectorTrackState &track = d->aState[cell].track[SPAWN_DIRECTOR_TRACK_CAR];
		int32_t deficit, target;
		float dist;
		bool better = false;

		if (SpawnVisibilityRank(d->aState[cell].arm) >= 3)
			continue;	/* refused (the bubble, the catchable zone) */
		deficit = (int32_t)track.target - (int32_t)track.current - (int32_t)track.asked;
		if (deficit <= 0)
			continue;
		target = track.target > 0 ? (int32_t)track.target : 1;
		dist = SpawnDirectorCellDistSqr(cell, playerX, playerY);
		if (best < 0)
			better = true;
		else if ((int32_t)deficit * bestTarget != (int32_t)bestDeficit * target)
			better = (int32_t)deficit * bestTarget > (int32_t)bestDeficit * target;
		else if (dist != bestDist)
			better = dist < bestDist;
		else
			better = deficit > bestDeficit;
		if (better) {
			best = cell;
			bestDeficit = deficit;
			bestTarget = target;
			bestDist = dist;
		}
	}
	if (best < 0)
		return false;
	d->aState[best].track[SPAWN_DIRECTOR_TRACK_CAR].asked++;
	SpawnDirectorRequestTarget(best, dirX, dirY, pX, pY);
	if (pWater)
		*pWater = d->aState[best].water ? 1 : 0;
	return true;
}

/*
 * The over-target release: true when the cull may take this ambient car early.
 * The cell must hold more than its target - by the slack only while the area
 * is below its budget, and by any surplus once the area is at or above it,
 * because then the budget itself is the ceiling the deficits wait behind and a
 * cell that is merely one over its target must give the car up or the dry cell
 * stays dry (the flow's own bunches are within the slack, so a starved cell
 * waits for the cull alone otherwise) - the car must be in the half of the
 * cell away from the player (its far edge, so the release thins the back of
 * the cell the player is leaving), and the cell's release clock must be free.
 * The cull's own gate still decides the deletion (hidden, past the grace,
 * outside the bubble), so this only offers the car.
 */
static inline bool
SpawnDirectorReleaseAllowed(const SSpawnDirector *d, int32_t cell, float x, float y, float playerX, float playerY, uint32_t nowMs)
{
	const SSpawnDirectorTrackState *track;
	int32_t slack;
	float centreX, centreY;
	float carDist, centreDist;

	if (cell < 0 || cell >= SPAWN_DIRECTOR_CELLS)
		return false;
	if (!(d->aState[cell].active & (1 << SPAWN_DIRECTOR_TRACK_CAR)))
		return false;
	if ((int32_t)(nowMs - d->aState[cell].releaseUntil) < 0)
		return false;
	track = &d->aState[cell].track[SPAWN_DIRECTOR_TRACK_CAR];
	/* The area at or above its budget: the release balances the cells
	 * against each other, so any surplus gives way. Below it, the slack is
	 * the flow's own jitter and the creations have the room. */
	slack = d->currentSum[SPAWN_DIRECTOR_TRACK_CAR] >= d->targetSum[SPAWN_DIRECTOR_TRACK_CAR]
		? 0 : SPAWN_DIRECTOR_OVERTARGET_SLACK;
	if ((int32_t)track->current <= (int32_t)track->target + slack)
		return false;
	centreX = SpawnDirectorCellCentreX(cell);
	centreY = SpawnDirectorCellCentreY(cell);
	centreDist = (centreX - playerX) * (centreX - playerX) + (centreY - playerY) * (centreY - playerY);
	carDist = (x - playerX) * (x - playerX) + (y - playerY) * (y - playerY);
	return carDist > centreDist;
}

/* The release happened: the cell's clock restarts. */
static inline void
SpawnDirectorNoteReleaseAt(SSpawnDirector *d, int32_t cell, uint32_t nowMs)
{
	if (cell >= 0 && cell < SPAWN_DIRECTOR_CELLS)
		d->aState[cell].releaseUntil = nowMs + SPAWN_DIRECTOR_RELEASE_COOLDOWN_MS;
}

/* --- The telemetry ------------------------------------------------------- */

/*
 * The director's counters, one owner per counter: the engine's single static
 * instance (CSpawnDirector::ms_Telemetry, SpawnDirector.cpp) read live by
 * utils/caps-audit/live-counters.py at the offsets its own static asserts pin.
 * The conservation rows repeat the service's own pair for the two tracks (the
 * service's pair carries the car track; the ped track is this struct's own);
 * the consistency rows are the metric: the starved cells of the last tick, the
 * defects since the reset and the worst per-cell minimum of the drive. The
 * request rows count the cells handed to the ambient calls and the calls that
 * found none (the area at its targets). The release row counts the over-target
 * releases the cull took.
 */
struct SSpawnDirectorTelemetry
{
	uint32_t m_nRefreshes;
	uint32_t m_nActiveCarCells;
	uint32_t m_nActivePedCells;
	uint32_t m_nCarTargetSum;
	uint32_t m_nCarGlobalTarget;
	uint32_t m_nCarBreaches;
	uint32_t m_nPedTargetSum;
	uint32_t m_nPedGlobalTarget;
	uint32_t m_nPedBreaches;
	uint32_t m_nRequestsIssued;
	uint32_t m_nRequestsEmpty;
	uint32_t m_nReleases;
	uint32_t m_nCarStarvedCells;
	uint32_t m_nCarDefects;
	uint32_t m_nCarWorstMin;
	uint32_t m_nPedStarvedCells;
	uint32_t m_nPedDefects;
	uint32_t m_nPedWorstMin;
};

static inline void
SpawnDirectorTelemetryReset(SSpawnDirectorTelemetry *tel)
{
	uint8_t *p = (uint8_t *)tel;
	size_t i;

	for (i = 0; i < sizeof(*tel); i++)
		p[i] = 0;
}

/* One refresh: the conservation pair of a track (the apportioned sum against
 * the global target; a mismatch is a breach) and the active set's size. */
static inline void
SpawnDirectorNoteRefresh(SSpawnDirectorTelemetry *tel, const SSpawnDirector *d)
{
	tel->m_nRefreshes++;
	tel->m_nActiveCarCells = (uint32_t)d->numActive[SPAWN_DIRECTOR_TRACK_CAR];
	tel->m_nActivePedCells = (uint32_t)d->numActive[SPAWN_DIRECTOR_TRACK_PED];
	tel->m_nCarTargetSum = (uint32_t)d->targetSum[SPAWN_DIRECTOR_TRACK_CAR];
	tel->m_nCarGlobalTarget = (uint32_t)d->target[SPAWN_DIRECTOR_TRACK_CAR];
	tel->m_nPedTargetSum = (uint32_t)d->targetSum[SPAWN_DIRECTOR_TRACK_PED];
	tel->m_nPedGlobalTarget = (uint32_t)d->target[SPAWN_DIRECTOR_TRACK_PED];
	if ((int32_t)tel->m_nCarTargetSum != (int32_t)tel->m_nCarGlobalTarget)
		tel->m_nCarBreaches++;
	if ((int32_t)tel->m_nPedTargetSum != (int32_t)tel->m_nPedGlobalTarget)
		tel->m_nPedBreaches++;
}

/* One tick's consistency rows. */
static inline void
SpawnDirectorNoteTick(SSpawnDirectorTelemetry *tel, const SSpawnDirector *d)
{
	tel->m_nCarStarvedCells = (uint32_t)SpawnDirectorStarvedCells(d, SPAWN_DIRECTOR_TRACK_CAR);
	tel->m_nCarDefects = (uint32_t)SpawnDirectorDefects(d, SPAWN_DIRECTOR_TRACK_CAR);
	tel->m_nCarWorstMin = (uint32_t)SpawnDirectorWorstMin(d, SPAWN_DIRECTOR_TRACK_CAR);
	tel->m_nPedStarvedCells = (uint32_t)SpawnDirectorStarvedCells(d, SPAWN_DIRECTOR_TRACK_PED);
	tel->m_nPedDefects = (uint32_t)SpawnDirectorDefects(d, SPAWN_DIRECTOR_TRACK_PED);
	tel->m_nPedWorstMin = (uint32_t)SpawnDirectorWorstMin(d, SPAWN_DIRECTOR_TRACK_PED);
}

static inline void
SpawnDirectorNoteRequest(SSpawnDirectorTelemetry *tel, bool served)
{
	if (served)
		tel->m_nRequestsIssued++;
	else
		tel->m_nRequestsEmpty++;
}

static inline void
SpawnDirectorNoteRelease(SSpawnDirectorTelemetry *tel)
{
	tel->m_nReleases++;
}

/* --- The engine side ----------------------------------------------------- */

/*
 * The engine's director (SpawnDirector.cpp): the one instance and its
 * telemetry, the build of the presence facts from the site index, the frame's
 * tick (the refresh when due, the arms, the counts) and the two questions the
 * ambient path asks - the next cell to serve and the over-target release. The
 * director is engine-free below this line; every engine type (the camera, the
 * zones, the rings, the timer, the vehicles) stays in the .cpp.
 */
class CSpawnDirector
{
public:
	/* The presence facts, after CSpawnSites::Build (the sites never move). */
	static void Build(void);

	/* The frame's tick: the refresh when the player's cell, the mode or the
	 * cadence moved it, then the live arms and the counts. Called by
	 * CCarCtrl::GenerateRandomCars before the ambient calls. */
	static void BeginFrame(void);

	/* The next cell the ambient requester should serve (the cell centre and
	 * the water flag). False when no cell has a deficit the ladder can
	 * serve: the call makes no request and the trickle stops at the targets. */
	static bool NextCellTarget(float *pX, float *pY, uint8_t *pWater);

	/* The over-target release (the near cull's own question, CarCtrl.cpp):
	 * the pure query (a cell above its target, the car at its far edge) and
	 * the note the cull takes once its own gate passed and the removal
	 * starts. The query never changes the state, so a car the hidden gate
	 * refuses does not burn the cell's release clock. */
	static bool AllowsOverTargetRelease(CVehicle *pVehicle, const CVector &playerPos);
	static void NoteOverTargetRelease(CVehicle *pVehicle);

	/* The restart, the load and the replay: the state and the counters go. */
	static void ReInit(void);

	/* The counters' read (the debug print and the harness's checks). */
	static const SSpawnDirectorTelemetry &Telemetry(void) { return ms_Telemetry; }

	/* A debug build's read (the loading screen): the grid, the active sets
	 * and the metric. Compiled out of a release build. */
	static void PrintStats(void);

private:
	/* The live facts' resolution of one active cell (the refresh's callback):
	 * the info zone's densities at the centre, the view. The engine types
	 * (CTheZones, TheCamera) stay in the .cpp. */
	static void ResolveLiveFacts(void *user, int32_t cell, float centreX, float centreY, SSpawnDirectorCellFacts *facts);

	/* The counters' one home (the live reader's symbol) and the state's. */
	static SSpawnDirectorTelemetry ms_Telemetry;
	static SSpawnDirector ms_Director;
};

#endif // __GTA_SPAWNDIRECTOR_H__
