#ifndef __GTA_FARTRAFFIC_H__
#define __GTA_FARTRAFFIC_H__

/*
 * The far traffic layer (VICE-128).
 *
 * The owner's ask 9 of VICE-119: a long sight line must show traffic all the
 * way to the fog. The near traffic of the ring table lives inside its despawn
 * (280 m at stock, 560 m at the local cap) and the near bubble (VICE-127) keeps
 * its creations out of view, so on a straight boulevard the road is bare from
 * the ring's edge to the fog, which at the owner's draw distance is more than a
 * kilometre of empty tarmac.
 *
 * The layer
 * ---------
 *
 * A far car is a real vehicle of the pool in a far state: it is rendered by the
 * ordinary vehicle path (streamed model, LOD, shadows, the clump alpha reveal)
 * but it is not simulated. The state is one thing and it is applied by one
 * place:
 *
 *	VehicleCreatedBy = FAR_TRAFFIC_VEHICLE	the car is outside the car budget
 *						(it still holds a pool slot)
 *	static, away from the moving list	CWorld::Process never runs its
 *						control: no car AI, no physics,
 *						no wheel or damage work
 *	bUsesCollision = false			the collision path skips it both
 *						ways (ProcessCollisionSectorList
 *						tests the flag): the player cannot
 *						touch it and the near AI cannot
 *						even brake for it
 *	the audio entity is inactive		ProcessEntity skips it (the engine
 *						sound of a car two kilometres
 *						away)
 *	no driver ped				the car holds no ped of the pool
 *
 * The far layer moves the car itself: along the path nodes with the same curve
 * arithmetic the on-rails simple car uses (the shared core of
 * CCarCtrl::UpdateCarOnRails), on a stagger, and the alpha reveal of a creation
 * (VICE-127) is stepped by the same update. All of it is arithmetic on the
 * car's own matrix: the layer touches no collision query, no sector scan, no
 * path search, no physics.
 *
 * The band
 * --------
 *
 * The layer fills the annulus between the near traffic and the fog of this
 * frame, from the one ring table (TrafficRings.h):
 *
 *	inner = rings.carDespawn	where the near traffic ends: the cull
 *					removes past it, so no near car is
 *					created there
 *	outer = rings.visibleRange	min(LOD distance, weather far clip): the
 *					fade and the fog hide the world there
 *
 * The band is active only while it has width (FAR_TRAFFIC_MIN_BAND, 160 m: a
 * couple of the ring's own reserve steps). At the stock draw distance the ring's
 * despawn (280 m) sits 80 m inside the visible range (360 m) and the near
 * traffic already ends in the fog; the far layer has nothing to add there and it
 * stays off, which is why the layer appears with the draw distance and the
 * owner's long sight lines. When the weather closes the band (a storm's far clip
 * below the despawn) the whole layer goes, and the fog hides every removal.
 *
 * The count
 * ---------
 *
 * The cap follows the band's area, the way the traffic target follows the
 * ring's (ADR 0009): FAR_TRAFFIC_MAX (64) over FAR_TRAFFIC_REF_AREA (4 km2, the
 * band of a two kilometre draw distance) mean the layer holds about sixty cars
 * at the owner's settings and eight at the first band that opens. The cap is a
 * constant of the module and the pool margin is its check:
 *
 *	MAX_CARS_IN_USE (2400) + FAR_TRAFFIC_MAX (64) <= NUMVEHICLES (2560)
 *
 * so the layer can never take the slot of a near car, and the cars outside the
 * budget (the planes, the trains, the helicopter, the set pieces, the shortcut
 * car) keep the rest of the margin. A creation additionally refuses when the
 * pool is down to FAR_TRAFFIC_POOL_RESERVE free slots or the ped pool to
 * PED_POOL_RESERVE (the driver of a promotion is a ped). There is no assert
 * anywhere in the layer: a refused creation is a counter, not a crash.
 *
 * The events, and where they may happen
 * -------------------------------------
 *
 * A creation and a removal are the events the player can catch, so they go
 * through the near bubble's rule and through nothing else
 * (CCarCtrl::NearBubbleAllowsCreation / NearBubbleAllowsRemoval): a far car is
 * created where the player cannot see it appear (beyond the fade, behind the
 * camera by the margin, occluded, under a fade) and removed where the player
 * cannot see it go. The layer's own sites are the two the bubble leaves on an
 * open road, and it asks no line-of-sight ray for them:
 *
 *	the fog site	beyond the visible range: a creation at the fade is a car
 *			forming out of the haze, a removal there is a car the fog
 *			already hides. This is the oncoming traffic of a long
 *			straight road. The layer places it at the fade plus
 *			FAR_TRAFFIC_FADE_MARGIN so the placement's own length
 *			lands inside the fog rather than at its edge.
 *	the behind site	behind the camera (the rear half): the traffic the
 *			player has already passed. This is the same-direction
 *			traffic that keeps flowing after the player stops. The
 *			rear half is off the frustum by the bubble's margin for
 *			every FOV the game has, so the soft arm is the site's own.
 *
 * The ray is refused (rayAllowed false) for the far layer: a site two
 * kilometres out spends a line-of-sight ray of that length, and the frame's
 * four rays are the near traffic's blind spots. The price is exact and it is
 * the conservative direction: an occluded far site is not created at all.
 * Removals ask the shared gate with its ray (the near cull's own behaviour);
 * the layer makes at most FAR_TRAFFIC_REMOVE_ATTEMPTS of them a frame, so even
 * the ray is bounded.
 *
 * The switches
 * ------------
 *
 * A car of the layer that the player comes near is promoted to a full traffic
 * car (the driver, the collision, the AI, the budget), and a traffic car that
 * leaves the near band is demoted into the layer instead of being deleted. Both
 * switches are one function of the car's state and nothing else, because both
 * of them are invisible:
 *
 *	the matrix, the model, the colours, the status (SIMPLE), the clump and its
 *	alpha are the same on both sides of a switch, so no frame can show it;
 *	the promotions of a frame are checked for every far car (not only the
 *	staggered share), so a car sits inside the band for one frame at most;
 *	the demotion happens where the near cull already decided the car is out of
 *	the bubble and hidden, so it is a blind-spot event by the near bubble's own
 *	rule - a car the player watched drive away does not blink out, it changes
 *	which budget counts it;
 *	the promotion happens at the band's inner edge, at least RING_CAR_DESPAWN
 *	(280 m) from the player, and a far car is never left inside the near
 *	bubble: the player cannot reach a car that is not a full vehicle (the
 *	harness checks the rule).
 *
 * The promotion refuses when the ped pool is short of PED_POOL_RESERVE or the
 * driver cannot be created; the car stays far one more frame and the refusal is
 * counted. The demotion refuses when the layer is off or its cap is full, and
 * the near cull deletes the car as it always did.
 *
 * Missions and the scripts
 * ------------------------
 *
 * The layer touches no mission and no script car: it creates its own cars
 * (FAR_TRAFFIC_VEHICLE) from the loaded class arrays, and it demotes only the
 * traffic spawner's own cars (RANDOM_VEHICLE and bIsTrafficCar) the cull is
 * about to delete anyway. A script car, a set piece, a parked car and the
 * player's car are never adopted and never promoted: their cull is unchanged.
 *
 * The counters (utils/caps-audit/live-counters.py)
 * ------------------------------------------------
 *
 *	ms_nFarTrafficCars		the live count of the layer
 *	ms_nFarTrafficCap		the cap of this frame, from the band
 *	ms_nFarTrafficCreations		creations since the counters were zeroed
 *	ms_nFarTrafficRemovals		removals
 *	ms_nFarTrafficPromotions	the switches to a full traffic car
 *	ms_nFarTrafficDemotions		the switches back (the cull's hand-overs)
 *	ms_nFarTrafficRefused		the creation attempts the rule stopped
 *	ms_nFarTrafficRefusedModel	a refusal of a model that is not streamed
 *	ms_nFarTrafficRefusedPool	the pool reserves of a creation or a driver
 *	ms_nFarTrafficAttempts		the creation attempts
 *	ms_nFarTrafficUpdates		the car updates of the stagger (the cost)
 *
 * The first four are the ticket's counters (the cap holds, the layer's churn);
 * the refusals split by cause so a live reading says why a road stays bare: an
 * absent model (the streaming) is not the rule refusing.
 *
 * No engine header is included here. The evidence program in utils/far-traffic
 * reads the band, the cap, the switches and the counters from this header
 * alone; the engine calls (the rings, the paths, the pool, the streaming) live
 * in FarTraffic.cpp.
 */

#include "PopulationBudget.h"
#include "TrafficRings.h"
#include "NearBubble.h"

class CVehicle;
class CVector;

/* The band. The inner edge is the near traffic's own despawn; a band narrower
 * than this is not a long road, it is the near ring's own fog. */
#define FAR_TRAFFIC_MIN_BAND		160.0f

/* The cap. FAR_TRAFFIC_MAX is the hard top (the pool margin above);
 * FAR_TRAFFIC_REF_AREA is the band area it describes (a two kilometre disc), so
 * the count per square metre of drawn world is about constant across the draw
 * distance slider. FAR_TRAFFIC_MIN_CARS is the floor of an open band: the first
 * band that opens is a few hundred metres wide, and a handful of cars is the
 * difference between a bare road and a road. */
#define FAR_TRAFFIC_MAX			64
#define FAR_TRAFFIC_REF_AREA		4000000.0f	/* 2 km x 2 km */
#define FAR_TRAFFIC_MIN_CARS		8

/* The creation sites. A site is placed this far beyond the fade (the bubble's
 * fade arm needs the camera distance at or past the visible range; the margin
 * also covers the length of the path segment the placement brackets) and the
 * outer hysteresis keeps a car created just past the fog from being removed on
 * its way out of the band. */
#define FAR_TRAFFIC_FADE_MARGIN		50.0f
#define FAR_TRAFFIC_OUTER_HYSTERESIS	100.0f

/* The demotion (the cull's hand-over) starts this far past the despawn, so a
 * car that sits exactly on the near band's edge is not handed over the frame it
 * is culled: the layer only takes a car that is really leaving. */
#define FAR_TRAFFIC_ADOPT_HYSTERESIS	40.0f

/* The stagger: the share of the layer moved by one frame. Three frames at the
 * game's 30 Hz is the 10 Hz of the ticket; the update is arithmetic on the
 * car's own matrix, so a third of sixty cars is nothing to a frame. */
#define FAR_TRAFFIC_STAGGER		3

/* The work a frame may spend on the layer's events: two creation attempts (a
 * placement from the path scan) and two removal attempts (the shared removal
 * gate, its ray included). */
#define FAR_TRAFFIC_CREATE_ATTEMPTS	2
#define FAR_TRAFFIC_REMOVE_ATTEMPTS	2

/* The free pool slots the layer leaves to everyone else. The car budget is
 * already clear of the layer (the static assert below); the reserve keeps the
 * near frame's own creations (six cars and their crews, PopulationBudget.h) and
 * the cars outside the budget alive while the layer fills. A promotion spends
 * its car's driver (FAR_TRAFFIC_PROMOTION_PEDS ped slots), the same reserve the
 * near spawner reads. */
#define FAR_TRAFFIC_POOL_RESERVE	16
#define FAR_TRAFFIC_PROMOTION_PEDS	2

/* The band of one frame. `known` is set once the time cycle has a far clip (a
 * visible range of zero is the boot path); `active` is the whole band: an
 * inactive band holds no car and the layer removes what it has. */
struct SFarTrafficBand
{
	float inner;		/* rings.carDespawn: the near traffic ends here */
	float outer;		/* rings.visibleRange: the fog hides the world here */
	float createOuter;	/* outer + FAR_TRAFFIC_FADE_MARGIN: the creation sites */
	float removeOuter;	/* createOuter + FAR_TRAFFIC_OUTER_HYSTERESIS */
	bool known;
	bool active;
};

static inline SFarTrafficBand
ComputeFarTrafficBand(const STrafficRings &rings)
{
	SFarTrafficBand band;

	band.inner = rings.carDespawn;
	band.outer = rings.visibleRange;
	band.createOuter = band.outer + FAR_TRAFFIC_FADE_MARGIN;
	band.removeOuter = band.createOuter + FAR_TRAFFIC_OUTER_HYSTERESIS;
	/* A visible range of zero is the boot path (the time cycle is not known
	 * yet, TrafficRings.h): the band is not known either, so the layer keeps
	 * what it has and waits instead of treating the whole map as fog. */
	band.known = band.outer > 0.0f;
	band.active = band.known && band.outer >= band.inner + FAR_TRAFFIC_MIN_BAND;
	return band;
}

/* The cap of the band: the area share of the reference band, floored for an
 * open band and topped by the constant the pool margin holds. */
static inline int
FarTrafficCap(const SFarTrafficBand &band)
{
	if (!band.active)
		return 0;
	float area = band.outer * band.outer - band.inner * band.inner;
	int cap = (int)(FAR_TRAFFIC_MAX * area / FAR_TRAFFIC_REF_AREA);
	if (cap < FAR_TRAFFIC_MIN_CARS)
		cap = FAR_TRAFFIC_MIN_CARS;
	if (cap > FAR_TRAFFIC_MAX)
		cap = FAR_TRAFFIC_MAX;
	return cap;
}

/* A creation site: inside the band, at or past its inner edge, and inside the
 * fog margin. The visibility of the site is the near bubble's answer, asked by
 * the caller; this is only the geometry of the band. */
static inline bool
FarTrafficCreateSiteIsInBand(const SFarTrafficBand &band, float dist)
{
	return band.active && dist >= band.inner && dist <= band.createOuter;
}

/* The demotion site: the near cull is about to remove a traffic car here. The
 * car is past the despawn by the hysteresis (it is really leaving) and inside
 * the fog margin (the layer does not adopt what it would remove at once). */
static inline bool
FarTrafficAdoptSiteIsInBand(const SFarTrafficBand &band, float dist)
{
	return band.active && dist > band.inner + FAR_TRAFFIC_ADOPT_HYSTERESIS && dist <= band.removeOuter;
}

/* The switch of one far car, decided every frame for every car of the layer.
 * `canPromote` is the pool's answer of the frame (the driver's ped slot and the
 * reserves); a car that cannot be promoted stays far while the player cannot
 * reach it and is given up inside the reach, so the layer never leaves a car
 * the player could drive through. */
enum EFarTrafficSwitch
{
	FAR_TRAFFIC_KEEP = 0,	/* nothing: the car stays far */
	FAR_TRAFFIC_PROMOTE,	/* it reached the near band: it becomes traffic */
	FAR_TRAFFIC_REMOVE,	/* the band is gone or it is beyond the fog: it goes */
};

static inline EFarTrafficSwitch
FarTrafficSwitchFor(const SFarTrafficBand &band, float dist, bool canPromote)
{
	if (!band.known)
		/* The boot path: the band is not known yet, the layer keeps what it
		 * has and creates nothing (the cap is zero). */
		return FAR_TRAFFIC_KEEP;
	if (!band.active)
		return FAR_TRAFFIC_REMOVE;
	/* The inner edge is the near traffic's despawn, which sits outside the
	 * near bubble and its reserve by the ring table's own numbers (the
	 * static assert below). A car that reached it is promoted in the same
	 * frame the gate pass sees it, so the layer never leaves a car inside
	 * the player's reach. */
	if (dist <= band.inner) {
		if (canPromote)
			return FAR_TRAFFIC_PROMOTE;
		/* The promotion refused (the pool's reserves): the car stays far
		 * only while the player cannot reach it. */
		return dist <= NEAR_BUBBLE_REMOVE_DISTANCE ? FAR_TRAFFIC_REMOVE : FAR_TRAFFIC_KEEP;
	}
	if (dist > band.removeOuter)
		return FAR_TRAFFIC_REMOVE;
	return FAR_TRAFFIC_KEEP;
}

/* The share of one frame: the entries whose index falls on the frame's step. */
static inline bool
FarTrafficUpdatesThisFrame(unsigned int frameCounter, int index)
{
	return ((int)(frameCounter % FAR_TRAFFIC_STAGGER)) == (index % FAR_TRAFFIC_STAGGER);
}

/* The promotion distance is the near traffic's own despawn (RING_CAR_DESPAWN at
 * the lowest FOV factor; TrafficRings.h never takes the scale below it), which
 * sits well outside the near bubble and its reserve: a far car is never left
 * where the player could touch it, whatever the sliders say. */
static_assert(RING_CAR_DESPAWN > NEAR_BUBBLE_REMOVE_DISTANCE,
	"the far band must start outside the near bubble");
/* The cap must fit the pool margin of the car budget: the layer can never take
 * a slot the near traffic or the cars outside the budget need. */
static_assert(MAX_CARS_IN_USE + FAR_TRAFFIC_MAX <= NUMVEHICLES,
	"the far cap must stay inside the vehicle pool's margin");
static_assert(FAR_TRAFFIC_MAX >= FAR_TRAFFIC_MIN_CARS, "the cap must not sit below its own floor");
static_assert(FAR_TRAFFIC_MIN_BAND > 0.0f, "an open band must have a width");
static_assert(FAR_TRAFFIC_STAGGER > 1, "the stagger shares the layer over more than one frame");
static_assert(FAR_TRAFFIC_CREATE_ATTEMPTS > 0, "a frame must be able to fill the layer");
static_assert(FAR_TRAFFIC_REMOVE_ATTEMPTS > 0, "a frame must be able to empty the layer");
static_assert(FAR_TRAFFIC_OUTER_HYSTERESIS > 0.0f, "a removal must keep off a fresh creation");
static_assert(FAR_TRAFFIC_ADOPT_HYSTERESIS > 0.0f, "a hand-over needs its hysteresis");
static_assert(FAR_TRAFFIC_POOL_RESERVE > 0, "the layer must leave the pool room to breathe");

/* The layer's own rules, the frame entry and the near cull's hand-over. The
 * engine side is FarTraffic.cpp; the counters are read live by
 * utils/caps-audit/live-counters.py. The types are the plain C++ ones on
 * purpose: this header compiles on its own, without the engine's common.h (the
 * evidence program includes it). */
class CFarTraffic
{
public:
	/* One frame of the layer: the gate pass (the switches), the creation
	 * attempts, the staggered update. Called from the car block of
	 * CGame::Process, after the near cull. */
	static void Process(void);

	/* The near cull found a traffic car it is about to remove (out of the
	 * bubble, hidden, past the grace): the layer takes it over as a far car
	 * when the band and the cap allow, and the cull keeps its hands off it.
	 * Returns true when the car was taken. */
	static bool AdoptOnCull(CVehicle *pVehicle, const CVector &playerPos);

	/* The far state and its inverse, one place each (VICE-128). The far state
	 * is what CCarCtrl::CreateCarFromPlacement applies to a far spawn and what
	 * the hand-over applies to an adopted car; the traffic state is what a
	 * promotion applies. Nothing else in the game writes these flags. */
	static void ApplyFarState(CVehicle *pVehicle);
	static void ApplyTrafficState(CVehicle *pVehicle);

	static unsigned int ms_nFarTrafficCars;
	static unsigned int ms_nFarTrafficCap;
	static unsigned int ms_nFarTrafficCreations;
	static unsigned int ms_nFarTrafficRemovals;
	static unsigned int ms_nFarTrafficPromotions;
	static unsigned int ms_nFarTrafficDemotions;
	static unsigned int ms_nFarTrafficRefused;
	static unsigned int ms_nFarTrafficRefusedModel;
	static unsigned int ms_nFarTrafficRefusedPool;
	static unsigned int ms_nFarTrafficAttempts;
	static unsigned int ms_nFarTrafficUpdates;
};

#endif // __GTA_FARTRAFFIC_H__
