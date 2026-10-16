/*
 * The evidence program for VICE-126, "burst fill: full streets on boot, interior
 * exit and cutscene end".
 *
 * It has three jobs, all offline:
 *
 *   A. The bounds of the burst. It reads the burst table (src/core/
 *      PopulationBudget.h) and the ring table (src/control/TrafficRings.h) and
 *      prints the work one burst can ask for: the attempts a frame spends while
 *      the screen hides the fill and while it does not, the attempts of a whole
 *      window (the hard contract of every run), and the give-up bound. The
 *      program also checks the one cross-reference the two tables cannot check
 *      themselves: the burst of a visible frame beats the trickle at the ring
 *      cap (PED_ATTEMPTS_PER_FRAME x RING_MAX_SCALE).
 *
 *   B. The state machine. It runs the fill-burst helper of src/core/FillBurst.h
 *      itself - the same inline functions the game calls - and checks every rule
 *      the burst promises: a productive frame spends the window, a frame the
 *      gates stop does not, a stall is an empty frame that ran, enough stalls in
 *      a row give the burst up, and the stop line ends the burst at the
 *      fraction of the local target. It also checks the target wait (VICE-146,
 *      fixed in the follow-up round): a frame whose local target is not known
 *      yet (0) rides its own wait - it does not spend the window and does not
 *      burn the stall bound, and it outlives that bound - so the boot burst
 *      runs the moment the area is live instead of being disarmed silently at
 *      boot (the live defect). Every helper path that ends a run reports its
 *      reason (the window spent, the give-up bound, or the target wait bound),
 *      and the caller mirror counts them, so a silent disarm fails the run.
 *
 *   C. The triggers. It prints the transitions that arm a burst and the file
 *      that owns each trigger, so the design of the ticket is in one place next
 *      to its numbers.
 *
 * What the program cannot measure: the cost of one attempt in the running game
 * (the collision queries of a car placement, the node search of a pedestrian)
 * and the fill of a real transition. Those are the orchestrator's live
 * measurement (utils/caps-audit/live-counters.py reads the counters of the
 * burst, and the perf recipe of VICE-119 measures the frame). This program
 * bounds the work; it does not pretend to measure it.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/control -O2 -o /tmp/burst-fill-evidence utils/burst-fill/burst.cpp
 *	/tmp/burst-fill-evidence
 *
 * The exit code is 0 when every check passes.
 */

#include "FillBurst.h"
#include "PopulationBudget.h"
#include "TrafficRings.h"

#include <stdio.h>

static int g_failures = 0;

static void Fail(const char *what)
{
	g_failures++;
	printf("FAIL: %s\n", what);
}

static void Check(bool condition, const char *what)
{
	if (!condition)
		Fail(what);
}

/* ------------------------------------------------------------ A. the bounds */

static void PrintBounds(void)
{
	printf("A. The work of one burst (src/core/PopulationBudget.h, src/control/TrafficRings.h)\n\n");
	printf("  the window:                  %d frames (%.1f s at 30 fps)\n",
		FILL_BURST_FRAMES, FILL_BURST_FRAMES / 30.0f);
	printf("  attempts, visible frame:     %d (the trickle at the ring cap is %.0f)\n",
		FILL_BURST_ATTEMPTS_PER_FRAME, PED_ATTEMPTS_PER_FRAME * RING_MAX_SCALE);
	printf("  attempts, hidden frame:      %d\n", FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME);
	printf("  the hidden threshold:        a fade of %d of 255 (%.0f %% of the screen)\n",
		FILL_BURST_HIDDEN_FADE, 100.0f * FILL_BURST_HIDDEN_FADE / 255.0f);
	printf("  attempts, whole window:      %d (every frame hidden) .. %d (every frame visible)\n",
		FILL_BURST_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME,
		FILL_BURST_FRAMES * FILL_BURST_ATTEMPTS_PER_FRAME);
	printf("  the stop line:               %.0f %% of the local target\n", 100.0f * FILL_BURST_TARGET_FRACTION);
	printf("  the give-up bound:           %d frames in a row without a placement (%.1f s)\n",
		FILL_BURST_STALL_FRAMES, FILL_BURST_STALL_FRAMES / 30.0f);
	printf("  the target wait bound:       %d frames (%.1f s at 30 fps; its own, not the give-up bound)\n\n",
		FILL_BURST_TARGET_WAIT_FRAMES, FILL_BURST_TARGET_WAIT_FRAMES / 30.0f);

	/* The cross-reference the two tables cannot make themselves: the budget
	 * table does not include the ring table, so the ratio between the burst and
	 * the trickle at the ring cap is checked here, against the ring table
	 * itself. */
	Check(FILL_BURST_ATTEMPTS_PER_FRAME >= PED_ATTEMPTS_PER_FRAME * RING_MAX_SCALE,
		"the burst of a visible frame beats the trickle it replaces");
	Check(FILL_BURST_FRAMES * FILL_BURST_ATTEMPTS_PER_FRAME >= FILL_BURST_FRAMES * PED_ATTEMPTS_PER_FRAME * RING_MAX_SCALE,
		"the burst of a window beats the trickle of the same window");
}

/* ------------------------------------------------------- B. the state machine */

/* What one burst run cost: the frames that ran, the frames that placed
 * something, the attempts the caller would have spent, and the reason the run
 * ended (the stop line, the window, or the give-up bound - VICE-146). */
struct SBurstRun
{
	int framesSpent;
	int framesPlaced;
	int attempts;
	bool endedByStopLine;
	bool endedByWindow;
	bool endedByStalls;
};

/*
 * Run one burst the way a population runs it: the stop line first (it disarms
 * the burst), then a frame that spends its attempts. `count` and `target` are
 * the local count and the local target of the caller; `places` is the number of
 * the run's frames that place something, counted from the first frame on. The
 * frames of the run all spend their attempts, so the loop ends at the window,
 * at the give-up bound or at the stop line, and never spins.
 */
static SBurstRun
RunBurst(float count, float target, int places, bool hidden)
{
	SBurstRun run = { 0, 0, 0, false, false, false };
	SFillBurst burst;
	int placedLeft = places;

	FillBurstArm(&burst);
	Check(FillBurstIsRunning(&burst), "an armed burst runs");
	Check(burst.nFrames == FILL_BURST_FRAMES, "an armed burst carries the whole window");
	Check(burst.nStalls == 0, "an armed burst starts without stalls");

	while (FillBurstIsRunning(&burst)) {
		if (FillBurstTargetReached(count, target)) {
			FillBurstDisarm(&burst);
			run.endedByStopLine = true;
			break;
		}
		run.attempts += FillBurstAttempts(&burst, hidden);
		bool placed = placedLeft > 0;
		if (placed)
			placedLeft--;
		EFillBurstEnded ended = FillBurstSpendFrame(&burst, true, placed);
		if (ended == FILL_BURST_ENDED_WINDOW)
			run.endedByWindow = true;
		else if (ended == FILL_BURST_ENDED_STALLS)
			run.endedByStalls = true;
		run.framesSpent++;
		if (placed)
			run.framesPlaced++;
	}
	return run;
}

static void CheckStateMachine(void)
{
	printf("B. The state machine (src/core/FillBurst.h, the same inline functions the game calls)\n\n");

	/* The attempts of a frame: the whole budget while hidden, the small one
	 * while visible. */
	SFillBurst burst;
	FillBurstArm(&burst);
	Check(FillBurstAttempts(&burst, true) == FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME,
		"a hidden frame spends the hidden budget");
	Check(FillBurstAttempts(&burst, false) == FILL_BURST_ATTEMPTS_PER_FRAME,
		"a visible frame spends the visible budget");

	/* The window: exactly FILL_BURST_FRAMES frames that spend attempts, not one
	 * more, and that is the worst case of every run. */
	SBurstRun full = RunBurst(0.0f, 1000.0f, FILL_BURST_FRAMES + 10, true);
	Check(full.framesSpent == FILL_BURST_FRAMES, "a burst runs exactly the frames of its window");
	Check(full.framesPlaced == FILL_BURST_FRAMES, "every frame of the run placed something");
	Check(full.attempts == FILL_BURST_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME,
		"the window of hidden frames spends the hidden budget of every frame");
	Check(!full.endedByStopLine, "the window can end without the stop line");
	Check(full.endedByWindow && !full.endedByStalls, "a spent window reports the window as the end reason");

	SBurstRun visible = RunBurst(0.0f, 1000.0f, FILL_BURST_FRAMES, false);
	Check(visible.attempts == FILL_BURST_FRAMES * FILL_BURST_ATTEMPTS_PER_FRAME,
		"a window of visible frames spends the visible budget of every frame");
	Check(FILL_BURST_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME > visible.attempts,
		"a hidden window spends more than a visible one");

	/* A frame that spends its attempts spends the window, placing or not: the
	 * window is the hard bound of the work, whatever the area does. */
	SBurstRun uneven = RunBurst(0.0f, 1000.0f, FILL_BURST_FRAMES / 2, true);
	Check(uneven.framesSpent == FILL_BURST_FRAMES,
		"a window with empty frames still runs its window frames");
	Check(uneven.attempts == FILL_BURST_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME,
		"no run can spend more than the window of hidden attempts");
	Check(uneven.framesPlaced == FILL_BURST_FRAMES / 2, "the empty frames placed nothing");

	/* A stall is a frame that ends without a placement, with attempts spent:
	 * the give-up bound of them in a row ends the burst before its window. */
	SFillBurst stalling;
	FillBurstArm(&stalling);
	for (int i = 0; i < FILL_BURST_STALL_FRAMES - 1; i++)
		FillBurstSpendFrame(&stalling, true, false);
	Check(FillBurstIsRunning(&stalling), "a burst survives stalls below the bound");
	Check(stalling.nFrames == FILL_BURST_FRAMES - (FILL_BURST_STALL_FRAMES - 1),
		"every stalled frame spends its window frame");
	Check(stalling.nStalls == FILL_BURST_STALL_FRAMES - 1, "the stalled frames are counted");
	FillBurstSpendFrame(&stalling, true, false);
	Check(!FillBurstIsRunning(&stalling), "the give-up bound ends the burst");

	/* A frame that cannot spend its attempts (the streaming wait) does not
	 * spend the window, but it counts against the same bound: the wait is
	 * bounded, and the window is still there when the models arrive. It
	 * reports the give-up reason when the bound fires. */
	SFillBurst waiting;
	FillBurstArm(&waiting);
	for (int i = 0; i < FILL_BURST_STALL_FRAMES - 1; i++)
		Check(FillBurstSpendFrame(&waiting, false, false) == FILL_BURST_NOT_ENDED,
			"a wait below the bound does not end the burst");
	Check(FillBurstIsRunning(&waiting), "a burst survives a wait below the bound");
	Check(waiting.nFrames == FILL_BURST_FRAMES, "a frame that spends no attempt leaves the window whole");
	Check(waiting.nStalls == FILL_BURST_STALL_FRAMES - 1, "the waiting frames are counted");
	Check(FillBurstSpendFrame(&waiting, false, false) == FILL_BURST_ENDED_STALLS,
		"the give-up bound reports the stalls as the end reason");
	Check(!FillBurstIsRunning(&waiting), "the wait bound ends the burst");

	/* The target wait (VICE-146 fix): a frame whose local target is not known
	 * yet (0) rides the dedicated wait. It spends no attempt, so the window
	 * stays whole; it does NOT burn the stall bound - an invalid target is not
	 * a placement stall - and it must outlive that bound (the live defect: the
	 * first cut routed this wait through FillBurstSpendFrame, the 30-frame
	 * stall bound fired inside the helper, and the pedestrian boot burst died
	 * with no attempt and no recorded reason). This case fails if the wait is
	 * ever routed back through the stall helper. */
	SFillBurst boot;
	FillBurstArm(&boot);
	for (int i = 0; i < FILL_BURST_STALL_FRAMES + 5; i++)
		Check(FillBurstSpendTargetWait(&boot) == FILL_BURST_NOT_ENDED,
			"a boot frame without a target does not end the burst");
	Check(FillBurstIsRunning(&boot), "the target wait outlives the stall bound it must not burn");
	Check(boot.nFrames == FILL_BURST_FRAMES, "the target wait leaves the window whole");
	Check(boot.nStalls == 0, "the target wait does not raise the stall counter");
	Check(boot.nTargetWaits == FILL_BURST_STALL_FRAMES + 5, "the target wait frames have their own counter");
	Check(FillBurstSpendFrame(&boot, true, true) == FILL_BURST_NOT_ENDED,
		"the first frame with a target runs");
	Check(boot.nFrames == FILL_BURST_FRAMES - 1, "the target's frame spends a window frame");
	Check(boot.nTargetWaits == 0, "a live frame clears the target wait counter");

	/* A world that never becomes live gives the burst up at the dedicated
	 * bound, with its own reason - never a silent disarm. */
	SFillBurst neverLive;
	FillBurstArm(&neverLive);
	for (int i = 0; i < FILL_BURST_TARGET_WAIT_FRAMES - 1; i++)
		Check(FillBurstSpendTargetWait(&neverLive) == FILL_BURST_NOT_ENDED,
			"the target wait below its own bound does not end the burst");
	Check(FillBurstIsRunning(&neverLive), "the burst survives a long target wait");
	Check(FillBurstSpendTargetWait(&neverLive) == FILL_BURST_ENDED_TARGET_WAIT,
		"the target wait bound reports its reason");
	Check(!FillBurstIsRunning(&neverLive), "the target wait bound ends the burst");
	Check(neverLive.nFrames == 0, "the ended target wait leaves no window behind");

	/* A placement resets the counter: the bound is a run of frames without one. */
	SFillBurst recovering;
	FillBurstArm(&recovering);
	for (int i = 0; i < FILL_BURST_STALL_FRAMES - 1; i++)
		FillBurstSpendFrame(&recovering, true, false);
	FillBurstSpendFrame(&recovering, true, true);
	Check(recovering.nStalls == 0, "a placement resets the counter");
	Check(FillBurstIsRunning(&recovering), "a placement keeps the burst alive");
	Check(recovering.nFrames == FILL_BURST_FRAMES - FILL_BURST_STALL_FRAMES, "a placement spends the frame");

	/* A run an area refuses (every frame spends attempts and places nothing)
	 * ends at the give-up bound, with the rest of the window unused. */
	SBurstRun refused = RunBurst(0.0f, 1000.0f, 0, true);
	Check(refused.framesSpent == FILL_BURST_STALL_FRAMES, "a run that always fails ends at the give-up bound");
	Check(refused.attempts == FILL_BURST_STALL_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME,
		"the failing run spends the attempts of its frames");
	Check(!refused.endedByStopLine, "the failing run is not the stop line");
	Check(refused.endedByStalls && !refused.endedByWindow, "a failing run reports the give-up bound");

	/* The window's last frame reports its reason too: no spend that ends a run
	 * is quiet (the stall and target-wait bounds above report likewise). */
	SFillBurst last;
	FillBurstArm(&last);
	last.nFrames = 1;
	Check(FillBurstSpendFrame(&last, true, true) == FILL_BURST_ENDED_WINDOW,
		"the last window frame reports the window as the end reason");
	Check(!FillBurstIsRunning(&last), "the last window frame ends the burst");

	/* The stop line: 90 % of the local target. The predicate reads true at a
	 * zero target (0 is at 90 % of 0): that is exactly why the callers check
	 * the target first and wait (VICE-146), and the caller-guard case below
	 * holds the wait. */
	Check(!FillBurstTargetReached(89.9f, 100.0f), "89.9 % of the target does not stop the burst");
	Check(FillBurstTargetReached(90.0f, 100.0f), "90 % of the target stops the burst");
	Check(FillBurstTargetReached(0.0f, 0.0f), "the stop-line predicate reads true at a zero target");
	Check(FillBurstTargetReached(5.0f, 0.0f), "the stop-line predicate reads true when the target is lost");

	/* The caller's guard and reason accounting (VICE-146 fix), as far as an
	 * offline program can reach: one frame of the callers' own decision, in
	 * their order - the target first (a 0 target rides the dedicated wait),
	 * the stop line of a live target second, the frame last (Population.cpp,
	 * CarCtrl.cpp) - with every end reason counted. The boot must not end the
	 * burst, and no frame may end it without a reason. */
	SFillBurst guarded;
	int bootWaits = 0, silentEnds = 0;
	FillBurstArm(&guarded);
	for (int i = 0; i < FILL_BURST_STALL_FRAMES + 10; i++) {
		float target = 0.0f;	/* the world is not live yet */
		if (target <= 0.0f) {
			/* The caller's wait: the stop line is never reached with a 0 target. */
			if (FillBurstSpendTargetWait(&guarded) != FILL_BURST_NOT_ENDED)
				silentEnds++;	/* an end while the world is not live would be wrong */
			bootWaits++;
			continue;
		}
		if (FillBurstTargetReached(0.0f, target)) {
			FillBurstDisarm(&guarded);	/* the caller's own stop-line disarm */
			break;
		}
		if (FillBurstSpendFrame(&guarded, true, true) != FILL_BURST_NOT_ENDED)
			silentEnds++;
	}
	Check(bootWaits == FILL_BURST_STALL_FRAMES + 10, "the caller's target wait covers the boot");
	Check(FillBurstIsRunning(&guarded), "the caller's guard survives the zero-target boot");
	Check(silentEnds == 0, "no frame of the zero-target boot ends the run");
	Check(guarded.nFrames == FILL_BURST_FRAMES, "the boot did not spend the window");
	Check(guarded.nStalls == 0 && guarded.nTargetWaits == FILL_BURST_STALL_FRAMES + 10,
		"the boot frames are the target wait's own");

	/* The target arrives: the live frames run, spend the window and clear the
	 * wait, and a run that does end returns its reason - never a silent end. */
	for (int i = 0; i < 5; i++) {
		float target = 500.0f;
		if (FillBurstTargetReached(0.0f, target))
			break;
		if (FillBurstSpendFrame(&guarded, true, true) != FILL_BURST_NOT_ENDED)
			silentEnds++;
	}
	Check(silentEnds == 0, "the live frames below the bounds do not end the run");
	Check(guarded.nFrames == FILL_BURST_FRAMES - 5, "the live frames spend the window");
	Check(guarded.nStalls == 0 && guarded.nTargetWaits == 0, "the live frames clear the waits");

	SBurstRun stopped = RunBurst(900.0f, 1000.0f, FILL_BURST_FRAMES, true);
	Check(stopped.endedByStopLine, "the stop line ends the burst");
	Check(stopped.attempts == 0, "the frame of the stop line spends no attempt");
	Check(stopped.framesSpent == 0, "the frame of the stop line does not run");

	printf("  the contract: a burst asks for at most %d attempts (%.1f s of hidden frames at 30 fps),\n",
		FILL_BURST_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME, FILL_BURST_FRAMES / 30.0f);
	printf("  whatever the gates do; the wall clock of a run is the frames it spends plus its waits\n");
	printf("  counters for the live rig: CPopulation::ms_FillBurst, ms_nFillBurstAttempts, ms_nFillBurstPlaced,\n");
	printf("  ms_fFillBurstTarget, and the since-boot pair ms_nFillBurstTotalAttempts / ms_nFillBurstTotalPlaced\n");
	printf("  with the end reasons ms_nFillBurstEndedLine / Stalls / Window (VICE-146); the same eight on\n");
	printf("  CCarCtrl. A boot burst that waits for its target is not burned on it, so the totals show\n");
	printf("  the run once the area is live.\n\n");
}

/* ------------------------------------------------------------ C. the triggers */

static void PrintTriggers(void)
{
	printf("C. The triggers (each one arms both bursts unless it says otherwise)\n\n");
	printf("  boot            CPopulation::m_CountDownToPedsAtStart / CCarCtrl::CountDownToCarsAtStart\n");
	printf("                  expire (Population.cpp, CarCtrl.cpp) and arm the burst of their own population\n");
	printf("  interior exit   CGame::SetCurrArea (Game.cpp); the script's COMMAND_SET_AREA_VISIBLE and the\n");
	printf("                  restart path call it (Script7.cpp, GameLogic.cpp)\n");
	printf("  cutscene end    CCutsceneMgr::DeleteCutsceneData (CutsceneMgr.cpp), the script's CLEAR_CUTSCENE\n");
	printf("  death, arrest   the restart countdowns of GameLogic.cpp arm both bursts 10 frames into the fade\n");
	printf("  the area is also set directly by paths that arm nothing: CGame::Initialise, the reset of\n");
	printf("  ReInitGameObjectVariables, the save load (CGenericGameStorage) and the replay player (CReplay)\n");
	printf("  the parked cars come with the shipped car arms (the countdowns and the cutscene end); the\n");
	printf("  interior exit leaves the generator timers alone (CTheCarGenerators::GenerateEvenIfPlayerIsCloseCounter)\n");
	printf("  a replay playback arms nothing (CGame::ArmFillBursts) and never spends a burst (the gates of\n");
	printf("  CPopulation::Update and CCarCtrl::GenerateRandomCars)\n\n");
}

int main(void)
{
	PrintBounds();
	CheckStateMachine();
	PrintTriggers();

	if (g_failures) {
		printf("%d check(s) FAILED\n", g_failures);
		return 1;
	}
	printf("every check passed\n");
	return 0;
}
