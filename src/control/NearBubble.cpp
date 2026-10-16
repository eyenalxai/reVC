#include "common.h"

#include "NearBubble.h"

#include "CarCtrl.h"

#include "Camera.h"
#include "FillBurst.h"
#include "Occlusion.h"
#include "Timer.h"
#include "Vehicle.h"
#include "World.h"

/*
 * The engine side of the near bubble (VICE-127). The rules live in
 * NearBubble.h; this file fills the facts the rules read from the frame (the
 * camera, the occluders, the ring table's visible range) and owns the
 * counters the acceptance reads (utils/caps-audit/live-counters.py).
 *
 * One frame of the bubble: CCarCtrl::NearBubbleBeginFrame resets the ray
 * budget and publishes the target of the fill; every creation and removal the
 * traffic makes asks the two gates below. The gates resolve the arm, spend the
 * confirmation ray when the arm asks for one (the occluded hint and the close
 * blind spot), count the events and say no where the rule says no. A refused
 * event costs one ladder: the placement loop of the caller moves to its next
 * candidate.
 *
 * The one test that decides: the line-of-sight ray. The occluder volumes are a
 * frame old and their screen-space answer against a turned camera can be
 * wrong, so they only *select* a position for the ray; the arm the counters
 * attribute is the hint's, and the allowance is the ray's. A stale or wrong
 * hint can waste a ray or refuse a spawn, never allow a car the player can
 * see.
 *
 * The counters (the fill burst's, next to its own in CarCtrl.cpp):
 *
 *	ms_nNearBubbleCatchableCreations	the acceptance counters (VICE-146):
 *	ms_nNearBubbleCatchableRemovals		the catchable zone, in the frustum
 *						nearer than the band minimum.
 *						Must stay 0.
 *	ms_nNearBubbleBandCreations		the in-view band: the events the
 *	ms_nNearBubbleBandRemovals		band allowed at or beyond the
 *						minimum (a creation covered by the
 *						car's own alpha ramp, a removal by
 *						the fade-out of the despawn)
 *	ms_nNearBubbleFadeCreations		the fade's work: the catchable
 *						events a fade covered, exempt
 *	ms_nNearBubbleFadeRemovals
 *	ms_nNearBubbleHiddenCreations		the events outside the
 *	ms_nNearBubbleHiddenRemovals		catchable set the rule allowed
 *	ms_nNearBubbleOccluderCreations		those of them the occluded hint
 *	ms_nNearBubbleOccluderRemovals		selected (the ray allowed them)
 *	ms_nNearBubbleRayCreations		those of them the close blind spot
 *						allowed through the ray
 *	ms_nNearBubbleRefused			the events the rule stopped
 *	ms_nNearBubbleBypassed			the events the top-down camera's
 *						view allowed
 *	ms_nNearBubbleRays			the rays of the current frame (<=
 *						the budget)
 *	ms_nNearBubbleRaysTotal			the rays since the counters were
 *						last zeroed
 *	ms_nNearBubbleFill			the traffic inside the bubble
 *	ms_fNearBubbleTarget			the share of the car target its
 *						area asks for
 *	ms_nNearBubbleOtherFill			the cars inside the bubble that the
 *						traffic spawner did not create
 *	ms_nNearBubbleOtherCreations		those cars' creations, and the
 *	ms_nNearBubbleOtherCreationsInView	in-view ones of them
 *	ms_nNearBubbleValveRemovals		the pool valve's own deletions
 *
 * The fill split (the traffic against the other cars) holds the acceptance
 * honest: the car target counts every RANDOM_VEHICLE, so the scripts, the set
 * pieces, the road blocks, the car parks, the garages and the cheat cars are
 * part of it, but only the spawner's own cars are the traffic the bubble
 * watches. A scripted car that appears in view moves the "other creations in
 * view" counter, never the traffic's, and the fill plus the other fill is
 * what the target describes.
 */

uint32 CCarCtrl::ms_nNearBubbleCatchableCreations;
uint32 CCarCtrl::ms_nNearBubbleCatchableRemovals;
uint32 CCarCtrl::ms_nNearBubbleBandCreations;
uint32 CCarCtrl::ms_nNearBubbleBandRemovals;
uint32 CCarCtrl::ms_nNearBubbleFadeCreations;
uint32 CCarCtrl::ms_nNearBubbleFadeRemovals;
uint32 CCarCtrl::ms_nNearBubbleHiddenCreations;
uint32 CCarCtrl::ms_nNearBubbleHiddenRemovals;
uint32 CCarCtrl::ms_nNearBubbleOccluderCreations;
uint32 CCarCtrl::ms_nNearBubbleOccluderRemovals;
uint32 CCarCtrl::ms_nNearBubbleRayCreations;
uint32 CCarCtrl::ms_nNearBubbleRefused;
uint32 CCarCtrl::ms_nNearBubbleBypassed;
uint32 CCarCtrl::ms_nNearBubbleRays;
uint32 CCarCtrl::ms_nNearBubbleRaysTotal;
uint32 CCarCtrl::ms_nNearBubbleFill;
uint32 CCarCtrl::ms_nNearBubbleOtherFill;
uint32 CCarCtrl::ms_nNearBubbleOtherCreations;
uint32 CCarCtrl::ms_nNearBubbleOtherCreationsInView;
uint32 CCarCtrl::ms_nNearBubbleValveRemovals;
float CCarCtrl::ms_fNearBubbleTarget;
int32 CCarCtrl::ms_nNearBubbleRaysLeft;

/* The view of this frame is not the player's: the top-down camera looks at the
 * map from above, where the street level is not the subject and the traffic
 * must keep flowing. The rule's visibility arms step aside (the events are
 * counted as bypassed, so nothing hides in the counters); the bubble does not.
 * The flag is the one the spawner's top-down branch uses. */
static bool
NearBubbleViewIsBypassed(void)
{
	return TheCamera.GetForward().z < -0.9f;
}

/* The active occluder set is built during the render, one frame after the
 * gates read it (COcclusion::ProcessBeforeRendering, Renderer.cpp, runs after
 * the car block of Game.cpp). The hint counts only while the camera still
 * matches the view the set was built for (NEAR_BUBBLE_OCCLUDER_*); past them
 * it is ignored, the other arms decide and no ray is wasted on another view's
 * candidates. The hint never decides on its own: the ray does. */
static bool
NearBubbleOccluderSetIsCurrent(void)
{
	if (COcclusion::NumActiveOccluders == 0)
		/* No set, no hint: IsPositionOccluded says false anyway. */
		return true;
	if ((TheCamera.GetPosition() - COcclusion::ms_vecProcessedCameraPos).Magnitude() > NEAR_BUBBLE_OCCLUDER_MAX_MOVE)
		return false;
	if (DotProduct(TheCamera.GetForward(), COcclusion::ms_vecProcessedCameraForward) < NEAR_BUBBLE_OCCLUDER_MIN_DOT)
		return false;
	if (Abs(TheCamera.Cams[TheCamera.ActiveCam].FOV - COcclusion::ms_fProcessedCameraFOV) > NEAR_BUBBLE_OCCLUDER_MAX_FOV)
		return false;
	return true;
}

/* One candidate of this frame, from the position and the live view. */
static SNearBubbleCandidate
NearBubbleCandidateAt(const CVector &pos, const CVector &playerCentre, float radius)
{
	SNearBubbleCandidate c;

	c.distToPlayer = (pos - playerCentre).Magnitude2D();
	c.distToCamera = (pos - TheCamera.GetPosition()).Magnitude();
	c.visibleRange = CurrentTrafficRings().visibleRange;
	c.onScreenSpawnMin = CurrentTrafficRings().carOnScreenSpawnMin;
	c.inFrustum = TheCamera.IsSphereVisible(pos, radius);
	c.inFrustumMargin = TheCamera.IsSphereVisible(pos, radius + NEAR_BUBBLE_SOFT_MARGIN);
	c.occluded = NearBubbleOccluderSetIsCurrent() && COcclusion::IsPositionOccluded(pos, radius);
	c.fadeHides = FillBurstIsHidden();
	c.bypassView = NearBubbleViewIsBypassed();
	return c;
}

/* The line-of-sight ray: is the spot hidden by a building between the camera
 * and it? The ray tests the buildings only; the see-through materials are
 * skipped, so a glass window is not a wall. A car hidden only by a fence or a
 * tree is treated as visible, the conservative direction: a refused spawn, not
 * a pop. */
static bool
NearBubblePositionIsHiddenByBuilding(const CVector &pos)
{
	CVector target = pos;
	target.z += 1.0f;
	return !CWorld::GetIsLineOfSightClear(TheCamera.GetPosition(), target,
		true, false, false, false, false, true, true);
}

/* The one resolution of a candidate: the arm, the confirmation ray when the
 * arm asks for one (the occluded hint and the close blind spot), and the final
 * answer. `rayAllowed` is false for the paths that cannot afford a ray per
 * candidate: the parked cars of the map (an empty generator retries every four
 * frames, and the generators run before the traffic in the frame), where those
 * two arms are refused outright. */
static bool
NearBubbleResolve(const SNearBubbleCandidate &c, const CVector &pos, bool rayAllowed, ENearBubbleArm *pArm)
{
	ENearBubbleArm arm = NearBubbleArmFor(c);
	*pArm = arm;

	switch (arm) {
	case NEAR_BUBBLE_ARM_NONE:
		return false;
	case NEAR_BUBBLE_ARM_FADE:
	case NEAR_BUBBLE_ARM_BYPASS:
	case NEAR_BUBBLE_ARM_FADE_DISTANCE:
	case NEAR_BUBBLE_ARM_SOFT:
	case NEAR_BUBBLE_ARM_ONSCREEN_BAND:
		return true;
	case NEAR_BUBBLE_ARM_OCCLUDER:
	case NEAR_BUBBLE_ARM_RAY:
	default:
		break;
	}

	/* The confirmation ray, from the frame's budget. A candidate that finds
	 * the budget spent is refused: the placement loop of the caller tries
	 * its next candidate, and the frame's work stays bounded. */
	if (!rayAllowed || CCarCtrl::ms_nNearBubbleRaysLeft <= 0)
		return false;
	CCarCtrl::ms_nNearBubbleRaysLeft--;
	CCarCtrl::ms_nNearBubbleRays++;
	CCarCtrl::ms_nNearBubbleRaysTotal++;
	return NearBubblePositionIsHiddenByBuilding(pos);
}

void
CCarCtrl::NearBubbleBeginFrame(void)
{
	/* The frame's budget and the frame's ray counter. The gates that run
	 * before this point (the parked-car gate of CTheCarGenerators::Process,
	 * Game.cpp) cannot spend a ray: the gate below tests `rayAllowed` before
	 * it reads the budget, so their stale view is irrelevant. */
	ms_nNearBubbleRaysLeft = NEAR_BUBBLE_LOS_RAYS_PER_FRAME;
	ms_nNearBubbleRays = 0;
	ms_fNearBubbleTarget = NearBubbleTarget((float)GetRandomCarTarget(), CurrentTrafficRings().carDespawn);
}

/* The creation gate: may a car appear at this position? */
bool
CCarCtrl::NearBubbleAllowsCreation(const CVector &pos, const CVector &playerCentre, float radius, bool rayAllowed)
{
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, playerCentre, radius);
	ENearBubbleArm arm;
	bool allowed = NearBubbleResolve(c, pos, rayAllowed, &arm);

	if (!allowed)
		ms_nNearBubbleRefused++;
	return allowed;
}

/* The removal gate: a car may go only when the grace has passed, it is beyond
 * the removal threshold, and it is hidden. The threshold takes the ring's own
 * despawn (which is already the spawn band plus the reserve) and the bubble
 * plus its reserve, whichever is farther. The occluded hint is confirmed by
 * the same ray the creations use: a stale occluder cannot fade a car out in
 * view. A removal that finds the ray budget spent waits; the car is hidden-ish
 * and leaves a frame later. */
bool
CCarCtrl::NearBubbleAllowsRemoval(CVehicle *pVehicle, const CVector &playerCentre, float ringThreshold)
{
	/* The visibility of the car itself: its bounding sphere, or the named
	 * radius of the bubble where the car is smaller. */
	float radius = Max(NEAR_BUBBLE_ENTITY_RADIUS, pVehicle->GetBoundRadius());
	SNearBubbleCandidate c = NearBubbleCandidateAt(pVehicle->GetBoundCentre(), playerCentre, radius);

	if (!c.bypassView) {
		if (CTimer::GetTimeInMilliseconds() < pVehicle->m_nRemovalProtectedUntil) {
			ms_nNearBubbleRefused++;
			return false;
		}
		float threshold = Max(ringThreshold, NEAR_BUBBLE_REMOVE_DISTANCE);
		if ((pVehicle->GetPosition() - playerCentre).Magnitude2D() <= threshold) {
			ms_nNearBubbleRefused++;
			return false;
		}
	}

	ENearBubbleArm arm;
	bool allowed = NearBubbleResolve(c, pVehicle->GetBoundCentre(), true, &arm);

	if (!allowed)
		ms_nNearBubbleRefused++;
	return allowed;
}

/* The acceptance counters, read after a car of the traffic entered the world.
 * The catchable set of the rule decides the row: an event the player could
 * catch is a violation unless the fade covered it (the fill burst's
 * exemption) or the in-view band allowed it (VICE-146), everything else is the
 * rule's own work, and the hidden events subdivide by the arm that allowed
 * them. A path that bypasses the gate shows up in the catchable-zone row. The
 * arm function decides the row of the catchable events, so the counters and
 * the ladder cannot drift apart: the fade first, then the band, and everything
 * under the band minimum is the acceptance counter. */
void
CCarCtrl::NearBubbleCountCreation(const CVector &pos, float radius)
{
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, FindPlayerCentreOfWorld(CWorld::PlayerInFocus), radius);
	if (c.bypassView) {
		ms_nNearBubbleBypassed++;
	} else if (NearBubblePositionIsCatchable(c)) {
		switch (NearBubbleArmFor(c)) {
		case NEAR_BUBBLE_ARM_FADE:		ms_nNearBubbleFadeCreations++; break;
		case NEAR_BUBBLE_ARM_ONSCREEN_BAND:	ms_nNearBubbleBandCreations++; break;
		default:				ms_nNearBubbleCatchableCreations++; break;
		}
	} else {
		ms_nNearBubbleHiddenCreations++;
		switch (NearBubbleArmFor(c)) {
		case NEAR_BUBBLE_ARM_OCCLUDER:	ms_nNearBubbleOccluderCreations++; break;
		case NEAR_BUBBLE_ARM_RAY:	ms_nNearBubbleRayCreations++; break;
		default:			break;
		}
	}
}

/* A removal the caller made. `wasInvisible` tells the counter the car was
 * already fully faded: the deletion of an invisible car is not an event the
 * player can catch, however the frustum reads. The arm function decides the
 * rows exactly as for a creation (VICE-146): the band removals are the
 * allowed ones at or beyond the minimum, and the acceptance counter is what
 * is left under it. */
void
CCarCtrl::NearBubbleCountRemoval(const CVector &pos, float radius, bool wasInvisible)
{
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, FindPlayerCentreOfWorld(CWorld::PlayerInFocus), radius);
	if (c.bypassView) {
		ms_nNearBubbleBypassed++;
	} else if (NearBubblePositionIsCatchable(c)) {
		switch (NearBubbleArmFor(c)) {
		case NEAR_BUBBLE_ARM_FADE:
			ms_nNearBubbleFadeRemovals++;
			break;
		case NEAR_BUBBLE_ARM_ONSCREEN_BAND:
			ms_nNearBubbleBandRemovals++;
			break;
		default:
			if (!wasInvisible)
				ms_nNearBubbleCatchableRemovals++;
			else
				ms_nNearBubbleHiddenRemovals++;
			break;
		}
	} else {
		ms_nNearBubbleHiddenRemovals++;
		switch (NearBubbleArmFor(c)) {
		case NEAR_BUBBLE_ARM_OCCLUDER:	ms_nNearBubbleOccluderRemovals++; break;
		default:			break;
		}
	}
}

/* A car that entered the world outside the traffic spawner (a script, a set
 * piece, a road block, a car park, a garage, a cheat, the RC car). It is not
 * the traffic the bubble fills, so it never moves the traffic's counters; the
 * pair below carries it: the total and the in-view ones, so a car that appears
 * in view is visible in the live numbers. The in-view row is the wider set the
 * catchable predicate describes (in the frustum, inside the fade, not
 * occluded), the band included; it is informational, so a road block or a
 * cheat car in front of the player reads there and not as a traffic violation. */
void
CCarCtrl::NearBubbleCountOtherCreation(const CVector &pos, float radius)
{
	ms_nNearBubbleOtherCreations++;
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, FindPlayerCentreOfWorld(CWorld::PlayerInFocus), radius);
	if (!c.fadeHides && NearBubblePositionIsCatchable(c))
		ms_nNearBubbleOtherCreationsInView++;
}

/* The pool valve's own deletion (RemoveCarsIfThePoolGetsFull): the pool is
 * down to its last slots and no car passes the rule, so the closest car goes.
 * It is the pool's last line, not the cull path, and it is counted apart so
 * the acceptance counters stay the rule's own. */
void
CCarCtrl::NearBubbleCountValveRemoval(void)
{
	ms_nNearBubbleValveRemovals++;
}
