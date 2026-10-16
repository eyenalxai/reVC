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
 * on the far clip. The cap only stops growth: it never shrinks the ring below
 * the stock ring (see RING_MIN_SCALE).
 *
 * The invariant, for every scale:
 *
 *	spawn radius + reserve <= that class's despawn threshold
 *
 * The reserve is a distance, not a ratio: the lane offset and the curve
 * placement of a fresh car do not shrink with the ring. Each despawn threshold
 * takes the stock radius (280 m at stock) and the spawn radius plus the
 * reserve, whichever binds; the inner edge of the on-screen band does the same
 * downwards. So a threshold is never tighter than the radius the game always
 * had, and never closer to its ring than the reserve. The stock reserve is the
 * game's own 30 m step from the on-screen spawn ring (250 m) to the on-screen
 * despawn (280 m) and back to the inner edge (220 m); it covers the vehicle
 * half length, the lane offset and the curve placement. A fresh spawn can
 * therefore never be culled on the frame it is created, at any slider value,
 * weather and FOV factor.
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
#define RING_CAR_SIDE_SPAWN		120.0f	/* OFFSCREEN_DESPAWN_RANGE */
#define RING_CAR_ONSCREEN_SPAWN_MAX	250.0f	/* REQUEST_ONSCREEN_DISTANCE */
#define RING_CAR_DESPAWN_RESERVE	30.0f	/* the step from 250 to 220 and to 280 */
#define RING_CAR_ONSCREEN_SPAWN_MIN	(RING_CAR_ONSCREEN_SPAWN_MAX - RING_CAR_DESPAWN_RESERVE)	/* MINIMAL_DISTANCE_TO_SPAWN_ONSCREEN */
#define RING_CAR_DESPAWN		(RING_CAR_ONSCREEN_SPAWN_MAX + RING_CAR_DESPAWN_RESERVE)
#define RING_CAR_OFFSCREEN_DESPAWN	(RING_CAR_SIDE_SPAWN + RING_CAR_DESPAWN_RESERVE)
#define RING_CAR_CAMERA_MIN		82.5f	/* a fresh on-screen spawn this close to the camera is deleted */
#define RING_CAR_TOPDOWN_DESPAWN	70.0f	/* the top-down camera keeps its own bubble */

/* The scale floor: the stock ring. The cap stops the ring from growing past
 * the visible range; it never shrinks it below the ring the game always had.
 * The stock numbers run in every weather (the smallest is 150 m of fog): the
 * car front ring is off screen there and drives in within its despawn, the
 * pedestrian removal (145 m) sits inside the far clip, and a ring smaller than
 * stock would only drop traffic into the player's view. */
#define RING_MIN_SCALE			1.0f

/* The boat. A water path node sits off the car path, so the water ring is 1.5
 * times the car ring. A vehicle with the extended range (a boat, or a set piece
 * the script marks) is removed 1.5 times as far. */
#define RING_BOAT_SPAWN_FACTOR		1.5f
#define RING_EXTENDED_RANGE_FACTOR	1.5f

/* The car generator: it holds its parked car between the near and the far ring
 * and keeps a visible boat car up to the boat ring. */
#define RING_GENERATOR_NEAR		210.0f
#define RING_GENERATOR_FAR		230.0f
#define RING_GENERATOR_BOAT_FAR		345.0f

/* The pedestrian. The creation ring is 100..130 m; a ped that is not visible
 * there is created in the off-screen band, which is the creation ring halved
 * and pulled back by its own width. The removal steps just past the creation
 * ring (the stock 1 m), and a culled ped beyond it fades, it is not deleted. */
#define RING_PED_CREATE_MIN		100.0f
#define RING_PED_CREATE_MAX		130.0f
#define RING_PED_OFFSCREEN_MULT		0.5f
#define RING_PED_OFFSCREEN_MAX		(RING_PED_CREATE_MAX * RING_PED_OFFSCREEN_MULT)
#define RING_PED_OFFSCREEN_MIN		(RING_PED_OFFSCREEN_MAX - (RING_PED_CREATE_MAX - RING_PED_CREATE_MIN))
#define RING_PED_REMOVE			(RING_PED_CREATE_MAX + 1.0f)
#define RING_PED_REMOVE_SPECIAL		(RING_PED_CREATE_MAX + 15.0f)

/* The invariant in the table itself. The scale cannot break it, but a wrong
 * stock number can, so the numbers that carry the invariant are checked here. */
static_assert(RING_CAR_DESPAWN_RESERVE > 0.0f, "a car needs a reserve over its spawn ring");
static_assert(RING_CAR_OFFSCREEN_DESPAWN > RING_CAR_SIDE_SPAWN, "an off-screen car must spawn inside its cull");
static_assert(RING_CAR_ONSCREEN_SPAWN_MIN > RING_CAR_CAMERA_MIN, "the camera rule must sit inside the on-screen band");
static_assert(RING_CAR_ONSCREEN_SPAWN_MIN < RING_CAR_ONSCREEN_SPAWN_MAX, "the on-screen band must have a width");
static_assert(RING_CAR_CAMERA_MIN * RING_MIN_SCALE < RING_CAR_ONSCREEN_SPAWN_MIN, "the camera rule must sit inside the on-screen band at the floor");
static_assert(RING_PED_REMOVE > RING_PED_CREATE_MAX, "a ped must spawn inside its removal");
static_assert(RING_PED_OFFSCREEN_MIN < RING_PED_OFFSCREEN_MAX, "the off-screen ped band must have a width");
static_assert(RING_PED_OFFSCREEN_MAX < RING_PED_REMOVE, "the off-screen ped band must stay inside the removal");
static_assert(RING_GENERATOR_FAR > RING_GENERATOR_NEAR, "the generator ring must have a width");
static_assert(RING_GENERATOR_BOAT_FAR > RING_GENERATOR_FAR, "the boat generator ring holds a visible boat");

/* The live game values the table derives from. */
struct STrafficRingInput
{
	float spawnDistance;	/* the Display slider, 1.0 is stock */
	float fovFactor;	/* TheCamera.GenerationDistMultiplier, 1.0 at FOV 70 */
	float lodDistance;	/* LOD_DISTANCE: the distance the world is drawn at */
	float farClip;		/* CTimeCycle::GetFarClip(): the weather far clip */
};

/* The radii of one frame, in metres. */
struct STrafficRings
{
	float scale;		/* the one ring scale, 1.0 at stock settings */
	float visibleRange;	/* min(lodDistance, farClip), 0 while it is not known */

	float carSideSpawn;		/* the off-screen band */
	float carOnScreenSpawnMin;	/* a fresh on-screen spawn may not be nearer */
	float carOnScreenSpawnMax;	/* the on-screen request */
	float carDespawn;		/* any car beyond this is removed */
	float carOffScreenDespawn;	/* the off-screen cull, for the build without EXTENDED_OFFSCREEN_DESPAWN_RANGE */
	float carCameraMin;		/* the 82.5 m camera rule */
	float carTopDownDespawn;	/* the top-down camera bubble */

	float boatSpawnFactor;		/* the water path factor on the ring */
	float extendedRangeFactor;	/* the despawn factor of an extended-range vehicle */

	float generatorNear;		/* the generator holds its car beyond this */
	float generatorFar;		/* and up to this */
	float generatorBoatFar;		/* a visible boat car is kept this far */

	float pedCreateMin;		/* the on-screen creation ring */
	float pedCreateMax;
	float pedOffScreenMin;		/* the off-screen creation band */
	float pedOffScreenMax;
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

	/* The requested ring: the spawn distance and the FOV factor, as before. */
	float requested = in.spawnDistance * in.fovFactor;
	/* The visible cap: the largest spawn radius stays the reserve inside the
	 * visible range. A range of zero is not known yet (the boot path), and
	 * there is nothing to fit: the requested ring stands. The floor keeps the
	 * stock ring when the cap would shrink below it. */
	if (r.visibleRange > 0.0f) {
		float visibleCap = (r.visibleRange - RING_CAR_DESPAWN_RESERVE) / RING_CAR_ONSCREEN_SPAWN_MAX;
		visibleCap = TrafficRingsMax(visibleCap, RING_MIN_SCALE);
		r.scale = TrafficRingsMin(requested, visibleCap);
	}else
		r.scale = requested;
	r.scale = TrafficRingsMax(r.scale, RING_MIN_SCALE);

	/* The car. The reserve is the same distance at every scale: it holds the
	 * vehicle half length, the lane offset and the curve placement of a fresh
	 * spawn, and those do not shrink with the ring. A threshold is the stock
	 * radius or the reserve over the ring, whichever binds. */
	r.carSideSpawn = RING_CAR_SIDE_SPAWN * r.scale;
	r.carOnScreenSpawnMax = RING_CAR_ONSCREEN_SPAWN_MAX * r.scale;
	r.carOnScreenSpawnMin = TrafficRingsMin(RING_CAR_ONSCREEN_SPAWN_MIN * r.scale,
		r.carOnScreenSpawnMax - RING_CAR_DESPAWN_RESERVE);
	r.carDespawn = TrafficRingsMax(RING_CAR_DESPAWN * r.scale,
		r.carOnScreenSpawnMax + RING_CAR_DESPAWN_RESERVE);
	r.carOffScreenDespawn = TrafficRingsMax(RING_CAR_SIDE_SPAWN * r.scale,
		r.carSideSpawn + RING_CAR_DESPAWN_RESERVE);
	/* The camera rule keeps today's 82.5 m times the FOV factor. Only when the
	 * visible range pulls the whole ring below that factor does the rule follow
	 * the ring down: a rule above the on-screen band would cull every fresh
	 * spawn the band makes, which is the one thing the table may not allow. */
	r.carCameraMin = RING_CAR_CAMERA_MIN * TrafficRingsMin(in.fovFactor, r.scale);
	r.carTopDownDespawn = RING_CAR_TOPDOWN_DESPAWN * r.scale;

	/* The boat: the water factor and the extended range factor stand. */
	r.boatSpawnFactor = RING_BOAT_SPAWN_FACTOR;
	r.extendedRangeFactor = RING_EXTENDED_RANGE_FACTOR;

	/* The car generator. */
	r.generatorNear = RING_GENERATOR_NEAR * r.scale;
	r.generatorFar = RING_GENERATOR_FAR * r.scale;
	r.generatorBoatFar = RING_GENERATOR_BOAT_FAR * r.scale;

	/* The pedestrian. The whole band is one geometry, so it scales whole. */
	r.pedCreateMin = RING_PED_CREATE_MIN * r.scale;
	r.pedCreateMax = RING_PED_CREATE_MAX * r.scale;
	r.pedOffScreenMin = RING_PED_OFFSCREEN_MIN * r.scale;
	r.pedOffScreenMax = RING_PED_OFFSCREEN_MAX * r.scale;
	r.pedRemove = RING_PED_REMOVE * r.scale;
	r.pedRemoveSpecial = RING_PED_REMOVE_SPECIAL * r.scale;

	return r;
}

/* The table for this frame, from the live game values (TrafficRings.cpp). */
STrafficRings CurrentTrafficRings(void);

#endif // __GTA_TRAFFICRINGS_H__
