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
 * A position may receive a car only while the player cannot see it. The rule is
 * a ladder, cheapest first, and every arm is a statement about the view:
 *
 *	1. occluded	COcclusion::IsPositionOccluded: an occluder volume covers
 *			the position, so the camera cannot see it however it turns;
 *	2. beyond the fade
 *			the position is at or past the visible range (min(LOD
 *			distance, weather far clip)): the world fades and the fog
 *			hides there, whether the camera looks at it or not;
 *	3. a line-of-sight ray
 *			the position is off the frustum and close (inside
 *			NEAR_BUBBLE_LOS_RANGE); a blocked ray (a building between
 *			the camera and the position) means a turn cannot reveal it.
 *			The ray is the one expensive test, so a frame may spend at
 *			most NEAR_BUBBLE_LOS_RAYS_PER_FRAME of them; a candidate
 *			that finds the budget spent is refused, not created.
 *
 * Anything left - in the frustum, inside the fade, not occluded - is refused.
 * The refused creation costs nothing: the placement loop of the caller moves to
 * its next candidate and the frame keeps its budget.
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
 *	- the car is hidden: off the frustum, occluded, or beyond the fade. A
 *	  removal is a soft event (the game fades the car out), so it does not
 *	  spend a ray: the cheap arms decide, and a car the player can see keeps
 *	  driving instead of disappearing.
 *
 * The fill burst (VICE-126)
 * -------------------------
 *
 * The burst fills the street while the screen hides the fill: the fade the
 * game draws over the world covers at least FILL_BURST_HIDDEN_FADE of it. A
 * creation in that state may sit in the frustum - the player cannot see it -
 * and the counters record it as fade exempt, so the acceptance of the ticket
 * (the frustum counters stay zero) reads cleanly. The bubble still applies:
 * even a hidden frame does not create a car at the player's elbow.
 *
 * The counters (the acceptance)
 * -----------------------------
 *
 *	creations in the frustum	the acceptance counter: a car placed where
 *					the player could catch it. Must stay 0.
 *	removals in the frustum		the same for a car deleted (not faded)
 *					where the player could catch it.
 *	fade creations / removals	the events a fade covered: exempt.
 *	hidden creations		the placements the rule allowed: the work
 *					of the bubble.
 *	refused				the placements the rule stopped.
 *	rays				the line-of-sight rays spent since the frame
 *					began: at most the per-frame bound.
 *	fill, target			the traffic inside the bubble and the share
 *					of the car target its area asks for.
 *
 * A creation or removal that bypasses the rule (a script, a mission) is not
 * counted: the counters read the churn of the traffic population. The parked
 * cars of the generators go through the same creation rule but have their own
 * unobserved gate (CarGen.cpp), so they are counted with the traffic and never
 * inflate a frustum counter.
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

/* The radius the visibility tests use for a car: the bounding sphere of the
 * largest shipped vehicle sits below this, so the tests are conservative. */
#define NEAR_BUBBLE_ENTITY_RADIUS	3.0f

/* The line-of-sight budget of one frame: the rays are the collision path's
 * work, so they are the scarcest resource of the rule. Four rays reach the
 * stock ring's own crossing points; a refused candidate costs nothing and the
 * placement loop moves on. */
#define NEAR_BUBBLE_LOS_RAYS_PER_FRAME	4
/* The longest ray: the stock on-screen ring plus its reserve. A candidate
 * beyond it that the fog does not hide is refused without a ray, so the walk
 * of one ray stays a handful of sectors. */
#define NEAR_BUBBLE_LOS_RANGE		300.0f

/* The fresh-spawn grace of a car: the five seconds the stuck cull waits before
 * it is allowed to take a car at all, so a car that was just placed can never
 * be the one the cull gives up on. */
#define NEAR_BUBBLE_GRACE_MS		5000

/* The answer of the ladder for one candidate. */
enum ENearBubbleAnswer
{
	NEAR_BUBBLE_REFUSED = 0,	/* the event is not allowed here */
	NEAR_BUBBLE_ALLOWED,		/* the position is hidden: the event may happen */
	NEAR_BUBBLE_SEEK_LOS		/* off the frustum and close: a blocked ray allows it */
};

/* One candidate as the frame sees it. The caller fills the facts from the
 * engine (TheCamera, COcclusion, CurrentTrafficRings) and the harness fills
 * them from its own matrix. */
struct SNearBubbleCandidate
{
	float distToPlayer;	/* 2D: the bubble is a disc on the map */
	float distToCamera;	/* 3D, the distance the renderer fades with */
	float visibleRange;	/* CurrentTrafficRings().visibleRange */
	bool inFrustum;		/* TheCamera.IsSphereVisible(pos, radius) */
	bool occluded;		/* COcclusion::IsPositionOccluded(pos, radius) */
	bool fadeHides;		/* FillBurstIsHidden(): the fade covers the screen */
	bool bypassView;	/* the top-down camera: the view is not the player's */
};

static inline ENearBubbleAnswer
NearBubbleAnswerFor(const SNearBubbleCandidate &c)
{
	/* The bubble. Nothing appears or disappears in it, whatever the view,
	 * the fade or the camera mode: a car placed at the player's elbow is
	 * there when the view that covered it is gone. */
	if (c.distToPlayer <= NEAR_BUBBLE_RADIUS)
		return NEAR_BUBBLE_REFUSED;
	/* The top-down camera looks at the map from above, where the street level
	 * is not the subject and the traffic must keep flowing: the visibility
	 * arms below step aside, the bubble above does not. */
	if (c.bypassView)
		return NEAR_BUBBLE_ALLOWED;
	/* The fade of a transition covers the world: the fill burst fills
	 * behind it (VICE-126) and the counters mark the events exempt. */
	if (c.fadeHides)
		return NEAR_BUBBLE_ALLOWED;
	/* The occluder volumes: the camera cannot see the position at all. */
	if (c.occluded)
		return NEAR_BUBBLE_ALLOWED;
	/* The fade and the fog: the world is not drawn at this distance, whether
	 * the frustum covers the position or not. A visible range of zero is the
	 * boot path before the time cycle and asks for the ray instead. */
	if (c.visibleRange > 0.0f && c.distToCamera >= c.visibleRange)
		return NEAR_BUBBLE_ALLOWED;
	/* The blind spot: off the frustum and close enough for a ray. */
	if (!c.inFrustum && c.distToCamera <= NEAR_BUBBLE_LOS_RANGE)
		return NEAR_BUBBLE_SEEK_LOS;
	/* In view, inside the fade, nothing between the camera and the position:
	 * the player would catch this one. */
	return NEAR_BUBBLE_REFUSED;
}

/* The catchable test of the counters, without a ray: the position is in the
 * frustum, inside the fade and not occluded, so the player could catch a change
 * there. Every arm of the ladder above keeps an allowed event clear of it. */
static inline bool
NearBubblePositionIsCatchable(const SNearBubbleCandidate &c)
{
	return c.inFrustum && !c.occluded &&
		(c.visibleRange <= 0.0f || c.distToCamera < c.visibleRange);
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
/* The ray reaches the stock ring and the stock despawn, so a close candidate
 * of the stock band is never refused for its length alone. */
static_assert(NEAR_BUBBLE_LOS_RANGE >= RING_CAR_DESPAWN,
	"the line-of-sight ray must reach the stock despawn");
static_assert(NEAR_BUBBLE_LOS_RAYS_PER_FRAME > 0, "the ray budget must make progress");
static_assert(NEAR_BUBBLE_ENTITY_RADIUS > 0.0f, "the visibility tests need a radius");
static_assert(NEAR_BUBBLE_GRACE_MS > 0, "the fresh-spawn grace must last a frame at least");

#endif // __GTA_NEARBUBBLE_H__
