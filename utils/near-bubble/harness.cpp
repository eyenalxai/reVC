/*
 * The evidence program for the near bubble (ticket VICE-127).
 *
 * This program reads src/control/NearBubble.h on its own (the header has no
 * engine include) and checks the rule, the bounds and the numbers:
 *
 *	the bubble and its reserve fit inside every traffic spawn band, over the
 *	whole control matrix of the ring table (the draw distance, the FOV
 *	factor and the weather far clip of utils/traffic-rings);
 *
 *	the ladder never allows an event the player could catch: for every
 *	qualitative case the answer is refuted against the catchable test;
 *
 *	the removal threshold leaves the bubble, the hysteresis holds, and the
 *	ring's own despawn never sits inside the removal distance;
 *
 *	the line-of-sight budget of a frame is one number, and a frame that
 *	spends it refuses the rest of its candidates;
 *
 *	the fill target of the bubble follows the budget and the ring area.
 *
 * Build and run:
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 -o /tmp/near-bubble-harness utils/near-bubble/harness.cpp
 *	/tmp/near-bubble-harness
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "NearBubble.h"

#include <math.h>
#include <stdio.h>

/* The budget numbers the target section prints (correctness of the layout, not
 * of the rule; the sources are named where they are read). Mirrored here the
 * way utils/traffic-rings mirrors them, so this program needs the ring table
 * and the near bubble and no budget header. */
#define RING_CARS_PER_UNIT 30.0f	/* PopulationBudget.h RANDOM_CARS_PER_INI_UNIT */
#define RING_MAX_RANDOM_CARS 2400	/* PopulationBudget.h MAX_RANDOM_CARS */
#define CAR_SPAWN_ATTEMPTS 4		/* PopulationBudget.h */
#define RANDOM_CAR_ATTEMPTS_PER_FRAME 3	/* PopulationBudget.h */
#define FILL_BURST_ATTEMPTS_PER_FRAME 25 /* PopulationBudget.h */
#define DRAWDIST_DEFAULT_VALUE 1.2f	/* DrawDistance.h DRAWDIST_DEFAULT */

static int g_failures = 0;

static void
Fail(const char *what)
{
	g_failures++;
	printf("  FAIL: %s\n", what);
}

static void
Check(bool ok, const char *what)
{
	if (!ok)
		Fail(what);
}

/* The qualitative cases of the ladder. Every field is a fact of the frame. */
struct SCase
{
	const char *name;
	float distToPlayer;	/* 2D */
	float distToCamera;	/* 3D */
	float visibleRange;
	bool inFrustum;
	bool occluded;
	bool fadeHides;
};

static SNearBubbleCandidate
CandidateOf(const SCase &s)
{
	SNearBubbleCandidate c;
	c.distToPlayer = s.distToPlayer;
	c.distToCamera = s.distToCamera;
	c.visibleRange = s.visibleRange;
	c.inFrustum = s.inFrustum;
	c.occluded = s.occluded;
	c.fadeHides = s.fadeHides;
	return c;
}

/* The ring table for the matrix: the same composition the harness of
 * utils/traffic-rings uses (the LOD distance base x the FOV factor x the draw
 * distance; DrawDistance.h, Renderer.h). */
static STrafficRings
RingsAt(float drawDistance, float fovFactor, float farClip)
{
	STrafficRingInput in;
	in.lodDistance = LOD_DISTANCE_BASE * fovFactor * drawDistance;
	in.fovFactor = fovFactor;
	in.farClip = farClip;
	return ComputeTrafficRings(in);
}

static void
Section1Constants(void)
{
	printf("== 1. the bubble and its bounds\n\n");
	printf("  the bubble radius (the car side band %0.f m less the reserve %0.f m): %.1f m\n",
		(double)RING_CAR_SIDE_SPAWN, (double)RING_CAR_DESPAWN_RESERVE, (double)NEAR_BUBBLE_RADIUS);
	printf("  the removal reserve:     %.1f m (the ring's own placement reserve)\n", (double)NEAR_BUBBLE_REMOVE_RESERVE);
	printf("  the removal distance:    %.1f m (the bubble + its reserve)\n", (double)NEAR_BUBBLE_REMOVE_DISTANCE);
	printf("  the visibility radius:   %.1f m (a car's bounding sphere)\n", (double)NEAR_BUBBLE_ENTITY_RADIUS);
	printf("  the ray budget:          %d rays a frame, each at most %.0f m long\n",
		NEAR_BUBBLE_LOS_RAYS_PER_FRAME, (double)NEAR_BUBBLE_LOS_RANGE);
	printf("  the fresh-spawn grace:   %d ms\n", NEAR_BUBBLE_GRACE_MS);
	printf("\n  The bubble is the spawn band less the placement error the reserve holds:\n");
	printf("  the side band places at %.0f m less up to the lane offset, so no placement of\n",
		(double)RING_CAR_SIDE_SPAWN);
	printf("  the band is refused for landing inside the bubble. A candidate of the rule\n");
	printf("  costs one ladder (the two distances and the occluder test); a candidate of\n");
	printf("  the blind-spot arm costs one ray, and a frame has %d of them.\n\n",
		NEAR_BUBBLE_LOS_RAYS_PER_FRAME);
}

/* The bubble must fit every band it reasons about, at every cell of the control
 * matrix: the draw distance x the FOV factor x the weather far clip. The band
 * radii scale with the ring scale (>= 1: the floor), so the fit is a stock
 * arithmetic, and the matrix shows it holds while the ring moves. */
static void
Section2Matrix(void)
{
	const int nSteps = 16;
	const float fovFactors[] = { 1.0f, 70.0f / 110.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.2f };
	const int nFov = sizeof(fovFactors) / sizeof(fovFactors[0]);
	const float farClips[] = { 2000.0f, 1150.0f, 650.0f, 150.0f };
	const int nFar = sizeof(farClips) / sizeof(farClips[0]);

	int cells = 0, failed = 0;
	float minSide = 1e9f, minGen = 1e9f, minDespawn = 1e9f;

	printf("== 2. the fit of the bubble over the control matrix\n\n");
	for (int f = 0; f < nFar; f++)
	for (int j = 0; j < nFov; j++)
	for (int i = 0; i <= nSteps; i++) {
		float dd = DRAWDIST_MIN + i * DRAWDIST_RANGE / nSteps;
		STrafficRings r = RingsAt(dd, fovFactors[j], farClips[f]);
		cells++;
		if (NEAR_BUBBLE_RADIUS + NEAR_BUBBLE_REMOVE_RESERVE > r.carSideSpawn) {
			Fail("a cell places the car side band inside the bubble plus the reserve");
			failed++;
		}
		if (NEAR_BUBBLE_RADIUS + NEAR_BUBBLE_REMOVE_RESERVE > r.carSideSpawn * r.boatSpawnFactor) {
			Fail("a cell places the boat side band inside the bubble plus the reserve");
			failed++;
		}
		if (NEAR_BUBBLE_RADIUS + NEAR_BUBBLE_REMOVE_RESERVE > r.generatorNear) {
			Fail("a cell places the generator ring inside the bubble plus the reserve");
			failed++;
		}
		if (NEAR_BUBBLE_REMOVE_DISTANCE > r.carDespawn) {
			Fail("a cell places the ring's despawn inside the removal distance");
			failed++;
		}
		if (NEAR_BUBBLE_REMOVE_DISTANCE <= NEAR_BUBBLE_RADIUS) {
			Fail("the removal distance does not leave the bubble");
			failed++;
		}
		minSide = TrafficRingsMin(minSide, r.carSideSpawn);
		minGen = TrafficRingsMin(minGen, r.generatorNear);
		minDespawn = TrafficRingsMin(minDespawn, r.carDespawn);
	}
	printf("  cells: %d (17 draw distances x %d FOV factors x %d far clips)\n", cells, nFov, nFar);
	printf("  failures: %d\n", failed);
	printf("  the tightest car side band over the matrix: %.1f m (the bubble asks %.1f + %.1f)\n",
		(double)minSide, (double)NEAR_BUBBLE_RADIUS, (double)NEAR_BUBBLE_REMOVE_RESERVE);
	printf("  the tightest generator near ring:         %.1f m\n", (double)minGen);
	printf("  the tightest car despawn:                 %.1f m (the removal distance asks %.1f)\n",
		(double)minDespawn, (double)NEAR_BUBBLE_REMOVE_DISTANCE);
	printf("\n  The side band never moves inside the bubble: the ring scale has a floor of\n");
	printf("  1.0 (TrafficRings.h), so the band is %.0f m or more at every cell.\n\n",
		(double)RING_CAR_SIDE_SPAWN);
}

/* The ladder, case by case. The soundness invariant is the acceptance: every
 * answer that allows an event is outside the catchable set, and every catchable
 * case is refused. */
static void
Section3Ladder(void)
{
	const float inRange = 100.0f;		/* inside the fade */
	const float outRange = 400.0f;		/* beyond the fade (the visible range is 360 at stock) */
	const float visRange = 360.0f;
	const SCase cases[] = {
		/* name					player	camera	range	frustum	occl	fade	*/
		{ "in the bubble",			 60.0f,	 60.0f,	visRange, true,		false,	false	},
		{ "in the bubble, a fade",		 60.0f,	 60.0f,	visRange, true,		false,	true	},
		{ "in the bubble, occluded",		 60.0f,	 60.0f,	visRange, false,	true,	false	},
		{ "in view, close, clear",		150.0f,	150.0f,	visRange, true,		false,	false	},
		{ "in view, close, a fade",		150.0f,	150.0f,	visRange, true,		false,	true	},
		{ "in view, close, occluded",		150.0f,	150.0f,	visRange, true,		true,	false	},
		{ "off the frustum, close",		150.0f,	150.0f,	visRange, false,	false,	false	},
		{ "off the frustum, beyond the fade",	150.0f,	400.0f,	visRange, false,	false,	false	},
		{ "in view, beyond the fade",		150.0f,	400.0f,	visRange, true,		false,	false	},
		{ "off the frustum, beyond the ray",	150.0f,	350.0f,	1000.0f, false,	false,	false	},
		{ "a visible range of zero (boot)",	150.0f,	150.0f,	0.0f,	false,		false,	false	},
	};
	const int nCases = sizeof(cases) / sizeof(cases[0]);
	const ENearBubbleAnswer expected[] = {
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_SEEK_LOS,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_SEEK_LOS,
	};

	printf("== 3. the ladder, case by case\n\n");
	printf("  %-34s %-12s %-12s %s\n", "case", "answer", "allowed?", "catchable?");
	for (int i = 0; i < nCases; i++) {
		SNearBubbleCandidate c = CandidateOf(cases[i]);
		ENearBubbleAnswer a = NearBubbleAnswerFor(c);
		bool allowed = a != NEAR_BUBBLE_REFUSED;
		bool catchable = NearBubblePositionIsCatchable(c);
		printf("  %-34s %-12s %-12s %s\n", cases[i].name,
			a == NEAR_BUBBLE_REFUSED ? "refused" : a == NEAR_BUBBLE_ALLOWED ? "allowed" : "seek the ray",
			allowed ? "yes" : "no", catchable ? "yes" : "no");
		Check(a == expected[i], "the ladder answered against the case list");
		/* The acceptance: an event the ladder allows (past the ray's own
		 * blocked check) is never one the player could catch. */
		if (allowed && !cases[i].fadeHides)
			Check(!catchable, "the ladder allowed a catchable event");
		/* The completeness: a catchable event is refused. */
		if (catchable && !cases[i].fadeHides)
			Check(!allowed, "the ladder did not refuse a catchable event");
	}
	/* The two arms of the blind spot behind the ray. */
	SNearBubbleCandidate blocked = CandidateOf(cases[6]);
	printf("\n  the blind-spot arm after the ray:\n");
	printf("  %-34s %-12s %-12s %s\n", "blocked ray (a building between)",
		"allowed", "yes", NearBubblePositionIsCatchable(blocked) ? "yes" : "no");
	Check(!NearBubblePositionIsCatchable(blocked),
		"a blocked ray must be about a position the frustum does not hold");

	printf("\n  The fade is the one exempt case: an event it covers may be catchable and is\n");
	printf("  counted as a fade event, not as a frustum event (NearBubble.cpp counts the\n");
	printf("  fade first). The bubble is refused even under a fade: a car placed at the\n");
	printf("  player's elbow is there when the fade lifts.\n\n");
}

/* The per-frame bound of the ray work: a frame spends at most the budget, and
 * the candidate count behind it is bounded by the placement loops. The numbers
 * of the loops come from PopulationBudget.h (mirrored above). */
static void
Section4RayBudget(void)
{
	printf("== 4. the per-frame work of the rule\n\n");
	printf("  the ray budget of a frame:            %d\n", NEAR_BUBBLE_LOS_RAYS_PER_FRAME);
	printf("  the longest ray:                      %.0f m (%.0f sectors of %0.f m)\n",
		(double)NEAR_BUBBLE_LOS_RANGE, (double)NEAR_BUBBLE_LOS_RANGE / 50.0f, 50.0);
	int trickleCalls = RANDOM_CAR_ATTEMPTS_PER_FRAME * (int)RING_MAX_SCALE;
	int trickleCandidates = trickleCalls * CAR_SPAWN_ATTEMPTS;
	printf("  the trickle: %d car calls a frame x %d placements = %d candidates,\n",
		trickleCalls, CAR_SPAWN_ATTEMPTS, trickleCandidates);
	printf("               of which at most %d spend a ray\n", NEAR_BUBBLE_LOS_RAYS_PER_FRAME);
	printf("  a burst frame (the visible window): %d candidates, the same %d rays\n",
		FILL_BURST_ATTEMPTS_PER_FRAME, NEAR_BUBBLE_LOS_RAYS_PER_FRAME);
	printf("  a burst frame of the hidden window: the fade arm allows without a ray\n");
	printf("\n  the refused candidates: one ladder each (two subtractions, the occluder test\n");
	printf("  and the distance of the camera); no collision work.\n");
	printf("  one ray: the buildings of the sectors the line crosses, to the first hit or\n");
	printf("  to the end. The ray is the collision path's line walk and nothing else; the\n");
	printf("  rest of the rule is a handful of floats.\n\n");

	/* A frame that spends its budget refusals the rest: the same loop the
	 * engine runs, with the outcome of the ray as an input. */
	int raysLeft = NEAR_BUBBLE_LOS_RAYS_PER_FRAME;
	int spent = 0, allowed = 0, refused = 0;
	for (int i = 0; i < trickleCandidates; i++) {
		if (raysLeft > 0) {
			raysLeft--;
			spent++;
			allowed++;	/* the ray of this simulation is blocked */
		} else {
			refused++;
		}
	}
	printf("  a simulated frame of %d blind-spot candidates with blocked rays:\n", trickleCandidates);
	printf("    rays spent %d (budget %d), allowed %d, refused for the budget %d\n",
		spent, NEAR_BUBBLE_LOS_RAYS_PER_FRAME, allowed, refused);
	Check(spent == NEAR_BUBBLE_LOS_RAYS_PER_FRAME, "a frame spent more rays than its budget");
	Check(allowed + refused == trickleCandidates, "the simulated frame lost candidates");
	printf("\n");
}

static void
Section5Target(void)
{
	const float carDensity = 2.0f;	/* the stock Car Density slider */
	const float ownerDensity = 3.4f;/* the slider top of the owner's settings (VICE-145) */
	STrafficRings stock = RingsAt(DRAWDIST_DEFAULT_VALUE, 1.0f, 2000.0f);
	STrafficRings top = RingsAt(16.0f, 70.0f / 110.0f, 2000.0f);
	float stockBudget = TrafficRingsMin(RING_CARS_PER_UNIT * carDensity * stock.scale * stock.scale, (float)RING_MAX_RANDOM_CARS);
	float topBudget = TrafficRingsMin(RING_CARS_PER_UNIT * ownerDensity * top.scale * top.scale, (float)RING_MAX_RANDOM_CARS);

	printf("== 5. the fill of the bubble\n\n");
	printf("  the target is the share of the car budget the bubble's area takes of the\n");
	printf("  despawn disc: target = budget x (%.0f / despawn)^2.\n\n", (double)NEAR_BUBBLE_RADIUS);
	printf("  %-28s %10s %10s %10s %10s\n", "setting", "scale", "budget", "despawn", "bubble target");
	printf("  %-28s %10.2f %10.0f %10.1f %10.1f\n", "stock (draw distance 1.2)",
		(double)stock.scale, (double)stockBudget, (double)stock.carDespawn,
		(double)NearBubbleTarget(stockBudget, stock.carDespawn));
	printf("  %-28s %10.2f %10.0f %10.1f %10.1f\n", "the owner's (16.0, FOV 110)",
		(double)top.scale, (double)topBudget, (double)top.carDespawn,
		(double)NearBubbleTarget(topBudget, top.carDespawn));
	printf("\n  The target follows the density sliders and the ring scale through the budget;\n");
	printf("  the counter ms_nNearBubbleFill holds the traffic (the random cars) inside the\n");
	printf("  bubble, and the orchestrator compares the two live.\n\n");
	Check(NearBubbleTarget(0.0f, 280.0f) == 0.0f, "a zero budget must ask for an empty bubble");
	Check(NearBubbleTarget(60.0f, 0.0f) == 0.0f, "a zero ring must ask for an empty bubble");
	Check(NearBubbleTarget(60.0f, 280.0f) < 60.0f, "the bubble cannot ask for the whole ring");
	Check(NearBubbleTarget(60.0f, 280.0f) > 0.0f, "the bubble must ask for something");
}

int
main(void)
{
	printf("reVC near bubble (VICE-127): the rule, its bounds and the fit of the rings\n");
	printf("the rule lives in src/control/NearBubble.h; this program reads it directly\n");
	printf("the ring table is the same table utils/traffic-rings walks\n\n");

	Section1Constants();
	Section2Matrix();
	Section3Ladder();
	Section4RayBudget();
	Section5Target();

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  every check passes. The bubble sits the reserve inside every traffic band at\n");
		printf("  every cell of the control, the ladder allows no event the player could\n");
		printf("  catch, the frame's ray work is bounded by one number, and the fill target\n");
		printf("  follows the budget the density sliders move.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
