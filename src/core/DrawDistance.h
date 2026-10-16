#ifndef __GTA_DRAWDISTANCE_H__
#define __GTA_DRAWDISTANCE_H__

/*
 * The one range control (VICE-125): the draw distance slider
 * (Frontend.cpp, MENUACTION_DRAWDIST). It scales the LOD multiplier the
 * renderer draws the world with (Renderer.cpp, CRenderer::ms_lodDistScale),
 * and the traffic ring follows it (TrafficRings.h).
 *
 * The bounds live here, in a header with no include, because the menu, the
 * settings registry (Settings.cpp), the old spawn distance key (re3.cpp) and
 * the ring all read the same range.
 */

#define DRAWDIST_MIN 0.925f
#define DRAWDIST_MAX 16.0f
// The value the game ships with: the renderer starts at it (Renderer.cpp) and
// the ring of that value is the stock ring (RING_LOD_REFERENCE).
#define DRAWDIST_DEFAULT 1.2f
#define DRAWDIST_RANGE (DRAWDIST_MAX - DRAWDIST_MIN)

#endif // __GTA_DRAWDISTANCE_H__
