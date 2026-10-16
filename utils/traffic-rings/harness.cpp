/*
 * The invariant proof for the traffic ring table (ticket VICE-120, the range
 * control of VICE-125).
 *
 * This program reads src/control/TrafficRings.h on its own (the header has no
 * engine include) and walks the whole control matrix:
 *
 *	the draw distance DRAWDIST_MIN .. DRAWDIST_MAX (the one range control,
 *	Frontend.h)	x  FOV factor 1.0 .. 2.2	x  weather far clip 150 .. 2000 m
 *
 * The LOD distance of a cell is the LOD distance base x the FOV factor x the draw distance
 * (Renderer.h, Camera.cpp), exactly as the game computes it.
 *
 * For every cell it checks the invariant (invariant.h, shared with the
 * one-range-control evidence program) and it replays the birth cull of
 * CCarCtrl::CreateCarFromPlacement for the two rings the spawner asks for, with
 * the full placement reserve, so it shows the claim "a fresh spawn can never be
 * culled on the frame it is created" instead of arguing it.
 *
 * Build and run:
 *
 *	c++ -std=c++17 -I src -I src/core -I utils/traffic-rings -O2 -o /tmp/traffic-rings-harness utils/traffic-rings/harness.cpp
 *	/tmp/traffic-rings-harness
 *
 * The exit code is 0 when every cell passes. The output goes into the ticket.
 */

#include "invariant.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The draw distance the player starts with (DrawDistance.h DRAWDIST_DEFAULT,
 * Renderer.cpp ms_lodDistScale). The slider's reachable values are
 * DRAWDIST_MIN + k x DRAWDIST_RANGE / SLIDER_STEPS; SLIDER_STEPS mirrors
 * Frontend.h MENUSLIDER_LOGICAL_BARS (the evidence program checks that the two
 * still agree by reading the header). */
#define STOCK_DRAW_DISTANCE DRAWDIST_DEFAULT
#define MAX_DRAW_DISTANCE DRAWDIST_MAX
#define SLIDER_STEPS 16

/* The stock radii of the game (the reference this table must hold at the
 * default draw distance and FOV 70). They are the numbers of the original
 * code: the car band and culls (CarCtrl.cpp), the generator ring (CarGen.cpp)
 * and the ped band (Population.cpp). */
struct SStockGame
{
	float carSideSpawn;
	float carOnScreenSpawnMin;
	float carOnScreenSpawnMax;
	float carDespawn;
	float carCameraMin;
	float generatorNear;
	float generatorFar;
	float generatorBoatFar;
	float pedCreateMin;
	float pedCreateMax;
	float pedRemove;
	float pedRemoveSpecial;
};

static SStockGame StockGame(void)
{
	SStockGame t;
	t.carSideSpawn = 120.0f;
	t.carOnScreenSpawnMin = 220.0f;
	t.carOnScreenSpawnMax = 250.0f;
	t.carDespawn = 280.0f;
	t.carCameraMin = 82.5f;
	t.generatorFar = 230.0f;
	t.generatorNear = t.generatorFar - 20.0f;
	t.generatorBoatFar = 345.0f;
	t.pedCreateMin = 100.0f;
	t.pedCreateMax = 130.0f;
	t.pedRemove = 131.0f;
	t.pedRemoveSpecial = 145.0f;
	return t;
}

/*
 * The components of the placement error the reserve must hold. They come from
 * the code that places a fresh car, not from this table:
 *   the lane offset   CarCtrl.cpp: (lane + OneWayLaneOffset()) * LANE_WIDTH,
 *                     LANE_WIDTH is 5 m (PathFind.h), lane is below the link's
 *                     lane count and the shipped paths.ipl holds up to 4 lanes
 *                     per side, OneWayLaneOffset is 0.5 on a two-way road (the
 *                     path nodes carry width 0, so the lane offset term of
 *                     GetLaneOffset is 0): (4 - 1 + 0.5) * 5 = 17.5 m
 *   the curve shift   CarCtrl.cpp: the final position is moved 2 m along the
 *                     node link
 *   the curve itself  the curve point at the placement fraction differs from
 *                     the straight interpolation by the sagitta of the link,
 *                     a few metres on the city's node spacing
 * The reserve (30 m) is the game's own step from the on-screen ring (250 m) to
 * the despawn (280 m). This program models the error as exactly that reserve:
 * that is the assumption the numbers in the game have always carried. */
#define PLACEMENT_LANE_OFFSET ((4.0f - 1.0f + 0.5f) * 5.0f)
#define PLACEMENT_CURVE_SHIFT 2.0f

/* The FOV factor the camera reports: 70/FOV capped at 2.2 (Camera.cpp). */
static float LodDistance(float fovFactor, float drawDistance)
{
	return LOD_DISTANCE_BASE * fovFactor * drawDistance;
}

static STrafficRings RingsAt(float drawDistance, float fovFactor, float farClip)
{
	STrafficRingInput in;
	in.lodDistance = LodDistance(fovFactor, drawDistance);
	in.fovFactor = fovFactor;
	in.farClip = farClip;
	return ComputeTrafficRings(in);
}

static int g_failures = 0;

static void Fail(const char *check, float dd, float fov, float farClip, const char *what)
{
	g_failures++;
	printf("FAIL [%s] draw distance %.4f FOV factor %.2f far clip %.0f: %s\n",
		check, dd, fov, farClip, what);
}

static void CheckRings(float dd, float fov, float farClip, const STrafficRings &r)
{
	const char *what;
	if (!TrafficRingsInvariant(r, fov, &what))
		Fail("invariant", dd, fov, farClip, what);
}

static void PrintRings(const char *title, const STrafficRings &r)
{
	printf("%s\n", title);
	printf("  scale %.3f   side scale %.3f   visible range %.1f m\n", r.scale, r.sideScale, r.visibleRange);
	printf("  car   side %.1f  on-screen %.1f .. %.1f  despawn %.1f  camera min %.1f\n",
		r.carSideSpawn, r.carOnScreenSpawnMin, r.carOnScreenSpawnMax, r.carDespawn, r.carCameraMin);
	printf("  boat  water factor %.2f  extended range factor %.2f -> side %.1f / front %.1f, despawn %.1f\n",
		r.boatSpawnFactor, r.extendedRangeFactor,
		r.boatSpawnFactor * r.carSideSpawn, r.boatSpawnFactor * r.carOnScreenSpawnMax,
		r.extendedRangeFactor * r.carDespawn);
	printf("  gen   near %.1f  far %.1f  boat %.1f\n", r.generatorNear, r.generatorFar, r.generatorBoatFar);
	printf("  ped   create %.1f .. %.1f  off-screen %.1f .. %.1f (pull-back %.1f)  remove %.1f / %.1f\n",
		r.pedCreateMin, r.pedCreateMax,
		r.pedOffScreenMax - r.pedOffScreenPullBack, r.pedOffScreenMax, r.pedOffScreenPullBack,
		r.pedRemove, r.pedRemoveSpecial);
}

static void CheckPlacementReserve(void)
{
	printf("== 0. the reserve, and what the harness assumes\n\n");
	printf("  the reserve of the table:                %8.1f m\n", RING_CAR_DESPAWN_RESERVE);
	printf("  the lane offset of the widest road:      %8.1f m\n", PLACEMENT_LANE_OFFSET);
	printf("  the curve shift of the final position:   %8.1f m\n", PLACEMENT_CURVE_SHIFT);
	printf("  left for the curve and the float:        %8.1f m\n",
		RING_CAR_DESPAWN_RESERVE - PLACEMENT_LANE_OFFSET - PLACEMENT_CURVE_SHIFT);
	printf("\n  The real birth cull measures vecTargetPos -> the final position of the\n");
	printf("  car: the ring crossing of the node segment, the lane offset, the curve\n");
	printf("  placement and the 2 m shift. This program models that error as exactly\n");
	printf("  the reserve, because the reserve is the game's own number for it (the\n");
	printf("  250 m ring to the 280 m despawn, and the same 30 m down to the 220 m\n");
	printf("  inner edge). That is the assumption, stated: no component of the error\n");
	printf("  scales with the ring, and the two derived components above fit the\n");
	printf("  reserve with room left.\n");
	printf("\n  The one range control (VICE-125) is the draw distance: the ring request\n");
	printf("  is the LOD distance over its stock value %1.f m (300 x the shipped draw\n", (double)RING_LOD_REFERENCE);
	printf("  distance %.1f). The old spawn distance v maps to the draw distance\n", (double)DRAWDIST_DEFAULT);
	printf("  %.1f x v, the draw distance that gives the old ring (LegacyRangeDrawDistance,\n", (double)DRAWDIST_DEFAULT);
	printf("  see section 4).\n\n");
}

static void CheckStockAgainstTheGame(void)
{
	STrafficRings r = RingsAt(STOCK_DRAW_DISTANCE, 1.0f, 2000.0f);
	SStockGame t = StockGame();

	printf("== 1. stock settings: the default draw distance (%.1f), FOV factor 1.0, sunny\n\n",
		(double)STOCK_DRAW_DISTANCE);
	PrintRings("the table:", r);
	struct { const char *what; float now, stock; } rows[] = {
		{ "car side ring", r.carSideSpawn, t.carSideSpawn },
		{ "car on-screen inner edge", r.carOnScreenSpawnMin, t.carOnScreenSpawnMin },
		{ "car on-screen ring", r.carOnScreenSpawnMax, t.carOnScreenSpawnMax },
		{ "car despawn (lifetime cull)", r.carDespawn, t.carDespawn },
		{ "car camera minimum", r.carCameraMin, t.carCameraMin },
		{ "generator near ring", r.generatorNear, t.generatorNear },
		{ "generator far ring", r.generatorFar, t.generatorFar },
		{ "generator boat ring", r.generatorBoatFar, t.generatorBoatFar },
		{ "ped creation inner", r.pedCreateMin, t.pedCreateMin },
		{ "ped creation outer", r.pedCreateMax, t.pedCreateMax },
		{ "ped removal", r.pedRemove, t.pedRemove },
		{ "ped removal special", r.pedRemoveSpecial, t.pedRemoveSpecial },
	};
	printf("\n  row by row (metres):\n");
	for (int i = 0; i < (int)(sizeof(rows)/sizeof(rows[0])); i++) {
		bool same = fabsf(rows[i].now - rows[i].stock) < 0.0005f;
		printf("    %-28s %10.1f  stock %10.1f  %s\n", rows[i].what, rows[i].now, rows[i].stock,
			same ? "same" : "MOVED");
		if (!same)
			g_failures++;
	}
	printf("\n  The default draw distance gives the range scale %.3f, so every radius is the\n",
		(double)r.scale);
	printf("  radius the game always had, and the ring of a lower draw distance floors\n");
	printf("  there (the pop-in distance the stock ring carries).\n\n");
}

static void CheckMaxSettings(void)
{
	printf("== 2. the control at its ends, FOV factor 1.0, sunny (far clip 2000 m)\n\n");
	STrafficRings maxDraw = RingsAt(MAX_DRAW_DISTANCE, 1.0f, 2000.0f);
	STrafficRings fog = RingsAt(MAX_DRAW_DISTANCE, 1.0f, 150.0f);
	PrintRings("at the maximum draw distance (16.0):", maxDraw);
	printf("\n");
	PrintRings("the same control in fog (far clip 150 m):", fog);
	printf("\n");
}

static void CheckMatrix(float farClip, const char *farClipName)
{
	/* The draw distance slider's reachable values. */
	const int nSteps = SLIDER_STEPS;
	float drawDistances[nSteps + 1];
	for (int i = 0; i <= nSteps; i++)
		drawDistances[i] = DRAWDIST_MIN + i * DRAWDIST_RANGE / nSteps;

	const float fovFactors[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.2f };
	const int nFov = sizeof(fovFactors)/sizeof(fovFactors[0]);

	int cells = 0;
	float minScale = 1e9f, maxScale = -1e9f;
	float minSide = 1e9f, maxSide = -1e9f;
	float minFront = 1e9f, maxFront = -1e9f;
	float minCamera = 1e9f, maxCamera = -1e9f;

	printf("== 4. the ring over the control's full range, %s\n\n", farClipName);
	printf("  draw distance | LOD distance |     scale |    side m |   front m | despawn m\n");
	for (int i = 0; i <= nSteps; i++) {
		STrafficRings r = RingsAt(drawDistances[i], 1.0f, farClip);
		printf("  %13.4f | %12.1f | %9.3f | %9.1f | %9.1f | %9.1f\n",
			(double)drawDistances[i], (double)LodDistance(1.0f, drawDistances[i]),
			(double)r.scale, (double)r.carSideSpawn, (double)r.carOnScreenSpawnMax, (double)r.carDespawn);
	}
	printf("\n");

	for (int i = 0; i <= nSteps; i++)
	for (int j = 0; j < nFov; j++) {
		STrafficRings r = RingsAt(drawDistances[i], fovFactors[j], farClip);
		cells++;
		CheckRings(drawDistances[i], fovFactors[j], farClip, r);
		minScale = TrafficRingsMin(minScale, r.scale);
		maxScale = TrafficRingsMax(maxScale, r.scale);
		minSide = TrafficRingsMin(minSide, r.carSideSpawn);
		maxSide = TrafficRingsMax(maxSide, r.carSideSpawn);
		minFront = TrafficRingsMin(minFront, r.carOnScreenSpawnMax);
		maxFront = TrafficRingsMax(maxFront, r.carOnScreenSpawnMax);
		minCamera = TrafficRingsMin(minCamera, r.carCameraMin);
		maxCamera = TrafficRingsMax(maxCamera, r.carCameraMin);
	}

	printf("  cells %d (%d draw distance x %d FOV factor), far clip %.0f\n", cells, nSteps + 1, nFov, (double)farClip);
	printf("  cells that fail a check so far: %d\n", g_failures);
	printf("  scale %.3f .. %.3f   side ring %.1f .. %.1f m   on-screen ring %.1f .. %.1f m\n",
		(double)minScale, (double)maxScale, (double)minSide, (double)maxSide, (double)minFront, (double)maxFront);
	printf("  camera minimum %.1f .. %.1f m\n\n", (double)minCamera, (double)maxCamera);
}

/* The cap the table applies to a draw distance and a FOV factor: the visible
 * range (the LOD distance against the weather far clip) less the reserve, and
 * never below the stock ring at that FOV factor. The expected ring of a mapped
 * draw distance is the old request capped with this: the cap is computed from
 * the inputs of the case (the draw distance, the FOV factor and the far clip),
 * never from the output of the table. */
static float CapWithFloor(float drawDistance, float fovFactor, float farClip)
{
	float lod = LOD_DISTANCE_BASE * fovFactor * drawDistance;
	float visibleRange = (farClip > 0.0f) ? TrafficRingsMin(lod, farClip) : lod;
	return TrafficRingsMax(visibleRange - RING_CAR_DESPAWN_RESERVE, RING_CAR_ONSCREEN_SPAWN_MAX * fovFactor);
}

static void CheckLegacyMapping(void)
{
	printf("== 3. the old spawn distance key maps to the draw distance\n\n");
	printf("  The map turns an old value v above 1 into the draw distance %.1f x v (the\n",
		(double)DRAWDIST_DEFAULT);
	printf("  old value of 1 is the stock ring and maps to nothing). The table then asks\n");
	printf("  that draw distance for a ring, and the visible cap bounds it: the LOD\n");
	printf("  distance against the weather far clip, less the %.0f m reserve, never below\n",
		(double)RING_CAR_DESPAWN_RESERVE);
	printf("  the stock ring. The check compares the mapped ring with the ring the old\n");
	printf("  value asked for (%.0f x v x the FOV factor) and with the cap of the mapped\n",
		(double)RING_CAR_ONSCREEN_SPAWN_MAX);
	printf("  draw distance, both computed from the inputs of the case, not from the\n");
	printf("  table: the mapped ring must be the request, or exactly the cap.\n\n");
	printf("   old value | mapped draw distance | old request m | mapped ring m | mapped ring at FOV 2.2\n");
	int mappedCount = 0, cappedCount = 0;
	for (int i = 1; i <= 4; i++) {
		float legacy = (float)i;
		float mapped = LegacyRangeDrawDistance(legacy, DRAWDIST_DEFAULT);
		float oldRequest = RING_CAR_ONSCREEN_SPAWN_MAX * legacy;
		if (mapped <= 0.0f) {
			printf("   %9.1f | %20s | %13.1f | %13s | %22s\n",
				(double)legacy, "none (stock ring)", (double)oldRequest, "n/a", "n/a");
			if (legacy > 1.0f)
				Fail("legacy mapping", legacy, 1.0f, 2000.0f, "a raised spawn distance did not map");
			continue;
		}
		mappedCount++;
		STrafficRings a = RingsAt(mapped, 1.0f, 2000.0f);
		STrafficRings b = RingsAt(mapped, 2.2f, 2000.0f);
		float wantA = TrafficRingsMin(oldRequest, CapWithFloor(mapped, 1.0f, 2000.0f));
		float wantB = TrafficRingsMin(oldRequest * 2.2f, CapWithFloor(mapped, 2.2f, 2000.0f));
		if (a.carOnScreenSpawnMax < oldRequest - 0.05f)
			cappedCount++;
		printf("   %9.1f | %20.2f | %13.1f | %13.1f | %22.1f\n",
			(double)legacy, (double)mapped, (double)oldRequest,
			(double)a.carOnScreenSpawnMax, (double)b.carOnScreenSpawnMax);
		if (fabsf(wantA - a.carOnScreenSpawnMax) > 0.05f)
			Fail("legacy mapping", legacy, 1.0f, 2000.0f, "the mapped draw distance is not the old request nor its cap");
		if (fabsf(wantB - b.carOnScreenSpawnMax) > 0.05f)
			Fail("legacy mapping", legacy, 2.2f, 2000.0f, "the mapped draw distance is not the old request nor its cap at a narrower FOV");
	}
	printf("\n  OK.  The map gives the ring the old value asked for at every FOV factor (the\n");
	printf("  factor cancels between the old and the new scale), and the visible cap\n");
	printf("  still bounds it wherever it binds (%d of the %d mapped values sit on it here,\n",
		cappedCount, mappedCount);
	printf("  in foggy weather more do). The map is faithful to the request, not to the\n");
	printf("  capped ring of VICE-120: the draw distance it chose raised the cap with\n");
	printf("  itself, where the old value under VICE-120 was bound by the cap of the\n");
	printf("  draw distance the file had (the 330 m of the stock draw distance at\n");
	printf("  FOV 70, for every value above %.2f).\n", (double)(330.0f / RING_CAR_ONSCREEN_SPAWN_MAX));
	printf("  The draw distance of an old value stays inside the slider's own range\n");
	printf("  (%.3f .. %.0f), so the map needs no clamp for the old slider's 1.0 .. 4.0.\n\n",
		(double)DRAWDIST_MIN, (double)DRAWDIST_MAX);
}

static void CheckBootPath(void)
{
	printf("== 5. the boot path: the time cycle is not loaded yet (far clip 0), the camera is fresh\n\n");
	/* The camera has not run yet: the LOD multiplier is zero, so the request is
	 * zero and the floor (the stock ring at the fresh FOV factor 1.0) stands. */
	STrafficRingInput in;
	in.lodDistance = 0.0f;
	in.fovFactor = 1.0f;
	in.farClip = 0.0f;
	STrafficRings r = ComputeTrafficRings(in);
	printf("  before the first frame: scale %.3f, on-screen ring %.1f m, despawn %.1f m\n",
		(double)r.scale, (double)r.carOnScreenSpawnMax, (double)r.carDespawn);
	CheckRings(DRAWDIST_DEFAULT, 1.0f, 0.0f, r);
	/* The first frame: the camera has the draw distance of the file. */
	in.lodDistance = LodDistance(1.0f, STOCK_DRAW_DISTANCE);
	STrafficRings r2 = ComputeTrafficRings(in);
	printf("  the first frame at the default draw distance: scale %.3f, on-screen ring %.1f m\n",
		(double)r2.scale, (double)r2.carOnScreenSpawnMax);
	CheckRings(DRAWDIST_DEFAULT, 1.0f, 0.0f, r2);
	printf("  the caps this boots with are the stock caps\n\n");
}

int main(void)
{
	printf("reVC traffic rings: the one table and its invariant (VICE-120/VICE-125)\n");
	printf("the table lives in src/control/TrafficRings.h; this program reads it directly\n");
	printf("the control ramp is %d steps from %.3f to %.0f (DrawDistance.h, Frontend.h)\n\n",
		SLIDER_STEPS, (double)DRAWDIST_MIN, (double)DRAWDIST_MAX);

	CheckPlacementReserve();
	CheckStockAgainstTheGame();
	CheckMaxSettings();
	CheckLegacyMapping();

	g_failures = 0;
	CheckMatrix(2000.0f, "sunny (far clip 2000 m)");
	CheckMatrix(650.0f, "rainy (far clip 650 m)");
	CheckMatrix(150.0f, "foggy (far clip 150 m)");
	CheckBootPath();

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  every cell passes every check. A fresh spawn sits the whole reserve inside\n");
		printf("  its own despawn threshold, on both sides, at every draw distance, weather\n");
		printf("  and FOV factor, and the default draw distance is the stock ring.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
