#ifndef __GTA_FILLBURST_H__
#define __GTA_FILLBURST_H__

/*
 * The fill burst (VICE-126).
 *
 * A fill burst is the transition fill of the population: after boot, an
 * interior exit (the script's COMMAND_SET_AREA_VISIBLE) and a cutscene end
 * (CCutsceneMgr), the counts jump to their local targets instead of trickling
 * up. One burst is one bounded work window of FILL_BURST_FRAMES frames that
 * each carry FILL_BURST_ATTEMPTS_PER_FRAME spawn attempts, or the larger
 * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME while the screen hides the fill (the
 * black frames of the transition). A frame the gates stop is not spent, so the
 * window is a work window and not a stopwatch. The bounds live in the cap
 * table, PopulationBudget.h, next to the trickle they replace.
 *
 * The state is shared by the two populations, one instance each
 * (CPopulation::ms_FillBurst and CCarCtrl::ms_FillBurst): the pedestrian burst
 * and the car burst run on their own clocks, because a frame of one is not a
 * frame of the other (a car spawn is the costlier of the two).
 *
 * The rules the caller owns
 * -------------------------
 *
 *  - The stop line: when the local count has reached
 *    FILL_BURST_TARGET_FRACTION of the local target, the burst is disarmed and
 *    the fill hands back to the trickle. A burst never runs past its target.
 *  - The gates: the burst runs inside the same blocks and behind the same
 *    checks as the trickle (the replay gate, the cutscene gate, the pool
 *    reserve, the resident models). A frame the gates stop does not spend the
 *    window: the fill waits, and the wait is not a stall. That is what keeps
 *    the burst from spending itself while the streaming has not loaded the
 *    models yet - every attempt would fail at the model check.
 *  - The stall: a frame that runs (the gates passed) and places nothing is a
 *    stall. FILL_BURST_STALL_FRAMES stalls in a row disarm the burst, so a
 *    target that cannot be reached (no free placement, an empty zone) cannot
 *    keep a burst alive forever.
 *
 * The random stream
 * -----------------
 *
 * The burst makes its attempts with the existing generators
 * (CPopulation::AddToPopulation, CCarCtrl::GenerateOneRandomCar), so it draws
 * from the one global random stream, exactly like the trickle. It does not
 * reseed and does not restore the stream: every draw a burst makes shifts
 * every later draw of the frame and of the frames after it (pedestrian models,
 * the accident rolls, the traffic route choices, the weather). The shift is
 * the price of the fill and it is documented, not hidden; the car generators
 * keep their own per-vehicle seeds (CCarCtrl reseeds from m_nRouteSeed), so
 * the car AI of an existing car is not shifted.
 *
 * No engine header is included here: the evidence program in
 * utils/burst-fill simulates the state machine with this header alone, and
 * utils/caps-audit/live-counters.py reads the counters of the two instances in
 * the running game.
 */

#include "PopulationBudget.h"

/* The state of one burst. Zero means idle. */
struct SFillBurst
{
	int nFrames;	/* the frames of the window that still carry attempts; 0 = idle */
	int nStalls;	/* the consecutive frames that ran and placed nothing */
};

/* Arm the window: a transition has happened and the fill may spend it. */
static inline void
FillBurstArm(SFillBurst *burst)
{
	burst->nFrames = FILL_BURST_FRAMES;
	burst->nStalls = 0;
}

static inline void
FillBurstDisarm(SFillBurst *burst)
{
	burst->nFrames = 0;
	burst->nStalls = 0;
}

static inline bool FillBurstIsRunning(const SFillBurst *burst)
{ return burst->nFrames > 0; }

/*
 * The attempts of one frame: the whole budget while the screen hides the fill
 * (the black frames of a transition), the small one while the player can see
 * the world.
 */
static inline int FillBurstAttempts(const SFillBurst *burst, bool hidden)
{ return hidden ? FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME : FILL_BURST_ATTEMPTS_PER_FRAME; }

/*
 * One frame of the burst is over. `ran` is false when the gates of the caller
 * stopped the frame (the models are not resident, a cutscene runs, the pool is
 * down to its reserve): the window is not spent and the frame is not a stall.
 * When the frame ran, `placed` says whether it created anything: an empty
 * frame is a stall, and FILL_BURST_STALL_FRAMES stalls in a row give the
 * burst up.
 */
static inline void
FillBurstSpendFrame(SFillBurst *burst, bool ran, bool placed)
{
	if (!ran)
		return;
	if (placed) {
		burst->nFrames--;
		burst->nStalls = 0;
	} else if (++burst->nStalls >= FILL_BURST_STALL_FRAMES) {
		FillBurstDisarm(burst);
	}
}

/* The stop line: the local count is at 90 % of the local target. A zero
 * target stops the burst too: there is nothing to fill. */
static inline bool
FillBurstTargetReached(float count, float target)
{
	return count >= FILL_BURST_TARGET_FRACTION * target;
}

/*
 * The screen hides the fill: the fade the game draws over the world covers at
 * least FILL_BURST_HIDDEN_FADE of it (the black frames of a transition and the
 * dark head of a fade). Defined in FillBurst.cpp (it needs the draw state).
 */
bool FillBurstIsHidden(void);

#endif // __GTA_FILLBURST_H__
