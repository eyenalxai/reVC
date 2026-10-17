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
 * CCarCtrl::UpdateCarOnRails, one copy of the rules in CarOnRails.h), on a
 * stagger, and the alpha reveal of a creation (VICE-127) is stepped by the same
 * update. All of it is arithmetic on the car's own matrix: the layer touches no
 * collision query, no sector scan, no path search, no physics. Since VICE-133
 * the arithmetic runs on the layer's worker thread (FarTrafficWorker.h) and
 * this file writes the poses it publishes; the rules are the same on both sides
 * of that boundary, and the boundary's contract is the worker header's first
 * comment.
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
 * The world's hold
 * ----------------
 *
 * The world's sector grid is a bounded rectangle (World.h: x [-2400, 1600),
 * y [-2000, 2000)), and `CPhysical::RemoveAndAdd` walks `CWorld::ms_aSectors`
 * with the pose it is given; the grid's own guard covers only the upper side,
 * so a point outside the rectangle - or one that is not finite - ends in a
 * null list write. The snapshot the worker reads therefore carries the grid's
 * bounds (SFarTrafficWorld::bounds, filled from the engine's macros by
 * BuildWorldSnapshot), and one engine-free predicate (FarTrafficPoseIsInWorld,
 * this header) is the layer's rule for a pose the world can hold: the
 * simulation refuses a pose the predicate rejects, and the applicator refuses
 * one again whatever producer wrote it (the inline mode, a stale block, the
 * catch-up path). A refused pose is never written into a car; the car leaves
 * the layer by the no-driver promotion and the near side owns it from there.
 *
 * The switch hands the state over, not only the car: while a car is far the
 * simulation owns its route state (the two links of the curve, the lanes, the
 * directions, the curve times), so the promotion writes that state - the
 * simulation's, read from the worker in the applicator's window - back into
 * the car's AutoPilot before the entry is dropped. The curve clock is part of
 * it: the simulation's clock is CTimer::GetTimeInMilliseconds, the same domain
 * the near rails' CurveProgress reads, so a promoted car's curve starts where
 * the simulation left it rather than at the clock the car was seeded with when
 * it became far (a stale clock reads a progress in the hundreds and
 * CCarCtrl::DragCarToPoint drags the car out of the world).
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
 * created where the player cannot see it appear and removed where the player
 * cannot see it go. The layer's own sites are the two the bubble leaves on an
 * open road, and it asks no line-of-sight ray for them:
 *
 *	the fog site	beyond the visible range: a creation at the fade is a car
 *			forming out of the haze, a removal there is a car the fog
 *			already hides. This is the oncoming traffic of a long
 *			straight road. The layer places it at the fade plus
 *			FAR_TRAFFIC_FADE_MARGIN so the placement's own length
 *			lands inside the fog rather than at its edge. The path
 *			network does not reach the fog everywhere (the city is
 *			3.6 km across, the visible range reaches 2 km), so this
 *			site returns nothing over most of the map and the layer
 *			falls back:
 *	the near site	the band's inner edge plus FAR_TRAFFIC_SITE_MARGIN, in
 *			both cones: ahead (the oncoming and receding traffic of the
 *			road the player is on) and behind (the rear half, the
 *			traffic the player has already passed). The margin is what
 *			makes the site placeable at all: the collector's placement
 *			lands at or INSIDE its target distance (the convexity of the
 *			distance along a node segment), so a site aimed at the
 *			band's edge itself lands in the near band and every
 *			placement is refused - the zero-creation defect of the
 *			layer's first live session. The near site is where the near
 *			traffic's own band is placed, so the network always reaches
 *			it where the roads are.
 *
 * Two arms of the ladder are the layer's on purpose and both are ray-free: the
 * fade and the soft arm above, and - since VICE-146 restored it - the in-view
 * band. A far creation whose placement brackets slightly inside the fade (the
 * fog site's own segment length) lands in the frustum at or beyond
 * rings.carOnScreenSpawnMin, where the band arm allows it as the stock on-screen
 * site: the car's own alpha reveal covers it, exactly as it covers a near
 * spawn, and it is counted in the band rows, never the acceptance pair.
 *
 * The removals use the band's own cover: a car the player can see that the rule
 * lets out at or beyond the band minimum goes by the despawn's fade-out
 * (`bFadeOut`, the way the near traffic leaves in view, VICE-146), and the
 * faded tail is deleted at alpha 0, where the near cull deletes its own. A car
 * that is invisible or off screen, and every car beyond the fog or of a closed
 * band, is deleted at once. The layer's deletion is counted with the near
 * bubble's hidden removals, never the catchable zone.
 *
 * The ray is refused (rayAllowed false) for the far layer, on both gates: a
 * site two kilometres out spends a line-of-sight ray of that length, and the
 * frame's four rays are the near traffic's blind spots. The price is exact and
 * it is the conservative direction: an occluded far site or removal is refused,
 * not confirmed. Removals are bounded at FAR_TRAFFIC_REMOVE_ATTEMPTS gate
 * answers a frame, so the layer's removals never touch the frame's ray budget
 * at all.
 *
 * The switches
 * ------------
 *
 * A car of the layer that the player comes near is promoted to a full traffic
 * car (the driver, the collision, the AI, the budget), and a traffic car that
 * leaves the near band is demoted into the layer instead of being deleted. A
 * switch writes nothing the renderer draws of the car itself:
 *
 *	the matrix, the model, the colours, the status (SIMPLE), the clump and its
 *	alpha are the same on both sides of a switch, so no frame can show the car
 *	change;
 *	the promotions of a frame are checked for every far car (not only the
 *	staggered share), so a car sits inside the band for one frame at most;
 *	the demotion happens where the near cull already decided the car is out of
 *	the bubble and hidden, so it is a blind-spot event by the near bubble's own
 *	rule - a car the player watched drive away does not blink out, it changes
 *	which budget counts it;
 *	the promotion happens at the band's inner edge, at least RING_CAR_DESPAWN
 *	(280 m) from the player - the engine's own floor for the ring table's
 *	despawn - so the player cannot reach a car that is not a full vehicle.
 *
 * The one fact a promotion does add is the driver: SetUpDriver creates the ped
 * and the engine's own in-car rule draws it (CPed::Render: only inside
 * 25 x TheCamera.LODDistMultiplier, never before the car's own reveal, see
 * below). The layer holds the driver's rendering while the car still holds its
 * invisible frame (CVehicle::bDriverWaitsForReveal), so a promotion of an
 * unseen car shows the driver with the car's last fade step and never before
 * the car itself; a promotion of a car the player is already watching adds a
 * few pixels of a driver inside a car that is already opaque, at 280 m or
 * more. The harness carries the fact list and this one exception.
 *
 * The reach is sealed: a promotion that the pool refuses (the ped reserve)
 * leaves the car far only outside the player's reach (NEAR_BUBBLE_REMOVE_DISTANCE,
 * the bubble plus its reserve); inside it the layer makes the car real without
 * a driver (FAR_TRAFFIC_SAFETY_PROMOTE) rather than leave a car the player
 * could drive through. `ms_nFarTrafficSafetyPromotions` counts those switches
 * and `ms_nFarTrafficInReach` is the acceptance gauge: the far cars inside the
 * reach after a frame, which must read 0.
 *
 * The demotion refuses when the layer is off or its cap is full, and the near
 * cull deletes the car as it always did.
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
 *	ms_nFarTrafficSafetyPromotions	the switches the pool forced without a driver
 *	ms_nFarTrafficDemotions		the switches back (the cull's hand-overs)
 *	ms_nFarTrafficInReach		the acceptance: the far cars inside the
 *					player's reach that the gate pass had
 *					to decide on; must stay 0
 *	ms_nFarTrafficRefused		the creation attempts the rule stopped
 *	ms_nFarTrafficRefusedNoSite	those the network could not place at all
 *					(no site of the attempt found a node pair)
 *	ms_nFarTrafficRefusedBand	those whose placements all fell outside the
 *					band (too near the near traffic, or past the fog)
 *	ms_nFarTrafficRefusedGate	those whose in-band placements the near
 *					bubble's gate or the car build refused
 *	ms_nFarTrafficRefusedModel	those stopped by the streaming
 *	ms_nFarTrafficRefusedPool	those stopped by a pool reserve (or a driver)
 *	ms_nFarTrafficAttempts		the creation attempts
 *	ms_nFarTrafficUpdates		the car updates of the stagger (the cost)
 *	ms_nFarTrafficWorkerCycles	the worker's cycles (VICE-133)
 *	ms_nFarTrafficWorkerLost	the boundary faults (a command or a pose the
 *					cycle and the list disagreed about): must
 *					stay 0
 *	ms_nFarTrafficShortCurves	the pick's clamped curves (the engine's
 *					"fout" case, counted instead of printed)
 *	ms_nFarTrafficRefusedPoses	the poses the world could not hold (the
 *					simulation's refusals, and any pose the
 *					apply refused that the simulation published):
 *					the pose is never written and the car leaves
 *					the layer, without a driver
 *	ms_nFarTrafficCatchUps		the applicator's catch-ups (a pending cycle
 *					run inline because the window between two
 *					cycles grew the command ring; 0 in normal
 *					play)
 *
 * The first four are the ticket's counters (the cap holds, the layer's churn);
 * the refusals split by cause so a live reading says why a road stays bare: an
 * absent model (the streaming) is not the rule refusing. `ms_nFarTrafficInReach`
 * is the second acceptance row next to the near bubble's two frustum counters:
 * it counts the cars that a frame of the layer left far where the player could
 * reach them, which the safety promotion of the switches makes impossible.
 *
 * Since VICE-133 the simulation of the stagger runs on a worker thread
 * (FarTrafficWorker.h): the frame's own work is the applicator - the gate pass,
 * the creations, the removals and the writing of the poses the worker
 * published - and the worker's rows are the four above. The layer's rules are
 * the same on both sides of the boundary: the worker only computes.
 *
 * No engine header is included here. The evidence program in utils/far-traffic
 * reads the band, the cap, the switches and the counters from this header
 * alone; the engine calls (the rings, the paths, the pool, the streaming) live
 * in FarTraffic.cpp.
 */

#include <float.h>

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

/* The site distance margin. `CPathFind::CollectCarCreationCoors` places a
 * candidate where the segment between its two path nodes is interpolated to the
 * target distance, and that interpolation always lands AT or INSIDE the target
 * (the distance from the player along a segment is convex, so its value at the
 * interpolated parameter is at most the interpolated distance - exactly the
 * target; the shortfall is the segment's own sagitta, L x L / 8R, which for the
 * shipped node spacing and the band's radii is a few metres at most). A site
 * aimed exactly at the band's inner edge therefore lands inside the near band
 * and every placement is refused by the layer's own band check - the live
 * zero-creation defect of VICE-128's first session. Every site is aimed this
 * margin PAST its edge instead, and the margin is also the clearance the layer
 * wants from the near traffic's own despawn (a car the layer creates is 50 m
 * beyond where the near traffic ends). It must stay smaller than the band. */
#define FAR_TRAFFIC_SITE_MARGIN		50.0f

/* The creation sites. The fog site is placed this far beyond the fade (the
 * bubble's fade arm needs the camera distance at or past the visible range; the
 * margin also covers the length of the path segment the placement brackets) and
 * the outer hysteresis keeps a car created just past the fog from being removed
 * on its way out of the band. The path network does not reach the fog
 * everywhere (the city is 3.6 km across and the visible range reaches 2 km), so
 * the layer also has the near site below, which the network always reaches. */
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

/* The simulation's home (VICE-133): 1 runs the layer's per-car simulation on
 * the worker thread (FarTrafficWorker.h), 0 runs the same cycle inline on the
 * main thread - the two paths are one implementation, so this constant only
 * decides where it runs, and the evidence program measures both. The default is
 * the measurement's (utils/far-traffic-worker): at the current cap the cycle
 * costs the frame thread about 1.0 us inline and the handshake costs it about
 * 0.8-1.0 us (the hand-over's futex wake and the join), a wash; the simulation's
 * cost grows with the cap and the handshake's does not, so the boundary wins
 * from about 55-64 cars up, which is where VICE-132's raised maxima would live.
 * The switch exists so the owner can put the layer back on the frame thread
 * with one edit if their own capture says so (the ticket's escape hatch). */
#define FAR_TRAFFIC_WORKER		1

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

/* The near site of the layer: the band's inner edge plus the margin, asked in
 * both cones (ahead and behind). This is the site the network always reaches -
 * the near traffic's own band is placed at these distances by the stock
 * spawner everywhere the roads are - so the layer can always place while the
 * player is on land. The fog site is tried first and the near site is the
 * fallback: the fog gives the oncoming traffic of a long sight line when the
 * map reaches it, and the near site keeps the layer alive when it does not. */
static inline float
FarTrafficNearSiteDistance(const SFarTrafficBand &band)
{
	return band.inner + FAR_TRAFFIC_SITE_MARGIN;
}

/* The fog site of the layer: past the fade, where a creation forms out of the
 * haze (the near bubble's fade arm covers it). It returns nothing when the
 * network does not reach that far. */
static inline float
FarTrafficFogSiteDistance(const SFarTrafficBand &band)
{
	return band.createOuter;
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
 * reserves). A car inside the band's inner edge becomes traffic: with a driver
 * when the pool can afford one, and without a driver at the player's reach
 * (FAR_TRAFFIC_SAFETY_PROMOTE) - the layer never leaves a car the player could
 * reach that is not a real vehicle. */
enum EFarTrafficSwitch
{
	FAR_TRAFFIC_KEEP = 0,		/* nothing: the car stays far */
	FAR_TRAFFIC_PROMOTE,		/* it reached the near band: it becomes traffic */
	FAR_TRAFFIC_SAFETY_PROMOTE,	/* the same, without a driver: the pool refused
					 * one and the player is too close to wait */
	FAR_TRAFFIC_REMOVE,		/* the band is gone or it is beyond the fog: it goes */
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
		/* The promotion's driver refused (the ped reserve): the car stays
		 * far only while the player cannot reach it. Inside the reach it is
		 * made real anyway, without a driver: a far car is a shell the
		 * player can drive through, and that is not allowed where the
		 * player can be. */
		if (dist <= NEAR_BUBBLE_REMOVE_DISTANCE)
			return FAR_TRAFFIC_SAFETY_PROMOTE;
		return FAR_TRAFFIC_KEEP;
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

/* The bounds of the world's sector grid (World.h: WORLD_MIN_X/WORLD_MIN_Y and
 * WORLD_MAX_X/WORLD_MAX_Y). The snapshot carries them (SFarTrafficWorld::bounds,
 * filled by CFarTraffic::BuildWorldSnapshot from the engine's own macros), so
 * the worker and the applicator can test a pose without including an engine
 * header. The grid is half-open: the minimum edge is a sector of the grid, the
 * maximum edge is one past it (CWorld::GetSector's own index). */
struct SFarTrafficWorldBounds
{
	float minX, maxX;
	float minY, maxY;
};

/* The layer's one pose rule: true when the world can hold the pose - the
 * position (x, y, z) and the directions finite, and x/y inside the sector grid.
 * The comparisons are the finite test and need no math.h: a NaN fails every
 * comparison, an infinity fails a bound (FLT_MAX is the largest finite float),
 * and the bounds of x/y reject the infinities with the grid's own edges. The
 * grid test is what keeps the pose out of the sector walk of
 * CPhysical::RemoveAndAdd. The simulation and the applicator share this
 * predicate, so there is no second copy of the test to drift: the simulation
 * marks a rejected pose (SFarSimPose::bRefused) and the applicator refuses it
 * again, whatever producer wrote it. */
static inline bool
FarTrafficPoseIsInWorld(const SFarTrafficWorldBounds &bounds,
	float x, float y, float z, float dirX, float dirY)
{
	return x >= bounds.minX && x < bounds.maxX &&
		y >= bounds.minY && y < bounds.maxY &&
		z >= -FLT_MAX && z <= FLT_MAX &&
		dirX >= -FLT_MAX && dirX <= FLT_MAX &&
		dirY >= -FLT_MAX && dirY <= FLT_MAX;
}

/* The promotion distance is the near traffic's own despawn (RING_CAR_DESPAWN at
 * the lowest FOV factor; TrafficRings.h never takes the scale below it), which
 * sits well outside the near bubble and its reserve: a far car is never left
 * where the player could touch it, whatever the sliders say. */
static_assert(RING_CAR_DESPAWN > NEAR_BUBBLE_REMOVE_DISTANCE,
	"the far band must start outside the near bubble");
/* The cap must fit the pool margin of the car budget: the layer can never take
 * a slot the near traffic or the cars outside the budget need. The second check
 * keeps the cars outside the budget (the planes, the trains, the helicopter, the
 * set pieces, the shortcut car; PopulationBudget.h) at least as much of the
 * margin as the layer takes, so the layer can never be the one that fills it. */
static_assert(MAX_CARS_IN_USE + FAR_TRAFFIC_MAX <= NUMVEHICLES,
	"the far cap must stay inside the vehicle pool's margin");
static_assert(2 * FAR_TRAFFIC_MAX <= NUMVEHICLES - MAX_CARS_IN_USE,
	"the cars outside the budget must keep at least the layer's own room");
static_assert(FAR_TRAFFIC_MAX >= FAR_TRAFFIC_MIN_CARS, "the cap must not sit below its own floor");
static_assert(FAR_TRAFFIC_MIN_BAND > 0.0f, "an open band must have a width");
static_assert(FAR_TRAFFIC_STAGGER > 1, "the stagger shares the layer over more than one frame");
static_assert(FAR_TRAFFIC_CREATE_ATTEMPTS > 0, "a frame must be able to fill the layer");
static_assert(FAR_TRAFFIC_REMOVE_ATTEMPTS > 0, "a frame must be able to empty the layer");
static_assert(FAR_TRAFFIC_OUTER_HYSTERESIS > 0.0f, "a removal must keep off a fresh creation");
static_assert(FAR_TRAFFIC_ADOPT_HYSTERESIS > 0.0f, "a hand-over needs its hysteresis");
static_assert(FAR_TRAFFIC_POOL_RESERVE > 0, "the layer must leave the pool room to breathe");
/* The site margin must fit inside the narrowest open band, so the near site
 * (the inner edge plus the margin) is a site of every band that opens. */
static_assert(FAR_TRAFFIC_SITE_MARGIN > 0.0f, "a site must clear the band's edge it aims past");
static_assert(FAR_TRAFFIC_MIN_BAND > FAR_TRAFFIC_SITE_MARGIN,
	"the near site must sit inside the narrowest open band");

/* The layer's own rules, the frame entry and the near cull's hand-over. The
 * engine side is FarTraffic.cpp; the counters are read live by
 * utils/caps-audit/live-counters.py. The types are the plain C++ ones on
 * purpose: this header compiles on its own, without the engine's common.h (the
 * evidence program includes it). */
class CFarTraffic
{
public:
	/* One frame of the layer: the gate pass (the switches), the creation
	 * attempts, the stagger, and - since VICE-133 - the application of the
	 * poses the worker's simulation published, plus the frame's own cycle for
	 * the worker. Called from the car block of CGame::Process, after the near
	 * cull. */
	static void Process(void);

	/* The worker's boundary (VICE-133). `JoinWorker` runs at the top of
	 * CGame::Process, before CWorld::Process: the worker's cycle is complete
	 * and its output is published, so the worker is idle for the rest of the
	 * frame (a debug build asserts it in CWorld::Process and RenderScene).
	 * `RunWorker` runs at the end of the frame, after the render: the cycle
	 * the applicator stamped is handed to the worker (or run inline). The two
	 * are the whole of the worker's schedule; the contract, the double buffer
	 * and the frame's order are in FarTrafficWorker.h. */
	static void JoinWorker(void);
	static void RunWorker(void);

	/* A debug build's audit of the same window (VICE-133): the worker is not
	 * inside a cycle. CWorld::Process and RenderScene assert it; a release
	 * build compiles the call away. */
	static void AssertWorkerIdle(void);

	/* The path snapshot the worker's simulation reads (VICE-133): a copy of the
	 * graph's nodes, links and connections, taken once, on the main thread,
	 * right after ThePaths.PreparePathData (and BuildNodeGrid) in
	 * CGame::Initialise. The script's road toggles and a save load are copied
	 * over later by the applicator, when the graph's own flag version moves. */
	static void BuildWorldSnapshot(void);

	/* The worker thread's end: CGame::ShutDown joins it (it sleeps between
	 * cycles, so the join is immediate) and the next Initialise starts a fresh
	 * one with a fresh snapshot. */
	static void ShutDown(void);

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
	static unsigned int ms_nFarTrafficSafetyPromotions;
	static unsigned int ms_nFarTrafficDemotions;
	static unsigned int ms_nFarTrafficInReach;
	static unsigned int ms_nFarTrafficRefused;
	static unsigned int ms_nFarTrafficRefusedNoSite;
	static unsigned int ms_nFarTrafficRefusedBand;
	static unsigned int ms_nFarTrafficRefusedGate;
	static unsigned int ms_nFarTrafficRefusedModel;
	static unsigned int ms_nFarTrafficRefusedPool;
	static unsigned int ms_nFarTrafficAttempts;
	static unsigned int ms_nFarTrafficUpdates;

	/* The worker's own rows (VICE-133): the cycles it ran (the rate is this
	 * over the elapsed time), the boundary faults (a command or a pose the
	 * cycle and the layer's list disagreed about: must stay 0), the pick's
	 * clamped curves (the engine's own "fout" diagnostic, counted instead of
	 * printed on the worker thread), and the applicator's catch-ups (the
	 * window between two cycles that grew the command ring towards its water
	 * mark: 0 in normal play). */
	static unsigned int ms_nFarTrafficWorkerCycles;
	static unsigned int ms_nFarTrafficWorkerLost;
	static unsigned int ms_nFarTrafficShortCurves;
	static unsigned int ms_nFarTrafficCatchUps;

	/* The poses the world could not hold (VICE-168): the simulation's
	 * refusals, and any pose the apply refused that the simulation published.
	 * The pose is never written into the car and the car leaves the layer by
	 * the no-driver promotion, so a broken producer cannot reach the sector
	 * insert of CPhysical::RemoveAndAdd. It is not a normal-play row: a
	 * healthy layer reads 0. */
	static unsigned int ms_nFarTrafficRefusedPoses;
};

#endif // __GTA_FARTRAFFIC_H__
