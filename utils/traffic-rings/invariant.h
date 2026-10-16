/*
 * The invariant of the traffic ring table (VICE-120), shared by the harness
 * (utils/traffic-rings/harness.cpp) and the one-range-control evidence program
 * (utils/one-range-control/evidence.cpp). Both read src/control/TrafficRings.h
 * directly; this header holds the checks so they cannot drift apart.
 */

#ifndef __REVC_TRAFFIC_RINGS_INVARIANT_H__
#define __REVC_TRAFFIC_RINGS_INVARIANT_H__

#include "control/TrafficRings.h"

/* The ped creation is widened by the player's vehicle speed
 * (Population.cpp, PedCreationDistMultiplier: 1.0 .. 1.5). */
#define PED_SPEED_MULT_MAX 1.5f

/*
 * True when every spawn radius of the table sits inside its own despawn
 * threshold by the reserve, at every scale, and when the rules that keep the
 * camera, the generator and the ped band in place hold. When it returns false,
 * *pWhat names the first check that failed.
 */
static inline bool
TrafficRingsInvariant(const STrafficRings &r, float fovFactor, const char **pWhat)
{
	const float reserve = RING_CAR_DESPAWN_RESERVE;

	/* The spawn rings of the table, each with its own cull. */
	if (r.carSideSpawn + reserve > r.carDespawn)
		return *pWhat = "the side ring is not the reserve inside the cull", false;
	if (r.carOnScreenSpawnMax + reserve > r.carDespawn)
		return *pWhat = "the front ring is not the reserve inside the cull", false;
	if (r.carOnScreenSpawnMax - reserve < r.carOnScreenSpawnMin)
		return *pWhat = "the front ring is not the reserve above the inner edge", false;
	if (r.carOnScreenSpawnMin < r.carCameraMin)
		return *pWhat = "the camera minimum reaches into the on-screen band", false;
	/* The cap stops growth, and its floor is the stock ring at the live FOV
	 * factor: a ring smaller than that would drop traffic into the player's
	 * view. The on-screen ring is therefore inside the visible range or, when
	 * the floor binds, at the stock radius. */
	float ringLimit = TrafficRingsMax(r.visibleRange - reserve, RING_CAR_ONSCREEN_SPAWN_MAX * fovFactor);
	if (r.carOnScreenSpawnMax > ringLimit)
		return *pWhat = "the on-screen ring aims past the visible range and the floor", false;
	if (r.carDespawn < r.carSideSpawn)
		return *pWhat = "the cull is inside the side ring", false;
	/* The floor of the two scales: the stock radius at the live FOV factor, and
	 * the stock off-screen band. */
	if (r.scale < fovFactor || r.sideScale < 1.0f)
		return *pWhat = "a scale is below its floor", false;

	/* The boat ring is the water factor over the car ring, and the extended
	 * range factor multiplies the whole threshold, so the reserve scales with
	 * it: the boat needs the same check with the factors in. */
	if (r.boatSpawnFactor * r.carSideSpawn + reserve > r.extendedRangeFactor * r.carDespawn)
		return *pWhat = "the boat side ring is not inside its cull", false;
	if (r.boatSpawnFactor * r.carOnScreenSpawnMax + reserve > r.extendedRangeFactor * r.carDespawn)
		return *pWhat = "the boat front ring is not inside its cull", false;

	/* The generator ring shares the scale with the car table: it must not
	 * outgrow the despawn of the cars it makes, or a parked car would be
	 * removed right after it appears. The boat generator makes a boat, so the
	 * boat factor applies to it. */
	if (r.generatorFar >= r.carDespawn)
		return *pWhat = "the generator ring reaches the car despawn", false;
	if (r.generatorFar <= r.generatorNear)
		return *pWhat = "the generator ring lost its width", false;
	if (r.generatorBoatFar >= r.extendedRangeFactor * r.carDespawn)
		return *pWhat = "the boat generator ring reaches the boat despawn", false;

	/* The pedestrian band. A ped beyond its removal fades, it is not deleted,
	 * so the stock one-metre step is enough: the creation ring stays inside.
	 * The player's vehicle speed multiplies the off-screen band's outer edge,
	 * never its pull-back, so the band is checked at the standing and at the
	 * driving multiplier. */
	if (r.pedCreateMin >= r.pedCreateMax)
		return *pWhat = "the creation ring lost its width", false;
	if (r.pedCreateMax >= r.pedRemove)
		return *pWhat = "the creation ring reaches the removal", false;
	for (float pcdm = 1.0f; pcdm <= PED_SPEED_MULT_MAX; pcdm += 0.5f) {
		float inner = pcdm * r.pedOffScreenMax - r.pedOffScreenPullBack;
		float outer = pcdm * r.pedOffScreenMax;
		if (inner <= 0.0f || inner >= outer)
			return *pWhat = "the off-screen band lost its width", false;
		if (outer >= pcdm * r.pedRemove)
			return *pWhat = "the off-screen band reaches the removal", false;
	}

	/* The two rings the spawner asks for, through the birth cull of
	 * CCarCtrl::CreateCarFromPlacement. The position of a fresh spawn can sit
	 * the whole reserve away from the ring it aimed at (the lane offset and the
	 * curve placement), so the error is walked to both ends. The side band is
	 * outside the camera cone, so it is created off screen; the front band is
	 * within it, so it is created on screen (unless the weather hides it, and
	 * then the off-screen rule is the looser one). */
	if (r.carSideSpawn + reserve > r.carDespawn)
		return *pWhat = "the side ring dies on its birth frame", false;
	if (r.carOnScreenSpawnMax + reserve > r.carDespawn ||
		r.carOnScreenSpawnMax - reserve < r.carOnScreenSpawnMin)
		return *pWhat = "the front ring dies on its birth frame", false;
	if (r.carOnScreenSpawnMax - reserve < r.carCameraMin)
		return *pWhat = "the front ring can be born inside the camera rule", false;

	return *pWhat = "ok", true;
}

#endif // __REVC_TRAFFIC_RINGS_INVARIANT_H__
