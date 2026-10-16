#include "common.h"

#include "TrafficRings.h"

#include "Camera.h"
#include "Renderer.h"
#include "Timecycle.h"

STrafficRings
CurrentTrafficRings(void)
{
	STrafficRingInput input;
	/* The one range control: the draw distance slider (CRenderer::ms_lodDistScale)
	 * scales the LOD distance, and the FOV factor comes with it (Camera.cpp).
	 * LOD_DISTANCE_LIVE composes the product from its parts (Renderer.h) instead
	 * of reading TheCamera.LODDistMultiplier, because CCamera::Process runs late
	 * in the frame (Game.cpp): the menu, the population and the caps of a frame
	 * all read this table before the camera, and the draw distance of a menu move
	 * must be in them in that same frame. The value is the LOD distance the
	 * renderer uses once the camera has run; the FOV factor is the camera's, so an
	 * FOV move is seen from the next frame. */
	input.lodDistance = LOD_DISTANCE_LIVE;
	/* The FOV factor of the camera: 70/FOV, capped at 2.2. The camera sets it on
	 * the first frame; the boot that refreshes the caps runs before that, where
	 * the global is zero and the stock factor stands. */
	input.fovFactor = TheCamera.GenerationDistMultiplier;
	if (input.fovFactor < 1.0f)
		input.fovFactor = 1.0f;
	/* The weather far clip: the hard edge of the visible world. */
	input.farClip = CTimeCycle::GetFarClip();
	return ComputeTrafficRings(input);
}
