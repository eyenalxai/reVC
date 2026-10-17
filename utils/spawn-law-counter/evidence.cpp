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
 *      (src/core/Pools.cpp, the branch src/core/config.h selects). The
 *      restore's own count is VICE-163's; the law flag rides it, so the read
 *      checks that too.
 *
 *   B. The replay. The counter transitions of the ticket's cases are replayed
 *      from that table: a live law car create/destroy, a mission car flagged
 *      law then destroyed, a quicksave-style restore of a flagged mission car
 *      then destroyed, a restore then ChangeLawEnforcerState(false), a double
 *      toggle. NumLawEnforcerCars must balance (end at zero) and never go
 *      below zero in every case.
 *
 *   C. The mutation guard. The program moves the law count of the parsed body
 *      back inside the RANDOM_VEHICLE case (the pre-fix shape), writes the
 *      mutated source under /tmp for inspection, reads the table out of the
 *      mutated text and replays the same cases: at least one must fail, or the
 *      battery does not test the fix.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -O2 -o /tmp/spawn-law-counter-evidence utils/spawn-law-counter/evidence.cpp
 *	/tmp/spawn-law-counter-evidence
 *	/tmp/spawn-law-counter-evidence <source root>	# default "."
 *
 * Limits, stated honestly:
 *   - The program does not run the game or the compiler: it reads the text of
 *     the sources and replays the transitions those sources describe, so the
 *     engine's arithmetic itself is not executed.
 *   - It models the load branch the config selects (COMPATIBLE_SAVES is
 *     defined in src/core/config.h, so the save branch of LoadVehiclePool is
 *     the active one); the other branch is not replayed.
 *   - The cases are the ones the ticket names. The paths that touch the same
 *     counters through the same UpdateCarCount (the far layer's promotion and
 *     demotion, the replay's pool copy-back) are not replayed; the Ped.cpp
 *     hand-over of a parked car changes the class by hand and sets the flag
 *     afterwards through ChangeLawEnforcerState.
 *   - The mutation rewrites the parsed UpdateCarCount body only.
 *
 * The exit code is 0 when every check passes.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

enum {
	CLS_RANDOM = 0,
	CLS_MISSION,
	CLS_PARKED,
	CLS_PERMANENT,
	CLS_FAR,
	CLS_COUNT
};

static const char *aClassCase[CLS_COUNT] = {
	"case RANDOM_VEHICLE:",
	"case MISSION_VEHICLE:",
	"case PARKED_VEHICLE:",
	"case PERMANENT_VEHICLE:",
	"case FAR_TRAFFIC_VEHICLE:",
};

/* The class counter of each class; the far layer holds none (VICE-128). */
static const char *aClassCounter[CLS_COUNT] = {
	"NumRandomCars",
	"NumMissionCars",
	"NumParkedCars",
	"NumPermanentCars",
	"",
};

static const char *aClassName[CLS_COUNT] = {
	"random", "mission", "parked", "permanent", "far",
};

static int g_failures;

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

/* ------------------------------------------------------------------ text */

static bool ReadFileText(const std::string &path, std::string &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	fclose(f);
	return true;
}

/* The text of a function: from its signature to the first line that closes a
 * brace at column 0 (the functions here hold no such brace of their own). */
static bool ExtractFunction(const std::string &text, const char *signature, std::string &out)
{
	size_t start = text.find(signature);
	if (start == std::string::npos)
		return false;
	size_t end = text.find("\n}\n", start);
	if (end == std::string::npos)
		return false;
	out = text.substr(start, end - start + 3);
	return true;
}

static std::string IndentByOneTab(const std::string &s)
{
	std::string out = "\t";
	for (size_t i = 0; i < s.size(); i++) {
		out += s[i];
		if (s[i] == '\n' && i + 1 < s.size())
			out += '\t';
	}
	return out;
}

/* ------------------------------------------------- A. the source read */

/* The counter facts of one side of UpdateCarCount (the remove half or the add
 * half, split at the if/else of the function). */
struct SHalfFacts
{
	bool lawInPrefix;	/* the law counter is written before the switch */
	bool lawIncrement;	/* ...with ++ anywhere in the half */
	bool lawDecrement;	/* ...with -- anywhere in the half */
	bool lawClamp;		/* ...and the -- clamps at zero */
	bool hasFarCase;	/* the half holds a FAR_TRAFFIC_VEHICLE case */
	bool caseFarNoOp;	/* that case returns without touching a counter */
	bool caseLaw[CLS_COUNT];		/* the case block writes the law counter */
	bool caseClassCounter[CLS_COUNT];	/* ...or the class's own counter */
};

struct SRule
{
	bool lawAdd[CLS_COUNT];
	bool lawRemove[CLS_COUNT];
	bool clsAdd[CLS_COUNT];
	bool clsRemove[CLS_COUNT];
};

static bool SplitHalves(const std::string &fn, std::string &removeHalf, std::string &addHalf)
{
	size_t r = fn.find("if (remove){");
	if (r == std::string::npos)
		r = fn.find("if (remove) {");
	if (r == std::string::npos)
		return false;
	size_t e = fn.find("else{", r);
	if (e == std::string::npos)
		e = fn.find("else {", r);
	if (e == std::string::npos)
		return false;
	removeHalf = fn.substr(r, e - r);
	addHalf = fn.substr(e);
	return true;
}

static void ParseHalf(const std::string &half, SHalfFacts &f)
{
	memset(&f, 0, sizeof(f));
	f.lawIncrement = half.find("++NumLawEnforcerCars") != std::string::npos;
	f.lawDecrement = half.find("--NumLawEnforcerCars") != std::string::npos;
	f.lawClamp = half.find("NumLawEnforcerCars < 0") != std::string::npos;

	size_t pos[CLS_COUNT];
	size_t firstCase = std::string::npos;
	for (int c = 0; c < CLS_COUNT; c++) {
		pos[c] = half.find(aClassCase[c]);
		if (pos[c] != std::string::npos && pos[c] < firstCase)
			firstCase = pos[c];
	}
	size_t lawPos = half.find("NumLawEnforcerCars");
	f.lawInPrefix = lawPos != std::string::npos && (firstCase == std::string::npos || lawPos < firstCase);

	for (int c = 0; c < CLS_COUNT; c++) {
		if (pos[c] == std::string::npos)
			continue;
		size_t end = half.size();
		for (int d = 0; d < CLS_COUNT; d++)
			if (pos[d] != std::string::npos && pos[d] > pos[c] && pos[d] < end)
				end = pos[d];
		std::string block = half.substr(pos[c], end - pos[c]);
		f.caseLaw[c] = block.find("NumLawEnforcerCars") != std::string::npos;
		f.caseClassCounter[c] = aClassCounter[c][0] != '\0' &&
			block.find(aClassCounter[c]) != std::string::npos;
		if (c == CLS_FAR) {
			f.hasFarCase = true;
			f.caseFarNoOp = block.find("return") != std::string::npos &&
				!f.caseLaw[c] && !f.caseClassCounter[c];
		}
	}
}

/* The function's first FAR_TRAFFIC_VEHICLE test that returns before the first
 * law write (the fixed shape): that one exempts the far layer from the law
 * count on both sides. */
static bool FarIsAnEarlyReturn(const std::string &fn)
{
	size_t far = fn.find("FAR_TRAFFIC_VEHICLE");
	size_t law = fn.find("NumLawEnforcerCars");
	if (far == std::string::npos || law == std::string::npos || far > law)
		return false;
	size_t ret = fn.find("return", far);
	return ret != std::string::npos && ret < law;
}

static bool LawCounted(const SHalfFacts &f, int cls, bool farExempt)
{
	if (cls == CLS_FAR) {
		if (farExempt)
			return false;			/* the early return of the fixed shape */
		if (f.hasFarCase && !f.caseLaw[cls])
			return false;			/* the old-style case returns before its counters */
	}
	if (f.caseLaw[cls])
		return true;
	return f.lawInPrefix;
}

static void BuildRule(const SHalfFacts &rem, const SHalfFacts &add, bool farExempt, SRule &rule)
{
	for (int c = 0; c < CLS_COUNT; c++) {
		rule.lawRemove[c] = LawCounted(rem, c, farExempt);
		rule.lawAdd[c] = LawCounted(add, c, farExempt);
		rule.clsRemove[c] = rem.caseClassCounter[c];
		rule.clsAdd[c] = add.caseClassCounter[c];
	}
}

static void CheckUpdateCarCount(const SHalfFacts &rem, const SHalfFacts &add, bool farExempt, const SRule &rule)
{
	if (!rem.lawDecrement || rem.lawIncrement)
		Fail("the remove side does not decrement the law count");
	if (!add.lawIncrement || add.lawDecrement)
		Fail("the add side does not increment the law count");
	if (!rem.lawClamp)
		Fail("the remove side lost the clamp of the law count");
	if (!farExempt && !(rem.hasFarCase && rem.caseFarNoOp && add.hasFarCase && add.caseFarNoOp))
		Fail("the far layer is not a no-op on both sides");

	for (int c = 0; c < CLS_COUNT; c++) {
		if (c == CLS_FAR) {
			if (rule.lawAdd[c] || rule.lawRemove[c] || rule.clsAdd[c] || rule.clsRemove[c])
				Fail("the far layer moves a counter");
			continue;
		}
		if (!rule.lawAdd[c] || !rule.lawRemove[c])
			Fail("the law flag of the %s class is not counted on both sides", aClassName[c]);
		if (!rule.clsAdd[c] || !rule.clsRemove[c])
			Fail("the %s class counter is not counted on both sides", aClassName[c]);
		if (rem.caseLaw[c] && rem.lawInPrefix)
			Fail("the remove side writes the law count twice (the switch and the %s case)", aClassName[c]);
		if (add.caseLaw[c] && add.lawInPrefix)
			Fail("the add side writes the law count twice (the switch and the %s case)", aClassName[c]);
	}
}

static void PrintUpdateCarCount(const SRule &rule, const char *label)
{
	printf("  %s\n", label);
	printf("    class       the add side              the remove side\n");
	for (int c = 0; c < CLS_COUNT; c++) {
		char a[64], r[64];
		if (c == CLS_FAR) {
			snprintf(a, sizeof(a), "no counter (VICE-128)");
			snprintf(r, sizeof(r), "no counter (VICE-128)");
		} else {
			snprintf(a, sizeof(a), "%s, class +1", rule.lawAdd[c] ? "law +1" : "-");
			snprintf(r, sizeof(r), "%s, class -1", rule.lawRemove[c] ? "law -1 clamped" : "-");
		}
		printf("    %-10s  %-24s %-24s\n", aClassName[c], a, r);
	}
	printf("    the law flag is counted on: add");
	for (int c = 0; c < CLS_COUNT; c++)
		if (rule.lawAdd[c] && c != CLS_FAR)
			printf(" %s", aClassName[c]);
	printf("; remove");
	for (int c = 0; c < CLS_COUNT; c++)
		if (rule.lawRemove[c] && c != CLS_FAR)
			printf(" %s", aClassName[c]);
	printf("\n\n");
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
				Note("the final %s count is %d", aClassName[c], counter[c]);
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
		Step("create %s (the flag is false)", aClassName[cls]);
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
		Step("destroy %s", aClassName[cls]);
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
		Step("restore %s (the save carries law %s)", aClassName[cls], lawFlag ? "true" : "false");
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
	{	/* 3. a mission car that carries the flag (SetPieces, a cop driver) */
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
	{	/* 5. a parked car that carries the flag */
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

/* ---------------------------------------------------------- the other sources */

struct SVehicleFacts
{
	bool ctorFlagFalseFirst;
	bool toggleGuarded;
	bool toggleClamp;
};

static bool ParseVehicleFacts(const std::string &text, SVehicleFacts &f)
{
	std::string ctor, toggle;
	if (!ExtractFunction(text, "CVehicle::CVehicle(uint8 CreatedBy)", ctor))
		return false;
	size_t flag = ctor.find("bIsLawEnforcer = false;");
	size_t count = ctor.find("CCarCtrl::UpdateCarCount(this, false);");
	f.ctorFlagFalseFirst = flag != std::string::npos && count != std::string::npos && flag < count;

	if (!ExtractFunction(text, "CVehicle::ChangeLawEnforcerState", toggle))
		return false;
	size_t guardOn = toggle.find("if (!bIsLawEnforcer)");
	size_t setOn = toggle.find("bIsLawEnforcer = true;");
	size_t inc = toggle.find("NumLawEnforcerCars++");
	size_t guardOff = toggle.find("if (bIsLawEnforcer)");
	size_t setOff = toggle.find("bIsLawEnforcer = false;");
	size_t dec = toggle.find("NumLawEnforcerCars--");
	f.toggleGuarded = guardOn != std::string::npos && setOn != std::string::npos &&
		inc != std::string::npos && guardOn < setOn && setOn < inc &&
		guardOff != std::string::npos && setOff != std::string::npos &&
		dec != std::string::npos && guardOff < setOff && setOff < dec;
	f.toggleClamp = toggle.find("NumLawEnforcerCars < 0") != std::string::npos;
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

struct SPoolFacts
{
	bool compatibleSaves;	/* src/core/config.h defines COMPATIBLE_SAVES */
	bool loadsFlag;		/* the saved class and flag come back */
	bool cancelsRandom;	/* the construction's count is given back */
	bool countsOnAdd;	/* the active branch calls UpdateCarCount(pVehicle, false) */
};

static bool ParsePoolFacts(const std::string &config, const std::string &pools, SPoolFacts &f)
{
	f.compatibleSaves = config.find("#define COMPATIBLE_SAVES") != std::string::npos;

	std::string fn;
	if (!ExtractFunction(pools, "CPools::LoadVehiclePool", fn))
		return false;
	size_t ifdef = fn.find("#ifdef COMPATIBLE_SAVES");
	if (ifdef == std::string::npos)
		return false;
	size_t els = fn.find("\n#else\n", ifdef);
	if (els == std::string::npos)
		return false;
	std::string active = f.compatibleSaves ? fn.substr(ifdef, els - ifdef) : fn.substr(els);
	f.loadsFlag = active.find("Load(buf)") != std::string::npos ||
		active.find("bIsLawEnforcer = pBufferVehicle->bIsLawEnforcer") != std::string::npos;
	f.cancelsRandom = active.find("--CCarCtrl::NumRandomCars") != std::string::npos;
	f.countsOnAdd = active.find("UpdateCarCount(pVehicle, false)") != std::string::npos;
	return true;
}

static void CheckPoolFacts(const SPoolFacts &f)
{
	if (!f.loadsFlag)
		Fail("the restore does not carry the class and the law flag of the save");
	if (!f.cancelsRandom)
		Fail("the restore does not give the construction's random count back");
	if (!f.countsOnAdd)
		Fail("the active restore branch does not count the restored car (class and law)");
}

static void PrintPoolFacts(const SPoolFacts &f)
{
	printf("  the vehicle restore (src/core/Pools.cpp)\n");
	printf("    src/core/config.h %s COMPATIBLE_SAVES: %s branch of LoadVehiclePool is the active one\n",
		f.compatibleSaves ? "defines" : "does not define",
		f.compatibleSaves ? "the save" : "the raw");
	printf("    the class and the law flag come from the save (Load)\n");
	printf("    the construction's random count is given back (--NumRandomCars)\n");
	printf("    the restored car is counted once (UpdateCarCount(pVehicle, false), the VICE-163 count)%s\n\n",
		f.countsOnAdd ? "" : "  <- MISSING");
}

/* --------------------------------------------------------- C. the mutation */

/* The law statement of a half: the if without braces on the add side, the if
 * with its block on the remove side. */
static bool LawStatement(const std::string &half, size_t &from, size_t &to)
{
	size_t p = half.find("if (pVehicle->bIsLawEnforcer)");
	if (p == std::string::npos)
		return false;
	size_t q = half.find(')', p);
	if (q == std::string::npos)
		return false;
	size_t brace = q + 1;
	while (brace < half.size() && (half[brace] == ' ' || half[brace] == '\t' || half[brace] == '\r' || half[brace] == '\n'))
		brace++;
	if (brace < half.size() && half[brace] == '{') {
		int depth = 0;
		size_t i = brace;
		for (; i < half.size(); i++) {
			if (half[i] == '{')
				depth++;
			else if (half[i] == '}') {
				depth--;
				if (depth == 0) {
					i++;
					break;
				}
			}
		}
		if (depth != 0)
			return false;
		to = i;
	} else {
		size_t semi = half.find(';', q);
		if (semi == std::string::npos)
			return false;
		to = semi + 1;
	}
	from = p;
	return true;
}

/* Move the half's law statement into its RANDOM_VEHICLE case (the pre-fix
 * shape): out of the prefix, one tab deeper under the case label. */
static bool MutateHalf(const std::string &half, std::string &out)
{
	size_t from, to;
	if (!LawStatement(half, from, to))
		return false;
	std::string stmt = half.substr(from, to - from);
	std::string rest = half.substr(0, from) + half.substr(to);
	size_t label = rest.find("case RANDOM_VEHICLE:");
	if (label == std::string::npos)
		return false;
	size_t lineEnd = rest.find('\n', label);
	if (lineEnd == std::string::npos)
		return false;
	lineEnd++;
	out = rest.substr(0, lineEnd) + IndentByOneTab(stmt) + rest.substr(lineEnd);
	return true;
}

static bool MutateUpdateCarCount(const std::string &fn, std::string &out)
{
	size_t removeStart = fn.find("if (remove)");
	if (removeStart == std::string::npos)
		return false;
	size_t elseStart = fn.find("else{", removeStart);
	if (elseStart == std::string::npos)
		return false;
	std::string head = fn.substr(0, removeStart);
	std::string rem = fn.substr(removeStart, elseStart - removeStart);
	std::string add = fn.substr(elseStart);
	std::string mutRemove, mutAdd;
	if (!MutateHalf(rem, mutRemove) || !MutateHalf(add, mutAdd))
		return false;
	out = head + mutRemove + mutAdd;
	return true;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	std::string root = (argc > 1) ? argv[1] : ".";

	printf("reVC the spawn law counter (VICE-166): the law flag counted apart from the class\n");
	printf("the source root is \"%s\"\n\n", root.c_str());

	/* ---- A. the source read */
	printf("== A. the source read\n\n");

	std::string carCtrl, vehicleText, poolsText, configText;
	if (!ReadFileText(root + "/src/control/CarCtrl.cpp", carCtrl)) {
		Fail("cannot read src/control/CarCtrl.cpp (run the program from the repository root)");
		return 1;
	}
	if (!ReadFileText(root + "/src/vehicles/Vehicle.cpp", vehicleText) ||
		!ReadFileText(root + "/src/core/Pools.cpp", poolsText) ||
		!ReadFileText(root + "/src/core/config.h", configText)) {
		Fail("cannot read src/vehicles/Vehicle.cpp, src/core/Pools.cpp or src/core/config.h");
		return 1;
	}

	std::string fn;
	if (!ExtractFunction(carCtrl, "void CCarCtrl::UpdateCarCount", fn)) {
		Fail("CCarCtrl::UpdateCarCount is not in src/control/CarCtrl.cpp");
		return 1;
	}
	std::string removeHalf, addHalf;
	if (!SplitHalves(fn, removeHalf, addHalf)) {
		Fail("CCarCtrl::UpdateCarCount no longer holds the remove/add halves");
		return 1;
	}
	SHalfFacts rem, add;
	ParseHalf(removeHalf, rem);
	ParseHalf(addHalf, add);
	bool farExempt = FarIsAnEarlyReturn(fn);
	SRule rule;
	BuildRule(rem, add, farExempt, rule);
	CheckUpdateCarCount(rem, add, farExempt, rule);
	PrintUpdateCarCount(rule, "CCarCtrl::UpdateCarCount (src/control/CarCtrl.cpp): the current sources");

	SVehicleFacts vehicleFacts;
	if (!ParseVehicleFacts(vehicleText, vehicleFacts)) {
		Fail("the constructor or ChangeLawEnforcerState is not in src/vehicles/Vehicle.cpp");
		return 1;
	}
	CheckVehicleFacts(vehicleFacts);
	PrintVehicleFacts(vehicleFacts);

	SPoolFacts poolFacts;
	if (!ParsePoolFacts(configText, poolsText, poolFacts)) {
		Fail("LoadVehiclePool or its COMPATIBLE_SAVES branch is not in src/core/Pools.cpp");
		return 1;
	}
	CheckPoolFacts(poolFacts);
	PrintPoolFacts(poolFacts);

	/* ---- B. the replay of the fixed sources */
	printf("== B. the replay: NumLawEnforcerCars must balance in every case\n\n");
	std::vector<SCase> clean;
	RunBattery(rule, poolFacts.countsOnAdd, clean);
	PrintCases(clean, false);
	for (size_t i = 0; i < clean.size(); i++)
		if (!clean[i].ok)
			Fail("the case \"%s\" does not balance", clean[i].name);
	printf("\n  %d case(s) of the current sources: %s\n\n", (int)clean.size(),
		g_failures == 0 ? "all balance" : "see the failures above");

	/* ---- C. the mutation guard */
	printf("== C. the mutation guard: the law count moved back inside the RANDOM case\n\n");
	std::string mutatedFn;
	if (!MutateUpdateCarCount(fn, mutatedFn)) {
		Fail("the mutation of the parsed body failed");
	} else {
		size_t fnStart = carCtrl.find("void CCarCtrl::UpdateCarCount");
		size_t fnEnd = carCtrl.find("\n}\n", fnStart);
		std::string mutatedSource = carCtrl;
		mutatedSource.replace(fnStart, fnEnd + 3 - fnStart, mutatedFn);
		FILE *f = fopen("/tmp/spawn-law-counter-mutated.cpp", "wb");
		if (f) {
			fwrite(mutatedSource.data(), 1, mutatedSource.size(), f);
			fclose(f);
			printf("  the mutated source is written to /tmp/spawn-law-counter-mutated.cpp\n");
		}

		std::string mutRemove, mutAdd;
		SHalfFacts mutRemF, mutAddF;
		SRule mutRule;
		if (!SplitHalves(mutatedFn, mutRemove, mutAdd)) {
			Fail("the mutated body no longer holds the remove/add halves");
		} else {
			ParseHalf(mutRemove, mutRemF);
			ParseHalf(mutAdd, mutAddF);
			BuildRule(mutRemF, mutAddF, FarIsAnEarlyReturn(mutatedFn), mutRule);
			printf("  the parse of the mutated source:\n");
			PrintUpdateCarCount(mutRule, "CCarCtrl::UpdateCarCount (the mutated text)");

			std::vector<SCase> mutated;
			RunBattery(mutRule, poolFacts.countsOnAdd, mutated);
			PrintCases(mutated, true);
			int failed = 0;
			for (size_t i = 0; i < mutated.size(); i++)
				if (!mutated[i].ok)
					failed++;
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
	}

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
