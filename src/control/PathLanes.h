#ifndef __GTA_PATHLANES_H__
#define __GTA_PATHLANES_H__

/*
 * The lane spacing of the car path links (LANE_WIDTH, which lived in
 * PathFind.h until VICE-133).
 *
 * Two domains measure with it: the path geometry itself (CCarPathLink's
 * GetLaneOffset and OneWayLaneOffset, src/control/PathFind.h) and the
 * car-on-rails arithmetic that places a car on a lane of its two links
 * (CarOnRails.h, the one copy of the rules the near traffic and the far
 * traffic layer's worker instantiate).
 *
 * It has its own header so that neither of the two domains has to include the
 * other: PathFind.h (engine types, the graph) does not pull the rail
 * arithmetic, and CarOnRails.h (engine-free, compiled by the evidence
 * programs) does not pull PathFind.h.
 */

#define LANE_WIDTH 5.0f

#endif // __GTA_PATHLANES_H__
