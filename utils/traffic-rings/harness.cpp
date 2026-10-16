/*
 * The invariant proof for the traffic ring table (ticket VICE-120).
 *
 * This program reads src/control/TrafficRings.h on its own (the header has no
 * engine include) and walks the whole slider matrix:
 *
 *	SpawnDistance 1.0 .. 4.0	x  FOV factor 1.0 .. 2.2  x  weather far clip 150 .. 2000 m
 *
 * For every cell it checks the one invariant of the table and it replays the
 * birth cull of CCarCtrl::CreateCarFromPlacement for the two rings the spawner
 * asks for (the off-screen side ring and the on-screen front ring), with the
 * full placement reserve, so it shows the claim "a fresh spawn can never be
 * culled on the frame it is created" instead of arguing it.
 *
 * Build and run:
 *
 *	c++ -std=c++17 -I src -O2 -o /tmp/traffic-rings-harness utils/traffic-rings/harness.cpp
 *	/tmp/traffic-rings-harness
 *
 * The exit code is 0 when every cell passes. The output goes into the ticket.
 */

#include "control/TrafficRings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The draw distance the player starts with (Renderer.cpp, ms_lodDistScale).
 * The LOD distance is 300 x the LOD multiplier, and the LOD multiplier is the
 * FOV factor times the draw distance (Camera.cpp). */
static const float STOCK_DRAW_DISTANCE = 1.2f;
static const float MAX_DRAW_DISTANCE = 16.0f;

/* The components of the placement error the reserve must hold. They come from
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
static const float PLACEMENT_LANE_OFFSET = (4.0f - 1.0f + 0.5f) * 5.0f;
static const float PLACEMENT_CURVE_SHIFT = 2.0f;

/* The FOV factor the camera reports: 70/FOV capped at 2.2 (Camera.cpp). The
 * ticket's axis. */
static float LodDistance(float fovFactor, float drawDistance)
{
	return 300.0f * fovFactor * drawDistance;
}

static STrafficRings RingsAt(float spawnDistance, float fovFactor, float farClip, float drawDistance)
{
	STrafficRingInput in;
	in.spawnDistance = spawnDistance;
	in.fovFactor = fovFactor;
	in.lodDistance = LodDistance(fovFactor, drawDistance);
	in.farClip = farClip;
	return ComputeTrafficRings(in);
}

/*
 * The radii the branch replaced, for the one comparison that matters: the
 * stock line must not move. The numbers are the old macros of CarCtrl.cpp,
 * Population.cpp and CarGen.cpp at their face value. The old birth cull of an
 * off-screen car used OFFSCREEN_DESPAWN_RANGE (120 x SDM, no FOV factor), and
 * the old lifetime cull of PossiblyRemoveVehicle used the on-screen range
 * (280 x SDM x FOV, config.h defines EXTENDED_OFFSCREEN_DESPAWN_RANGE), which
 * is why the two could disagree.
 */
struct STodayRings
{
	float carSideSpawn;
	float carOnScreenSpawnMin;
	float carOnScreenSpawnMax;
	float carDespawn;
	float carBirthOffScreenCull;
	float carCameraMin;
	float generatorNear;
	float generatorFar;
	float generatorBoatFar;
	float pedCreateMin;
	float pedCreateMax;
	float pedRemove;
	float pedRemoveSpecial;
};

static STodayRings TodayAt(float spawnDistance, float fovFactor)
{
	STodayRings t;
	t.carSideSpawn = 120.0f * spawnDistance;
	t.carOnScreenSpawnMin = 220.0f * spawnDistance * fovFactor;
	t.carOnScreenSpawnMax = 250.0f * spawnDistance * fovFactor;
	t.carDespawn = 280.0f * spawnDistance * fovFactor;
	t.carBirthOffScreenCull = 120.0f * spawnDistance;
	t.carCameraMin = 82.5f * fovFactor;
	t.generatorFar = 230.0f * fovFactor;
	t.generatorNear = t.generatorFar - 20.0f;
	t.generatorBoatFar = 345.0f * fovFactor;
	t.pedCreateMin = 100.0f * spawnDistance * fovFactor;
	t.pedCreateMax = 130.0f * spawnDistance * fovFactor;
	t.pedRemove = 131.0f * spawnDistance * fovFactor;
	t.pedRemoveSpecial = 145.0f * spawnDistance * fovFactor;
	return t;
}

/* The ped creation is widened by the player's vehicle speed
 * (Population.cpp, PedCreationDistMultiplier: 1.0 .. 1.5). */
static const float PED_SPEED_MULT_MAX = 1.5f;

static int g_failures = 0;

static void Fail(const char *check, float sd, float fov, float farClip, const char *what)
{
	g_failures++;
	printf("FAIL [%s] SpawnDistance %.2f FOV factor %.2f far clip %.0f: %s\n",
		check, sd, fov, farClip, what);
}

/*
 * The birth cull of a fresh car, exactly as CCarCtrl::CreateCarFromPlacement
 * runs it. The position of a fresh spawn can sit the whole reserve away from
 * the ring it aimed at (the lane offset and the curve placement), so the error
 * is walked to both ends.
 */
static bool FreshSpawnSurvives(bool bOnScreen, float ring, float error, const STrafficRings &r)
{
	float distance = ring + error;
	if (!bOnScreen)
		return distance <= r.carDespawn;
	return distance <= r.carDespawn && distance >= r.carOnScreenSpawnMin;
}

/* The checks of one table. Every spawn radius must sit inside its own despawn
 * threshold by the reserve, at every scale. */
static void CheckRings(float sd, float fov, float farClip, const STrafficRings &r)
{
	float reserve = RING_CAR_DESPAWN_RESERVE;

	/* The spawn rings of the table, each with its own cull. */
	if (r.carSideSpawn + reserve > r.carDespawn)
		Fail("off-screen spawn band", sd, fov, farClip, "the side ring is not the reserve inside the cull");
	if (r.carOnScreenSpawnMax + reserve > r.carDespawn)
		Fail("on-screen spawn band", sd, fov, farClip, "the front ring is not the reserve inside the cull");
	if (r.carOnScreenSpawnMax - reserve < r.carOnScreenSpawnMin)
		Fail("on-screen spawn band", sd, fov, farClip, "the front ring is not the reserve above the inner edge");
	if (r.carOnScreenSpawnMin < r.carCameraMin)
		Fail("camera rule", sd, fov, farClip, "the camera minimum reaches into the on-screen band");
	/* The cap stops growth, and its floor is the stock ring at the live FOV
	 * factor: a ring smaller than that would drop traffic into the player's
	 * view. The on-screen ring is therefore inside the visible range or, when
	 * the floor binds, at the stock radius. */
	float ringLimit = TrafficRingsMax(r.visibleRange - reserve, RING_CAR_ONSCREEN_SPAWN_MAX * fov);
	if (r.carOnScreenSpawnMax > ringLimit)
		Fail("visible range", sd, fov, farClip, "the on-screen ring aims past the visible range and the floor");
	if (r.carDespawn < r.carSideSpawn)
		Fail("off-screen spawn band", sd, fov, farClip, "the cull is inside the side ring");
	/* The floor of the two scales: the stock radius at the live FOV factor, and
	 * the stock off-screen band. */
	if (r.scale < fov || r.sideScale < 1.0f)
		Fail("scale", sd, fov, farClip, "a scale is below its floor");

	/* The boat ring is the water factor over the car ring, and the extended
	 * range factor multiplies the whole threshold, so the reserve scales with
	 * it: the boat needs the same check with the factors in. */
	if (r.boatSpawnFactor * r.carSideSpawn + reserve > r.extendedRangeFactor * r.carDespawn)
		Fail("off-screen boat band", sd, fov, farClip, "the boat side ring is not inside its cull");
	if (r.boatSpawnFactor * r.carOnScreenSpawnMax + reserve > r.extendedRangeFactor * r.carDespawn)
		Fail("on-screen boat band", sd, fov, farClip, "the boat front ring is not inside its cull");

	/* The generator ring is clamped by the car table: what it must not do is
	 * outgrow the despawn of the cars it makes, or the parked car would be
	 * removed right after it appears. The boat generator makes a boat, so the
	 * boat factor applies to it. */
	if (r.generatorFar >= r.carDespawn)
		Fail("generator ring", sd, fov, farClip, "the generator ring reaches the car despawn");
	if (r.generatorFar <= r.generatorNear)
		Fail("generator ring", sd, fov, farClip, "the generator ring lost its width");
	if (r.generatorBoatFar >= r.extendedRangeFactor * r.carDespawn)
		Fail("generator ring", sd, fov, farClip, "the boat generator ring reaches the boat despawn");

	/* The pedestrian band. A ped beyond its removal fades, it is not deleted,
	 * so the stock one-metre step is enough: the creation ring stays inside.
	 * The player's vehicle speed multiplies the off-screen band's outer edge,
	 * never its pull-back, so the band is checked at the standing and at the
	 * driving multiplier. */
	if (r.pedCreateMin >= r.pedCreateMax)
		Fail("ped band", sd, fov, farClip, "the creation ring lost its width");
	if (r.pedCreateMax >= r.pedRemove)
		Fail("ped band", sd, fov, farClip, "the creation ring reaches the removal");
	for (float pcdm = 1.0f; pcdm <= PED_SPEED_MULT_MAX; pcdm += 0.5f) {
		float inner = pcdm * r.pedOffScreenMax - r.pedOffScreenPullBack;
		float outer = pcdm * r.pedOffScreenMax;
		if (inner <= 0.0f || inner >= outer)
			Fail("ped band", sd, fov, farClip, "the off-screen band lost its width");
		if (outer >= pcdm * r.pedRemove)
			Fail("ped band", sd, fov, farClip, "the off-screen band reaches the removal");
	}

	/* The two rings the spawner asks for, through the birth cull. The side band
	 * is outside the camera cone, so it is created off screen; the front band is
	 * within it, so it is created on screen (unless the weather hides it, and
	 * then the off-screen rule is the looser one). */
	if (!FreshSpawnSurvives(false, r.carSideSpawn, reserve, r))
		Fail("fresh off-screen spawn", sd, fov, farClip, "the side ring dies on its birth frame");
	if (!FreshSpawnSurvives(false, r.carOnScreenSpawnMax, reserve, r))
		Fail("fresh off-screen spawn", sd, fov, farClip, "the front ring dies on its birth frame off screen");
	if (!FreshSpawnSurvives(true, r.carOnScreenSpawnMax, reserve, r) ||
		!FreshSpawnSurvives(true, r.carOnScreenSpawnMax, -reserve, r))
		Fail("fresh on-screen spawn", sd, fov, farClip, "the front ring dies on its birth frame on screen");
	if (r.carOnScreenSpawnMax - reserve < r.carCameraMin)
		Fail("fresh on-screen spawn", sd, fov, farClip, "the front ring can be born inside the camera rule");
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

static void CheckStockAgainstToday(void)
{
	STrafficRings r = RingsAt(1.0f, 1.0f, 2000.0f, STOCK_DRAW_DISTANCE);
	STodayRings t = TodayAt(1.0f, 1.0f);

	printf("== 1. stock settings: SpawnDistance 1.0, FOV factor 1.0, sunny (far clip 2000 m)\n\n");
	PrintRings("the table:", r);
	printf("\n  the same rows before this branch:\n");
	printf("  car   side %.1f  on-screen %.1f .. %.1f  despawn %.1f  camera min %.1f\n",
		t.carSideSpawn, t.carOnScreenSpawnMin, t.carOnScreenSpawnMax, t.carDespawn, t.carCameraMin);
	printf("  car   the birth cull of a fresh off-screen car stood at %.1f m\n", t.carBirthOffScreenCull);
	printf("  gen   near %.1f  far %.1f  boat %.1f\n", t.generatorNear, t.generatorFar, t.generatorBoatFar);
	printf("  ped   create %.1f .. %.1f  remove %.1f / %.1f\n",
		t.pedCreateMin, t.pedCreateMax, t.pedRemove, t.pedRemoveSpecial);

	struct { const char *what; float now, before; } rows[] = {
		{ "car side ring", r.carSideSpawn, t.carSideSpawn },
		{ "car on-screen inner edge", r.carOnScreenSpawnMin, t.carOnScreenSpawnMin },
		{ "car on-screen ring", r.carOnScreenSpawnMax, t.carOnScreenSpawnMax },
		{ "car despawn (lifetime cull)", r.carDespawn, t.carDespawn },
		{ "car birth cull, off screen", r.carDespawn, t.carBirthOffScreenCull },
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
		bool same = fabsf(rows[i].now - rows[i].before) < 0.0005f;
		printf("    %-28s %10.1f  before %10.1f  %s\n", rows[i].what, rows[i].now, rows[i].before,
			same ? "same" : "moved");
	}
	printf("\n  One row moves: the birth cull of a fresh off-screen car, from 120 m to 280 m.\n");
	printf("  It has to move. The old birth cull used the off-screen ring itself\n");
	printf("  (120 m), so a placement that lands a lane offset past its ring died on\n");
	printf("  the frame it was created, while the lifetime cull of the same car\n");
	printf("  (PossiblyRemoveVehicle, extended off-screen range) kept it to 280 m. The\n");
	printf("  two now read the same row. No car roams farther: the lifetime cull is\n");
	printf("  unchanged at 280 m, so the move only stops the deaths at birth.\n");
	printf("  Every spawn ring, the on-screen despawn and the camera rule are unchanged.\n\n");
}

static void CheckMaxSettings(void)
{
	printf("== 2. maximum settings: SpawnDistance 4.0, FOV factor 1.0, sunny\n\n");
	STrafficRings stockDraw = RingsAt(4.0f, 1.0f, 2000.0f, STOCK_DRAW_DISTANCE);
	STrafficRings maxDraw = RingsAt(4.0f, 1.0f, 2000.0f, MAX_DRAW_DISTANCE);
	STodayRings t = TodayAt(4.0f, 1.0f);

	printf("  the old ring at these settings: side %.1f  front %.1f  cull %.1f\n",
		t.carSideSpawn, t.carOnScreenSpawnMax, t.carDespawn);
	printf("  the old birth cull deleted an off-screen spawn beyond %.1f m, so every\n",
		t.carBirthOffScreenCull);
	printf("  off-screen spawn of the %.1f m side ring and every front spawn that the\n",
		t.carSideSpawn);
	printf("  weather hid died on the frame it was created.\n\n");
	PrintRings("at the stock draw distance (1.2):", stockDraw);
	printf("\n");
	PrintRings("at the maximum draw distance (16.0):", maxDraw);
	printf("\n");
}

static void CheckMatrix(float drawDistance, const char *title)
{
	const float spawnDistances[] = { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f };
	const float fovFactors[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.2f };
	const float farClips[] = { 150.0f, 200.0f, 250.0f, 400.0f, 650.0f, 1000.0f, 1150.0f, 1500.0f, 2000.0f };
	const int nSd = sizeof(spawnDistances)/sizeof(spawnDistances[0]);
	const int nFov = sizeof(fovFactors)/sizeof(fovFactors[0]);
	const int nFar = sizeof(farClips)/sizeof(farClips[0]);

	int cells = 0;
	float minScale = 1e9f, maxScale = -1e9f;
	float minSide = 1e9f, maxSide = -1e9f;
	float minFront = 1e9f, maxFront = -1e9f;
	float minRange = 1e9f, maxRange = -1e9f;
	float minCamera = 1e9f, maxCamera = -1e9f;

	printf("== %s\n\n", title);
	printf("  the ring scale, SpawnDistance 4.0 (rows FOV factor, columns far clip m):\n");
	printf("    fov |");
	for (int k = 0; k < nFar; k++)
		printf(" %7.0f", farClips[k]);
	printf("\n");
	printf("    ----+");
	for (int k = 0; k < nFar; k++)
		printf("--------");
	printf("\n");
	for (int j = 0; j < nFov; j++) {
		printf("  %4.2f |", fovFactors[j]);
		for (int k = 0; k < nFar; k++) {
			STrafficRings r = RingsAt(4.0f, fovFactors[j], farClips[k], drawDistance);
			printf(" %7.3f", r.scale);
		}
		printf("\n");
	}
	printf("\n  the on-screen ring, SpawnDistance 4.0 (m):\n");
	printf("    fov |");
	for (int k = 0; k < nFar; k++)
		printf(" %7.0f", farClips[k]);
	printf("\n");
	printf("    ----+");
	for (int k = 0; k < nFar; k++)
		printf("--------");
	printf("\n");
	for (int j = 0; j < nFov; j++) {
		printf("  %4.2f |", fovFactors[j]);
		for (int k = 0; k < nFar; k++) {
			STrafficRings r = RingsAt(4.0f, fovFactors[j], farClips[k], drawDistance);
			printf(" %7.1f", r.carOnScreenSpawnMax);
		}
		printf("\n");
	}
	printf("\n");

	for (int i = 0; i < nSd; i++)
	for (int j = 0; j < nFov; j++)
	for (int k = 0; k < nFar; k++) {
		STrafficRings r = RingsAt(spawnDistances[i], fovFactors[j], farClips[k], drawDistance);
		cells++;
		CheckRings(spawnDistances[i], fovFactors[j], farClips[k], r);
		minScale = TrafficRingsMin(minScale, r.scale);
		maxScale = TrafficRingsMax(maxScale, r.scale);
		minSide = TrafficRingsMin(minSide, r.carSideSpawn);
		maxSide = TrafficRingsMax(maxSide, r.carSideSpawn);
		minFront = TrafficRingsMin(minFront, r.carOnScreenSpawnMax);
		maxFront = TrafficRingsMax(maxFront, r.carOnScreenSpawnMax);
		minRange = TrafficRingsMin(minRange, r.visibleRange);
		maxRange = TrafficRingsMax(maxRange, r.visibleRange);
		minCamera = TrafficRingsMin(minCamera, r.carCameraMin);
		maxCamera = TrafficRingsMax(maxCamera, r.carCameraMin);
	}

	printf("  cells %d (%d SpawnDistance x %d FOV factor x %d far clip), draw distance %.1f, LOD 300 x multiplier\n",
		cells, nSd, nFov, nFar, drawDistance);
	printf("  cells that fail a check so far: %d\n", g_failures);
	printf("  scale %.3f .. %.3f   side ring %.1f .. %.1f m   on-screen ring %.1f .. %.1f m\n",
		minScale, maxScale, minSide, maxSide, minFront, maxFront);
	printf("  camera minimum %.1f .. %.1f m   visible range %.1f .. %.1f m\n\n",
		minCamera, maxCamera, minRange, maxRange);
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
	printf("  reserve with room left.\n\n");
}

static void CheckBootPath(void)
{
	printf("== 5. the boot path: the time cycle is not loaded yet (far clip 0), LOD at the stock draw distance\n\n");
	STrafficRingInput in;
	in.spawnDistance = 1.0f;
	in.fovFactor = 1.0f;
	in.lodDistance = LodDistance(1.0f, STOCK_DRAW_DISTANCE);
	in.farClip = 0.0f;
	STrafficRings r = ComputeTrafficRings(in);
	printf("  visible range %.1f m, scale %.3f, on-screen ring %.1f m, despawn %.1f m\n",
		r.visibleRange, r.scale, r.carOnScreenSpawnMax, r.carDespawn);
	CheckRings(1.0f, 1.0f, 0.0f, r);
	printf("  the caps this boots with are the stock caps (scale 1.0 when the camera is fresh)\n\n");
}

int main(void)
{
	printf("reVC traffic rings: the one table and its invariant (VICE-120)\n");
	printf("the table lives in src/control/TrafficRings.h; this program reads it directly\n\n");

	CheckPlacementReserve();
	CheckStockAgainstToday();
	CheckMaxSettings();

	g_failures = 0;
	CheckMatrix(STOCK_DRAW_DISTANCE, "3. the matrix at the stock draw distance");
	CheckMatrix(MAX_DRAW_DISTANCE, "3b. the matrix at the maximum draw distance");
	CheckBootPath();

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  every cell passes every check. A fresh spawn sits the whole reserve inside\n");
		printf("  its own despawn threshold, on both sides, at every slider value, weather\n");
		printf("  and FOV factor.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
