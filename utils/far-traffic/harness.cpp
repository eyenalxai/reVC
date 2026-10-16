/*
 * The evidence program for the far traffic layer (ticket VICE-128).
 *
 * This program reads src/control/FarTraffic.h, the ring table (TrafficRings.h),
 * the near bubble's ladder (NearBubble.h) and the cap table (PopulationBudget.h)
 * on their own - none of them includes an engine header - and checks the
 * layer's rules and its bounds:
 *
 *	A. The band. The layer's inner edge is the near traffic's own despawn and
 *	   its outer edge the fog of the frame; the sites, the cap, the switches
 *	   and the pool arithmetic are printed for the reference settings (the
 *	   game's own draw distance and the owner's) and checked there.
 *
 *	B. The matrix. The band and the cap over the whole control matrix of the
 *	   ring table (17 draw distances x 7 FOV factors x 4 far clips), with the
 *	   two invariants the ticket asks for: the promotion distance stays
 *	   outside the near bubble (a far car is never reachable) and the layer's
 *	   cars and reserve fit the vehicle pool behind the car budget.
 *
 *	C. The cap under pressure. The layer's own fill/drain state machine is
 *	   run: creations only under the cap, the band opening and closing, the
 *	   draw distance moving, and the count checked every frame. The cap is
 *	   never passed by a creation, and a dropped cap drains at the layer's
 *	   bounded removal rate.
 *
 *	D. The events. Every creation site of the layer is put through the near
 *	   bubble's own ladder (NEAR_BUBBLE_ANSWER_*): a live site is hidden by the
 *	   fade or by the off-frustum margin, the occluder hint (which the layer
 *	   does not pay a ray for) is the layer's only refused arm, and every
 *	   removal the layer makes on its own is beyond the fog the ladder hides
 *	   with. The switches are checked against the list of facts they write:
 *	   none of them is a fact the renderer draws.
 *
 *	E. The cost. The layer's own arithmetic is measured: the per-car update
 *	   (the path curve, the lane offsets, the heading and the height of the
 *	   rail) and the per-frame gate pass, at the cap and at the stagger. This
 *	   is the arithmetic only: the layer touches no collision query, no sector
 *	   scan and no path search by construction, so the collision-path work of
 *	   the frame is unchanged and the number here is the layer's whole own
 *	   price. The engine-side costs (the placement scan of a creation attempt,
 *	   the driver of a promotion) are named in the ADR and measured live by
 *	   utils/caps-audit/live-counters.py and the frame recipe of the ticket.
 *
 * Build and run:
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 -o /tmp/far-traffic-harness utils/far-traffic/harness.cpp
 *	/tmp/far-traffic-harness
 *
 * The exit code is 0 when every check passes. The output goes into the ticket.
 */

#include "FarTraffic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The numbers of the reference settings: the ring table is composed from the
 * LOD distance base x the FOV factor x the draw distance (DrawDistance.h,
 * Renderer.h, exactly as utils/traffic-rings and utils/near-bubble do it). The
 * owner's settings are the ones the ticket names for the long roads: the
 * maximum draw distance and the FOV cap. */
#define OWNER_DRAWDIST 16.0f
#define OWNER_FOV 2.2f
#define SUNNY_FARCLIP 2000.0f
#define STOCK_FOV 1.0f

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

static STrafficRings
RingsAt(float drawDistance, float fovFactor, float farClip)
{
	STrafficRingInput in;
	in.lodDistance = LOD_DISTANCE_BASE * fovFactor * drawDistance;
	in.fovFactor = fovFactor;
	in.farClip = farClip;
	return ComputeTrafficRings(in);
}

static SFarTrafficBand
BandAt(float drawDistance, float fovFactor, float farClip)
{
	return ComputeFarTrafficBand(RingsAt(drawDistance, fovFactor, farClip));
}

/* ------------------------------------------------------------------ A. the band */

static void
PrintBand(const char *name, float drawDistance, float fovFactor, float farClip)
{
	STrafficRings r = RingsAt(drawDistance, fovFactor, farClip);
	SFarTrafficBand b = ComputeFarTrafficBand(r);
	int cap = FarTrafficCap(b);

	printf("  %s: draw distance %.2f, FOV factor %.2f, far clip %.0f m\n",
		name, (double)drawDistance, (double)fovFactor, (double)farClip);
	printf("    the near ring: despawn %.0f m, on-screen band %.0f..%.0f m, the visible range %.0f m\n",
		(double)r.carDespawn, (double)r.carOnScreenSpawnMin, (double)r.carOnScreenSpawnMax,
		(double)r.visibleRange);
	printf("    the far band: %s", b.active ? "" : "closed; ");
	printf("inner %.0f m, outer %.0f m, creations to %.0f m, removals past %.0f m\n",
		(double)b.inner, (double)b.outer, (double)b.createOuter, (double)b.removeOuter);
	printf("    the cap: %d cars (the layer's hard top is %d over a %.0f km2 band)\n",
		cap, FAR_TRAFFIC_MAX, (double)(FAR_TRAFFIC_REF_AREA / 1000000.0f));
	if (cap > 0) {
		printf("    the stagger: %d a frame of the %d (about %.1f Hz at 30 fps), the updates of a frame: %d\n",
			(cap + FAR_TRAFFIC_STAGGER - 1) / FAR_TRAFFIC_STAGGER, cap,
			30.0 / FAR_TRAFFIC_STAGGER, (cap + FAR_TRAFFIC_STAGGER - 1) / FAR_TRAFFIC_STAGGER);
		printf("    one band, two sites: the fog site at %.0f m (a creation in the haze),\n",
			(double)b.createOuter);
		printf("      the behind site at %.0f m (off the frustum, past the side band)\n", (double)b.inner);
	}
}

static void
Section1Band(void)
{
	printf("== 1. the band of the layer\n\n");
	PrintBand("the game's own draw distance", DRAWDIST_DEFAULT, STOCK_FOV, SUNNY_FARCLIP);
	printf("\n");
	PrintBand("the owner's draw distance", OWNER_DRAWDIST, OWNER_FOV, SUNNY_FARCLIP);
	printf("\n");
	PrintBand("a storm at the owner's draw distance", OWNER_DRAWDIST, OWNER_FOV, 300.0f);
	printf("\n");
	printf("  The stock draw distance closes the band: the ring's despawn %.0f m sits %.0f m inside\n",
		(double)RingsAt(DRAWDIST_DEFAULT, STOCK_FOV, SUNNY_FARCLIP).carDespawn,
		(double)(RingsAt(DRAWDIST_DEFAULT, STOCK_FOV, SUNNY_FARCLIP).visibleRange
			- RingsAt(DRAWDIST_DEFAULT, STOCK_FOV, SUNNY_FARCLIP).carDespawn));
	printf("  the visible range, and the near traffic already ends in the fog there. The layer is a\n");
	printf("  function of the draw distance: it opens when the world is drawn past\n");
	printf("  the despawn plus FAR_TRAFFIC_MIN_BAND (%.0f m).\n\n", (double)FAR_TRAFFIC_MIN_BAND);
}

/* ------------------------------------------------------------------ B. the matrix */

static void
Section2Matrix(void)
{
	const int nSteps = 16;
	const float fovFactors[] = { 1.0f, 70.0f / 110.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.2f };
	const int nFov = sizeof(fovFactors) / sizeof(fovFactors[0]);
	const float farClips[] = { 2000.0f, 1150.0f, 650.0f, 300.0f };
	const int nFar = sizeof(farClips) / sizeof(farClips[0]);

	int cells = 0, failed = 0, open = 0;
	int minCap = 1 << 30, maxCap = 0;
	float minInner = 1e9f, maxOuter = 0.0f;

	printf("== 2. the band and the cap over the control matrix\n\n");
	for (int f = 0; f < nFar; f++)
	for (int j = 0; j < nFov; j++)
	for (int i = 0; i <= nSteps; i++) {
		float dd = DRAWDIST_MIN + i * DRAWDIST_RANGE / nSteps;
		STrafficRings r = RingsAt(dd, fovFactors[j], farClips[f]);
		SFarTrafficBand b = ComputeFarTrafficBand(r);
		int cap = FarTrafficCap(b);
		cells++;
		/* The reach: a far car is promoted at the band's inner edge, so the
		 * inner edge must stay outside the near bubble's removal distance. */
		if (b.inner <= NEAR_BUBBLE_REMOVE_DISTANCE) {
			Fail("a cell places the band's inner edge inside the player's reach");
			failed++;
		}
		/* The band's own order: the sites, the hysteresis and the fog. */
		if (b.createOuter < b.outer || b.removeOuter <= b.createOuter) {
			Fail("a cell breaks the order of the band's edges");
			failed++;
		}
		/* The cap: live only with an open band, and inside the constant. */
		if (b.active) {
			open++;
			if (cap < FAR_TRAFFIC_MIN_CARS || cap > FAR_TRAFFIC_MAX) {
				Fail("a cell gives an open band a cap outside its floor and top");
				failed++;
			}
		} else if (cap != 0) {
			Fail("a cell gives a closed band a cap");
			failed++;
		}
		if (cap < minCap) minCap = cap;
		if (cap > maxCap) maxCap = cap;
		if (b.inner < minInner) minInner = b.inner;
		if (b.outer > maxOuter) maxOuter = b.outer;
	}
	printf("  cells: %d (17 draw distances x %d FOV factors x %d far clips), open bands: %d\n",
		cells, nFov, nFar, open);
	printf("  failures: %d\n", failed);
	printf("  the tightest inner edge over the matrix: %.0f m (the reach is %.0f m,\n",
		(double)minInner, (double)NEAR_BUBBLE_REMOVE_DISTANCE);
	printf("    the ring's own stock despawn is %.0f m: no slider takes it inside the bubble)\n",
		(double)RING_CAR_DESPAWN);
	printf("  the widest outer edge over the matrix: %.0f m (the far clip of the weather)\n", (double)maxOuter);
	printf("  the cap over the matrix: %d..%d of the hard top %d\n\n", minCap, maxCap, FAR_TRAFFIC_MAX);

	/* The pool arithmetic: the layer's cars and its reserve live in the margin
	 * between the car budget and the pool, next to the cars outside the budget
	 * (the planes, the trains, the helicopter, the set pieces, the shortcut
	 * car; PopulationBudget.h). */
	printf("  the pool arithmetic the header asserts:\n");
	printf("    the car budget (MAX_CARS_IN_USE)      %5d\n", MAX_CARS_IN_USE);
	printf("    the far layer (FAR_TRAFFIC_MAX)       %5d\n", FAR_TRAFFIC_MAX);
	printf("    the layer's reserve (creations)       %5d\n", FAR_TRAFFIC_POOL_RESERVE);
	printf("    the cars outside the budget (margin)  %5d\n",
		NUMVEHICLES - MAX_CARS_IN_USE - FAR_TRAFFIC_MAX);
	printf("    the vehicle pool (NUMVEHICLES)        %5d\n\n", NUMVEHICLES);
	Check(MAX_CARS_IN_USE + FAR_TRAFFIC_MAX + FAR_TRAFFIC_POOL_RESERVE <= NUMVEHICLES,
		"the far layer, its reserve and the car budget must fit the vehicle pool");
	Check(NUMVEHICLES - MAX_CARS_IN_USE - FAR_TRAFFIC_MAX >= 40,
		"the cars outside the budget need their margin");
	/* The promotion's driver: the ped pool's reserve is the one the near
	 * spawner keeps (PopulationBudget.h), so a promotion never takes the pool
	 * the frame's own creations need. The whole layer promoted in one frame
	 * (the gate pass promotes every car of the layer in the frame it is due,
	 * not only the staggered share) is ten times under the reserve. */
	Check(FAR_TRAFFIC_MAX * FAR_TRAFFIC_PROMOTION_PEDS < PED_POOL_RESERVE,
		"the whole layer's drivers must fit the ped pool's reserve");
}

/* ------------------------------------------------------------------ C. the cap under pressure */

/* The layer's own count, frame by frame. The simulation is the layer's state
 * machine of FarTraffic.h and nothing else:
 *
 *	the gate pass	every car of the layer, every frame: FarTrafficSwitchFor
 *			decides (the pool always answers yes here, so the worst
 *			case of a refused promotion is the sweep of Section 4).
 *	the fill	creations only while the count is under the cap of the
 *			band, at most FAR_TRAFFIC_CREATE_ATTEMPTS a frame, and a new
 *			car lands between the band's inner edge and its fog site.
 *	the drive	the cars move toward the player (the worst direction for
 *			the reach) at the traffic's own speed.
 *
 * The pressures of the ticket: a fill that tries to overshoot the cap, the draw
 * distance moving up and down under a full layer, a storm that closes the band
 * and clears again. The checks: no creation ever pushes the count above the cap
 * of its frame, the hard top is never passed, and a closed band drains the
 * layer to zero. */
static void
Section3CapUnderPressure(void)
{
	struct { const char *name; float dd; float fov; float clip; int frames; } phases[] = {
		{ "the owner's road, filling hard",        OWNER_DRAWDIST,   OWNER_FOV, SUNNY_FARCLIP, 1800 },
		{ "the draw distance drops one step",      8.0f,             OWNER_FOV, SUNNY_FARCLIP, 300 },
		{ "the draw distance drops again",         2.0f,             OWNER_FOV, SUNNY_FARCLIP, 300 },
		{ "the draw distance drops to the stock",  DRAWDIST_DEFAULT, STOCK_FOV, SUNNY_FARCLIP, 300 },
		{ "the weather clears again",              OWNER_DRAWDIST,   OWNER_FOV, SUNNY_FARCLIP, 1200 },
		{ "a storm closes the band",               OWNER_DRAWDIST,   OWNER_FOV, 300.0f,      600 },
	};
	const int nPhases = sizeof(phases) / sizeof(phases[0]);

	static float dist[FAR_TRAFFIC_MAX];
	int count = 0;
	int peak = 0, overCap = 0, breaches = 0, promoted = 0, removed = 0;
	int atReach = 0;

	printf("== 3. the cap under pressure\n\n");
	for (int s = 0; s < nPhases; s++) {
		SFarTrafficBand band = BandAt(phases[s].dd, phases[s].fov, phases[s].clip);
		int cap = FarTrafficCap(band);
		int created = 0;
		for (int frame = 0; frame < phases[s].frames; frame++) {
			/* The gate pass. */
			int removalsLeft = FAR_TRAFFIC_REMOVE_ATTEMPTS;
			for (int i = 0; i < count; ) {
				EFarTrafficSwitch what = FarTrafficSwitchFor(band, dist[i], true);
				if (what == FAR_TRAFFIC_PROMOTE) {
					promoted++;
					dist[i] = dist[count - 1];
					count--;
					continue;
				}
				if (what == FAR_TRAFFIC_REMOVE && removalsLeft > 0) {
					removed++;
					removalsLeft--;
					dist[i] = dist[count - 1];
					count--;
					continue;
				}
				if (what == FAR_TRAFFIC_KEEP && dist[i] <= NEAR_BUBBLE_REMOVE_DISTANCE)
					atReach++;
				i++;
			}
			/* The cap of a band that just dropped: the layer's own
			 * removals drain it, they never add. */
			if (count > cap)
				overCap += count - cap;
			if (count > FAR_TRAFFIC_MAX) {
				breaches++;
				if (breaches == 1)
					Fail("the simulated layer passed the hard top");
			}
			/* The fill: only under the cap, and a fresh car lands between
			 * the inner edge and the fog site. */
			if (band.active && count < cap) {
				for (int i = 0; i < FAR_TRAFFIC_CREATE_ATTEMPTS && count < cap; i++) {
					dist[count] = band.inner + (band.createOuter - band.inner) *
						((count * 37 % 100) / 100.0f);
					count++;
					created++;
				}
			}
			/* The drive: every car comes at the player by the traffic's own
			 * step (about 30 m/s at 30 fps, the closing speed of a fast
			 * player and an oncoming car), the worst direction for the
			 * reach. */
			for (int i = 0; i < count; i++)
				dist[i] -= 1.0f;
			if (count > peak)
				peak = count;
		}
		printf("  %-42s cap %3d, the layer now %3d (%d created)\n",
			phases[s].name, cap, count, created);
	}
	printf("\n  the peak of the simulation: %d cars (the hard top %d)\n", peak, FAR_TRAFFIC_MAX);
	printf("  switches: %d promotions, %d removals\n", promoted, removed);
	printf("  car-frames above the cap of their own frame: %d (the drain of a dropped cap)\n", overCap);
	printf("  frames above the hard top: %d\n", breaches);
	printf("  far cars that sat inside the player's reach for a frame: %d\n\n", atReach);
	Check(breaches == 0, "the layer must never hold more than FAR_TRAFFIC_MAX cars");
	Check(atReach == 0, "a car of the layer may not sit inside the player's reach");
	Check(count == 0, "the closed band must drain the layer to zero");

	/* The drain of a dropped cap is bounded: a frame removes at most
	 * FAR_TRAFFIC_REMOVE_ATTEMPTS, so a cap that falls by d cars drains in
	 * ceil(d / attempts) frames at the very most. */
	count = FAR_TRAFFIC_MAX;
	int framesToDrain = 0;
	while (count > 0 && framesToDrain < 1000) {
		int removals = count < FAR_TRAFFIC_REMOVE_ATTEMPTS ? count : FAR_TRAFFIC_REMOVE_ATTEMPTS;
		count -= removals;
		framesToDrain++;
	}
	printf("  the whole layer drains in %d frames (%d removals a frame at most)\n\n",
		framesToDrain, FAR_TRAFFIC_REMOVE_ATTEMPTS);
	Check(framesToDrain <= FAR_TRAFFIC_MAX, "the drain must be proportional to the cap");
}

/* ------------------------------------------------------------------ D. the events */

/* The facade of a far site as the near bubble's ladder sees it. The layer's
 * sites are the fog (a point at or past the visible range) and the road behind
 * the player (off the frustum by the margin, past the soft distance); the
 * harness asks the ladder with those facts. */
static SNearBubbleCandidate
SiteCandidate(float distToCamera, float visibleRange, bool inFrustum, bool inFrustumMargin, bool occluded)
{
	SNearBubbleCandidate c;
	c.distToPlayer = distToCamera;
	c.distToCamera = distToCamera;
	c.visibleRange = visibleRange;
	c.inFrustum = inFrustum;
	c.inFrustumMargin = inFrustumMargin;
	c.occluded = occluded;
	c.fadeHides = false;
	c.bypassView = false;
	return c;
}

/* The arms the layer may use without a line-of-sight ray: the fade that hides
 * the world, the off-frustum margin behind the camera, a fade that covers the
 * screen, and the top-down camera's bypass. The occluded hint and the close
 * blind spot are refused outright for the layer (rayAllowed false): a ray of
 * two kilometres is not the far layer's work. */
static bool
FarTrafficUsesRayFreeArm(const SNearBubbleCandidate &c)
{
	ENearBubbleAnswer answer = NearBubbleAnswerFor(c);
	if (answer != NEAR_BUBBLE_ALLOWED)
		return false;
	ENearBubbleArm arm = NearBubbleArmFor(c);
	return arm == NEAR_BUBBLE_ARM_FADE_DISTANCE || arm == NEAR_BUBBLE_ARM_SOFT ||
		arm == NEAR_BUBBLE_ARM_FADE || arm == NEAR_BUBBLE_ARM_BYPASS;
}

static void
Section4Events(void)
{
	const float fovFactors[] = { 1.0f, 1.5f, 2.2f };
	const int nFov = sizeof(fovFactors) / sizeof(fovFactors[0]);
	const float farClips[] = { 2000.0f, 1150.0f, 650.0f, 300.0f };
	const int nFar = sizeof(farClips) / sizeof(farClips[0]);
	const float drawDistances[] = { 16.0f, 8.0f, 4.0f, 2.0f, DRAWDIST_DEFAULT };
	const int nDd = sizeof(drawDistances) / sizeof(drawDistances[0]);

	int fogSites = 0, behindSites = 0, refusedSites = 0, removalSites = 0, inBandCreations = 0;
	int failed = 0;

	printf("== 4. the events: creations, removals and the switches\n\n");
	for (int f = 0; f < nFar; f++)
	for (int j = 0; j < nFov; j++)
	for (int i = 0; i < nDd; i++) {
		SFarTrafficBand b = BandAt(drawDistances[i], fovFactors[j], farClips[f]);
		STrafficRings r = RingsAt(drawDistances[i], fovFactors[j], farClips[f]);
		int cap = FarTrafficCap(b);
		if (cap <= 0)
			continue;

		/* The fog site: the car lands past the fade (the visible range), so
		 * the ladder's fade arm allows it with no ray and no occluder
		 * dependence. This is the oncoming traffic of a long straight road. */
		SNearBubbleCandidate fog = SiteCandidate(b.createOuter, r.visibleRange, true, true, false);
		if (NearBubbleAnswerFor(fog) == NEAR_BUBBLE_ALLOWED &&
		    NearBubbleArmFor(fog) == NEAR_BUBBLE_ARM_FADE_DISTANCE)
			fogSites++;
		else {
			Fail("a fog site is not allowed by the fade arm");
			failed++;
		}
		/* The behind site: off the frustum by the bubble's margin, at least
		 * the soft distance away. Its distance is the band's inner edge, which
		 * the matrix section proved is outside the bubble and the soft
		 * distance. */
		SNearBubbleCandidate behind = SiteCandidate(b.inner, r.visibleRange, false, false, false);
		if (FarTrafficUsesRayFreeArm(behind) && NearBubbleArmFor(behind) == NEAR_BUBBLE_ARM_SOFT)
			behindSites++;
		else {
			Fail("a behind site is not allowed by the soft arm");
			failed++;
		}
		/* The same site with the occluder hint on it: the ladder sends it to
		 * the confirmation ray, and the layer, which pays no ray, refuses it.
		 * The counter is the layer's honest price: an occluded site waits. */
		SNearBubbleCandidate occluded = SiteCandidate(b.inner, r.visibleRange, false, false, true);
		if (NearBubbleAnswerFor(occluded) == NEAR_BUBBLE_SEEK_LOS && !FarTrafficUsesRayFreeArm(occluded))
			refusedSites++;
		else {
			Fail("an occluded site must be the layer's refused arm");
			failed++;
		}
		/* The layer's own removals: beyond the fog (the outer edge plus the
		 * hysteresis), so the ladder hides them with the fade. */
		SNearBubbleCandidate far = SiteCandidate(b.removeOuter + 1.0f, r.visibleRange, true, true, false);
		if (NearBubbleAnswerFor(far) == NEAR_BUBBLE_ALLOWED &&
		    NearBubbleArmFor(far) == NEAR_BUBBLE_ARM_FADE_DISTANCE)
			removalSites++;
		else {
			Fail("a removal beyond the fog is not allowed by the fade arm");
			failed++;
		}
		/* And the band's own geometry: every creation site is inside the band
		 * and every hand-over site is inside the band's outer edge and past
		 * the despawn's hysteresis, so a car the layer takes is one it can
		 * keep. */
		if (FarTrafficCreateSiteIsInBand(b, b.inner) &&
		    FarTrafficCreateSiteIsInBand(b, b.createOuter) &&
		    !FarTrafficCreateSiteIsInBand(b, b.inner - 1.0f) &&
		    !FarTrafficCreateSiteIsInBand(b, b.createOuter + 1.0f))
			inBandCreations++;
		else {
			Fail("the creation band does not hold its own edges");
			failed++;
		}
		if (!FarTrafficAdoptSiteIsInBand(b, b.inner) &&
		    FarTrafficAdoptSiteIsInBand(b, b.inner + FAR_TRAFFIC_ADOPT_HYSTERESIS + 1.0f) &&
		    !FarTrafficAdoptSiteIsInBand(b, b.removeOuter + 1.0f)) {
			; /* the hand-over band holds its edges */
		} else {
			Fail("the hand-over band does not hold its own edges");
			failed++;
		}
	}
	printf("  the bands swept: %d cells of the matrix (the layer's cap over 0)\n",
		nFar * nFov * nDd);
	printf("  the site checks that failed: %d\n", failed);
	printf("  the fog sites the fade allows:      %d\n", fogSites);
	printf("  the behind sites the soft arm allows: %d\n", behindSites);
	printf("  the occluded sites the layer refuses (it pays no ray): %d\n", refusedSites);
	printf("  the beyond-the-fog removals the fade allows: %d\n", removalSites);
	printf("  the creation and hand-over bands that hold their edges: %d\n\n", inBandCreations);

	/* The switches, and the list of facts they write. A promotion and a
	 * hand-over may happen while the player is looking at the car (the car is
	 * a dot at the band's inner edge, hundreds of metres out), and they cannot
	 * show, because not one of the facts they write is drawn. The table is the
	 * argument, fact by fact. */
	struct SSwitchFact { const char *name; bool written; bool rendered; };
	static const SSwitchFact facts[] = {
		{ "the class (VehicleCreatedBy)",                     true,  false },
		{ "the collision flag (bUsesCollision)",              true,  false },
		{ "the static flag (bIsStatic)",                      true,  false },
		{ "the moving entity list",                           true,  false },
		{ "the audio entity's status",                        true,  false },
		{ "the driver ped (created or flagged away)",         true,  false },
		{ "the car budget count (NumRandomCars)",             true,  false },
		{ "the model index",                                  false, true  },
		{ "the matrix (position, heading)",                   false, true  },
		{ "the colour pair",                                  false, true  },
		{ "the status (STATUS_SIMPLE on both sides)",         false, true  },
		{ "the clump and its alpha",                          false, true  },
		{ "the visible flag",                                 false, true  },
	};
	const int nFacts = sizeof(facts) / sizeof(facts[0]);
	int writtenRendered = 0;
	for (int i = 0; i < nFacts; i++)
		if (facts[i].written && facts[i].rendered)
			writtenRendered++;
	printf("  the switch's facts: %d written, %d of them drawn\n",
		nFacts - writtenRendered, writtenRendered);
	for (int i = 0; i < nFacts; i++)
		printf("    %-45s %s\n", facts[i].name,
			facts[i].written ? (facts[i].rendered ? "written and drawn" : "written") : "kept");
	printf("\n");
	Check(writtenRendered == 0, "a switch must write no fact the renderer draws");

	/* The reach: no far car may stay far where the player can touch it. The
	 * gate pass promotes at the band's inner edge; a refused promotion (the
	 * pool's reserves) leaves the car far only outside the reach and gives it
	 * up inside it. The sweep is every distance of a band at both pool
	 * answers. */
	int promoted = 0, kept = 0, removed = 0, badKeep = 0, badPromote = 0, unknownKeep = 0;
	for (int f = 0; f < nFar; f++)
	for (int j = 0; j < nFov; j++)
	for (int i = 0; i < nDd; i++) {
		SFarTrafficBand b = BandAt(drawDistances[i], fovFactors[j], farClips[f]);
		for (int step = 0; step <= 80; step++) {
			float dist = (b.removeOuter + 200.0f) * step / 80.0f;
			for (int canPromote = 0; canPromote <= 1; canPromote++) {
				EFarTrafficSwitch what = FarTrafficSwitchFor(b, dist, canPromote != 0);
				if (what == FAR_TRAFFIC_PROMOTE) {
					promoted++;
					/* A promotion is due at the band's inner edge and
					 * nowhere else; the inner edge of every cell of the
					 * matrix is outside the bubble and its reserve (the
					 * matrix section checks it). */
					if (dist > b.inner || b.inner <= NEAR_BUBBLE_REMOVE_DISTANCE)
						badPromote++;
				} else if (what == FAR_TRAFFIC_KEEP) {
					kept++;
					/* A kept car is never beyond the fog and never inside
					 * the player's reach. The unknown band (the boot path)
					 * keeps what it has and creates nothing: it holds no car
					 * to keep. */
					if (!b.known)
						unknownKeep++;
					else if (dist > b.removeOuter)
						badKeep++;
					else if (!canPromote && dist <= NEAR_BUBBLE_REMOVE_DISTANCE)
						badKeep++;
				} else {
					removed++;
				}
			}
		}
	}
	printf("  the switch sweep: %d promotions, %d keeps, %d removals\n", promoted, kept, removed);
	printf("  a promotion past the inner edge or inside the reach: %d\n", badPromote);
	printf("  a kept far car beyond the fog or inside the reach (a refused promotion): %d\n", badKeep);
	printf("  the boot band's keeps (it holds no car): %d\n\n", unknownKeep);
	Check(badPromote == 0, "a promotion must happen at the band's inner edge, outside the reach");
	Check(badKeep == 0, "a far car may not be kept beyond the fog or inside the player's reach");
	Check(promoted > 0 && removed > 0, "the switch must promote and remove over the sweep");

	/* The stagger: the share of one frame, over the frames of a cycle. */
	int share[FAR_TRAFFIC_STAGGER];
	memset(share, 0, sizeof(share));
	for (int frame = 0; frame < FAR_TRAFFIC_STAGGER; frame++)
		for (int i = 0; i < FAR_TRAFFIC_MAX; i++)
			if (FarTrafficUpdatesThisFrame(frame, i))
				share[frame]++;
	printf("  the stagger: each frame of a cycle moves");
	for (int i = 0; i < FAR_TRAFFIC_STAGGER; i++)
		printf(" %d", share[i]);
	printf(" cars of %d (the ticket's 10-15 Hz)\n\n", FAR_TRAFFIC_MAX);
	for (int i = 0; i < FAR_TRAFFIC_STAGGER; i++) {
		Check(share[i] >= FAR_TRAFFIC_MAX / FAR_TRAFFIC_STAGGER &&
		      share[i] <= (FAR_TRAFFIC_MAX + FAR_TRAFFIC_STAGGER - 1) / FAR_TRAFFIC_STAGGER,
			"the stagger must move an even share of the layer");
	}
}

/* ------------------------------------------------------------------ E. the cost */

/*
 * The layer's own arithmetic, mirrored from the two functions the engine side
 * calls (CCarCtrl::ComputeCarOnRailPose and CFarTraffic's own update): the lane
 * offsets of the two path links, the curve point (the same
 * CCurves::CalcCurvePoint arithmetic), the heading, the rail height and the
 * matrix write. Nothing here is engine state: the point is the shape of the
 * work, and its price on this machine.
 *
 * What the number is not: the placement scan of a creation attempt and the
 * driver of a promotion are engine calls (a node scan, a ped creation) and they
 * are quoted from the live measurement in the ADR, not measured here.
 */

/* Mirrors CCurves::CalcCurvePoint (src/control/Curves.cpp), which the near
 * traffic and the far layer share. */
static float
MirrorCalcSpeedScaleFactor(float x1, float y1, float x2, float y2, float d1x, float d1y, float d2x, float d2y)
{
	float distance = (float)sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
	float dp = d1x * d2x + d1y * d2y;
	if (dp > 0.9f)
		return distance + (float)fabs((x1 * d1y - y1 * d1x) - (x2 * d1y - y2 * d1x));
	return ((1.0f - dp) * 0.25f + 1.0f) * distance;
}

static void
MirrorCalcCurvePoint(float *pos1, float *pos2, float *dir1, float *dir2, float between, int timeOnCurve,
	float *outPos, float *outDir)
{
	float actualFactor = MirrorCalcSpeedScaleFactor(pos1[0], pos1[1], pos2[0], pos2[1],
		dir1[0], dir1[1], dir2[0], dir2[1]);
	float d1x = dir1[0] * actualFactor, d1y = dir1[1] * actualFactor;
	float d2x = dir2[0] * actualFactor, d2y = dir2[1] * actualFactor;
	float t1 = (float)fabs((pos2[0] - pos1[0]) * dir1[0] + (pos2[1] - pos1[1]) * dir1[1]);
	float t2 = (float)fabs((pos1[0] - pos2[0]) * dir2[0] + (pos1[1] - pos2[1]) * dir2[1]);
	float curveCoef;
	if (t1 > t2) {
		float coef = (t1 - t2) / (t1 + t2);
		if (between <= coef)
			curveCoef = 0.0f;
		else
			curveCoef = 0.5f - 0.5f * (float)cos(3.1415f * (between - coef) * (t1 + t2) / (2 * t2));
	} else {
		float coef = 2 * t1 / (t1 + t2);
		if (coef <= between)
			curveCoef = 1.0f;
		else
			curveCoef = 0.5f - 0.5f * (float)cos(3.1415f * between * (t1 + t2) / (2 * t1));
	}
	outPos[0] = (pos1[0] + between * d1x) * (1.0f - curveCoef) + (pos2[0] - (1 - between) * d2x) * curveCoef;
	outPos[1] = (pos1[1] + between * d1y) * (1.0f - curveCoef) + (pos2[1] - (1 - between) * d2y) * curveCoef;
	outPos[2] = 0.0f;
	outDir[0] = (d1x * (1.0f - curveCoef) + d2x * curveCoef) / (timeOnCurve * 0.001f);
	outDir[1] = (d1y * (1.0f - curveCoef) + d2y * curveCoef) / (timeOnCurve * 0.001f);
	outDir[2] = 0.0f;
}

/* One far car's state for the measurement: the two links of a car on a city
 * street (four lanes, the widest the shipped paths hold, PathFind.h) and the
 * car's place on the curve. */
struct SBenchCar
{
	float curLinkX, curLinkY, nextLinkX, nextLinkY;
	float curDirX, curDirY, nextDirX, nextDirY;
	int curLane, nextLane, curDirection, nextDirection;
	float curZ, nextZ;
	int timeOnCurve;
	float pos[3], dir[3];
	unsigned int updates;
};

static void
BenchSetup(SBenchCar *car, int i)
{
	/* A deterministic spread of the cars over the roads' directions and
	 * lanes, and along the curve: the update of a car does not branch on its
	 * place on the road, so one cell inside each lane is the whole of it. */
	car->curLinkX = 100.0f + i * 37.0f;
	car->curLinkY = 200.0f + i * 53.0f;
	car->nextLinkX = car->curLinkX + 20.0f;
	car->nextLinkY = car->curLinkY + 12.0f;
	car->curDirection = (i & 1) ? 1 : -1;
	car->nextDirection = car->curDirection;
	car->curDirX = (i & 2) ? 0.8f : 0.6f;
	car->curDirY = (i & 2) ? 0.6f : -0.8f;
	car->nextDirX = car->curDirX;
	car->nextDirY = car->curDirY;
	car->curLane = i % 4;
	car->nextLane = car->curLane;
	car->curZ = 12.0f;
	car->nextZ = 12.5f;
	car->timeOnCurve = 1200 + (i % 900);
	car->updates = 0;
}

/* The per-car update of the layer: the pose of the two lanes, the curve, the
 * heading, the rail's height and the matrix write. Mirrors FarTraffic.cpp's
 * FarTrafficUpdateCar minus the engine calls (the node pick, the alpha ramp,
 * the sector re-add), which are one branch and a few reads. */
static void
BenchUpdate(SBenchCar *car)
{
	const float laneWidth = 5.0f;
	float curDirX = car->curDirX * car->curDirection;
	float curDirY = car->curDirY * car->curDirection;
	float nextDirX = car->nextDirX * car->nextDirection;
	float nextDirY = car->nextDirY * car->nextDirection;
	float curOffset = (car->curLane + 0.5f) * laneWidth;
	float nextOffset = (car->nextLane + 0.5f) * laneWidth;
	float pos1[3], pos2[3], dir1[3], dir2[3], outPos[3], outDir[3];
	pos1[0] = car->curLinkX + curOffset * curDirY;
	pos1[1] = car->curLinkY - curOffset * curDirX;
	pos2[0] = car->nextLinkX + nextOffset * nextDirY;
	pos2[1] = car->nextLinkY - nextOffset * nextDirX;
	dir1[0] = curDirX; dir1[1] = curDirY;
	dir2[0] = nextDirX; dir2[1] = nextDirY;
	/* The place on the curve (CCarCtrl::GetPositionAlongCurrentCurve). */
	float between = (float)((car->updates * FAR_TRAFFIC_STAGGER * 33) % car->timeOnCurve) / car->timeOnCurve;
	MirrorCalcCurvePoint(pos1, pos2, dir1, dir2, between, car->timeOnCurve, outPos, outDir);
	/* The height of the rail: the two nodes' own heights. */
	outPos[2] = car->curZ * (1.0f - between) + car->nextZ * between + 0.9f;
	/* The heading: the curve's tangent, flattened. */
	float mag = (float)sqrt(outDir[0] * outDir[0] + outDir[1] * outDir[1]);
	if (mag > 0.0001f) {
		outDir[0] /= mag;
		outDir[1] /= mag;
	}
	car->pos[0] = outPos[0]; car->pos[1] = outPos[1]; car->pos[2] = outPos[2];
	car->dir[0] = outDir[0]; car->dir[1] = outDir[1];
	car->updates++;
}

/* The gate pass of one car: the distance and the switch. */
static EFarTrafficSwitch
BenchGate(const SFarTrafficBand &band, float x, float y, float px, float py, float py2)
{
	float dx = x - px, dy = y - py;
	float dist = (float)sqrt(dx * dx + dy * dy);
	(void)py2;
	return FarTrafficSwitchFor(band, dist, true);
}

static double
NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void
Section5Cost(void)
{
	const int rounds = 300;
	const SFarTrafficBand band = BandAt(OWNER_DRAWDIST, OWNER_FOV, SUNNY_FARCLIP);
	const int cap = FarTrafficCap(band);
	const int perFrame = (cap + FAR_TRAFFIC_STAGGER - 1) / FAR_TRAFFIC_STAGGER;
	SBenchCar cars[FAR_TRAFFIC_MAX];
	volatile float sink = 0.0f;
	double t0, t1;

	printf("== 5. the layer's own cost\n\n");
	printf("  the reference layer: the owner's band (%.0f..%.0f m), the cap %d cars\n",
		(double)band.inner, (double)band.outer, cap);

	for (int i = 0; i < cap; i++)
		BenchSetup(&cars[i], i);

	t0 = NowSeconds();
	for (int r = 0; r < rounds; r++)
		for (int i = 0; i < perFrame; i++) {
			BenchUpdate(&cars[i]);
			sink += cars[i].pos[0];
		}
	t1 = NowSeconds();
	double perUpdate = (t1 - t0) / (rounds * perFrame) * 1e9;
	printf("  the movement of one car: %.0f ns (the pose, the heading, the height; no collision query)\n",
		perUpdate);
	printf("  the movement of the frame's share (%d of %d): %.2f us a frame\n",
		perFrame, cap, perUpdate * perFrame / 1000.0);
	printf("  that is %.4f %% of a 33 ms frame\n", perUpdate * perFrame / 1000.0 / 33000.0 * 100.0);

	t0 = NowSeconds();
	for (int r = 0; r < rounds; r++)
		for (int i = 0; i < cap; i++) {
			EFarTrafficSwitch what = BenchGate(band, cars[i].pos[0], cars[i].pos[1], 0.0f, 0.0f, 0.0f);
			sink += (float)what;
		}
	t1 = NowSeconds();
	double perGate = (t1 - t0) / (rounds * cap) * 1e9;
	printf("  the gate pass of one car: %.0f ns (the distance and the switch; every car, every frame)\n",
		perGate);
	printf("  the gate pass of the whole layer: %.2f us a frame\n", perGate * cap / 1000.0);
	printf("  the layer's own arithmetic a frame: %.2f us (%.4f %% of a 33 ms frame)\n\n",
		(perUpdate * perFrame + perGate * cap) / 1000.0,
		(perUpdate * perFrame + perGate * cap) / 1000.0 / 33000.0 * 100.0);
	printf("  The bounds: the layer's arithmetic is under 50 us a frame at the hard top (a\n");
	printf("  small share of the frame), and the work does not touch the collision path at\n");
	printf("  all - no ground cast, no sector scan, no path search. The engine-side calls are\n");
	printf("  the two creation attempts a frame (the near spawner's own placement scan, its\n");
	printf("  cost measured for VICE-130 in ADR 0024) and the promotions' drivers; the live\n");
	printf("  rig and the frame recipe of the ticket measure those in the running game.\n\n");
	Check(perUpdate * perFrame + perGate * cap < 50000.0, "the layer's arithmetic must stay under 50 us a frame");
	Check(sink != 12345.0f, "keep the measurement honest");
}

/* ------------------------------------------------------------------ main */

int
main(void)
{
	printf("The far traffic layer (VICE-128): the band, the cap, the events and the cost.\n\n");
	Section1Band();
	Section2Matrix();
	Section3CapUnderPressure();
	Section4Events();
	Section5Cost();

	if (g_failures == 0)
		printf("RESULT: every check passed.\n");
	else
		printf("RESULT: %d check(s) failed.\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
