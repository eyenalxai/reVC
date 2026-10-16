/*
 * The evidence program for VICE-126, "burst fill: full streets on boot, interior
 * exit and cutscene end".
 *
 * It has three jobs, all offline:
 *
 *   A. The bounds of the burst. It reads the burst table (src/core/
 *      PopulationBudget.h) and the ring table (src/control/TrafficRings.h) and
 *      prints the work one burst can ask for: the attempts a frame spends while
 *      the screen hides the fill and while it does not, the attempts of the
 *      whole window, and the numbers of the acceptance (the 1-2 s window, the
 *      90 % stop line). The program also checks the one cross-reference the two
 *      tables cannot check themselves: the burst of a visible frame beats the
 *      trickle at the ring cap (TRICKLE_ATTEMPTS_AT_RING_CAP against
 *      PED_ATTEMPTS_PER_FRAME x RING_MAX_SCALE).
 *
 *   B. The state machine. It runs the fill-burst helper of src/core/FillBurst.h
 *      itself - the same inline functions the game calls - and checks every rule
 *      the burst promises: a productive frame spends the window, a frame the
 *      gates stop does not, a stall is an empty frame that ran, enough stalls in
 *      a row give the burst up, and the stop line ends the burst at the
 *      fraction of the local target.
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
	printf("  the streaming wait:          %d empty frames in a row give the burst up (%.1f s)\n\n",
		FILL_BURST_STALL_FRAMES, FILL_BURST_STALL_FRAMES / 30.0f);

	/* The cross-reference the two tables cannot make themselves: this table
	 * does not include the ring table, so it states the trickle at the cap and
	 * this program checks the statement against the ring table itself. */
	Check(TRICKLE_ATTEMPTS_AT_RING_CAP == PED_ATTEMPTS_PER_FRAME * RING_MAX_SCALE,
		"TRICKLE_ATTEMPTS_AT_RING_CAP is the trickle at the ring cap");
	Check(FILL_BURST_ATTEMPTS_PER_FRAME >= PED_ATTEMPTS_PER_FRAME * RING_MAX_SCALE,
		"the burst of a visible frame beats the trickle it replaces");
	Check(FILL_BURST_FRAMES * FILL_BURST_ATTEMPTS_PER_FRAME >= FILL_BURST_FRAMES * PED_ATTEMPTS_PER_FRAME * RING_MAX_SCALE,
		"the burst of a window beats the trickle of the same window");
}

/* ------------------------------------------------------- B. the state machine */

/* What one burst run cost: the frames that ran, the frames that placed
 * something and the attempts the caller would have spent. */
struct SBurstRun
{
	int framesRan;
	int framesPlaced;
	int attempts;
	bool endedByStopLine;
};

/*
 * Run one burst the way a population runs it: the stop line first (it disarms
 * the burst), then a frame that runs. `count` and `target` are the local count
 * and the local target of the caller; `places` is the number of the run's
 * frames that place something, counted from the first frame on. Every frame
 * passes the gate of the caller, so the loop ends at the window, at the stall
 * bound or at the stop line, and never spins.
 */
static SBurstRun
RunBurst(float count, float target, int places, bool hidden)
{
	SBurstRun run = { 0, 0, 0, false };
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
		FillBurstSpendFrame(&burst, true, placed);
		run.framesRan++;
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

	/* The window: exactly FILL_BURST_FRAMES productive frames, not one more. */
	SBurstRun full = RunBurst(0.0f, 1000.0f, FILL_BURST_FRAMES + 10, true);
	Check(full.framesRan == FILL_BURST_FRAMES, "a burst runs exactly the frames of its window");
	Check(full.framesPlaced == FILL_BURST_FRAMES, "every frame of the run placed something");
	Check(full.attempts == FILL_BURST_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME,
		"the window of hidden frames spends the hidden budget of every frame");
	Check(!full.endedByStopLine, "the window can end without the stop line");

	SBurstRun visible = RunBurst(0.0f, 1000.0f, FILL_BURST_FRAMES, false);
	Check(visible.attempts == FILL_BURST_FRAMES * FILL_BURST_ATTEMPTS_PER_FRAME,
		"a window of visible frames spends the visible budget of every frame");
	Check(FILL_BURST_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME > visible.attempts,
		"a hidden window spends more than a visible one");

	/* A frame the gates stop costs nothing: the streaming may hold a burst for
	 * as long as it needs without spending the window, and the wait is not a
	 * stall (the stall bound is what ends a burst that cannot place). */
	SFillBurst waiting;
	FillBurstArm(&waiting);
	for (int i = 0; i < 10 * FILL_BURST_FRAMES + FILL_BURST_STALL_FRAMES; i++)
		FillBurstSpendFrame(&waiting, false, false);
	Check(FillBurstIsRunning(&waiting), "a frame the gates stop does not spend the window");
	Check(waiting.nFrames == FILL_BURST_FRAMES, "the window is untouched by gated frames");
	Check(waiting.nStalls == 0, "a gated frame is not a stall");

	/* A stall is a frame that ran and placed nothing: enough of them in a row
	 * give the burst up, and the window stays whole for a frame that does
	 * place. */
	SFillBurst stalling;
	FillBurstArm(&stalling);
	for (int i = 0; i < FILL_BURST_STALL_FRAMES - 1; i++)
		FillBurstSpendFrame(&stalling, true, false);
	Check(FillBurstIsRunning(&stalling), "a burst survives stalls below the bound");
	Check(stalling.nFrames == FILL_BURST_FRAMES, "a stall does not spend the window");
	FillBurstSpendFrame(&stalling, true, false);
	Check(!FillBurstIsRunning(&stalling), "the stall bound gives the burst up");

	/* A placement resets the stalls: the bound is a run of empty frames. */
	SFillBurst recovering;
	FillBurstArm(&recovering);
	for (int i = 0; i < FILL_BURST_STALL_FRAMES - 1; i++)
		FillBurstSpendFrame(&recovering, true, false);
	FillBurstSpendFrame(&recovering, true, true);
	Check(recovering.nStalls == 0, "a placement resets the stall counter");
	Check(FillBurstIsRunning(&recovering), "a placement keeps the burst alive");
	Check(recovering.nFrames == FILL_BURST_FRAMES - 1, "a placement spends the frame");

	/* A run an area refuses (every frame runs and places nothing) ends at the
	 * stall bound, with the rest of the window unused. */
	SBurstRun refused = RunBurst(0.0f, 1000.0f, 0, true);
	Check(refused.framesRan == FILL_BURST_STALL_FRAMES, "a run that always fails ends at the stall bound");
	Check(refused.attempts == FILL_BURST_STALL_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME,
		"the failing run spends the attempts of its stalled frames");
	Check(!refused.endedByStopLine, "the failing run is not the stop line");

	/* The stop line: 90 % of the local target, and a zero target stops too. */
	Check(!FillBurstTargetReached(89.9f, 100.0f), "89.9 % of the target does not stop the burst");
	Check(FillBurstTargetReached(90.0f, 100.0f), "90 % of the target stops the burst");
	Check(FillBurstTargetReached(0.0f, 0.0f), "a zero target stops the burst");
	Check(FillBurstTargetReached(5.0f, 0.0f), "a run that lost its target stops the burst");

	SBurstRun stopped = RunBurst(900.0f, 1000.0f, FILL_BURST_FRAMES, true);
	Check(stopped.endedByStopLine, "the stop line ends the burst");
	Check(stopped.attempts == 0, "the frame of the stop line spends no attempt");
	Check(stopped.framesRan == 0, "the frame of the stop line does not run");

	printf("  a burst that runs its whole window spends at most %d attempts (%.1f s at 30 fps)\n",
		FILL_BURST_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME, FILL_BURST_FRAMES / 30.0f);
	printf("  counters for the live rig: CPopulation::ms_FillBurst, ms_nFillBurstAttempts, ms_nFillBurstPlaced,\n");
	printf("  ms_fFillBurstTarget; the same four on CCarCtrl\n\n");
}

/* ------------------------------------------------------------ C. the triggers */

static void PrintTriggers(void)
{
	printf("C. The triggers (each one arms both bursts unless it says otherwise)\n\n");
	printf("  boot            CPopulation::m_CountDownToPedsAtStart / CCarCtrl::CountDownToCarsAtStart\n");
	printf("                  expire (Population.cpp, CarCtrl.cpp) and arm the burst of their own population\n");
	printf("  interior exit   CGame::SetCurrArea, the one writer of CGame::currArea (Game.cpp); the script's\n");
	printf("                  COMMAND_SET_AREA_VISIBLE and the restart path call it (Script7.cpp, GameLogic.cpp)\n");
	printf("  cutscene end    CCutsceneMgr::DeleteCutsceneData (CutsceneMgr.cpp), the script's CLEAR_CUTSCENE\n");
	printf("  death, arrest   the restart countdowns of GameLogic.cpp arm both bursts 10 frames into the fade\n");
	printf("  the parked cars come with the car burst (CTheCarGenerators::GenerateEvenIfPlayerIsCloseCounter)\n");
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
