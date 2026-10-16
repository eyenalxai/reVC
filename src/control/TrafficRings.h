#ifndef __GTA_TRAFFICRINGS_H__
#define __GTA_TRAFFICRINGS_H__

/*
 * The one table of traffic radii.
 *
 * Every spawn band, despawn threshold and camera minimum the traffic and the
 * population use comes from here: the car, the boat, the car generator and the
 * pedestrian. No other file holds a radius of the ring.
 *
 * The table is a set of stock radii (spawn distance 1.0, FOV factor 1.0, the
 * radii the game always used), plus the one distance that does not scale: the
 * reserve between a spawn ring and its despawn threshold. One scale multiplies
 * the radii:
 *
 *	scale = min(spawnDistance * fovFactor, visibleCap)
 *
 * The first part is what the game always did: the Display slider and the FOV
 * factor scale every radius. The cap is the fix for the empty streets at
 * maximum settings. The visible range is min(the LOD distance, the weather far
 * clip). A ring that aims past it only wastes spawns: the weather hides the car
 * or the world, and the birth cull deletes it on the frame it is created. The
 * cap keeps the largest spawn radius (the on-screen car ring) one
 * RING_CAR_DESPAWN_RESERVE inside the visible range, so a grown ring never sits
 * on the far clip.
 *
 * The cap has a floor under it: the stock ring at the live FOV factor. The cap
 * stops growth, it never pulls a radius below the radius the game always had:
 * a smaller ring would only drop traffic into the player's view, and a smaller
 * despawn would remove the cars a wider FOV factor put there. The floor also
 * keeps the camera rule and the generator ring inside their bands, which the
 * checks below spell out.
 *
 * Two radii were never multiplied by the FOV factor (the game scales with it
 * what the camera sees ahead, not what is out of sight) and keep their own
 * scale, the spawn distance alone, capped the same way: the off-screen car band
 * and the top-down request sit on it.
 *
 * The invariant, for every scale:
 *
 *	spawn radius + reserve <= that class's despawn threshold
 *
 * The reserve is a distance, not a ratio: the placement error of a fresh car
 * does not shrink with the ring. Each despawn threshold takes the stock radius
 * (280 m at stock) and the spawn radius plus the reserve, whichever binds; the
 * inner edge of the on-screen band does the same downwards. So a threshold is
 * never tighter than the radius the game always had, and never closer to its
 * ring than the reserve. The reserve is the game's own 30 m step from the
 * on-screen spawn ring (250 m) to the on-screen despawn (280 m) and back to the
 * inner edge (220 m); its components are derived in the harness in
 * utils/traffic-rings (the lane offset of the widest road, the 2 m shift of the
 * final position and the curve deviation), and none of them scales with the
 * ring. A fresh spawn can therefore never be culled on the frame it is created,
 * at any slider value, weather and FOV factor.
 *
 * No engine header is included here. The harness in utils/traffic-rings reads
 * this table on its own and proves the invariant over the slider matrix in the
 * ticket VICE-120.
 */

static inline float TrafficRingsMin(float a, float b) { return a < b ? a : b; }
static inline float TrafficRingsMax(float a, float b) { return a > b ? a : b; }

/* The stock radii, in metres, with the name the game used before the table. */

/* The car. The off-screen band spawns at the side of the player, the on-screen
 * band in front. */
#define RING_CAR_SIDE_SPAWN		120.0f	/* OFFSCREEN_DESPAWN_RANGE; the off-screen band, spawn distance only */
#define RING_CAR_ONSCREEN_SPAWN_MAX	250.0f	/* REQUEST_ONSCREEN_DISTANCE; the FOV factor scales it */
#define RING_CAR_DESPAWN_RESERVE	30.0f	/* the step from 250 to 220 and to 280 */
#define RING_CAR_ONSCREEN_SPAWN_MIN	(RING_CAR_ONSCREEN_SPAWN_MAX - RING_CAR_DESPAWN_RESERVE)	/* MINIMAL_DISTANCE_TO_SPAWN_ONSCREEN */
#define RING_CAR_DESPAWN		(RING_CAR_ONSCREEN_SPAWN_MAX + RING_CAR_DESPAWN_RESERVE)
#define RING_CAR_CAMERA_MIN		82.5f	/* a fresh on-screen spawn this close to the camera is deleted */
#define RING_MAX_FOV_FACTOR		2.2f	/* the camera caps the FOV factor here (Camera.cpp) */

/* The boat. A water path node sits off the car path, so the water ring is 1.5
 * times the car ring. A vehicle with the extended range (a boat, or a set piece
 * the script marks) is removed 1.5 times as far. */
#define RING_BOAT_SPAWN_FACTOR		1.5f
#define RING_EXTENDED_RANGE_FACTOR	1.5f

/* The car generator: it holds its parked car between the near and the far ring
 * and keeps a visible boat car up to the boat ring. The FOV factor scales it,
 * as it always did, and the despawn of the cars it makes is always wider (280 m
 * against 230 m at the same factor), so its car is never removed at its own
 * door; the harness checks that. */
#define RING_GENERATOR_FAR		230.0f
#define RING_GENERATOR_NEAR_GAP		20.0f	/* the far ring minus the near ring, stock */
#define RING_GENERATOR_BOAT_FAR		345.0f

/* The pedestrian. The creation ring is 100..130 m; a ped that is not visible
 * there is created in the off-screen band: the creation ring halved, pulled
 * back by the width of the ring. The removal steps just past the creation ring
 * (the stock 1 m), and a culled ped beyond it fades, it is not deleted. The
 * player's vehicle speed widens the band (PedCreationDistMultiplier); it
 * multiplies the band's outer edge, and the pull-back stays the width of the
 * ring, which is what the game always did. */
#define RING_PED_CREATE_MIN		100.0f
#define RING_PED_CREATE_MAX		130.0f
#define RING_PED_OFFSCREEN_MAX		(RING_PED_CREATE_MAX / 2.0f)
#define RING_PED_OFFSCREEN_PULLBACK	(RING_PED_CREATE_MAX - RING_PED_CREATE_MIN)
#define RING_PED_REMOVE			(RING_PED_CREATE_MAX + 1.0f)
#define RING_PED_REMOVE_SPECIAL		(RING_PED_CREATE_MAX + 15.0f)

/* The invariant in the table itself. The cap cannot break it, but a wrong
 * stock number can, so the numbers that carry the invariant are checked here. */
static_assert(RING_CAR_DESPAWN_RESERVE > 0.0f, "a car needs a reserve over its spawn ring");
static_assert(RING_CAR_ONSCREEN_SPAWN_MIN > 0.0f, "the on-screen band must have a width");
static_assert(RING_CAR_ONSCREEN_SPAWN_MIN < RING_CAR_ONSCREEN_SPAWN_MAX, "the on-screen band must have a width");
/* The camera rule keeps its own FOV factor, so the band must hold that factor
 * at its cap: the band's inner edge is 220 m or more at every scale (the floor
 * keeps the scale at the FOV factor), and the rule stops at 82.5 x 2.2. */
static_assert(RING_CAR_CAMERA_MIN * RING_MAX_FOV_FACTOR < RING_CAR_ONSCREEN_SPAWN_MIN, "the camera rule must sit inside the on-screen band at the FOV cap");
/* The generator ring keeps the FOV factor too, so its far ring must hold it
 * against the smaller car despawn: 230 m < 280 m at the same factor, and the
 * boat ring 345 m < the 1.5 x 280 m of a boat. */
static_assert(RING_GENERATOR_FAR < RING_CAR_DESPAWN, "the generator ring must stay inside the car despawn");
static_assert(RING_GENERATOR_BOAT_FAR < RING_EXTENDED_RANGE_FACTOR * RING_CAR_DESPAWN, "the boat generator ring must stay inside the boat despawn");
static_assert(RING_GENERATOR_FAR > RING_GENERATOR_NEAR_GAP, "the generator ring must have a width");
static_assert(RING_GENERATOR_BOAT_FAR > RING_GENERATOR_FAR, "the boat generator ring holds a visible boat");
static_assert(RING_PED_REMOVE > RING_PED_CREATE_MAX, "a ped must spawn inside its removal");
static_assert(RING_PED_OFFSCREEN_PULLBACK < RING_PED_OFFSCREEN_MAX, "the off-screen ped band must have a width");
static_assert(RING_PED_OFFSCREEN_MAX < RING_PED_REMOVE, "the off-screen ped band must stay inside the removal");

/* The live game values the table derives from. */
struct STrafficRingInput
{
	float spawnDistance;	/* the Display slider, 1.0 is stock */
	float fovFactor;	/* TheCamera.GenerationDistMultiplier, 1.0 at FOV 70, capped at 2.2 */
	float lodDistance;	/* LOD_DISTANCE: the distance the world is drawn at */
	float farClip;		/* CTimeCycle::GetFarClip(): the weather far clip */
};

/* The radii of one frame, in metres. */
struct STrafficRings
{
	float scale;		/* the one ring scale: spawn distance x FOV factor, capped */
	float sideScale;	/* the scale of the radii that never took the FOV factor */
	float visibleRange;	/* min(lodDistance, farClip), 0 while it is not known */

	float carSideSpawn;		/* the off-screen band */
	float carOnScreenSpawnMin;	/* a fresh on-screen spawn may not be nearer */
	float carOnScreenSpawnMax;	/* the on-screen request */
	float carDespawn;		/* any car beyond this is removed, birth cull and lifetime cull alike */
	float carCameraMin;		/* the 82.5 m camera rule */

	float boatSpawnFactor;		/* the water path factor on the ring */
	float extendedRangeFactor;	/* the despawn factor of an extended-range vehicle */

	float generatorNear;		/* the generator holds its car beyond this */
	float generatorFar;		/* and up to this */
	float generatorBoatFar;		/* a visible boat car is kept this far */

	float pedCreateMin;		/* the on-screen creation ring */
	float pedCreateMax;
	float pedOffScreenMax;		/* the outer edge of the off-screen creation band */
	float pedOffScreenPullBack;	/* the width the band pulls its inner edge back by */
	float pedRemove;		/* the removal distance */
	float pedRemoveSpecial;		/* for peds with the extra-far-away flag */
};

static inline STrafficRings
ComputeTrafficRings(const STrafficRingInput &in)
{
	STrafficRings r;

	/* A zero far clip means the weather is not known yet (the boot path that
	 * refreshes the caps runs before the time cycle updates), so the LOD
	 * distance alone sets the visible range. */
	if (in.farClip > 0.0f)
		r.visibleRange = TrafficRingsMin(in.lodDistance, in.farClip);
	else
		r.visibleRange = in.lodDistance;

	/* The caps: the largest spawn radius stays the reserve inside the visible
	 * range. The cap never pulls a radius below the stock radius at the live
	 * FOV factor; it only stops growth. A range of zero is not known yet (the
	 * boot path), and there is nothing to fit: the requested rings stand. */
	float requested = in.spawnDistance * in.fovFactor;
	float scaleCap = requested;
	float sideCap = in.spawnDistance;
	if (r.visibleRange > 0.0f) {
		float visibleCap = (r.visibleRange - RING_CAR_DESPAWN_RESERVE) / RING_CAR_ONSCREEN_SPAWN_MAX;
		/* The floor of the on-screen family is the FOV factor, the floor of the
		 * off-screen family is the stock band itself. */
		scaleCap = TrafficRingsMax(visibleCap, in.fovFactor);
		sideCap = TrafficRingsMax(visibleCap / in.fovFactor, 1.0f);
	}
	r.scale = TrafficRingsMin(requested, scaleCap);
	/* The off-screen band and the top-down request never took the FOV factor. */
	r.sideScale = TrafficRingsMin(in.spawnDistance, sideCap);

	/* The car. The reserve is the same distance at every scale: it holds the
	 * placement error of a fresh spawn, and that does not shrink with the ring.
	 * A threshold is the stock radius or the reserve over the ring, whichever
	 * binds. */
	r.carSideSpawn = RING_CAR_SIDE_SPAWN * r.sideScale;
	r.carOnScreenSpawnMax = RING_CAR_ONSCREEN_SPAWN_MAX * r.scale;
	r.carOnScreenSpawnMin = TrafficRingsMin(RING_CAR_ONSCREEN_SPAWN_MIN * r.scale,
		r.carOnScreenSpawnMax - RING_CAR_DESPAWN_RESERVE);
	r.carDespawn = TrafficRingsMax(RING_CAR_DESPAWN * r.scale,
		r.carOnScreenSpawnMax + RING_CAR_DESPAWN_RESERVE);
	/* The camera rule: today's 82.5 m times the FOV factor, exactly as it was.
	 * It fits the on-screen band at every scale: the band's inner edge is
	 * 220 m or more (the floor keeps the scale at the FOV factor or above), and
	 * the rule stops at 82.5 x 2.2 = 181.5 m. */
	r.carCameraMin = RING_CAR_CAMERA_MIN * in.fovFactor;

	/* The boat: the water factor and the extended range factor stand. */
	r.boatSpawnFactor = RING_BOAT_SPAWN_FACTOR;
	r.extendedRangeFactor = RING_EXTENDED_RANGE_FACTOR;

	/* The car generator: the FOV factor scales it, as it always did (-20 m for
	 * the near ring). The car despawn is wider at the same factor, so the
	 * parked car is never removed at its own door (the static asserts say so,
	 * the harness checks it per cell). */
	r.generatorFar = RING_GENERATOR_FAR * in.fovFactor;
	r.generatorNear = r.generatorFar - RING_GENERATOR_NEAR_GAP;
	r.generatorBoatFar = RING_GENERATOR_BOAT_FAR * in.fovFactor;

	/* The pedestrian. The creation ring takes the whole scale; the off-screen
	 * band is its half, with the ring's width as the pull-back. */
	r.pedCreateMin = RING_PED_CREATE_MIN * r.scale;
	r.pedCreateMax = RING_PED_CREATE_MAX * r.scale;
	r.pedOffScreenMax = RING_PED_OFFSCREEN_MAX * r.scale;
	r.pedOffScreenPullBack = RING_PED_OFFSCREEN_PULLBACK * r.scale;
	r.pedRemove = RING_PED_REMOVE * r.scale;
	r.pedRemoveSpecial = RING_PED_REMOVE_SPECIAL * r.scale;

	return r;
}

/* The table for this frame, from the live game values (TrafficRings.cpp). */
STrafficRings CurrentTrafficRings(void);

#endif // __GTA_TRAFFICRINGS_H__
