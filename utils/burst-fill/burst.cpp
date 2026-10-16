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
	printf("  the give-up bound:           %d frames in a row without a placement (%.1f s)\n\n",
		FILL_BURST_STALL_FRAMES, FILL_BURST_STALL_FRAMES / 30.0f);

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
 * something and the attempts the caller would have spent. */
struct SBurstRun
{
	int framesSpent;
	int framesPlaced;
	int attempts;
	bool endedByStopLine;
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
	 * bounded, and the window is still there when the models arrive. */
	SFillBurst waiting;
	FillBurstArm(&waiting);
	for (int i = 0; i < FILL_BURST_STALL_FRAMES - 1; i++)
		FillBurstSpendFrame(&waiting, false, false);
	Check(FillBurstIsRunning(&waiting), "a burst survives a wait below the bound");
	Check(waiting.nFrames == FILL_BURST_FRAMES, "a frame that spends no attempt leaves the window whole");
	Check(waiting.nStalls == FILL_BURST_STALL_FRAMES - 1, "the waiting frames are counted");
	FillBurstSpendFrame(&waiting, false, false);
	Check(!FillBurstIsRunning(&waiting), "the wait bound ends the burst");

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

	/* The stop line: 90 % of the local target, and a zero target stops too. */
	Check(!FillBurstTargetReached(89.9f, 100.0f), "89.9 % of the target does not stop the burst");
	Check(FillBurstTargetReached(90.0f, 100.0f), "90 % of the target stops the burst");
	Check(FillBurstTargetReached(0.0f, 0.0f), "a zero target stops the burst");
	Check(FillBurstTargetReached(5.0f, 0.0f), "a run that lost its target stops the burst");

	SBurstRun stopped = RunBurst(900.0f, 1000.0f, FILL_BURST_FRAMES, true);
	Check(stopped.endedByStopLine, "the stop line ends the burst");
	Check(stopped.attempts == 0, "the frame of the stop line spends no attempt");
	Check(stopped.framesSpent == 0, "the frame of the stop line does not run");

	printf("  the contract: a burst asks for at most %d attempts (%.1f s of hidden frames at 30 fps),\n",
		FILL_BURST_FRAMES * FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME, FILL_BURST_FRAMES / 30.0f);
	printf("  whatever the gates do; the wall clock of a run is the frames it spends plus its waits\n");
	printf("  counters for the live rig: CPopulation::ms_FillBurst, ms_nFillBurstAttempts, ms_nFillBurstPlaced,\n");
	printf("  ms_fFillBurstTarget; the same four on CCarCtrl\n\n");
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
