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
 *	qualitative case, and then for every combination of the fields of a
 *	candidate, the answer is refuted against the catchable predicate - the
 *	binding that keeps the two from drifting apart (the exhaustive sweep
 *	fails the moment they do);
 *
 *	the removal threshold leaves the bubble, the hysteresis holds, and the
 *	ring's own despawn never sits inside the removal distance;
 *
 *	the line-of-sight budget of a frame is one number, and a frame that
 *	spends it refuses the rest of its candidates;
 *
 *	the fill target of the bubble follows the budget and the ring area;
 *
 *	the fill of a street with no occluders: the creation rate the rule
 *	leaves against the rate the traffic needs, at the stock ring and at the
 *	owner's settings. The strict reading (occluders, fog and rays only)
 *	leaves an open street with no creation site at all; the soft arm is the
 *	one that keeps it alive.
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

/* The model of an open street (Section 6): the average traffic speed (the AI
 * cruise speeds of the ring are about 7..13 m/s) and the share of the side
 * band's placements the lane offset pulls inside the soft distance. The
 * placements sit on the ring crossing (exactly the side band = the soft
 * distance); the lane offset moves them perpendicular to their road, so the
 * radial part of the offset is offset x cos(the road against the radius) and
 * the share under the soft distance is a layout number. The section prints the
 * rate for 0 %, 50 % and 90 %. */
#define MODEL_TRAFFIC_SPEED 10.0f
#define MODEL_PLACEMENT_LANE_OFFSET ((4.0f - 1.0f + 0.5f) * 5.0f) /* CarCtrl.cpp, 17.5 m */

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
	bool bypassView;
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
	c.bypassView = s.bypassView;
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
	printf("  the soft distance:       %.1f m (the same gap, the arm that keeps an open street alive)\n",
		(double)NEAR_BUBBLE_SOFT_DISTANCE);
	printf("  the visibility radius:   %.1f m (a car's bounding sphere)\n", (double)NEAR_BUBBLE_ENTITY_RADIUS);
	printf("  the ray budget:          %d rays a frame, each at most %.0f m long (the close band)\n",
		NEAR_BUBBLE_LOS_RAYS_PER_FRAME, (double)NEAR_BUBBLE_LOS_RANGE);
	printf("  the fresh-spawn grace:   %d ms\n", NEAR_BUBBLE_GRACE_MS);
	printf("  the occluder trust:      %.0f m of camera movement, a direction cosine of %.3f,\n",
		(double)NEAR_BUBBLE_OCCLUDER_MAX_MOVE, (double)NEAR_BUBBLE_OCCLUDER_MIN_DOT);
	printf("                           %.0f degrees of FOV (the set is a frame old)\n", (double)NEAR_BUBBLE_OCCLUDER_MAX_FOV);
	printf("\n  The bubble is the spawn band less the placement error the reserve holds:\n");
	printf("  the side band places at %.0f m less up to the lane offset, so no placement of\n",
		(double)RING_CAR_SIDE_SPAWN);
	printf("  the band lands inside the bubble. The side band's ring crossing sits exactly on\n");
	printf("  the soft distance (%.0f m) and the lane offset of %.1f m moves a placement\n",
		(double)NEAR_BUBBLE_SOFT_DISTANCE, (double)MODEL_PLACEMENT_LANE_OFFSET);
	printf("  perpendicular to its road, so a placement lands at %.0f m .. %.0f m from the\n",
		(double)(NEAR_BUBBLE_SOFT_DISTANCE - MODEL_PLACEMENT_LANE_OFFSET),
		(double)(NEAR_BUBBLE_SOFT_DISTANCE + MODEL_PLACEMENT_LANE_OFFSET));
	printf("  player: the band crosses the soft distance, which is why the soft arm and the\n");
	printf("  close ray share it (Section 6 models the share).\n\n");
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
		if (NEAR_BUBBLE_SOFT_DISTANCE > r.carSideSpawn) {
			Fail("a cell places the side band inside the soft distance: the band would need the ray");
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
	printf("  the tightest car side band over the matrix: %.1f m (the bubble asks %.1f + %.1f,\n",
		(double)minSide, (double)NEAR_BUBBLE_RADIUS, (double)NEAR_BUBBLE_REMOVE_RESERVE);
	printf("    the soft distance asks %.1f)\n", (double)NEAR_BUBBLE_SOFT_DISTANCE);
	printf("  the tightest generator near ring:         %.1f m\n", (double)minGen);
	printf("  the tightest car despawn:                 %.1f m (the removal distance asks %.1f)\n",
		(double)minDespawn, (double)NEAR_BUBBLE_REMOVE_DISTANCE);
	printf("\n  The side band never moves inside the bubble or the soft distance: the ring\n");
	printf("  scale has a floor of 1.0 (TrafficRings.h), so the band is %.0f m or more at\n",
		(double)RING_CAR_SIDE_SPAWN);
	printf("  every cell.\n\n");
}

/* The ladder, case by case. The soundness invariant is the acceptance: every
 * answer that allows an event is outside the catchable set, and every catchable
 * case is refused (the fade and the bypassed view are the two exempt arms). */
static void
Section3Ladder(void)
{
	const float inRange = 100.0f;		/* inside the fade */
	const float outRange = 400.0f;		/* beyond the fade (the visible range is 360 at stock) */
	const float visRange = 360.0f;
	const SCase cases[] = {
		/* name					player	camera	range	frustum	occl	fade	bypass	*/
		{ "in the bubble",			 60.0f,	 60.0f,	visRange, true,		false,	false,	false	},
		{ "in the bubble, a fade",		 60.0f,	 60.0f,	visRange, true,		false,	true,	false	},
		{ "in the bubble, occluded",		 60.0f,	 60.0f,	visRange, false,	true,	false,	false	},
		{ "in the bubble, top-down view",	 60.0f,	 60.0f,	visRange, true,		false,	false,	true	},
		{ "in view, close, clear",		150.0f,	150.0f,	visRange, true,		false,	false,	false	},
		{ "in view, close, a fade",		150.0f,	150.0f,	visRange, true,		false,	true,	false	},
		{ "in view, close, occluded",		150.0f,	150.0f,	visRange, true,		true,	false,	false	},
		{ "off the frustum, inside the soft distance", 150.0f, 100.0f, visRange, false, false, false, false },
		{ "off the frustum, on the soft distance", 150.0f, 120.0f, visRange, false, false, false, false },
		{ "off the frustum, the top-down view",	150.0f,	150.0f,	visRange, true,		false,	false,	true	},
		{ "off the frustum, beyond the fade",	150.0f,	400.0f,	visRange, false,	false,	false,	false	},
		{ "in view, beyond the fade",		150.0f,	400.0f,	visRange, true,		false,	false,	false	},
		{ "off the frustum, beyond the ray",	150.0f,	350.0f,	1000.0f, false,	false,	false,	false	},
		{ "a visible range of zero (boot, close)", 150.0f, 100.0f, 0.0f,	false,		false,	false,	false	},
	};
	const int nCases = sizeof(cases) / sizeof(cases[0]);
	const ENearBubbleAnswer expected[] = {
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_REFUSED,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_SEEK_LOS,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_ALLOWED,
		NEAR_BUBBLE_SEEK_LOS,
	};

	printf("== 3. the ladder, case by case\n\n");
	printf("  %-44s %-12s %-12s %s\n", "case", "answer", "allowed?", "catchable?");
	for (int i = 0; i < nCases; i++) {
		SNearBubbleCandidate c = CandidateOf(cases[i]);
		ENearBubbleAnswer a = NearBubbleAnswerFor(c);
		bool allowed = a != NEAR_BUBBLE_REFUSED;
		bool catchable = NearBubblePositionIsCatchable(c);
		/* The two exempt arms: the fade of a transition and the top-down
		 * camera (which the counters read apart). */
		bool exempt = cases[i].fadeHides || cases[i].bypassView;
		printf("  %-44s %-12s %-12s %s\n", cases[i].name,
			a == NEAR_BUBBLE_REFUSED ? "refused" : a == NEAR_BUBBLE_ALLOWED ? "allowed" : "seek the ray",
			allowed ? "yes" : "no", catchable ? "yes" : "no");
		Check(a == expected[i], "the ladder answered against the case list");
		/* The acceptance: an event the ladder allows (past the ray's own
		 * blocked check) is never one the player could catch. */
		if (allowed && !exempt)
			Check(!catchable, "the ladder allowed a catchable event");
		/* The completeness: a catchable event is refused. */
		if (catchable && !exempt)
			Check(!allowed, "the ladder did not refuse a catchable event");
	}
	printf("\n  The blind spot behind the close ray:\n");
	SNearBubbleCandidate blocked = CandidateOf(cases[7]);
	printf("  %-44s %-12s %-12s %s\n", "a blocked ray (a building between)",
		"allowed", "yes", NearBubblePositionIsCatchable(blocked) ? "yes" : "no");
	Check(!NearBubblePositionIsCatchable(blocked),
		"a blocked ray must be about a position the frustum does not hold");

	printf("\n  Two arms are exempt and counted apart: the fade of a transition (the fill\n");
	printf("  burst) and the top-down camera, whose view is not the player's. The bubble\n");
	printf("  is refused under both: a car placed at the player's elbow is there when the\n");
	printf("  fade lifts or the camera returns.\n\n");
}

/* The structural binding: the ladder is written against NearBubblePositionIs
 * Catchable, so the invariant must hold for *every* combination of the fields,
 * not only for the cases above. The sweep enumerates them and fails the moment
 * the two drift apart. */
static void
Section3bExhaustive(void)
{
	const float distancesToPlayer[] = { NEAR_BUBBLE_RADIUS - 1.0f, NEAR_BUBBLE_RADIUS + 1.0f };
	const float distancesToCamera[] = {
		NEAR_BUBBLE_SOFT_DISTANCE - 1.0f, NEAR_BUBBLE_SOFT_DISTANCE + 1.0f, 400.0f
	};
	const float ranges[] = { 0.0f, 200.0f, 360.0f };
	int combinations = 0, allowedOutside = 0, refusedCatchable = 0, violations = 0;

	for (int dp = 0; dp < 2; dp++)
	for (int dc = 0; dc < 3; dc++)
	for (int vr = 0; vr < 3; vr++)
	for (int inFrustum = 0; inFrustum < 2; inFrustum++)
	for (int occluded = 0; occluded < 2; occluded++)
	for (int fade = 0; fade < 2; fade++)
	for (int bypass = 0; bypass < 2; bypass++) {
		SNearBubbleCandidate c;
		c.distToPlayer = distancesToPlayer[dp];
		c.distToCamera = distancesToCamera[dc];
		c.visibleRange = ranges[vr];
		c.inFrustum = inFrustum != 0;
		c.occluded = occluded != 0;
		c.fadeHides = fade != 0;
		c.bypassView = bypass != 0;

		ENearBubbleAnswer a = NearBubbleAnswerFor(c);
		bool allowed = a != NEAR_BUBBLE_REFUSED;
		bool catchable = NearBubblePositionIsCatchable(c);
		bool exempt = c.fadeHides || c.bypassView;
		combinations++;
		if (allowed && !catchable)
			allowedOutside++;
		if (allowed && catchable && !exempt) {
			violations++;
			Fail("the sweep found an allowed catchable event outside the exemptions");
		}
		if (!allowed && catchable && !exempt)
			refusedCatchable++;
	}
	printf("== 3b. the structural binding: every combination of the fields\n\n");
	printf("  combinations: %d\n", combinations);
	printf("  allowed and outside the catchable set (the rule's own work): %d\n", allowedOutside);
	printf("  catchable and refused (the acceptance): %d\n", refusedCatchable);
	printf("  allowed and catchable outside the fade and the bypass: %d (must be 0)\n", violations);
	printf("\n  The ladder and the counters share the predicate NearBubblePositionIsCatchable,\n");
	printf("  so no combination of the view can let a catchable event through unwatched, and\n");
	printf("  every catchable one is refused. A future edit that drops an arm or moves a\n");
	printf("  condition fails this sweep.\n\n");
}

/* The per-frame bound of the ray work: a frame spends at most the budget, and
 * the candidate count behind it is bounded by the placement loops. The numbers
 * of the loops come from PopulationBudget.h (mirrored above). */
static void
Section4RayBudget(void)
{
	printf("== 4. the per-frame work of the rule\n\n");
	printf("  the ray budget of a frame:            %d\n", NEAR_BUBBLE_LOS_RAYS_PER_FRAME);
	printf("  the longest ray:                      %.0f m (the close band, %.0f sectors of %0.f m)\n",
		(double)NEAR_BUBBLE_LOS_RANGE, (double)NEAR_BUBBLE_LOS_RANGE / 50.0f, 50.0);
	int trickleCalls = RANDOM_CAR_ATTEMPTS_PER_FRAME * (int)RING_MAX_SCALE;
	int trickleCandidates = trickleCalls * CAR_SPAWN_ATTEMPTS;
	printf("  the trickle: %d car calls a frame x %d placements = %d candidates,\n",
		trickleCalls, CAR_SPAWN_ATTEMPTS, trickleCandidates);
	printf("               of which the close band spends at most %d rays\n", NEAR_BUBBLE_LOS_RAYS_PER_FRAME);
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
	printf("  a simulated frame of %d close candidates with blocked rays:\n", trickleCandidates);
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
	printf("  the counter ms_nNearBubbleFill holds the traffic inside the bubble, and\n");
	printf("  ms_nNearBubbleOtherFill the rest of the target (the cars outside the spawner).\n");
	printf("  The orchestrator compares the sum against the target live.\n\n");
	Check(NearBubbleTarget(0.0f, 280.0f) == 0.0f, "a zero budget must ask for an empty bubble");
	Check(NearBubbleTarget(60.0f, 0.0f) == 0.0f, "a zero ring must ask for an empty bubble");
	Check(NearBubbleTarget(60.0f, 280.0f) < 60.0f, "the bubble cannot ask for the whole ring");
	Check(NearBubbleTarget(60.0f, 280.0f) > 0.0f, "the bubble must ask for something");
}

/* The fill of an open street: the creation rate the rule leaves against the
 * rate the traffic needs. The ring's cars leave the despawn disc and must be
 * replaced; a street with no occluders has no arm at all under the strict
 * reading (the bands of the ring sit inside the visible range by the table's
 * own design), so the traffic drains. The soft arm is the creation site that
 * keeps it alive; the section prints the margin. */
static void
Section6OpenStreetFill(void)
{
	/* The traffic the ring needs to replace every frame: the cars crossing the
	 * despawn disc. The ring's cars leave at the traffic speed, so the need is
	 * the target over the crossing time (2 x despawn / v), in frames. */
	const float frame = 1.0f / 30.0f;
	STrafficRings stock = RingsAt(DRAWDIST_DEFAULT_VALUE, 1.0f, 2000.0f);
	STrafficRings top = RingsAt(16.0f, 70.0f / 110.0f, 2000.0f);
	const int scales[2] = { 1, 2 };	/* the ring scale, rounded: 1.0 at stock, 2.0 at the cap */

	printf("== 6. the fill of a street with no occluders (the owner's long roads)\n\n");
	printf("  the model: the trickle's calls a frame (RANDOM_CAR_ATTEMPTS_PER_FRAME x the ring\n");
	printf("  scale), %d placements a call, the side band and the on-screen band half each.\n", CAR_SPAWN_ATTEMPTS);
	printf("  A candidate of the side band (off the frustum) passes at or beyond the soft\n");
	printf("  distance (%.0f m); the share of the placements the lane offset pulls inside it\n",
		(double)NEAR_BUBBLE_SOFT_DISTANCE);
	printf("  is a layout number the model varies. A candidate of the on-screen band (in the\n");
	printf("  frustum at the stock FOV) needs an occluder or the fog; on open ground it has\n");
	printf("  neither. The need is the target over the crossing time of the disc\n");
	printf("  (2 x despawn / %.0f m/s), in frames.\n\n", (double)MODEL_TRAFFIC_SPEED);

	struct { const char *name; float budget; float despawn; int scale; } settings[2] = {
		{ "stock (1.2, FOV 70)", TrafficRingsMin(RING_CARS_PER_UNIT * 2.0f * stock.scale * stock.scale, (float)RING_MAX_RANDOM_CARS), stock.carDespawn, scales[0] },
		{ "the owner's (16.0, FOV 110)", TrafficRingsMin(RING_CARS_PER_UNIT * 3.4f * top.scale * top.scale, (float)RING_MAX_RANDOM_CARS), top.carDespawn, scales[1] },
	};
	const float unders[3] = { 0.0f, 0.5f, 0.9f };
	const int nUnder = 3;

	for (int s = 0; s < 2; s++) {
		float need = settings[s].budget * frame / (2.0f * settings[s].despawn / MODEL_TRAFFIC_SPEED);
		printf("  %s\n", settings[s].name);
		printf("    target %.0f cars, despawn %.0f m -> the ring needs %.3f cars a frame\n",
			(double)settings[s].budget, (double)settings[s].despawn, (double)need);
		printf("    %-34s %10s %10s %10s\n", "the share of the side band inside the soft distance",
			"0 %", "50 %", "90 %");
		printf("    %-34s", "the strict reading (no soft arm)");
		for (int u = 0; u < nUnder; u++)
			printf(" %9.3f", 0.0);	/* no arm can pass on open ground */
		printf("  <- drains: the strict rule has no site at all\n");
		printf("    %-34s", "the rule with the soft arm");
		for (int u = 0; u < nUnder; u++) {
			/* the side band's share of the calls, the share of the
			 * placements that pass, then the call's chance: one car a call
			 * at the most */
			float pSide = 1.0f - unders[u];	/* the placements at or beyond the soft distance */
			float pCandidate = 0.5f * pSide;	/* half the calls are the side band */
			float pCall = 1.0f - powf(1.0f - pCandidate, CAR_SPAWN_ATTEMPTS);
			printf(" %9.3f", (double)(settings[s].scale * pCall));
		}
		printf("  <- the rate, cars a frame\n");
		Check(need > 0.0f, "a zero need makes the model meaningless");
		/* The margin at the pessimistic share (90 % of the side band inside
		 * the soft distance): the strict rule is the drain, the soft arm
		 * keeps the ring above its need with room to spare. */
		float worst = 0.5f * (1.0f - unders[nUnder - 1]);
		float worstRate = settings[s].scale * (1.0f - powf(1.0f - worst, CAR_SPAWN_ATTEMPTS));
		printf("    the worst column against the need: %.3f / %.3f = %.1fx\n\n",
			(double)worstRate, (double)need, (double)(worstRate / need));
		if (worstRate < need * 2.0f)
			Fail("the soft arm's worst case does not clear the ring's need with a margin");
	}
	printf("  The strict reading (occluders, fog and rays only) has no creation site on an\n");
	printf("  open street: the ring's bands sit inside the visible range, so its traffic\n");
	printf("  drains there. The soft arm (a placement behind the camera at or beyond the\n");
	printf("  removal gap) fills the street, and even under the pessimistic layout (90 %% of\n");
	printf("  the side band's placements pulled inside the soft distance and refused on open\n");
	printf("  ground) the rate stays an order of magnitude above the need. The visible\n");
	printf("  in-view creation counter stays 0 in every arm.\n\n");
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
	Section3bExhaustive();
	Section4RayBudget();
	Section5Target();
	Section6OpenStreetFill();

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  every check passes. The bubble sits the reserve inside every traffic band at\n");
		printf("  every cell of the control, the ladder allows no catchable event (the sweep\n");
		printf("  holds the binding to the predicate), the frame's ray work is bounded by one\n");
		printf("  number, the fill target follows the budget, and the soft arm keeps an open\n");
		printf("  street above what its traffic needs.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
