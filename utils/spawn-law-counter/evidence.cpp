/*
 * The evidence program for VICE-166, "save restore: count the law flag
 * independently of the vehicle class".
 *
 * Offline, in three parts:
 *
 *   A. The source read. The transition table comes out of the sources that
 *      own it: CCarCtrl::UpdateCarCount (src/control/CarCtrl.cpp), the
 *      constructor and CVehicle::ChangeLawEnforcerState
 *      (src/vehicles/Vehicle.cpp) and the vehicle restore
 *      (src/core/Pools.cpp, both branches; src/core/config.h selects the
 *      live one, COMPATIBLE_SAVES). The restore's own count is VICE-163's;
 *      the law flag rides it, so the read checks that too.
 *
 *   B. The replay. The counter transitions of the ticket's cases are replayed
 *      from that table: a live law car create/destroy, a mission car flagged
 *      law then destroyed, a quicksave-style restore of a flagged mission car
 *      then destroyed, a restore then ChangeLawEnforcerState(false), a double
 *      toggle. NumLawEnforcerCars must balance (end at zero) and never go
 *      below zero in every case.
 *
 *   C. The mutation guard. The program moves the law count of the parsed body
 *      back inside the RANDOM_VEHICLE case (the pre-fix law placement; the
 *      far early return stays), writes the mutated source under /tmp for
 *      inspection, re-reads the table from it and replays the same cases: at
 *      least one must fail, or the battery does not test the fix. A second
 *      argument reads a whole pre-fix CarCtrl.cpp instead (the base file to
 *      compare against).
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -O2 -I utils/save-vehicle-count -o /tmp/spawn-law-counter-evidence utils/spawn-law-counter/evidence.cpp
 *	/tmp/spawn-law-counter-evidence
 *	/tmp/spawn-law-counter-evidence <source root>			# default "."
 *	/tmp/spawn-law-counter-evidence <source root> <CarCtrl.cpp>	# a pre-fix file
 *
 * The checks read C++ as text, so they rest on these shapes of the files:
 *   - the count's shape comes from utils/save-vehicle-count/CarCountShape.h,
 *     the parser shared with utils/save-vehicle-count/evidence.cpp (VICE-163),
 *     so the two checks cannot drift apart: the law count is the op under the
 *     pVehicle->bIsLawEnforcer guard, the class counters are the ops of their
 *     case, and FAR_TRAFFIC_VEHICLE returns before the count (VICE-128);
 *   - a function body closes with a brace at column 0, and a count op is a
 *     line with "++Num" or "--Num";
 *   - the law statement's own guard is exactly if (pVehicle->bIsLawEnforcer)
 *     and no VehicleCreatedBy gate sits above it in the side;
 *   - the far early return is the first FAR_TRAFFIC_VEHICLE test, two lines,
 *     before any count op;
 *   - the restore's branch table is one #ifdef COMPATIBLE_SAVES, one #else and
 *     the body's last #endif;
 *   - the constructor sets the flag false before its count.
 * A structural edit that does not fit fails the read (a missing function,
 * class, op or counter is reported), never passes silently.
 *
 * Limits, stated honestly:
 *   - The program does not run the game or the compiler: it reads the text of
 *     the sources and replays the transitions those sources describe, so the
 *     engine's arithmetic itself is not executed.
 *   - It models the load branch the config selects (COMPATIBLE_SAVES is
 *     defined in src/core/config.h, so the save branch of LoadVehiclePool is
 *     the live one); the inactive branch is checked for its shape too (the
 *     class and the flag arrive, its own decrements, exactly one recount) but
 *     is not replayed.
 *   - The battery replays the paths the ticket names. It does not replay the
 *     permanent class (the same two lines as the parked one), the far layer's
 *     promotion and demotion (UpdateCarCount runs on RANDOM before the class
 *     becomes FAR_TRAFFIC_VEHICLE, and on RANDOM again at the promotion), or
 *     the replay's pool copy-back (Replay.cpp recounts every restored
 *     vehicle through the same count).
 *   - The live row is utils/caps-audit/live-counters.py, which prints
 *     "cars: NumLawEnforcerCars" (and the other class counters, including
 *     NumCarsInUse) from the running game.
 *   - "The pre-fix shape" of section C is only the pre-fix law placement: the
 *     law statement of each side moves back inside its RANDOM_VEHICLE case,
 *     the far early return stays. The optional second argument compares
 *     against a whole pre-fix CarCtrl.cpp instead.
 *   - The mutated copy of CarCtrl.cpp is the only file written (under /tmp);
 *     a write that fails is a warning on the output, not a silent skip.
 *
 * The exit code is 0 when every check passes.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "CarCountShape.h"

static int g_failures = 0;

static void Fail(const char *fmt, ...)
{
	va_list ap;
	g_failures++;
	printf("FAIL: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* ------------------------------------------------- A. the sources */

enum {
	CLS_RANDOM = 0,
	CLS_MISSION,
	CLS_PARKED,
	CLS_PERMANENT,
	CLS_FAR,
	CLS_COUNT
};

/* The class counter of each class; the far layer holds none (VICE-128). */
static const char *aClassCounter[CLS_COUNT] = {
	"NumRandomCars", "NumMissionCars", "NumParkedCars", "NumPermanentCars", ""
};

struct SRule
{
	bool lawAdd[CLS_COUNT], lawRemove[CLS_COUNT];
	bool clsAdd[CLS_COUNT], clsRemove[CLS_COUNT];
};

static int CountOps(const std::vector<SCountOp> &ops, const char *counter)
{
	int n = 0;
	for (size_t i = 0; i < ops.size(); i++)
		if (ops[i].counter == counter)
			n++;
	return n;
}

static void BuildRule(const SCountShape &shape, SRule &rule)
{
	memset(&rule, 0, sizeof(rule));
	for (int c = 0; c < CLS_COUNT; c++) {
		std::vector<SCountOp> add = EffectiveOps(shape, true, aClassNames[c]);
		std::vector<SCountOp> rem = EffectiveOps(shape, false, aClassNames[c]);
		rule.lawAdd[c] = CountOps(add, "NumLawEnforcerCars") > 0;
		rule.lawRemove[c] = CountOps(rem, "NumLawEnforcerCars") > 0;
		rule.clsAdd[c] = aClassCounter[c][0] != '\0' && CountOps(add, aClassCounter[c]) > 0;
		rule.clsRemove[c] = aClassCounter[c][0] != '\0' && CountOps(rem, aClassCounter[c]) > 0;
	}
}

/* The shape of the count against the VICE-166 rules: one guarded law
 * statement per side, before the switch; the far layer a no-op whose early
 * return precedes the law write; the class counters exactly once per class. */
static void CheckCountShape(const SSource &carCtrl, const SCountShape &shape)
{
	int nCreateLaw = 0, nRemoveLaw = 0;
	int firstPrefixLaw = -1;
	for (size_t i = 0; i < shape.law.size(); i++) {
		const SLawStatement &ls = shape.law[i];
		if (ls.inPrefix && (firstPrefixLaw < 0 || ls.firstIndex < firstPrefixLaw))
			firstPrefixLaw = ls.firstIndex;
		if (ls.create)
			nCreateLaw++;
		else
			nRemoveLaw++;
		if (!LawStatementShapeOk(ls)) {
			if (ls.guardIndex < 0 || !ls.guardExact)
				Fail("the law statement of the %s side is not the guarded if (pVehicle->bIsLawEnforcer) (line %d)",
					ls.create ? "create" : "remove", ls.op.line);
			else
				Fail("a VehicleCreatedBy test sits between the side's start and the law guard (line %d): the law must not be under a class gate",
					ls.op.line);
		}
		if (!ls.inPrefix)
			Fail("the law count of the %s side is not before the switch (line %d)",
				ls.create ? "create" : "remove", ls.op.line);
		if (ls.op.sign != (ls.create ? +1 : -1))
			Fail("the law count of the %s side has the wrong sign (line %d)",
				ls.create ? "create" : "remove", ls.op.line);
		if (!ls.create) {
			bool clamp = false, zero = false;
			for (int j = ls.firstIndex; j >= 0 && j <= ls.lastIndex; j++) {
				if (Contains(carCtrl.lines[j], "NumLawEnforcerCars < 0"))
					clamp = true;
				if (Contains(carCtrl.lines[j], "NumLawEnforcerCars = 0"))
					zero = true;
			}
			if (!clamp || !zero)
				Fail("the remove side lost the clamp of the law count (line %d)", ls.op.line);
		}
	}
	if (nCreateLaw != 1 || nRemoveLaw != 1)
		Fail("the law count is written %d time(s) on the create side and %d on the remove side (one each)",
			nCreateLaw, nRemoveLaw);

	/* M4: the prefix law must sit below the far early return; above it (or
	 * without one), the far layer would count the law and a demoted car
	 * would double-decrement at the destructor. */
	if (firstPrefixLaw >= 0 && shape.farTestIndex < 0)
		Fail("the law count is in the prefix but the far layer has no early return (VICE-128): a far car would count");
	else if (firstPrefixLaw >= 0 && firstPrefixLaw < shape.farTestIndex)
		Fail("the law count is written before the far early return (line %d before %d): a demoted law car would double-decrement at the destructor",
			firstPrefixLaw + 1, shape.farTestIndex + 1);

	for (int c = 0; c < CLS_COUNT; c++) {
		std::vector<SCountOp> add = EffectiveOps(shape, true, aClassNames[c]);
		std::vector<SCountOp> rem = EffectiveOps(shape, false, aClassNames[c]);
		if (c == CLS_FAR) {
			if (!add.empty() || !rem.empty())
				Fail("the far layer moves a counter");
			continue;
		}
		if (CountOps(add, "NumLawEnforcerCars") != 1 || CountOps(rem, "NumLawEnforcerCars") != 1)
			Fail("the law flag of the %s class is not counted exactly once on both sides", aClassNames[c]);
		if (CountOps(add, aClassCounter[c]) != 1 || CountOps(rem, aClassCounter[c]) != 1)
			Fail("the %s counter is not counted exactly once on both sides", aClassCounter[c]);
	}
}

static void PrintCountShape(const SCountShape &shape, const SRule &rule, const char *label)
{
	printf("  %s\n", label);
	printf("    class       the add side              the remove side\n");
	for (int c = 0; c < CLS_COUNT; c++) {
		char a[64], r[64];
		if (c == CLS_FAR) {
			snprintf(a, sizeof(a), "no counter (VICE-128)");
			snprintf(r, sizeof(r), "no counter (VICE-128)");
		} else {
			snprintf(a, sizeof(a), "%s, %s +1", rule.lawAdd[c] ? "law +1" : "-", aClassCounter[c]);
			snprintf(r, sizeof(r), "%s, %s -1", rule.lawRemove[c] ? "law -1 clamped" : "-", aClassCounter[c]);
		}
		printf("    %-20s %-30s %-30s\n", aClassNames[c], a, r);
	}
	printf("    the law flag is counted on: add");
	for (int c = 0; c < CLS_COUNT; c++)
		if (rule.lawAdd[c] && c != CLS_FAR)
			printf(" %s", aClassNames[c]);
	printf("; remove");
	for (int c = 0; c < CLS_COUNT; c++)
		if (rule.lawRemove[c] && c != CLS_FAR)
			printf(" %s", aClassNames[c]);
	printf("\n    the law statements:");
	for (size_t i = 0; i < shape.law.size(); i++)
		printf(" %s (line %d, %s)", shape.law[i].create ? "create" : "remove",
			shape.law[i].op.line, shape.law[i].inPrefix ? "before the switch" : "inside the case");
	printf("\n\n");
}

/* --- the constructor and the toggle --- */

struct SVehicleFacts
{
	bool ctorFlagFalseFirst;
	bool toggleGuarded;
	bool toggleClamp;
};

static bool ParseVehicleFacts(const SSource &src, SVehicleFacts &f)
{
	int begin, end;
	if (!FindBody(src, "CVehicle::CVehicle(uint8 CreatedBy)", begin, end))
		return false;
	int flag = FindInBody(src, begin, end, "bIsLawEnforcer = false;", begin);
	int count = FindInBody(src, begin, end, "CCarCtrl::UpdateCarCount(this, false);", begin);
	f.ctorFlagFalseFirst = flag >= 0 && count >= 0 && flag < count;

	if (!FindBody(src, "CVehicle::ChangeLawEnforcerState", begin, end))
		return false;
	int guardOn = FindInBody(src, begin, end, "if (!bIsLawEnforcer)", begin);
	int setOn = FindInBody(src, begin, end, "bIsLawEnforcer = true;", begin);
	int inc = FindInBody(src, begin, end, "NumLawEnforcerCars++", begin);
	int guardOff = FindInBody(src, begin, end, "if (bIsLawEnforcer)", begin);
	int setOff = FindInBody(src, begin, end, "bIsLawEnforcer = false;", begin);
	int dec = FindInBody(src, begin, end, "NumLawEnforcerCars--", begin);
	f.toggleGuarded = guardOn >= 0 && setOn >= 0 && inc >= 0 && guardOn < setOn && setOn < inc &&
		guardOff >= 0 && setOff >= 0 && dec >= 0 && guardOff < setOff && setOff < dec;
	f.toggleClamp = FindInBody(src, begin, end, "NumLawEnforcerCars < 0", begin) >= 0;
	return true;
}

static void CheckVehicleFacts(const SVehicleFacts &f)
{
	if (!f.ctorFlagFalseFirst)
		Fail("the constructor does not set the flag false before the construction count");
	if (!f.toggleGuarded)
		Fail("ChangeLawEnforcerState is not a guarded toggle (a repeated state would move the count)");
}

static void PrintVehicleFacts(const SVehicleFacts &f)
{
	printf("  the constructor (src/vehicles/Vehicle.cpp)\n");
	printf("    bIsLawEnforcer = false comes before CCarCtrl::UpdateCarCount(this, false):\n");
	printf("    the construction of any class counts no law (the flag is false)\n\n");
	printf("  CVehicle::ChangeLawEnforcerState (src/vehicles/Vehicle.cpp)\n");
	printf("    the toggle is guarded: only a change of the flag moves the count, a repeat is a no-op\n");
	printf("    the decrement has %s: the invariant is \"the flag is set if and only if the car is counted\"\n\n",
		f.toggleClamp ? "its own clamp" : "no clamp of its own");
}

/* --- the restore, both branches --- */

struct SBranchFacts
{
	int ctors, decrements, loads, setClass, setLaw, recounts;
};

struct SPoolFacts
{
	bool compatibleSaves;	/* src/core/config.h defines COMPATIBLE_SAVES */
	SBranchFacts compatible, direct;
	bool countsOnAdd;	/* the active branch recounts the restored car (VICE-163) */
};

static void ScanBranchCounts(const SSource &src, int first, int last, SBranchFacts &b)
{
	memset(&b, 0, sizeof(b));
	for (int i = first; i <= last; i++) {
		const std::string &s = src.lines[i];
		if (Contains(s, "new(slot)") && Contains(s, "RANDOM_VEHICLE"))
			b.ctors++;
		if (Contains(s, "--CCarCtrl::NumRandomCars") || Contains(s, "CCarCtrl::NumRandomCars--"))
			b.decrements++;
		if (Contains(s, "pVehicle->Load(buf)"))
			b.loads++;
		if (Contains(s, "pVehicle->VehicleCreatedBy = pBufferVehicle->VehicleCreatedBy"))
			b.setClass++;
		if (Contains(s, "pVehicle->bIsLawEnforcer = pBufferVehicle->bIsLawEnforcer"))
			b.setLaw++;
		if (Contains(s, "CCarCtrl::UpdateCarCount(pVehicle, false)"))
			b.recounts++;
	}
}

static bool ParsePoolFacts(const SSource &pools, const SSource &config, SPoolFacts &f)
{
	int begin, end, ifdef, split, endif;
	if (!FindBody(pools, "CPools::LoadVehiclePool(uint8* buf, uint32 size)", begin, end))
		return false;
	ifdef = FindInBody(pools, begin, end, "#ifdef COMPATIBLE_SAVES", begin);
	if (ifdef < 0)
		return false;
	split = FindInBody(pools, ifdef, end, "#else", ifdef);
	if (split < 0)
		return false;
	endif = -1;
	for (int i = split; i < end; i++)
		if (Trim(pools.lines[i]) == "#endif")
			endif = i;
	if (endif < 0)
		return false;
	ScanBranchCounts(pools, ifdef + 1, split - 1, f.compatible);
	ScanBranchCounts(pools, split + 1, endif - 1, f.direct);
	f.compatibleSaves = FindLine(config, "#define COMPATIBLE_SAVES", 0) >= 0;
	f.countsOnAdd = (f.compatibleSaves ? f.compatible.recounts : f.direct.recounts) == 1;
	return true;
}

static void CheckPoolFacts(const SPoolFacts &f)
{
	if (f.compatible.ctors != 3 || f.compatible.loads != 1 ||
	    f.compatible.decrements != 1 || f.compatible.recounts != 1)
		Fail("the save branch of the restore is not construct/Load/decrement/recount (3/1/1/1): %d/%d/%d/%d",
			f.compatible.ctors, f.compatible.loads, f.compatible.decrements, f.compatible.recounts);
	if (f.direct.ctors != 3 || f.direct.setClass != 1 || f.direct.setLaw != 1 ||
	    f.direct.decrements != 3 || f.direct.recounts != 1)
		Fail("the direct branch of the restore is not construct/copy/copy/decrement/recount (3/1/1/3/1): %d/%d/%d/%d/%d",
			f.direct.ctors, f.direct.setClass, f.direct.setLaw,
			f.direct.decrements, f.direct.recounts);
	if (!f.countsOnAdd)
		Fail("the active restore branch does not count the restored car (VICE-163)");
}

static void PrintPoolFacts(const SPoolFacts &f)
{
	printf("  the vehicle restore (src/core/Pools.cpp)\n");
	printf("    src/core/config.h %s COMPATIBLE_SAVES: the %s branch of LoadVehiclePool is the active one\n",
		f.compatibleSaves ? "defines" : "does not define",
		f.compatibleSaves ? "save" : "direct");
	printf("    the save branch: %d constructions with RANDOM_VEHICLE, Load carries the class and the law\n",
		f.compatible.ctors);
	printf("      flag, %d NumRandomCars decrement, %d recount\n",
		f.compatible.decrements, f.compatible.recounts);
	printf("    the direct branch: %d constructions, %d class copy, %d law flag copy, %d decrements, %d recount\n",
		f.direct.ctors, f.direct.setClass, f.direct.setLaw, f.direct.decrements, f.direct.recounts);
	printf("    the active branch counts the restored car once (VICE-163); the law flag of the save rides\n");
	printf("    that count (VICE-166)%s\n\n", f.countsOnAdd ? "" : "  <- MISSING");
}

/* ---------------------------------------------------------------- B. the cases */

struct SVehicleState
{
	int cls;
	bool law;
};

struct SCase
{
	const char *name;
	std::string detail;
	bool ok;
	bool failed;
	int counter[CLS_COUNT];	/* the class counters; the far class holds none */
	int law;
	int minLaw;
	std::vector<SVehicleState> cars;
	const SRule *rule;
	bool countsOnAdd;	/* the active restore branch calls UpdateCarCount */

	void Init(const char *n, const SRule *r, bool poolCountsOnAdd)
	{
		name = n;
		ok = false;
		failed = false;
		memset(counter, 0, sizeof(counter));
		law = 0;
		minLaw = 0;
		cars.clear();
		detail.clear();
		rule = r;
		countsOnAdd = poolCountsOnAdd;
	}

	void Finish()
	{
		if (minLaw < 0) {
			failed = true;
			Note("the law count reached %d", minLaw);
		}
		if (law != 0) {
			failed = true;
			Note("the final law count is %d", law);
		}
		for (int c = 0; c < CLS_COUNT - 1; c++) {
			if (counter[c] != 0) {
				failed = true;
				Note("the final %s count is %d", aClassNames[c], counter[c]);
			}
		}
		ok = !failed;
	}

	void Note(const char *fmt, ...)
	{
		char buf[256];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		if (!detail.empty())
			detail += ", ";
		detail += buf;
	}

	void Step(const char *fmt, ...)
	{
		char buf[256];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		char full[320];
		snprintf(full, sizeof(full), "%s law=%d", buf, law);
		Note("%s", full);
	}

	void LawIs(int want)
	{
		if (law != want) {
			failed = true;
			Note("the law count is %d, wanted %d", law, want);
		}
	}

	int Create(int cls)
	{
		SVehicleState v;
		v.cls = cls;
		v.law = false;
		cars.push_back(v);
		if (rule->clsAdd[cls])
			counter[cls]++;
		/* the construction never adds law: the flag is false before the count */
		Step("create %s (the flag is false)", aClassNames[cls]);
		return (int)cars.size() - 1;
	}

	void SetLaw(int id, bool on)
	{
		if (cars[id].law == on) {
			Step("set law(%s) is a guarded no-op", on ? "true" : "false");
			return;
		}
		cars[id].law = on;
		law += on ? 1 : -1;
		if (law < minLaw)
			minLaw = law;
		if (law < 0) {
			failed = true;
			Note("ChangeLawEnforcerState(false) took the law count below zero");
		}
		Step("set law(%s)", on ? "true" : "false");
	}

	void Destroy(int id)
	{
		int cls = cars[id].cls;
		if (rule->clsRemove[cls]) {
			if (--counter[cls] < 0)
				counter[cls] = 0;
		}
		if (cars[id].law && rule->lawRemove[cls]) {
			law--;
			if (law < minLaw)
				minLaw = law;
			if (law < 0) {
				law = 0;	/* the source's clamp on the remove side */
				failed = true;
				Note("the remove side went below zero and clamped");
			}
		}
		Step("destroy %s", aClassNames[cls]);
	}

	void BlowUp(int id)
	{
		Step("blow up: ChangeLawEnforcerState(false) first");
		SetLaw(id, false);
		Destroy(id);
	}

	/* The quicksave-style restore: a RANDOM construction, its count given
	 * back, then the class and the flag of the save; the active branch of
	 * LoadVehiclePool decides whether the restored car is counted. */
	int Restore(int cls, bool lawFlag)
	{
		SVehicleState v;
		v.cls = cls;
		v.law = lawFlag;
		cars.push_back(v);
		int id = (int)cars.size() - 1;
		if (rule->clsAdd[CLS_RANDOM])
			counter[CLS_RANDOM]++;
		counter[CLS_RANDOM]--;		/* --CCarCtrl::NumRandomCars */
		if (countsOnAdd) {
			if (rule->clsAdd[cls])
				counter[cls]++;
			if (lawFlag && rule->lawAdd[cls])
				law++;
		} else {
			failed = true;
			Note("the restore counts nothing: the active branch has no UpdateCarCount");
		}
		Step("restore %s (the save carries law %s)", aClassNames[cls], lawFlag ? "true" : "false");
		return id;
	}
};

static void AddCase(std::vector<SCase> &out, SCase c)
{
	c.Finish();
	out.push_back(c);
}

static void RunBattery(const SRule &rule, bool countsOnAdd, std::vector<SCase> &out)
{
	{	/* 1. the control: a live law car of the random class */
		SCase c;
		c.Init("a live law car (random): create, flag true, destroy", &rule, countsOnAdd);
		int v = c.Create(CLS_RANDOM);
		c.LawIs(0);
		c.SetLaw(v, true);
		c.LawIs(1);
		c.Destroy(v);
		c.LawIs(0);
		AddCase(out, c);
	}
	{	/* 2. the same car through the blow-up path */
		SCase c;
		c.Init("a live law car (random): create, flag true, blow up", &rule, countsOnAdd);
		int v = c.Create(CLS_RANDOM);
		c.SetLaw(v, true);
		c.LawIs(1);
		c.BlowUp(v);
		c.LawIs(0);
		AddCase(out, c);
	}
	{	/* 3. a mission car that takes the flag: a cop driver entering one
		 * (Ped.cpp), or the save restoring one. The SetPieces cop car is
		 * RANDOM, not mission. */
		SCase c;
		c.Init("a mission car flagged law then destroyed", &rule, countsOnAdd);
		int v = c.Create(CLS_MISSION);
		c.LawIs(0);
		c.SetLaw(v, true);
		c.LawIs(1);
		c.Destroy(v);
		c.LawIs(0);
		AddCase(out, c);
	}
	{	/* 4. the same, blown up (ChangeLawEnforcerState(false) first) */
		SCase c;
		c.Init("a mission car flagged law then blown up", &rule, countsOnAdd);
		int v = c.Create(CLS_MISSION);
		c.SetLaw(v, true);
		c.LawIs(1);
		c.BlowUp(v);
		c.LawIs(0);
		AddCase(out, c);
	}
	{	/* 5. a parked car that takes the flag (the cop hand-over, Ped.cpp) */
		SCase c;
		c.Init("a parked car flagged law then destroyed", &rule, countsOnAdd);
		int v = c.Create(CLS_PARKED);
		c.SetLaw(v, true);
		c.LawIs(1);
		c.Destroy(v);
		c.LawIs(0);
		AddCase(out, c);
	}
	{	/* 6. the quicksave restore of a flagged mission car */
		SCase c;
		c.Init("a quicksave-style restore of a flagged mission car then destroyed", &rule, countsOnAdd);
		int v = c.Restore(CLS_MISSION, true);
		c.LawIs(1);
		c.Destroy(v);
		c.LawIs(0);
		AddCase(out, c);
	}
	{	/* 7. that restored car blown up */
		SCase c;
		c.Init("a quicksave-style restore of a flagged mission car then blown up", &rule, countsOnAdd);
		int v = c.Restore(CLS_MISSION, true);
		c.LawIs(1);
		c.BlowUp(v);
		c.LawIs(0);
		AddCase(out, c);
	}
	{	/* 8. the restored car loses the flag (the cop leaves it) */
		SCase c;
		c.Init("a restore then ChangeLawEnforcerState(false)", &rule, countsOnAdd);
		int v = c.Restore(CLS_MISSION, true);
		c.LawIs(1);
		c.SetLaw(v, false);
		c.LawIs(0);
		c.Destroy(v);
		c.LawIs(0);
		AddCase(out, c);
	}
	{	/* 9. a double toggle on the random class */
		SCase c;
		c.Init("a double toggle (random)", &rule, countsOnAdd);
		int v = c.Create(CLS_RANDOM);
		c.SetLaw(v, true);
		c.LawIs(1);
		c.SetLaw(v, true);
		c.LawIs(1);
		c.SetLaw(v, false);
		c.LawIs(0);
		c.SetLaw(v, false);
		c.LawIs(0);
		c.Destroy(v);
		c.LawIs(0);
		AddCase(out, c);
	}
	{	/* 10. a double toggle on the mission class */
		SCase c;
		c.Init("a double toggle (mission)", &rule, countsOnAdd);
		int v = c.Create(CLS_MISSION);
		c.SetLaw(v, true);
		c.LawIs(1);
		c.SetLaw(v, true);
		c.LawIs(1);
		c.SetLaw(v, false);
		c.LawIs(0);
		c.SetLaw(v, false);
		c.LawIs(0);
		c.Destroy(v);
		c.LawIs(0);
		AddCase(out, c);
	}
}

static int CountFailed(const std::vector<SCase> &cases)
{
	int n = 0;
	for (size_t i = 0; i < cases.size(); i++)
		if (!cases[i].ok)
			n++;
	return n;
}

static void PrintCases(const std::vector<SCase> &cases, bool onlyFailed)
{
	for (size_t i = 0; i < cases.size(); i++) {
		const SCase &c = cases[i];
		if (onlyFailed && c.ok)
			continue;
		printf("  %s\n", c.name);
		printf("    %s\n", c.detail.c_str());
		printf("    -> %s\n", c.ok ? "PASS" : "FAIL");
	}
}

static const SCase *FindCase(const std::vector<SCase> &cases, const char *name)
{
	for (size_t i = 0; i < cases.size(); i++)
		if (strcmp(cases[i].name, name) == 0)
			return &cases[i];
	return NULL;
}

/* --------------------------------------------------------- C. the mutation */

/* The pre-fix law placement: each law statement moves from its side's prefix
 * into the RANDOM_VEHICLE case of the same side. The far early return stays:
 * the mutation is the law placement alone. */
static bool MutateLawToRandomCase(const SSource &src, const SCountShape &shape, SSource &out)
{
	out.lines = src.lines;
	for (int i = (int)shape.law.size() - 1; i >= 0; i--) {
		const SLawStatement &ls = shape.law[i];
		if (ls.firstIndex < 0 || ls.lastIndex < ls.firstIndex)
			return false;
		const SClassOps *random = FindClass(ls.create ? shape.create : shape.remove, "RANDOM_VEHICLE");
		if (!random)
			return false;
		std::vector<std::string> stmt;
		for (int j = ls.firstIndex; j <= ls.lastIndex; j++)
			stmt.push_back("\t" + out.lines[j]);
		int count = ls.lastIndex - ls.firstIndex + 1;
		out.lines.erase(out.lines.begin() + ls.firstIndex, out.lines.begin() + ls.lastIndex + 1);
		int at = random->index + 1;
		if (ls.firstIndex < at)
			at -= count;
		out.lines.insert(out.lines.begin() + at, stmt.begin(), stmt.end());
	}
	return true;
}

static void WriteLines(const SSource &src, const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		printf("  warning: the mutated source cannot be written to %s\n", path);
		return;
	}
	for (size_t i = 0; i < src.lines.size(); i++) {
		fputs(src.lines[i].c_str(), f);
		fputc('\n', f);
	}
	fclose(f);
	printf("  the mutated source is written to %s\n", path);
}

static void RunMutationGuard(const SSource &src, const SCountShape &shape, bool countsOnAdd, int argc, char **argv)
{
	printf("== C. the mutation guard: the law count moved back inside the RANDOM case\n\n");

	SSource mutant;
	if (argc > 2) {
		if (!ReadSource(argv[2], mutant)) {
			Fail("the pre-fix source %s cannot be read", argv[2]);
			return;
		}
		printf("  the compared source: %s\n", argv[2]);
	} else {
		if (!MutateLawToRandomCase(src, shape, mutant)) {
			Fail("the mutation of the parsed body failed");
			return;
		}
		printf("  the mutation: each law statement moved into its side's RANDOM_VEHICLE case;\n");
		printf("  the far early return stays (the pre-fix law placement alone)\n");
		WriteLines(mutant, "/tmp/spawn-law-counter-mutated.cpp");
	}
	printf("\n");

	SCountShape mutShape;
	if (!ParseUpdateCarCount(mutant, mutShape)) {
		Fail("the mutated source does not hold the count shape");
		return;
	}
	SRule mutRule;
	BuildRule(mutShape, mutRule);
	PrintCountShape(mutShape, mutRule, "CCarCtrl::UpdateCarCount (the mutated text)");

	std::vector<SCase> mutated;
	RunBattery(mutRule, countsOnAdd, mutated);
	PrintCases(mutated, true);
	int failed = CountFailed(mutated);
	const SCase *mission = FindCase(mutated, "a mission car flagged law then destroyed");
	const SCase *restore = FindCase(mutated, "a restore then ChangeLawEnforcerState(false)");
	if (failed == 0)
		Fail("the mutation guard: the pre-fix shape passed the battery");
	else if (!mission || mission->ok || !restore || restore->ok)
		Fail("the mutation guard: the pre-fix shape misses the leak or the negative it must show");
	else
		printf("\n  %d of %d case(s) fail under the mutation: the battery tests the fix\n\n",
			failed, (int)mutated.size());
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	std::string root = (argc > 1) ? argv[1] : ".";

	printf("reVC the spawn law counter (VICE-166): the law flag counted apart from the class\n");
	printf("the source root is \"%s\"\n\n", root.c_str());

	printf("== A. the source read\n\n");

	SSource carCtrl, vehicleSrc, pools, config;
	if (!ReadSource((root + "/src/control/CarCtrl.cpp").c_str(), carCtrl)) {
		Fail("cannot read src/control/CarCtrl.cpp (run the program from the repository root)");
		return 1;
	}
	if (!ReadSource((root + "/src/vehicles/Vehicle.cpp").c_str(), vehicleSrc) ||
	    !ReadSource((root + "/src/core/Pools.cpp").c_str(), pools) ||
	    !ReadSource((root + "/src/core/config.h").c_str(), config)) {
		Fail("cannot read src/vehicles/Vehicle.cpp, src/core/Pools.cpp or src/core/config.h");
		return 1;
	}
	SCountShape shape;
	if (!ParseUpdateCarCount(carCtrl, shape)) {
		Fail("CCarCtrl::UpdateCarCount is not in src/control/CarCtrl.cpp");
		return 1;
	}
	CheckCountShape(carCtrl, shape);
	SRule rule;
	BuildRule(shape, rule);
	PrintCountShape(shape, rule, "CCarCtrl::UpdateCarCount (src/control/CarCtrl.cpp): the current sources");

	SVehicleFacts vehicleFacts;
	if (!ParseVehicleFacts(vehicleSrc, vehicleFacts)) {
		Fail("the constructor or ChangeLawEnforcerState is not in src/vehicles/Vehicle.cpp");
		return 1;
	}
	CheckVehicleFacts(vehicleFacts);
	PrintVehicleFacts(vehicleFacts);

	SPoolFacts poolFacts;
	if (!ParsePoolFacts(pools, config, poolFacts)) {
		Fail("LoadVehiclePool or its branch table is not in src/core/Pools.cpp");
		return 1;
	}
	CheckPoolFacts(poolFacts);
	PrintPoolFacts(poolFacts);

	printf("== B. the replay: NumLawEnforcerCars must balance in every case\n\n");
	std::vector<SCase> clean;
	RunBattery(rule, poolFacts.countsOnAdd, clean);
	PrintCases(clean, false);
	for (size_t i = 0; i < clean.size(); i++)
		if (!clean[i].ok)
			Fail("the case \"%s\" does not balance", clean[i].name);
	printf("\n  %d case(s) of the current sources: %s\n\n", (int)clean.size(),
		g_failures == 0 ? "all balance" : "see the failures above");

	RunMutationGuard(carCtrl, shape, poolFacts.countsOnAdd, argc, argv);

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  the law count follows the flag on both sides of UpdateCarCount for every\n");
		printf("  class, the far layer moves nothing, the restore counts the car once, every\n");
		printf("  replayed case balances above zero, and the pre-fix shape fails the battery.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
