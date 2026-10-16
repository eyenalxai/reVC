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
 *	ms_nNearBubbleFadeCreations		the fade's work: the catchable
 *						events a fade covered, exempt
 *	ms_nNearBubbleFadeRemovals
 *	ms_nNearBubbleHiddenCreations		the events outside the
 *	ms_nNearBubbleHiddenRemovals		catchable set the rule allowed
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
 *	ms_nNearBubbleOtherCreationsInFrustum	catchable ones of them
 *	ms_nNearBubbleValveRemovals		the pool valve's own deletions
 *
 * The fill split (the traffic against the other cars) holds the acceptance
 * honest: the car target counts every RANDOM_VEHICLE, so the scripts, the set
 * pieces, the road blocks, the car parks, the garages and the cheat cars are
 * part of it, but only the spawner's own cars are the traffic the bubble
 * watches. A scripted car that appears in view moves the "other creations in
 * frustum" counter, never the traffic's, and the fill plus the other fill is
 * what the target describes.
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
uint32 CCarCtrl::ms_nNearBubbleRaysTotal;
uint32 CCarCtrl::ms_nNearBubbleFill;
uint32 CCarCtrl::ms_nNearBubbleOtherFill;
uint32 CCarCtrl::ms_nNearBubbleOtherCreations;
uint32 CCarCtrl::ms_nNearBubbleOtherCreationsInFrustum;
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
 * the car block of Game.cpp). An "occluded" answer counts only while the
 * camera still matches the view the set was built for; past the
 * NEAR_BUBBLE_OCCLUDER_* thresholds it is ignored and the other arms decide -
 * a refused spawn, not a pop the counters cannot see. */
static bool
NearBubbleOccluderSetIsCurrent(void)
{
	if (COcclusion::NumActiveOccluders == 0)
		/* No set, no answer: IsPositionOccluded says false anyway. */
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
	c.inFrustum = TheCamera.IsSphereVisible(pos, radius);
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

/* The creation gate: may a car appear at this position? `rayAllowed` is false
 * for the paths that cannot afford a ray per candidate: the parked cars of the
 * map (an empty generator retries every four frames, and the generators run
 * before the traffic in the frame), where a close blind spot is treated as
 * visible. */
bool
CCarCtrl::NearBubbleAllowsCreation(const CVector &pos, const CVector &playerCentre, float radius, bool rayAllowed)
{
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, playerCentre, radius);
	ENearBubbleAnswer answer = NearBubbleAnswerFor(c);

	if (answer == NEAR_BUBBLE_SEEK_LOS) {
		/* The one expensive test, from the frame's budget. A candidate that
		 * finds the budget spent is refused: the placement loop of the caller
		 * tries its next candidate, and the frame's work stays bounded. */
		if (rayAllowed && ms_nNearBubbleRaysLeft > 0) {
			ms_nNearBubbleRaysLeft--;
			ms_nNearBubbleRays++;
			ms_nNearBubbleRaysTotal++;
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

/* The acceptance counters, read after a car of the traffic entered the world.
 * The catchable set of the rule decides the row: an event the player could
 * catch is a violation unless the fade covered it (the fill burst's
 * exemption), and everything else is the rule's own work. A path that bypasses
 * the gate shows up in the frustum row. */
void
CCarCtrl::NearBubbleCountCreation(const CVector &pos, float radius)
{
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, FindPlayerCentreOfWorld(CWorld::PlayerInFocus), radius);
	if (c.bypassView) {
		ms_nNearBubbleBypassed++;
	} else if (NearBubblePositionIsCatchable(c)) {
		if (c.fadeHides)
			ms_nNearBubbleFadeCreations++;
		else
			ms_nNearBubbleCreationsInFrustum++;
	} else {
		ms_nNearBubbleHiddenCreations++;
	}
}

/* A removal the caller made. `wasInvisible` tells the counter the car was
 * already fully faded: the deletion of an invisible car is not an event the
 * player can catch, however the frustum reads. */
void
CCarCtrl::NearBubbleCountRemoval(const CVector &pos, float radius, bool wasInvisible)
{
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, FindPlayerCentreOfWorld(CWorld::PlayerInFocus), radius);
	if (c.bypassView) {
		ms_nNearBubbleBypassed++;
	} else if (NearBubblePositionIsCatchable(c)) {
		if (c.fadeHides)
			ms_nNearBubbleFadeRemovals++;
		else if (!wasInvisible)
			ms_nNearBubbleRemovalsInFrustum++;
		else
			ms_nNearBubbleHiddenRemovals++;
	} else {
		ms_nNearBubbleHiddenRemovals++;
	}
}

/* A car that entered the world outside the traffic spawner (a script, a set
 * piece, a road block, a car park, a garage, a cheat, the RC car). It is not
 * the traffic the bubble fills, so it never moves the traffic's counters; the
 * pair below carries it: the total and the catchable ones, so a car that
 * appears in view is visible in the live numbers. */
void
CCarCtrl::NearBubbleCountOtherCreation(const CVector &pos, float radius)
{
	ms_nNearBubbleOtherCreations++;
	SNearBubbleCandidate c = NearBubbleCandidateAt(pos, FindPlayerCentreOfWorld(CWorld::PlayerInFocus), radius);
	if (!c.fadeHides && NearBubblePositionIsCatchable(c))
		ms_nNearBubbleOtherCreationsInFrustum++;
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
