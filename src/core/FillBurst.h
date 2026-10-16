#ifndef __GTA_FILLBURST_H__
#define __GTA_FILLBURST_H__

/*
 * The fill burst (VICE-126).
 *
 * A fill burst is the transition fill of the population: after boot, an
 * interior exit (the script's COMMAND_SET_AREA_VISIBLE) and a cutscene end
 * (CCutsceneMgr), the counts jump to their local targets instead of trickling
 * up. One burst is one bounded work window: FILL_BURST_FRAMES frames that spend
 * FILL_BURST_ATTEMPTS_PER_FRAME spawn attempts each, or the larger
 * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME while the screen hides the fill (the
 * black frames of a transition). The bounds live in the cap table,
 * PopulationBudget.h, next to the trickle they replace.
 *
 * The contract is one number and it holds for every run: a burst asks for at
 * most FILL_BURST_FRAMES x FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME attempts
 * (6000). A frame that spends its attempts spends a window frame, placing or
 * not; a frame that cannot spend them (the streaming has no model of the zone
 * yet) does not spend the window, but it counts against the same give-up bound,
 * so the wait is bounded too and the wall clock cannot run away from the
 * attempts.
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
 *    the fill hands back to the trickle. The caller checks it before the frame
 *    and every attempt inside the frame, and the count it checks is the live
 *    one: the pedestrian burst recomputes the totals every attempt, exactly as
 *    the shipped start-of-game burst did, so the overshoot of the line is one
 *    placement at most.
 *  - The gates: the burst runs inside the same blocks and behind the same
 *    checks as the trickle (the replay gate, the cutscene gate, the pool
 *    reserve, the resident models).
 *  - The stall: a frame that ends without a placement is a stall, whatever
 *    stopped it - no free placement, or a model that is not in memory yet.
 *    FILL_BURST_STALL_FRAMES stalls in a row disarm the burst, so a target that
 *    cannot be reached and a streaming that never arrives cannot keep a burst
 *    alive forever, and the trickle takes the fill back.
 *
 * The random stream
 * -----------------
 *
 * The burst makes its attempts with the existing generators
 * (CPopulation::AddToPopulation, CCarCtrl::GenerateOneRandomCar), so it draws
 * from the one global random stream, exactly like the trickle. It does not
 * reseed and does not restore the stream: every draw a burst makes shifts the
 * later draws of the shared stream until something reseeds it. The shift is
 * the price of the fill and it is documented, not hidden; the accurate
 * statement of what it does and does not reach is in the ADR
 * (docs/adr/0021-the-fill-burst.md) and in PopulationBudget.h.
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
	int nFrames;	/* the window frames that still carry attempts; 0 = idle */
	int nStalls;	/* the frames in a row without a placement */
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
 * One frame of the burst is over.
 *
 * `attempts` is true when the frame spent its attempt budget (the models were
 * resident and the caller let the frame run): the frame spends the window,
 * whether it placed something or not. `placed` is true when it created at
 * least one entity.
 *
 * A frame that spent no attempt (the streaming has no model of the zone yet)
 * does not spend the window: the fill waits for the models instead of burning
 * its frames on attempts that would fail at the model check. It counts against
 * the give-up bound all the same, so the wait is bounded like a stall.
 *
 * A placement resets the counter of the frames in a row without one, and
 * FILL_BURST_STALL_FRAMES of them give the burst up.
 */
static inline void
FillBurstSpendFrame(SFillBurst *burst, bool attempts, bool placed)
{
	if (attempts)
		burst->nFrames--;
	if (attempts && placed)
		burst->nStalls = 0;
	else if (++burst->nStalls >= FILL_BURST_STALL_FRAMES)
		FillBurstDisarm(burst);
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
