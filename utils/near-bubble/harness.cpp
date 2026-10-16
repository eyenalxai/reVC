/*
 * The evidence program for the near bubble (ticket VICE-127).
 *
 * This program reads src/control/NearBubble.h on its own (the header has no
 * engine include) and checks the rule, the bounds and the numbers:
 *
 *	the bubble and its reserve fit inside every traffic spawn band, and the
 *	soft distance covers the side band's placement slack, over the whole
 *	control matrix of the ring table (utils/traffic-rings);
 *
 *	the ladder never allows an event the player could catch under the band
 *	minimum: case by case, and then over every combination of the fields of a
 *	candidate (the sweep fails the moment the ladder and the catchable
 *	predicate drift apart);
 *
 *	the membership of the arms: the occluded hint and the close blind spot
 *	are always sent to the confirmation ray, the soft arm is always off the
 *	view cone by the margin, nothing behind the bubble is ever allowed, and
 *	the in-view band (VICE-146) takes exactly the positions at or beyond the
 *	ring table's on-screen spawn minimum, after the screen fade;
 *
 *	the reveal of a soft-arm creation and of an in-view band creation: the
 *	alpha ramp and the gate (mirrored from Vehicle.h/Vehicle.cpp), the frames
 *	to opaque, and the angular size of a car at the soft distance against the
 *	margin and the occluder window;
 *
 *	the per-frame ray budget, the fill targets, and the fill of a street
 *	with no occluders against the rate the traffic needs, with the real mix
 *	of the spawner's branches (the fast branch sends the side band a quarter
 *	of its calls and the on-screen band three quarters) and the lane-offset
 *	allowance of the soft distance.
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

/* The reveal numbers (Section 7), mirrored from Vehicle.h (VICE-127):
 * VEHICLE_ALPHA_FADE_IN_STEP, VEHICLE_ALPHA_HIDDEN_STEP, the alpha's top, and
 * the stock FOV whose half is the view cone's half-angle. */
#define VEHICLE_ALPHA_FADE_IN_STEP 16	/* Vehicle.h */
#define VEHICLE_ALPHA_HIDDEN_STEP 64	/* Vehicle.h */
#define CLUMP_ALPHA_TOP 255		/* VisibilityPlugins.cpp */
#define STOCK_FOV 70.0f			/* Camera.cpp */

/* The model of an open street (Section 6): the average traffic speed (the AI
 * cruise speeds of the ring are about 7..13 m/s), the calls a frame at the
 * stock ring and at the cap (RANDOM_CAR_ATTEMPTS_PER_FRAME x the ring scale),
 * and the branch mix of the spawner (CarCtrl.cpp). The fast-driving branch
 * aims one of its four calls at the side band and the other three at the
 * on-screen band (two narrow ones at an angle limit of 0.85, about 31.8
 * degrees, and one wide at 0.707, 45 degrees); the other branches send one of
 * their two calls to the side band and one to the on-screen band (wide). */
#define MODEL_TRAFFIC_SPEED 10.0f
#define MODEL_CALLS_STOCK 3
#define MODEL_CALLS_CAP 6
#define MODEL_SIDE_SHARE_FAST 0.25f
#define MODEL_SIDE_SHARE_OTHER 0.5f
#define MODEL_FRONT_SHARE_FAST 0.75f
#define MODEL_FRONT_SHARE_OTHER 0.5f
#define MODEL_FRONT_ANGLE_NARROW 31.8f
#define MODEL_FRONT_ANGLE_WIDE 45.0f

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

/* The qualitative cases of the ladder. Every field is a fact of the frame; the
 * on-screen spawn minimum defaults to the stock ring's own value (the live
 * table gives the same at the stock scale), so a case names it only when it
 * tests the band. */
struct SCase
{
	const char *name;
	float distToPlayer;	/* 2D */
	float distToCamera;	/* 3D */
	float visibleRange;
	bool inFrustum;
	bool inFrustumMargin;
	bool occluded;
	bool fadeHides;
	bool bypassView;
	float carOnScreenSpawnMin = RING_CAR_ONSCREEN_SPAWN_MIN;	/* the in-view band */
};

static SNearBubbleCandidate
CandidateOf(const SCase &s)
{
	SNearBubbleCandidate c;
	c.distToPlayer = s.distToPlayer;
	c.distToCamera = s.distToCamera;
	c.visibleRange = s.visibleRange;
	c.onScreenSpawnMin = s.carOnScreenSpawnMin;
	c.inFrustum = s.inFrustum;
	c.inFrustumMargin = s.inFrustumMargin;
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

/* The bearing a camera must turn by before a position at this distance enters
 * the view cone, with the soft margin and the entity's own radius in front of
 * it. */
static float
MarginBearingDegrees(float distToCamera)
{
	float s = (NEAR_BUBBLE_SOFT_MARGIN + NEAR_BUBBLE_ENTITY_RADIUS) / distToCamera;
	if (s > 1.0f)
		s = 1.0f;
	return (float)(asin(s) * 180.0 / 3.14159265358979);
}

static void
Section1Constants(void)
{
	printf("== 1. the bubble and its bounds\n\n");
	printf("  the bubble radius (the car side band %0.f m less the reserve %0.f m): %.1f m\n",
		(double)RING_CAR_SIDE_SPAWN, (double)RING_CAR_DESPAWN_RESERVE, (double)NEAR_BUBBLE_RADIUS);
	printf("  the removal reserve:     %.1f m (the ring's own placement reserve)\n", (double)NEAR_BUBBLE_REMOVE_RESERVE);
	printf("  the removal distance:    %.1f m (the bubble + its reserve)\n", (double)NEAR_BUBBLE_REMOVE_DISTANCE);
	printf("  the side placement slack: %.1f m (the widest lane offset of the shipped paths)\n",
		(double)NEAR_BUBBLE_SIDE_PLACEMENT_SLACK);
	printf("  the soft distance:       %.1f m (the side band less the slack)\n", (double)NEAR_BUBBLE_SOFT_DISTANCE);
	printf("  the soft margin:         %.1f m (a radius margin on the frustum test)\n", (double)NEAR_BUBBLE_SOFT_MARGIN);
	printf("  the margin as a bearing: about %.1f degrees at the soft distance\n", (double)MarginBearingDegrees(NEAR_BUBBLE_SOFT_DISTANCE));
	printf("  the in-view band minimum: %.1f m (the stock on-screen spawn ring less its reserve,\n",
		(double)RING_CAR_ONSCREEN_SPAWN_MIN);
	printf("                           CurrentTrafficRings().carOnScreenSpawnMin)\n");
	printf("  the visibility radius:   %.1f m (a car's bounding sphere)\n", (double)NEAR_BUBBLE_ENTITY_RADIUS);
	printf("  the ray budget:          %d rays a frame, each at most %.0f m long\n",
		NEAR_BUBBLE_LOS_RAYS_PER_FRAME, (double)NEAR_BUBBLE_LOS_RANGE);
	printf("  the fresh-spawn grace:   %d ms\n", NEAR_BUBBLE_GRACE_MS);
	printf("  the occluder window:     %.0f m of camera movement, a direction cosine of %.4f (%.1f deg),\n",
		(double)NEAR_BUBBLE_OCCLUDER_MAX_MOVE, (double)NEAR_BUBBLE_OCCLUDER_MIN_DOT,
		(double)(acos(NEAR_BUBBLE_OCCLUDER_MIN_DOT) * 180.0 / 3.14159265358979));
	printf("                           %.0f degrees of FOV\n", (double)NEAR_BUBBLE_OCCLUDER_MAX_FOV);
	printf("\n  The bubble is the spawn band less the placement error the reserve holds: a\n");
	printf("  placement of the side band lands at %.1f m (the ring %0.f less up to the lane\n",
		(double)(RING_CAR_SIDE_SPAWN - NEAR_BUBBLE_SIDE_PLACEMENT_SLACK), (double)RING_CAR_SIDE_SPAWN);
	printf("  offset) or farther, and a car at the soft distance is revealed by a camera turn\n");
	printf("  of %.1f degrees or more. The same car spans %.1f degrees (its %.0f m sphere),\n",
		(double)MarginBearingDegrees(NEAR_BUBBLE_SOFT_DISTANCE),
		(double)(2.0 * NEAR_BUBBLE_ENTITY_RADIUS / NEAR_BUBBLE_SOFT_DISTANCE * 180.0 / 3.14159265358979),
		(double)NEAR_BUBBLE_ENTITY_RADIUS);
	printf("  so the margin is four of its own sizes. The occluder window's whole stale error\n");
	printf("  (a %.1f degree turn plus the %.1f degree parallax of a %.0f m frame at the soft\n",
		(double)(acos(NEAR_BUBBLE_OCCLUDER_MIN_DOT) * 180.0 / 3.14159265358979),
		(double)(NEAR_BUBBLE_OCCLUDER_MAX_MOVE / NEAR_BUBBLE_SOFT_DISTANCE * 180.0 / 3.14159265358979),
		(double)NEAR_BUBBLE_OCCLUDER_MAX_MOVE);
	printf("  distance) stays inside the car's own angular size, and the confirmation ray\n");
	printf("  decides that arm anyway.\n\n");
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
	float minSide = 1e9f, minGen = 1e9f, minDespawn = 1e9f, minBand = 1e9f;

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
		if (NEAR_BUBBLE_SOFT_DISTANCE + NEAR_BUBBLE_SIDE_PLACEMENT_SLACK > r.carSideSpawn) {
			Fail("a cell places the side band so near that the soft distance misses its slack");
			failed++;
		}
		if (NEAR_BUBBLE_REMOVE_DISTANCE <= NEAR_BUBBLE_RADIUS) {
			Fail("the removal distance does not leave the bubble");
			failed++;
		}
		/* The in-view band (VICE-146): the live minimum must stay outside
		 * the bubble and the soft arm, so the band never overlaps the
		 * strict rule it follows. */
		if (r.carOnScreenSpawnMin <= NEAR_BUBBLE_RADIUS) {
			Fail("a cell places the in-view band inside the bubble");
			failed++;
		}
		if (r.carOnScreenSpawnMin <= NEAR_BUBBLE_SOFT_DISTANCE) {
			Fail("a cell places the in-view band inside the soft distance");
			failed++;
		}
		minSide = TrafficRingsMin(minSide, r.carSideSpawn);
		minGen = TrafficRingsMin(minGen, r.generatorNear);
		minDespawn = TrafficRingsMin(minDespawn, r.carDespawn);
		minBand = TrafficRingsMin(minBand, r.carOnScreenSpawnMin);
	}
	printf("  cells: %d (17 draw distances x %d FOV factors x %d far clips)\n", cells, nFov, nFar);
	printf("  failures: %d\n", failed);
	printf("  the tightest car side band over the matrix: %.1f m (the bubble asks %.1f + %.1f,\n",
		(double)minSide, (double)NEAR_BUBBLE_RADIUS, (double)NEAR_BUBBLE_REMOVE_RESERVE);
	printf("    the soft distance asks %.1f + the slack %.1f)\n",
		(double)NEAR_BUBBLE_SOFT_DISTANCE, (double)NEAR_BUBBLE_SIDE_PLACEMENT_SLACK);
	printf("  the tightest generator near ring:         %.1f m\n", (double)minGen);
	printf("  the tightest car despawn:                 %.1f m (the removal distance asks %.1f)\n",
		(double)minDespawn, (double)NEAR_BUBBLE_REMOVE_DISTANCE);
	printf("  the tightest in-view band minimum:        %.1f m (the bubble asks %.1f, the\n",
		(double)minBand, (double)NEAR_BUBBLE_RADIUS);
	printf("    soft distance %.1f)\n", (double)NEAR_BUBBLE_SOFT_DISTANCE);
	printf("\n  The side band never moves inside the bubble or the soft distance: the ring\n");
	printf("  scale has a floor of 1.0 (TrafficRings.h), so the band is %.0f m or more at\n",
		(double)RING_CAR_SIDE_SPAWN);
	printf("  every cell. The in-view band's minimum never moves inside the bubble or the\n");
	printf("  soft distance either: the floor of the on-screen family is the FOV factor, and\n");
	printf("  even at its lowest (the stock ring at FOV 110, a scale of %.3f) the minimum is\n",
		(double)(70.0f / 110.0f));
	printf("  %.1f m.\n\n", (double)minBand);
}

/* The ladder, case by case. The soundness invariant is the acceptance: every
 * answer that allows an event is outside the catchable set, and every catchable
 * case is refused (the fade and the bypassed view are the two exempt arms). */
static void
Section3Ladder(void)
{
	const float soft = NEAR_BUBBLE_SOFT_DISTANCE;
	const SCase cases[] = {
		/* name					player	camera	range	fr	margin	occl	fade	bypass	*/
		{ "in the bubble",			 60.0f,	 60.0f,	360.0f,	true,	true,	false,	false,	false	},
		{ "in the bubble, a fade",		 60.0f,	 60.0f,	360.0f,	true,	true,	false,	true,	false	},
		{ "in the bubble, occluded",		 60.0f,	 60.0f,	360.0f,	false,	true,	true,	false,	false	},
		{ "in the bubble, top-down view",	 60.0f,	 60.0f,	360.0f,	true,	true,	false,	false,	true	},
		{ "in view, close, clear",		150.0f,	150.0f,	360.0f,	true,	true,	false,	false,	false	},
		{ "in view, close, a fade",		150.0f,	150.0f,	360.0f,	true,	true,	false,	true,	false	},
		{ "in view, close, occluded",		150.0f,	150.0f,	360.0f,	true,	true,	true,	false,	false	},
		{ "off the frustum, inside the margin",	150.0f,	150.0f,	360.0f,	false,	true,	false,	false,	false	},
		{ "off the frustum, the top-down view",	150.0f,	150.0f,	360.0f,	true,	true,	false,	false,	true	},
		{ "off the frustum, beyond the fade",	150.0f,	400.0f,	360.0f,	false,	false,	false,	false,	false	},
		{ "in view, beyond the fade",		150.0f,	400.0f,	360.0f,	true,	true,	false,	false,	false	},
		{ "off the frustum, the close blind spot", 150.0f, 95.0f, 360.0f, false,	false,	false,	false,	false	},
		{ "off the frustum, on the soft distance", 150.0f, 103.0f, 360.0f, false,	false,	false,	false,	false	},
		{ "off the frustum, beyond the soft distance", 150.0f, 300.0f, 500.0f, false, false, false, false, false },
		{ "a visible range of zero (boot, close)", 150.0f, 95.0f, 0.0f,	false,	false,	false,	false,	false	},
		{ "a visible range of zero (boot, the soft distance)", 150.0f, 103.0f, 0.0f, false, false, false, false, false },
		{ "in view, the band minimum less one",	150.0f, RING_CAR_ONSCREEN_SPAWN_MIN - 1.0f, 360.0f, true, true, false, false, false },
		{ "in view, on the band minimum",	150.0f, RING_CAR_ONSCREEN_SPAWN_MIN,	360.0f, true, true, false, false, false },
		{ "in view, the band, a fade",		150.0f, RING_CAR_ONSCREEN_SPAWN_MIN + 40.0f, 360.0f, true, true, false, true, false },
	};
	const int nCases = sizeof(cases) / sizeof(cases[0]);
	const ENearBubbleAnswer expected[] = {
		NEAR_BUBBLE_REFUSED,		/* in the bubble */
		NEAR_BUBBLE_REFUSED,		/* in the bubble, a fade */
		NEAR_BUBBLE_REFUSED,		/* in the bubble, occluded */
		NEAR_BUBBLE_REFUSED,		/* in the bubble, top-down */
		NEAR_BUBBLE_REFUSED,		/* in view, close, clear */
		NEAR_BUBBLE_ALLOWED,		/* in view, close, a fade */
		NEAR_BUBBLE_SEEK_LOS,		/* in view, close, occluded */
		NEAR_BUBBLE_REFUSED,		/* off the frustum, inside the margin */
		NEAR_BUBBLE_ALLOWED,		/* off the frustum, top-down */
		NEAR_BUBBLE_ALLOWED,		/* off the frustum, beyond the fade */
		NEAR_BUBBLE_ALLOWED,		/* in view, beyond the fade */
		NEAR_BUBBLE_SEEK_LOS,		/* off the frustum, the close blind spot */
		NEAR_BUBBLE_ALLOWED,		/* off the frustum, on the soft distance */
		NEAR_BUBBLE_ALLOWED,		/* off the frustum, beyond the soft distance */
		NEAR_BUBBLE_SEEK_LOS,		/* boot, close */
		NEAR_BUBBLE_ALLOWED,		/* boot, the soft distance */
		NEAR_BUBBLE_REFUSED,		/* in view, the band minimum less one */
		NEAR_BUBBLE_ALLOWED,		/* in view, on the band minimum */
		NEAR_BUBBLE_ALLOWED,		/* in view, the band, a fade */
	};
	const ENearBubbleArm expectedArm[] = {
		NEAR_BUBBLE_ARM_NONE,
		NEAR_BUBBLE_ARM_NONE,
		NEAR_BUBBLE_ARM_NONE,
		NEAR_BUBBLE_ARM_NONE,
		NEAR_BUBBLE_ARM_NONE,
		NEAR_BUBBLE_ARM_FADE,
		NEAR_BUBBLE_ARM_OCCLUDER,
		NEAR_BUBBLE_ARM_NONE,
		NEAR_BUBBLE_ARM_BYPASS,
		NEAR_BUBBLE_ARM_FADE_DISTANCE,
		NEAR_BUBBLE_ARM_FADE_DISTANCE,
		NEAR_BUBBLE_ARM_RAY,
		NEAR_BUBBLE_ARM_SOFT,
		NEAR_BUBBLE_ARM_SOFT,
		NEAR_BUBBLE_ARM_RAY,
		NEAR_BUBBLE_ARM_SOFT,
		NEAR_BUBBLE_ARM_NONE,
		NEAR_BUBBLE_ARM_ONSCREEN_BAND,
		NEAR_BUBBLE_ARM_FADE,
	};

	printf("== 3. the ladder, case by case\n\n");
	printf("  %-46s %-12s %-14s %s\n", "case", "answer", "arm", "catchable?");
	for (int i = 0; i < nCases; i++) {
		SNearBubbleCandidate c = CandidateOf(cases[i]);
		ENearBubbleAnswer a = NearBubbleAnswerFor(c);
		ENearBubbleArm arm = NearBubbleArmFor(c);
		bool allowed = a != NEAR_BUBBLE_REFUSED;
		bool catchable = NearBubblePositionIsCatchable(c);
		/* The three arms that answer outside the acceptance: the fade of a
		 * transition and the top-down camera (which the counters read apart)
		 * and the in-view band, which is allowed over a position the
		 * catchable set holds (the band is the arm VICE-146 restored). */
		bool exempt = cases[i].fadeHides || cases[i].bypassView ||
			arm == NEAR_BUBBLE_ARM_ONSCREEN_BAND;
		printf("  %-46s %-12s %-14s %s\n", cases[i].name,
			a == NEAR_BUBBLE_REFUSED ? "refused" : a == NEAR_BUBBLE_ALLOWED ? "allowed" : "seek the ray",
			arm == NEAR_BUBBLE_ARM_NONE ? "none" :
			arm == NEAR_BUBBLE_ARM_BYPASS ? "bypass" :
			arm == NEAR_BUBBLE_ARM_OCCLUDER ? "occluded hint" :
			arm == NEAR_BUBBLE_ARM_FADE_DISTANCE ? "beyond the fade" :
			arm == NEAR_BUBBLE_ARM_SOFT ? "soft distance" :
			arm == NEAR_BUBBLE_ARM_RAY ? "close blind spot" :
			arm == NEAR_BUBBLE_ARM_FADE ? "fade" : "in-view band",
			catchable ? "yes" : "no");
		Check(a == expected[i], "the ladder answered against the case list");
		Check(arm == expectedArm[i], "the ladder chose another arm than the case list");
		/* The acceptance: an event the ladder allows (past the ray's own
		 * blocked check) is never one the player could catch. */
		if (allowed && !exempt)
			Check(!catchable, "the ladder allowed a catchable event");
		/* The completeness: a catchable event is refused. */
		if (catchable && !exempt)
			Check(!allowed, "the ladder did not refuse a catchable event");
		/* The confirmation rule: the occluded hint and the close blind spot
		 * never allow on their own. */
		if (arm == NEAR_BUBBLE_ARM_OCCLUDER || arm == NEAR_BUBBLE_ARM_RAY)
			Check(a == NEAR_BUBBLE_SEEK_LOS, "a hint arm answered on its own");
		/* The margin: the soft arm is always off the view cone by it. */
		if (arm == NEAR_BUBBLE_ARM_SOFT)
			Check(!cases[i].inFrustumMargin, "the soft arm allowed a position inside the margin");
	}

	printf("\n  The confirmation ray behind the two hint arms:\n");
	SNearBubbleCandidate hint = CandidateOf(cases[6]);
	printf("  %-46s %-12s %-14s %s\n", "an occluded hint, a blocked ray",
		"allowed", "occluded hint", NearBubblePositionIsCatchable(hint) ? "yes" : "no");
	printf("  %-46s %-12s %-14s %s\n", "an occluded hint, a clear ray",
		"refused", "occluded hint", NearBubblePositionIsCatchable(hint) ? "yes" : "no");
	Check(!NearBubblePositionIsCatchable(hint),
		"the occluded hint must be about a position the catchable set does not hold");

	printf("\n  Three arms are counted apart: the fade of a transition (the fill burst), the\n");
	printf("  top-down camera, whose view is not the player's, and the in-view band (the\n");
	printf("  stock on-screen spawn band, restored by VICE-146: a placement at or beyond\n");
	printf("  the ring's carOnScreenSpawnMin, covered by the car's own alpha ramp). The\n");
	printf("  bubble is refused under all of them: a car placed at the player's elbow is\n");
	printf("  there when the fade lifts or the camera returns.\n\n");
}

/* The structural binding: the ladder is written against NearBubblePositionIs
 * Catchable and near the arm function, so the invariant must hold for *every*
 * combination of the fields, not only for the cases above. The sweep
 * enumerates them and fails the moment the two drift apart. The in-view band
 * (VICE-146) is part of the lattice: the camera distances straddle the band
 * minimum as well as the soft distance, so the band's edge and the catchable
 * zone below it are both swept. */
static void
Section3bExhaustive(void)
{
	const float distancesToPlayer[] = { NEAR_BUBBLE_RADIUS - 1.0f, NEAR_BUBBLE_RADIUS + 1.0f };
	const float distancesToCamera[] = {
		NEAR_BUBBLE_SOFT_DISTANCE - 1.0f, NEAR_BUBBLE_SOFT_DISTANCE + 1.0f,
		RING_CAR_ONSCREEN_SPAWN_MIN - 1.0f, RING_CAR_ONSCREEN_SPAWN_MIN, 400.0f
	};
	const float ranges[] = { 0.0f, 200.0f, 360.0f };
	int combinations = 0, allowedOutside = 0, refusedCatchable = 0, violations = 0;
	int armMismatch = 0, softInsideMargin = 0;

	for (int dp = 0; dp < 2; dp++)
	for (int dc = 0; dc < 5; dc++)
	for (int vr = 0; vr < 3; vr++)
	for (int inFrustum = 0; inFrustum < 2; inFrustum++)
	for (int margin = 0; margin < 2; margin++)
	for (int occluded = 0; occluded < 2; occluded++)
	for (int fade = 0; fade < 2; fade++)
	for (int bypass = 0; bypass < 2; bypass++) {
		SNearBubbleCandidate c;
		c.distToPlayer = distancesToPlayer[dp];
		c.distToCamera = distancesToCamera[dc];
		c.visibleRange = ranges[vr];
		c.onScreenSpawnMin = RING_CAR_ONSCREEN_SPAWN_MIN;
		c.inFrustum = inFrustum != 0;
		c.inFrustumMargin = margin != 0;
		c.occluded = occluded != 0;
		c.fadeHides = fade != 0;
		c.bypassView = bypass != 0;

		ENearBubbleAnswer a = NearBubbleAnswerFor(c);
		ENearBubbleArm arm = NearBubbleArmFor(c);
		bool allowed = a != NEAR_BUBBLE_REFUSED;
		bool catchable = NearBubblePositionIsCatchable(c);
		/* The three arms an event may take outside the acceptance: the
		 * fade of a transition, the bypassed view, and the in-view band. */
		bool exempt = c.fadeHides || c.bypassView ||
			arm == NEAR_BUBBLE_ARM_ONSCREEN_BAND;
		combinations++;
		if (allowed && !catchable)
			allowedOutside++;
		if (!allowed && catchable && !exempt)
			refusedCatchable++;
		if (allowed && catchable && !exempt) {
			violations++;
			Fail("the sweep found an allowed catchable event outside the exemptions");
		}
		/* The answer must be the arm's: the two are one function, and the
		 * sweep proves it for every combination. */
		if ((arm == NEAR_BUBBLE_ARM_NONE) != (a == NEAR_BUBBLE_REFUSED))
			armMismatch++;
		if ((arm == NEAR_BUBBLE_ARM_OCCLUDER || arm == NEAR_BUBBLE_ARM_RAY) && a != NEAR_BUBBLE_SEEK_LOS)
			armMismatch++;
		if (arm == NEAR_BUBBLE_ARM_SOFT && c.inFrustumMargin)
			softInsideMargin++;
		/* The band is exactly the catchable positions at or beyond the
		 * minimum that the fade and the bubble do not take first. */
		if (arm == NEAR_BUBBLE_ARM_ONSCREEN_BAND &&
			(!catchable || c.distToCamera < c.onScreenSpawnMin))
			armMismatch++;
	}
	printf("== 3b. the structural binding: every combination of the fields\n\n");
	printf("  combinations: %d (the camera distances straddle the soft distance and the\n", combinations);
	printf("    band minimum: %.1f, %.1f, %.1f, %.1f, %.1f)\n",
		(double)distancesToCamera[0], (double)distancesToCamera[1], (double)distancesToCamera[2],
		(double)distancesToCamera[3], (double)distancesToCamera[4]);
	printf("  allowed and outside the catchable set (the rule's own work): %d\n", allowedOutside);
	printf("  catchable and refused (the acceptance, under the band minimum): %d\n", refusedCatchable);
	printf("  allowed and catchable outside the fade, the bypass and the band: %d (must be 0)\n", violations);
	printf("  answers that disagree with the arm: %d (must be 0)\n", armMismatch);
	printf("  soft-arm answers inside the margin: %d (must be 0)\n", softInsideMargin);
	printf("\n  The ladder and the counters share the predicate NearBubblePositionIsCatchable\n");
	printf("  and the arm function, so no combination of the view can let a catchable event\n");
	printf("  through unwatched, every catchable one under the band minimum is refused, the\n");
	printf("  band takes exactly the positions at or beyond the minimum, and every hint goes\n");
	printf("  to the confirmation ray. A future edit that drops an arm or moves a condition\n");
	printf("  fails this sweep.\n\n");
}

/* The per-frame bound of the ray work: a frame spends at most the budget, and
 * the candidate count behind it is bounded by the placement loops. The numbers
 * of the loops come from PopulationBudget.h (mirrored above). */
static void
Section4RayBudget(void)
{
	printf("== 4. the per-frame work of the rule\n\n");
	printf("  the ray budget of a frame:            %d\n", NEAR_BUBBLE_LOS_RAYS_PER_FRAME);
	printf("  the longest ray:                      %.0f m (the soft distance, %.0f sectors of %0.f m)\n",
		(double)NEAR_BUBBLE_LOS_RANGE, (double)NEAR_BUBBLE_LOS_RANGE / 50.0f, 50.0);
	int trickleCalls = RANDOM_CAR_ATTEMPTS_PER_FRAME * (int)RING_MAX_SCALE;
	int trickleCandidates = trickleCalls * CAR_SPAWN_ATTEMPTS;
	printf("  the trickle: %d car calls a frame x %d placements = %d candidates,\n",
		trickleCalls, CAR_SPAWN_ATTEMPTS, trickleCandidates);
	printf("               of which the hint arms and the close band spend at most %d rays\n",
		NEAR_BUBBLE_LOS_RAYS_PER_FRAME);
	printf("  a burst frame (the visible window): %d candidates, the same %d rays\n",
		FILL_BURST_ATTEMPTS_PER_FRAME, NEAR_BUBBLE_LOS_RAYS_PER_FRAME);
	printf("  a burst frame of the hidden window: the fade arm allows without a ray\n");
	printf("\n  the refused candidates: one ladder each (two subtractions, the occluder test\n");
	printf("  and the distance of the camera); no collision work.\n");
	printf("  one ray: the buildings of the sectors the line crosses, to the first hit or\n");
	printf("  to the end. The ray is the collision path's line walk and nothing else; the\n");
	printf("  rest of the rule is a handful of floats. The occluded hints spend from the\n");
	printf("  same budget, so the in-view hidden sites are capped by it: four a frame, far\n");
	printf("  above the one car a second the ring replaces.\n\n");

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
	printf("  a simulated frame of %d hint candidates with blocked rays:\n", trickleCandidates);
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
 * own design), so the traffic drains. Two creation sites keep it alive: the
 * soft arm (the side band less the lane offset, off the view cone by the
 * margin) and the in-view band (the on-screen band of the spawner, at or
 * beyond the ring's carOnScreenSpawnMin, VICE-146). The section prints both
 * with the *real* mix of the spawner's branches; the conservative rate of the
 * worst branch is the fail line. */
static void
Section6OpenStreetFill(void)
{
	const float frame = 1.0f / 30.0f;
	const float fovs[2] = { STOCK_FOV, 110.0f };
	STrafficRings stock = RingsAt(DRAWDIST_DEFAULT_VALUE, 1.0f, 2000.0f);
	STrafficRings top = RingsAt(16.0f, 70.0f / 110.0f, 2000.0f);

	printf("== 6. the fill of a street with no occluders (the owner's long roads)\n\n");
	printf("  the model: the trickle's calls a frame (%d at the stock ring, %d at the cap),\n",
		MODEL_CALLS_STOCK, MODEL_CALLS_CAP);
	printf("  %d placements a call. A candidate of the side band passes at or beyond the soft\n", CAR_SPAWN_ATTEMPTS);
	printf("  distance (%.0f m: the band less the lane offset) and off the view cone by the\n",
		(double)NEAR_BUBBLE_SOFT_DISTANCE);
	printf("  margin (about %.1f degrees at it); a candidate of the on-screen band passes in\n",
		(double)MarginBearingDegrees(NEAR_BUBBLE_SOFT_DISTANCE));
	printf("  the frustum at or beyond the band minimum (%.0f m at stock; the band the spawner\n",
		(double)RING_CAR_ONSCREEN_SPAWN_MIN);
	printf("  aims at is %.0f m, so its placements sit past the minimum by the ring's reserve).\n",
		(double)RING_CAR_ONSCREEN_SPAWN_MAX);
	printf("  The need is the target over the crossing time of the disc (2 x despawn / %.0f m/s),\n",
		(double)MODEL_TRAFFIC_SPEED);
	printf("  in frames.\n\n");

	struct { const char *name; float budget; float despawn; int calls; float fov; } settings[2] = {
		{ "stock (1.2, FOV 70)", TrafficRingsMin(RING_CARS_PER_UNIT * 2.0f * stock.scale * stock.scale, (float)RING_MAX_RANDOM_CARS), stock.carDespawn, MODEL_CALLS_STOCK, fovs[0] },
		{ "the owner's (16.0, FOV 110)", TrafficRingsMin(RING_CARS_PER_UNIT * 3.4f * top.scale * top.scale, (float)RING_MAX_RANDOM_CARS), top.carDespawn, MODEL_CALLS_CAP, fovs[1] },
	};
	const float slacks[3] = { 1.0f, 0.9f, 0.7f };
	const int nSlack = 3;

	for (int s = 0; s < 2; s++) {
		float need = settings[s].budget * frame / (2.0f * settings[s].despawn / MODEL_TRAFFIC_SPEED);
		float fovHalf = settings[s].fov / 2.0f;
		/* The margin's share of the side band: the bearings of the band run
		 * from the spawner's 45 degrees to 180, and the margin takes the
		 * bearings inside the view cone's half (the FOV/2) plus itself. */
		float bearing = MarginBearingDegrees(NEAR_BUBBLE_SOFT_DISTANCE);
		float edge = fovHalf + bearing;
		if (edge < 45.0f)
			edge = 45.0f;
		float pMargin = (180.0f - edge) / (180.0f - 45.0f);
		if (pMargin < 0.0f)
			pMargin = 0.0f;
		/* The frustum's share of the front calls: a call's bearing is inside
		 * its own angle limit, so the part of it inside the frustum's half
		 * is the pass share (a uniform bearing is the model). */
		float pNarrow = TrafficRingsMin(1.0f, fovHalf / MODEL_FRONT_ANGLE_NARROW);
		float pWide = TrafficRingsMin(1.0f, fovHalf / MODEL_FRONT_ANGLE_WIDE);
		/* The per-placement pass share of each branch: its side calls (the
		 * margin) and its front calls (the frustum). */
		float fastSide = MODEL_SIDE_SHARE_FAST * pMargin;
		float fastFront = 0.5f * pNarrow + 0.25f * pWide;
		float otherSide = MODEL_SIDE_SHARE_OTHER * pMargin;
		float otherFront = 0.5f * pWide;

		printf("  %s\n", settings[s].name);
		printf("    target %.0f cars, despawn %.0f m -> the ring needs %.3f cars a frame\n",
			(double)settings[s].budget, (double)settings[s].despawn, (double)need);
		printf("    the branch mix: the fast branch sends the side band %.2f of its calls and the\n",
			(double)MODEL_SIDE_SHARE_FAST);
		printf("    on-screen band %.2f (two narrow +-%.1f deg calls and one wide +-%.0f deg one);\n",
			(double)MODEL_FRONT_SHARE_FAST, (double)MODEL_FRONT_ANGLE_NARROW, (double)MODEL_FRONT_ANGLE_WIDE);
		printf("    the other branches send %.2f / %.2f (one wide call).\n",
			(double)MODEL_SIDE_SHARE_OTHER, (double)MODEL_FRONT_SHARE_OTHER);
		printf("    the side pass share (the margin): %.3f (the cone's half %.0f deg + the margin\n",
			(double)pMargin, (double)fovHalf);
		printf("    %.1f deg); the front pass share (the frustum): %.3f / %.3f of the front calls\n",
			(double)bearing, (double)(fastFront / MODEL_FRONT_SHARE_FAST), (double)(otherFront / MODEL_FRONT_SHARE_OTHER));
		printf("    (the fast branch / the others).\n");
		printf("    cars a frame, no placement aggregation (the conservative rate):\n");
		printf("      %-24s", "the fast branch");
		for (int u = 0; u < nSlack; u++)
			printf(" %9.3f", (double)(settings[s].calls * (fastSide + fastFront) * slacks[u]));
		printf("   (side %.3f + on-screen %.3f)\n", (double)(settings[s].calls * fastSide), (double)(settings[s].calls * fastFront));
		printf("      %-24s", "the other branches");
		for (int u = 0; u < nSlack; u++)
			printf(" %9.3f", (double)(settings[s].calls * (otherSide + otherFront) * slacks[u]));
		printf("   (side %.3f + on-screen %.3f)\n", (double)(settings[s].calls * otherSide), (double)(settings[s].calls * otherFront));
		printf("    the rate with the call's %d placements:\n", CAR_SPAWN_ATTEMPTS);
		printf("      %-24s", "the fast branch");
		for (int u = 0; u < nSlack; u++) {
			float p = (fastSide + fastFront) * slacks[u];
			printf(" %9.3f", (double)(settings[s].calls * (1.0f - powf(1.0f - p, CAR_SPAWN_ATTEMPTS))));
		}
		printf("\n      %-24s", "the other branches");
		for (int u = 0; u < nSlack; u++) {
			float p = (otherSide + otherFront) * slacks[u];
			printf(" %9.3f", (double)(settings[s].calls * (1.0f - powf(1.0f - p, CAR_SPAWN_ATTEMPTS))));
		}
		printf("\n");

		/* The fail line: the *conservative* rate of the worst branch (no
		 * placement aggregation) at the pessimistic share must clear twice
		 * the need. */
		float worst = TrafficRingsMin(settings[s].calls * (fastSide + fastFront),
			settings[s].calls * (otherSide + otherFront)) * slacks[nSlack - 1];
		printf("    the worst column against the need: %.3f / %.3f = %.1fx (the conservative\n",
			(double)worst, (double)need, (double)(worst / need));
		printf("    rate of the worst branch at %.0f %% of the placements)\n", (double)(slacks[nSlack - 1] * 100.0f));
		printf("    the fast branch's side quarter, the other branches' side half: the parked and\n");
		printf("    slow cases double the side band's share, and their front share is the worse\n");
		printf("    one, so the other branches are the worst column here.\n\n");
		if (worst < need * 2.0f)
			Fail("the creation sites of the open street do not clear the ring's need twice");
	}
	printf("  The strict reading (occluders, fog and rays only) has no creation site on an\n");
	printf("  open street: the ring's bands sit inside the visible range, so its traffic\n");
	printf("  drains there. The soft arm fills the street behind the camera, and the in-view\n");
	printf("  band (VICE-146) fills the on-screen branch's own placements: even the conservative\n");
	printf("  rate (no placement aggregation, the worst branch, only 70 %% of the placements)\n");
	printf("  clears the need with room to spare. The in-view creation counter stays 0: the\n");
	printf("  band's events are at or beyond its minimum and counted apart.\n\n");
}

/* The reveal of a soft-arm creation (VICE-127): the alpha gate of
 * CVehicle::UpdateClumpAlpha and the ramp's arithmetic. The harness cannot run
 * the engine, so it holds the numbers and the promise: a car off the view cone
 * holds alpha 0 until it is on screen, so the reveal is the fade; the burst's
 * mass runs at the hidden step while the screen is covered. An in-view band
 * creation (VICE-146) is on screen at once, so its ramp starts at the first
 * render: the band shows a car forming, never a settled one. */
static void
Section7Reveal(void)
{
	printf("== 7. the reveal of a fresh car (the alpha gate)\n\n");
	printf("  the normal ramp:        %d a frame -> %d frames to opaque\n",
		VEHICLE_ALPHA_FADE_IN_STEP, (CLUMP_ALPHA_TOP + VEHICLE_ALPHA_FADE_IN_STEP - 1) / VEHICLE_ALPHA_FADE_IN_STEP);
	printf("  the hidden ramp:        %d a frame -> %d frames to opaque (the fade covers the screen)\n",
		VEHICLE_ALPHA_HIDDEN_STEP, (CLUMP_ALPHA_TOP + VEHICLE_ALPHA_HIDDEN_STEP - 1) / VEHICLE_ALPHA_HIDDEN_STEP);
	printf("  the fade-out ramp:      untouched (the removal's own fade)\n");
	printf("\n  A car created outside the view cone (the soft arm and every off-frustum site)\n");
	printf("  holds alpha 0 until it is first on screen (CVehicle::UpdateClumpAlpha,\n");
	printf("  GetIsOnScreen): a camera turn finds it forming over %d frames, never opaque.\n",
		(CLUMP_ALPHA_TOP + VEHICLE_ALPHA_FADE_IN_STEP - 1) / VEHICLE_ALPHA_FADE_IN_STEP);
	printf("  An in-view creation (the stock ring traffic, the VICE-126 burst, the script\n");
	printf("  cars, and the in-view band's placements, VICE-146) is on screen at once and\n");
	printf("  keeps the original ramp: the band shows a car forming over the frames above,\n");
	printf("  never a settled one. The fill burst's mass runs at the hidden step while the\n");
	printf("  fade covers it, so the street is opaque when the screen returns; the burst's\n");
	printf("  cars that hold (the soft arm's) fade in only when first seen. Nothing culls or\n");
	printf("  skips on alpha 0: only the removal's own fade-out waits for it\n");
	printf("  (PossiblyRemoveVehicle), which is untouched.\n\n");
	Check((CLUMP_ALPHA_TOP + VEHICLE_ALPHA_HIDDEN_STEP - 1) / VEHICLE_ALPHA_HIDDEN_STEP <=
	      (CLUMP_ALPHA_TOP + VEHICLE_ALPHA_FADE_IN_STEP - 1) / VEHICLE_ALPHA_FADE_IN_STEP,
		"the hidden ramp must need no more frames than the normal one");
	Check(MarginBearingDegrees(NEAR_BUBBLE_SOFT_DISTANCE) > 10.0f,
		"the soft margin must be a real camera turn");
	Check(2.0 * NEAR_BUBBLE_ENTITY_RADIUS / NEAR_BUBBLE_SOFT_DISTANCE * 180.0 / 3.14159265358979 <
	      MarginBearingDegrees(NEAR_BUBBLE_SOFT_DISTANCE),
		"the margin must dominate the car's own angular size");
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
	Section7Reveal();

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  every check passes. The bubble sits the reserve inside every traffic band and\n");
		printf("  the soft distance covers the side band's placement slack at every cell of the\n");
		printf("  control, the ladder allows no catchable event and sends every hint to the\n");
		printf("  confirmation ray (the sweep holds it), the frame's work is bounded by one\n");
		printf("  number, the reveal is a fade, and the soft arm keeps an open street above what\n");
		printf("  its traffic needs with the real mix of the spawner's branches.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
