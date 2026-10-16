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
 * traffic makes asks the two gates below. The gates spend the rays, count the
 * events and say no where the rule says no. A refused event costs one ladder:
 * the placement loop of the caller moves to its next candidate.
 *
 * The counters (the fill burst's, next to its own in CarCtrl.cpp):
 *
 *	ms_nNearBubbleCreationsInFrustum	the acceptance counter, must stay 0
 *	ms_nNearBubbleRemovalsInFrustum		the acceptance counter, must stay 0
 *	ms_nNearBubbleFadeCreations		the fill burst's work: exempt
 *	ms_nNearBubbleFadeRemovals		a removal the fade covered: exempt
 *	ms_nNearBubbleHiddenCreations		the placements the rule allowed
 *	ms_nNearBubbleHiddenRemovals		the removals the rule allowed
 *	ms_nNearBubbleRefused			the events the rule stopped
 *	ms_nNearBubbleBypassed			the events the view bypassed (the top-down camera)
 *	ms_nNearBubbleRays			the rays spent since the frame began
 *	ms_nNearBubbleFill			the traffic inside the bubble
 *	ms_fNearBubbleTarget			the share of the car target its area asks for
 */

uint32 CCarCtrl::ms_nNearBubbleCreationsInFrustum;
uint32 CCarCtrl::ms_nNearBubbleRemovalsInFrustum;
uint32 CCarCtrl::ms_nNearBubbleFadeCreations;
uint32 CCarCtrl::ms_nNearBubbleFadeRemovals;
uint32 CCarCtrl::ms_nNearBubbleHiddenCreations;
uint32 CCarCtrl::ms_nNearBubbleHiddenRemovals;
uint32 CCarCtrl::ms_nNearBubbleRefused;
uint32 CCarCtrl::ms_nNearBubbleBypassed;
uint32 CCarCtrl::ms_nNearBubbleRays;
uint32 CCarCtrl::ms_nNearBubbleFill;
float CCarCtrl::ms_fNearBubbleTarget;
int32 CCarCtrl::ms_nNearBubbleRaysLeft;

/* The view of this frame is not the player's: the top-down camera looks at the
 * map from above, where the street level is not the subject and the traffic
 * must keep flowing. The rule steps aside (the events are counted as bypassed,
 * so nothing hides in the counters). The flag is the one the spawner's
 * top-down branch uses. */
static bool
NearBubbleViewIsBypassed(void)
{
	return TheCamera.GetForward().z < -0.9f;
}

/* One candidate of this frame, from the position and the live view. */
static SNearBubbleCandidate
NearBubbleCandidateAt(const CVector &pos, const CVector &playerCentre, float radius)
{
	SNearBubbleCandidate c;

	c.distToPlayer = (pos - playerCentre).Magnitude2D();
	c.distToCamera = (pos - TheCamera.GetPosition()).Magnitude();
	c.visibleRange = CurrentTrafficRings().visibleRange;
	c.inFrustum = TheCamera.IsSphereVisible(pos, radius);
	c.occluded = COcclusion::IsPositionOccluded(pos, radius);
	c.fadeHides = FillBurstIsHidden();
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

void
CCarCtrl::NearBubbleBeginFrame(void)
{
	ms_nNearBubbleRaysLeft = NEAR_BUBBLE_LOS_RAYS_PER_FRAME;
	ms_fNearBubbleTarget = NearBubbleTarget((float)GetRandomCarTarget(), CurrentTrafficRings().carDespawn);
}

/* The creation gate: may a car appear at this position? `rayAllowed` is false
 * for the paths that cannot afford a ray per candidate: the parked cars of the
 * map (an empty generator retries every four frames, and the generators run
 * before the traffic in the frame), where a close blind spot is treated as
 * visible. */
bool
CCarCtrl::NearBubbleAllowsCreation(const CVector &pos, const CVector &playerCentre, float radius, bool rayAllowed)
{
	if (NearBubbleViewIsBypassed()) {
		ms_nNearBubbleBypassed++;
		return true;
	}

	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, playerCentre, radius);
	ENearBubbleAnswer answer = NearBubbleAnswerFor(c);

	if (answer == NEAR_BUBBLE_SEEK_LOS) {
		/* The one expensive test, from the frame's budget. A candidate that
		 * finds the budget spent is refused: the placement loop of the caller
		 * tries its next candidate, and the frame's work stays bounded. */
		if (rayAllowed && ms_nNearBubbleRaysLeft > 0) {
			ms_nNearBubbleRaysLeft--;
			ms_nNearBubbleRays++;
			if (NearBubblePositionIsHiddenByBuilding(pos))
				answer = NEAR_BUBBLE_ALLOWED;
			else
				answer = NEAR_BUBBLE_REFUSED;
		} else {
			answer = NEAR_BUBBLE_REFUSED;
		}
	}

	if (answer == NEAR_BUBBLE_REFUSED)
		ms_nNearBubbleRefused++;
	return answer != NEAR_BUBBLE_REFUSED;
}

/* The removal gate: a car may go only when the grace has passed, it is beyond
 * the removal threshold, and it is hidden. The threshold takes the ring's own
 * despawn (which is already the spawn band plus the reserve) and the bubble
 * plus its reserve, whichever is farther. A removal spends no ray: it is the
 * game's soft event, and the cheap arms of the ladder decide. */
bool
CCarCtrl::NearBubbleAllowsRemoval(CVehicle *pVehicle, const CVector &playerCentre, float ringThreshold)
{
	if (NearBubbleViewIsBypassed()) {
		ms_nNearBubbleBypassed++;
		return true;
	}

	if (CTimer::GetTimeInMilliseconds() < pVehicle->m_nRemovalProtectedUntil) {
		ms_nNearBubbleRefused++;
		return false;
	}

	float threshold = Max(ringThreshold, NEAR_BUBBLE_REMOVE_DISTANCE);
	if ((pVehicle->GetPosition() - playerCentre).Magnitude2D() <= threshold) {
		ms_nNearBubbleRefused++;
		return false;
	}

	/* The visibility of the car itself: its bounding sphere, or the named
	 * radius of the bubble where the car is smaller. */
	float radius = Max(NEAR_BUBBLE_ENTITY_RADIUS, pVehicle->GetBoundRadius());
	SNearBubbleCandidate c = NearBubbleCandidateAt(pVehicle->GetBoundCentre(), playerCentre, radius);
	ENearBubbleAnswer answer = NearBubbleAnswerFor(c);
	/* The close blind spot is an allow for a removal: the ray is the creation
	 * rule's budget, and a car behind the camera may leave (the game fades a
	 * removal in view, and a removal the player can see is refused above). */
	if (answer == NEAR_BUBBLE_SEEK_LOS)
		answer = NEAR_BUBBLE_ALLOWED;

	if (answer == NEAR_BUBBLE_REFUSED)
		ms_nNearBubbleRefused++;
	return answer != NEAR_BUBBLE_REFUSED;
}

/* The acceptance counters, read after a car of the traffic entered the world
 * (any path: the trickle, the fill burst, an emergency car, a generator). An
 * event the fade covered is exempt and counted as such; an event at a
 * catchable position moves the counter the acceptance watches. The gate above
 * never allows one, so the counter catches the paths that bypass it. */
void
CCarCtrl::NearBubbleCountCreation(const CVector &pos, float radius)
{
	if (NearBubbleViewIsBypassed()) {
		ms_nNearBubbleBypassed++;
		return;
	}
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, FindPlayerCentreOfWorld(CWorld::PlayerInFocus), radius);
	if (c.fadeHides)
		ms_nNearBubbleFadeCreations++;
	else if (NearBubblePositionIsCatchable(c))
		ms_nNearBubbleCreationsInFrustum++;
	else
		ms_nNearBubbleHiddenCreations++;
}

/* A removal the caller made. `wasInvisible` tells the counter the car was
 * already fully faded: the deletion of an invisible car is not an event the
 * player can catch, however the frustum reads. */
void
CCarCtrl::NearBubbleCountRemoval(const CVector &pos, float radius, bool wasInvisible)
{
	if (NearBubbleViewIsBypassed()) {
		ms_nNearBubbleBypassed++;
		return;
	}
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, FindPlayerCentreOfWorld(CWorld::PlayerInFocus), radius);
	if (c.fadeHides)
		ms_nNearBubbleFadeRemovals++;
	else if (!wasInvisible && NearBubblePositionIsCatchable(c))
		ms_nNearBubbleRemovalsInFrustum++;
	else
		ms_nNearBubbleHiddenRemovals++;
}
