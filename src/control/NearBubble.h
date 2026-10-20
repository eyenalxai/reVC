#ifndef __GTA_NEARBUBBLE_H__
#define __GTA_NEARBUBBLE_H__

/*
 * The near bubble (VICE-127).
 *
 * The owner must never catch the game creating or removing a car in view, and
 * no car may appear from nowhere when the camera turns. The traffic already
 * spawns in a band around the player (TrafficRings.h) and the fill burst
 * (VICE-126) fills the street after a transition. What was missing is a rule
 * for the two events themselves: where a car may appear, and where it may go.
 *
 * The near bubble
 * ---------------
 *
 * The bubble is the 360-degree disc of NEAR_BUBBLE_RADIUS metres around the
 * player. It is view independent: the population inside it is the traffic that
 * drove in from outside, and the rule holds whatever the camera looks at. Two
 * facts follow, and they are the rule:
 *
 *	creation inside the bubble is not allowed, so a car never pops into
 *	the player's personal space from behind the camera;
 *	removal inside the bubble is not allowed, so the traffic the player
 *	watched drive by does not vanish when they look away.
 *
 * The bubble sits NEAR_BUBBLE_REMOVE_RESERVE inside its innermost creation
 * band: the car side band is RING_CAR_SIDE_SPAWN (120 m) and a fresh car lands
 * up to the lane offset past its ring, the same placement error the despawn
 * reserve holds (TrafficRings.h). The static asserts below check the fit for
 * every band the bubble is allowed to reason about.
 *
 * The bubble is the traffic's. The pedestrian bands of the ring table are
 * inside it by their own stock numbers (the off-screen band is 35..65 m), and
 * the ticket that owns them forbids a change of the ring radii (VICE-120/145),
 * so the crowd keeps its own visibility gates (Population.cpp, VICE-122). The
 * counters below read the traffic for the same reason: the subject of the
 * ticket is the car that appears from nowhere.
 *
 * The creation rule (outside the bubble only)
 * -------------------------------------------
 *
 * The rule is written against one set: the *catchable* positions - in the
 * frustum, inside the fade and fog of the weather, not occluded - are the ones
 * the player can catch an event at, and the ladder lets an event through only
 * outside that set, under the fade that covers the screen, or inside the
 * in-view band (arm 5). Every arm is a statement about the view:
 *
 *	1. the occluded hint
 *			COcclusion::IsPositionOccluded says an occluder volume
 *			covers the position. The active set is built during the
 *			render, one frame before the gates read it, by projecting
 *			the volumes with the *previous* camera, so the answer is
 *			not trusted on its own: it selects the position for a
 *			confirmation ray (arm 4), the exact test. A stale or wrong
 *			set can waste a ray or refuse a spawn, never allow a
 *			visible one. The set is read only while the camera still
 *			matches the view it was built for (NEAR_BUBBLE_OCCLUDER_*:
 *			2 m, a direction cosine of 0.9995, 2 degrees), so a turned
 *			camera spends no rays on stale candidates at all.
 *	2. beyond the fade
 *			the position is at or past the visible range (min(LOD
 *			distance, weather far clip)): the world fades and the fog
 *			hides there, whether the camera looks at it or not;
 *	3. the soft arm
 *			the position is off the frustum by the margin
 *			(NEAR_BUBBLE_SOFT_MARGIN: a radius margin, about 12 degrees
 *			of bearing at the soft distance, so a flick cannot reveal a
 *			fresh creation) and at least NEAR_BUBBLE_SOFT_DISTANCE away.
 *			This is the traffic behind the camera, and it is the arm
 *			that keeps a street with no occluders alive: the ring's
 *			bands sit *inside* the visible range by the table's own
 *			design, so with the occluders and the fog alone an open
 *			street has no creation site at all and its traffic drains
 *			(the model in utils/near-bubble Section 6). The arm's
 *			distance is the side band less the widest lane offset, so
 *			every placement of the band sits at or beyond it; a car
 *			created here starts at alpha 0 and holds it until the
 *			player can see it (CVehicle::UpdateClumpAlpha), so the
 *			reveal is the fade itself;
 *	4. the confirmation ray
 *			a line of sight (CWorld::GetIsLineOfSightClear, buildings
 *			only) that is blocked by a building means the position is
 *			hidden, however the stale data read. The ray is the one
 *			expensive test, so a frame may spend at most
 *			NEAR_BUBBLE_LOS_RAYS_PER_FRAME of them; a candidate that
 *			finds the budget spent is refused, not created;
 *	5. the in-view band
 *			the position is in the frustum, inside the fade and fog
 *			and not occluded, and it sits at or beyond the ring
 *			table's on-screen spawn minimum
 *			(CurrentTrafficRings().carOnScreenSpawnMin: 220 m stock,
 *			440 m at the owner's settings). The stock game always had
 *			this band - the spawner's on-screen branch aims at
 *			carOnScreenSpawnMax and the birth cull holds a fresh
 *			on-screen car at or beyond the minimum - and the car's own
 *			reveal covers it: the alpha of a fresh car starts at 0 and
 *			ramps up from the first render (CVehicle::UpdateClumpAlpha),
 *			so the band shows a car forming, never a settled one. It
 *			is the last allowed arm: the screen fade is decided first,
 *			and the band is reached only for a position the catchable
 *			set holds. Without it the rule refused every on-screen
 *			placement of the spawner on an open street and starved the
 *			traffic (VICE-146).
 *
 * Anything left - in the frustum or within the margin of it, not occluded,
 * inside the fade and under the band minimum - is refused: that is the
 * catchable zone, and the two catchable-zone counters of the acceptance read
 * it. The refused creation costs nothing: the placement loop of the caller
 * moves to its next candidate and the frame keeps its budget.
 *
 * The removal rule
 * ----------------
 *
 * A car may only be removed when all of this holds:
 *
 *	- the fresh-spawn grace has passed: a car created now is protected for
 *	  NEAR_BUBBLE_GRACE_MS, so a spawn can never be undone on its way in;
 *	- the distance is beyond the removal threshold: the ring's own despawn
 *	  (which already sits the reserve past the spawn band, the hysteresis of
 *	  the table) and the bubble plus its reserve, whichever is farther;
 *	- the car is hidden by an arm that does not rest on stale data: off the
 *	  frustum by the margin, beyond the fade, under a fade, behind a
 *	  confirmation ray - or it sits in the in-view band, at or beyond the
 *	  on-screen spawn minimum, the same arm a creation takes there. The
 *	  occluded hint is confirmed exactly like a creation's, so a stale
 *	  occluder cannot fade a car out in view; the removal spends from the
 *	  same ray budget, and a removal that finds the budget spent simply
 *	  waits - the car is hidden-ish and leaves a frame later.
 *
 * The fill burst (VICE-126)
 * -------------------------
 *
 * The burst fills the street while the screen hides the fill: the fade the
 * game draws over the world covers at least FILL_BURST_HIDDEN_FADE of it. A
 * creation in that state may sit in the frustum - the player cannot see it -
 * and the counters record it as fade exempt, so the acceptance of the ticket
 * (the catchable-zone counters stay zero) reads cleanly. The bubble still
 * applies:
 * even a hidden frame does not create a car at the player's elbow. The fill's
 * mass is opaque when the screen returns: the ramp of a car's alpha runs at
 * the hidden step while the fade covers the screen (CVehicle::
 * UpdateClumpAlpha), and the cars that hold their invisible frame (the soft
 * arm) fade in only when first seen.
 *
 * The counters (the acceptance)
 * -----------------------------
 *
 *	catchable creations / removals	the acceptance counters (VICE-146): a
 *					car placed or deleted in the frustum
 *					nearer than the band minimum - the
 *					catchable zone, where the player
 *					could catch it. Must stay 0.
 *	band creations / removals	the in-view band: a car placed or
 *					deleted in the frustum at or beyond the
 *					band minimum. Allowed: a creation is
 *					covered by the car's own reveal (its
 *					alpha ramp from the first render), a
 *					removal by the fade-out of the despawn.
 *					The counters rise with the traffic the
 *					band restores.
 *	fade creations / removals	the catchable events a fade covered:
 *					exempt. The hidden events of a faded frame are
 *					counted with the hidden ones, so the fade
 *					counter is exactly the fade's own work.
 *	hidden creations / removals	the events the rule allowed outside the
 *					catchable set.
 *	occluder creations / removals	those of the hidden events the occluded
 *					hint selected and the confirmation ray allowed:
 *					the arm the play-test can quantify.
 *	ray creations			the placements the close blind spot allowed
 *					through the confirmation ray.
 *	refused				the events the rule stopped.
 *	bypassed			the events the top-down camera's view allowed.
 *	rays / rays total		the rays of the frame (at most the budget)
 *					and since the counters were last zeroed.
 *	fill, fill target		the traffic (the spawner's random cars)
 *					inside the bubble and the share of the car
 *					target its area asks for.
 *	other fill, other creations	the cars created outside the traffic
 *					spawner (the scripts, the set pieces, the road
 *					blocks, the car parks, the garages, the cheats,
 *					the RC car). They are part of the car budget, so
 *					the target counts them: the fill plus the other
 *					fill is what the target describes. Their creations
 *					are counted apart, with the catchable ones of them
 *					subdivided, so a scripted car that appears in view
 *					is visible in the live numbers instead of hiding
 *					inside the traffic's.
 *
 * A creation or removal that bypasses the rule (a script, a mission) never
 * moves the traffic's counters: the counters read the churn of the traffic
 * population, and the "other" pair above carries the rest. The parked cars of
 * the generators go through the same creation rule but have their own
 * unobserved gate (CarGen.cpp) and are parked vehicles, so they are in neither
 * fill. The trains are created once at boot at the origin (CTrain::InitTrains)
 * as permanent, locked vehicles - not a traffic event, and not counted; the
 * planes and the helicopter come from the scripts and the cheats, whose cars
 * the "other" rows carry.
 *
 * No engine header is included here. The evidence program in utils/near-bubble
 * reads the ladder and the targets from this header alone; the engine calls
 * (the camera, the occluders, the ray) live in NearBubble.cpp.
 */

#include "TrafficRings.h"

/* The bubble. The radius is the car side band less the placement reserve the
 * despawn uses: the innermost position a car can be placed at is the side ring
 * less the lane offset, so the bubble never asks the side band to place inside
 * itself. */
#define NEAR_BUBBLE_RADIUS		(RING_CAR_SIDE_SPAWN - RING_CAR_DESPAWN_RESERVE)
/* The hysteresis of a removal over the bubble, the same distance the despawn
 * uses over the spawn ring. */
#define NEAR_BUBBLE_REMOVE_RESERVE	RING_CAR_DESPAWN_RESERVE
#define NEAR_BUBBLE_REMOVE_DISTANCE	(NEAR_BUBBLE_RADIUS + NEAR_BUBBLE_REMOVE_RESERVE)

/* The widest lane offset of the shipped paths (PathFind.h LANE_WIDTH x the
 * widest lane count and the two-way offset, the arithmetic utils/traffic-rings
 * derives: (4 - 1 + 0.5) x 5). A side-band placement lands up to this past its
 * ring along the radius, so the soft distance takes it off the ring: every
 * placement of the band sits at or beyond the soft distance. */
#define NEAR_BUBBLE_SIDE_PLACEMENT_SLACK 17.5f

/* The soft distance of the creation rule: the off-frustum traffic behind the
 * camera. It is the side band less the placement slack, so the whole side band
 * qualifies - the arm that keeps a street with no occluders alive (the ring's
 * bands sit inside the visible range by the table's design). Inside it the
 * confirmation ray decides. */
#define NEAR_BUBBLE_SOFT_DISTANCE	(RING_CAR_SIDE_SPAWN - NEAR_BUBBLE_SIDE_PLACEMENT_SLACK)

/* The margin the soft and ray arms require a position to be off the frustum
 * by: a radius margin, about 12 degrees of bearing at the soft distance
 * (asin(NEAR_BUBBLE_SOFT_MARGIN / NEAR_BUBBLE_SOFT_DISTANCE)), so a flick of
 * the camera cannot reveal a fresh creation; the margin is in front of the
 * position before the position is even out of the view cone. */
#define NEAR_BUBBLE_SOFT_MARGIN		25.0f

/* The radius the visibility tests use for a car: the bounding sphere of the
 * largest shipped vehicle sits below this, so the tests are conservative. */
#define NEAR_BUBBLE_ENTITY_RADIUS	3.0f

/* The line-of-sight budget of one frame: the rays are the collision path's
 * work, so they are the scarcest resource of the rule. Four rays reach the
 * occluded hints and the close band of the frame; a refused candidate costs
 * nothing and the placement loop moves on. */
#define NEAR_BUBBLE_LOS_RAYS_PER_FRAME	4
/* The longest ray: the soft distance, a couple of sectors. Inside it the
 * occluded hints and the close band live. */
#define NEAR_BUBBLE_LOS_RANGE		NEAR_BUBBLE_SOFT_DISTANCE

/* The staleness the occluder hint is read for. The set is built during the
 * render and read by the gates of the next frame, so the camera may have moved
 * a frame's worth; past this the hint is ignored and the other arms decide. A
 * dot of 0.9995 is about 1.8 degrees; the move is a frame at 60 m/s. The hint
 * never decides on its own - the confirmation ray does - so the window's cost
 * is a wasted ray, not a pop. */
#define NEAR_BUBBLE_OCCLUDER_MAX_MOVE	2.0f
#define NEAR_BUBBLE_OCCLUDER_MIN_DOT	0.9995f
#define NEAR_BUBBLE_OCCLUDER_MAX_FOV	2.0f

/* The fresh-spawn grace of a car: the five seconds the stuck cull waits before
 * it is allowed to take a car at all, so a car that was just placed can never
 * be the one the cull gives up on. */
#define NEAR_BUBBLE_GRACE_MS		5000

/* The arm of the ladder that answered for a candidate: the reason an event was
 * allowed, refused, or sent to the confirmation ray. The counters subdivide
 * the hidden events by it. */
enum ENearBubbleArm
{
	NEAR_BUBBLE_ARM_NONE = 0,	/* refused */
	NEAR_BUBBLE_ARM_BYPASS,		/* the top-down camera's view */
	NEAR_BUBBLE_ARM_OCCLUDER,	/* the occluded hint: a ray confirms it */
	NEAR_BUBBLE_ARM_FADE_DISTANCE,	/* beyond the visible range */
	NEAR_BUBBLE_ARM_SOFT,		/* off the frustum by the margin, beyond the soft distance */
	NEAR_BUBBLE_ARM_RAY,		/* off the frustum by the margin, close: a ray decides */
	NEAR_BUBBLE_ARM_FADE,		/* the screen fade covers the event */
	NEAR_BUBBLE_ARM_ONSCREEN_BAND,	/* in the frustum, at or beyond the on-screen spawn minimum */
};

/* One candidate as the frame sees it. The caller fills the facts from the
 * engine (TheCamera, COcclusion, CurrentTrafficRings) and the harness fills
 * them from its own matrix. `occluded` is the *trusted* hint: the caller folds
 * the staleness of the occluder set in before it sets the field. */
struct SNearBubbleCandidate
{
	float distToPlayer;	/* 2D: the bubble is a disc on the map */
	float distToCamera;	/* 3D, the distance the renderer fades with */
	float visibleRange;	/* CurrentTrafficRings().visibleRange */
	float onScreenSpawnMin;	/* CurrentTrafficRings().carOnScreenSpawnMin:
				 * the lower edge of the in-view band */
	bool inFrustum;		/* TheCamera.IsSphereVisible(pos, radius) */
	bool inFrustumMargin;	/* IsSphereVisible(pos, radius + NEAR_BUBBLE_SOFT_MARGIN) */
	bool occluded;		/* COcclusion::IsPositionOccluded(pos, radius), trusted */
	bool fadeHides;		/* FillBurstIsHidden(): the fade covers the screen */
	bool bypassView;	/* the top-down camera: the view is not the player's */
};

/* The catchable set of the rule: the position is in the frustum, inside the
 * fade of the weather and not occluded, so the player could catch an event
 * there. The ladder is written against this one predicate, so the two cannot
 * drift apart; the evidence program sweeps every combination of the fields to
 * hold it. */
static inline bool
NearBubblePositionIsCatchable(const SNearBubbleCandidate &c)
{
	return c.inFrustum && !c.occluded &&
		(c.visibleRange <= 0.0f || c.distToCamera < c.visibleRange);
}

/* The arm that answers for a candidate. The answer is derived from it, so the
 * counters can subdivide by the arm and the ladder stays one function. */
static inline ENearBubbleArm
NearBubbleArmFor(const SNearBubbleCandidate &c)
{
	/* The bubble. Nothing appears or disappears in it, whatever the view,
	 * the fade or the camera mode: a car placed at the player's elbow is
	 * there when the view that covered it is gone. */
	if (c.distToPlayer <= NEAR_BUBBLE_RADIUS)
		return NEAR_BUBBLE_ARM_NONE;
	/* The top-down camera looks at the map from above, where the street level
	 * is not the subject and the traffic must keep flowing: the visibility
	 * arms step aside, the bubble above does not. */
	if (c.bypassView)
		return NEAR_BUBBLE_ARM_BYPASS;
	/* Outside the catchable set the event is hidden, and the arms are the
	 * ways a position lands there. The subdivision is structural: with the
	 * predicate above, an occluded position is out, a position beyond the
	 * fade is out, and what is left over is off the frustum. */
	if (!NearBubblePositionIsCatchable(c)) {
		/* The occluder volumes, as a hint: the confirmation ray (the
		 * caller resolves it) decides. */
		if (c.occluded)
			return NEAR_BUBBLE_ARM_OCCLUDER;
		/* The fade and the fog: the world is not drawn at this distance,
		 * whether the frustum covers the position or not. A visible range
		 * of zero is the boot path before the time cycle and uses the
		 * close arms instead. */
		if (c.visibleRange > 0.0f && c.distToCamera >= c.visibleRange)
			return NEAR_BUBBLE_ARM_FADE_DISTANCE;
		/* The position must be off the view cone by the margin: in the
		 * cone, or within the margin of it, an event is refused (a turn
		 * of the camera would reveal it at once). */
		if (c.inFrustumMargin)
			return NEAR_BUBBLE_ARM_NONE;
		/* Off the view cone: the traffic behind the camera. Far enough,
		 * the soft arm allows it; closer, the ray must confirm a
		 * building between the camera and it. */
		if (c.distToCamera >= NEAR_BUBBLE_SOFT_DISTANCE)
			return NEAR_BUBBLE_ARM_SOFT;
		return NEAR_BUBBLE_ARM_RAY;
	}
	/* In the frustum, inside the fade, nothing between the camera and the
	 * position: only the fade that covers the screen (the fill burst of a
	 * transition, exempt in the counters) and the in-view band let an event
	 * through. The fade is decided first, so the band is reached only for a
	 * position the screen does not already hide. */
	if (c.fadeHides)
		return NEAR_BUBBLE_ARM_FADE;
	if (c.distToCamera >= c.onScreenSpawnMin)
		return NEAR_BUBBLE_ARM_ONSCREEN_BAND;
	return NEAR_BUBBLE_ARM_NONE;
}

/* The answer of the ladder for one candidate. */
enum ENearBubbleAnswer
{
	NEAR_BUBBLE_REFUSED = 0,	/* the event is not allowed here */
	NEAR_BUBBLE_ALLOWED,		/* the position is hidden: the event may happen */
	NEAR_BUBBLE_SEEK_LOS		/* the occluded hint or the close blind spot:
					 * a blocked ray allows it, a clear one refuses */
};

/* The arms that answer without a ray: the hidden ones and the in-view band.
 * The arms that are not here - the refusal and the two that ask for the
 * confirmation ray - are refused when the frame cannot spend a ray, exactly
 * what the resolution does with `rayAllowed` false (the parked cars of the
 * generators and the far layer). One truth, so the two cannot drift apart:
 * NearBubbleResolve (NearBubble.cpp) and the service's parked path both read
 * this predicate. */
static inline bool
NearBubbleArmAllowsWithoutRay(ENearBubbleArm arm)
{
	switch (arm) {
	case NEAR_BUBBLE_ARM_BYPASS:
	case NEAR_BUBBLE_ARM_FADE_DISTANCE:
	case NEAR_BUBBLE_ARM_SOFT:
	case NEAR_BUBBLE_ARM_FADE:
	case NEAR_BUBBLE_ARM_ONSCREEN_BAND:
		return true;
	default:
		return false;
	}
}

static inline ENearBubbleAnswer
NearBubbleAnswerFor(const SNearBubbleCandidate &c)
{
	ENearBubbleArm arm = NearBubbleArmFor(c);

	/* The one classification of the arms: the ray-free arms allow, the
	 * refusal refuses, and what is left is the two arms the confirmation
	 * ray decides. */
	if (NearBubbleArmAllowsWithoutRay(arm))
		return NEAR_BUBBLE_ALLOWED;
	if (arm == NEAR_BUBBLE_ARM_NONE)
		return NEAR_BUBBLE_REFUSED;
	return NEAR_BUBBLE_SEEK_LOS;
}

/* The bubble's share of a budget: the cars the ring holds over its area, times
 * the bubble's area. The count is what the fill counter is compared against;
 * the target follows the density sliders and the ring scale through the budget
 * the caller passes (CCarCtrl::GetRandomCarTarget). */
static inline float
NearBubbleTarget(float budget, float despawnRadius)
{
	if (budget <= 0.0f || despawnRadius <= 0.0f)
		return 0.0f;
	return budget * (NEAR_BUBBLE_RADIUS * NEAR_BUBBLE_RADIUS) / (despawnRadius * despawnRadius);
}

/* The bubble never asks a band to place inside itself. */
static_assert(NEAR_BUBBLE_RADIUS + NEAR_BUBBLE_REMOVE_RESERVE <= RING_CAR_SIDE_SPAWN,
	"the bubble must sit the reserve inside the car side band");
static_assert(NEAR_BUBBLE_RADIUS + NEAR_BUBBLE_REMOVE_RESERVE <= RING_CAR_SIDE_SPAWN * RING_BOAT_SPAWN_FACTOR,
	"the bubble must sit inside the boat side band");
static_assert(NEAR_BUBBLE_RADIUS + NEAR_BUBBLE_REMOVE_RESERVE <= RING_GENERATOR_FAR,
	"the bubble must sit inside the generator ring");
/* The hysteresis: the removal threshold leaves the bubble and stays inside the
 * ring's own despawn, so the distance cull never removes a car the bubble
 * protects and never waits longer than the ring does. */
static_assert(NEAR_BUBBLE_REMOVE_DISTANCE > NEAR_BUBBLE_RADIUS,
	"a removal must leave the bubble");
static_assert(NEAR_BUBBLE_REMOVE_DISTANCE <= RING_CAR_DESPAWN,
	"the removal threshold must stay inside the despawn of the ring");
/* The soft distance takes the side band's placement slack off its ring, so
 * every placement of the band sits at or beyond the arm. */
static_assert(NEAR_BUBBLE_SOFT_DISTANCE <= RING_CAR_SIDE_SPAWN,
	"the soft distance must not reach past the side band");
static_assert(NEAR_BUBBLE_SOFT_DISTANCE + NEAR_BUBBLE_SIDE_PLACEMENT_SLACK >= RING_CAR_SIDE_SPAWN,
	"the soft distance must cover the side band's placement slack");
static_assert(NEAR_BUBBLE_SOFT_DISTANCE > NEAR_BUBBLE_RADIUS,
	"the soft distance must leave the bubble");
/* The in-view band sits beyond the bubble and the soft arm at the stock
 * numbers, and the ring table's scale floor keeps the live minimum at or above
 * the stock value (the harness checks it over the control matrix). */
static_assert(NEAR_BUBBLE_RADIUS < RING_CAR_ONSCREEN_SPAWN_MIN,
	"the in-view band must sit beyond the bubble");
static_assert(NEAR_BUBBLE_SOFT_DISTANCE < RING_CAR_ONSCREEN_SPAWN_MIN,
	"the in-view band must sit beyond the soft arm");
/* The margin is a bearing the camera must turn by before an event behind it
 * can be seen. */
static_assert(NEAR_BUBBLE_SOFT_MARGIN > NEAR_BUBBLE_ENTITY_RADIUS,
	"the margin must dominate the entity's own radius");
static_assert(NEAR_BUBBLE_SOFT_MARGIN < NEAR_BUBBLE_SOFT_DISTANCE,
	"the margin must stay inside the soft distance");
/* The ray reaches the soft distance by construction; the close band inside it
 * shares the ray with the occluded hints. */
static_assert(NEAR_BUBBLE_LOS_RANGE == NEAR_BUBBLE_SOFT_DISTANCE,
	"the ray covers the close band");
static_assert(NEAR_BUBBLE_LOS_RAYS_PER_FRAME > 0, "the ray budget must make progress");
static_assert(NEAR_BUBBLE_ENTITY_RADIUS > 0.0f, "the visibility tests need a radius");
static_assert(NEAR_BUBBLE_GRACE_MS > 0, "the fresh-spawn grace must last a frame at least");
static_assert(NEAR_BUBBLE_OCCLUDER_MAX_MOVE > 0.0f, "the occluder hint must allow a frame of movement");
static_assert(NEAR_BUBBLE_OCCLUDER_MIN_DOT > 0.0f && NEAR_BUBBLE_OCCLUDER_MIN_DOT <= 1.0f,
	"the occluder hint is a direction cosine");
static_assert(NEAR_BUBBLE_OCCLUDER_MAX_FOV >= 0.0f && NEAR_BUBBLE_OCCLUDER_MAX_FOV <= 5.0f,
	"the occluder hint allows a frame of zoom");

#endif // __GTA_NEARBUBBLE_H__
