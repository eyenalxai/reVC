#include "common.h"

#include "TrafficRings.h"

#include "Camera.h"
#include "IniFile.h"
#include "Renderer.h"
#include "Timecycle.h"

STrafficRings
CurrentTrafficRings(void)
{
	STrafficRingInput input;
	/* The Display slider keeps its value in CIniFile; the game clamps it. */
	input.spawnDistance = CIniFile::SpawnDistanceMultiplier;
	/* The FOV factor of the camera: 70/FOV, capped at 2.2. The camera sets it
	 * on the first frame; the boot that refreshes the caps runs before that,
	 * where the global is zero and the stock factor stands. */
	input.fovFactor = TheCamera.GenerationDistMultiplier;
	if (input.fovFactor < 1.0f)
		input.fovFactor = 1.0f;
	/* The draw distance slider scales the LOD distance (Renderer.h): 300 x the
	 * LOD multiplier. A cutscene sets the FOV part of the multiplier to 1.0,
	 * exactly as it does to the rings themselves; the draw distance stays. */
	input.lodDistance = LOD_DISTANCE;
	/* The weather far clip: the hard edge of the visible world. */
	input.farClip = CTimeCycle::GetFarClip();
	return ComputeTrafficRings(input);
}
