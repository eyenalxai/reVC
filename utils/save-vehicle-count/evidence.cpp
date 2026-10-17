/*
 * The evidence program for VICE-163, "spawn: fix the save-load vehicle
 * counter".
 *
 * The restore of the vehicle pool (CPools::LoadVehiclePool) builds every
 * saved vehicle with RANDOM_VEHICLE - the constructor counts it - and the
 * pool takes that count back, because the save carries the real class. The
 * fix of VICE-163 gives the one count of the restored vehicle to the class
 * the save carries. Before it, the count was taken back and never given
 * again: NumMissionCars stayed at zero with every restored mission car
 * live, and NumCarsInUse, which sums the class counters, read short by the
 * restored set for the rest of the session (CarCtrl.cpp).
 *
 * This program cannot run the restore itself: LoadVehiclePool builds real
 * vehicles and adds them to the world, and neither the vehicle pool nor the
 * streaming nor the world exists without the game. So the program does the
 * two strongest offline things instead:
 *
 *   A. It reads the sources that own the transition - Pools.cpp (the two
 *      branches of LoadVehiclePool and the save's IsVehicleSaved),
 *      Vehicle.cpp (the constructor, Load, Save and the destructor),
 *      CarCtrl.cpp (UpdateCarCount, ReInit, NumCarsInUse), Game.cpp (the
 *      load path), GenericGameStorage.cpp (the quick save) and config.h
 *      (the COMPATIBLE_SAVES selection) - and checks the shape of the
 *      transition in both branches: every construction uses
 *      RANDOM_VEHICLE, one manual NumRandomCars decrement takes the
 *      constructor's count back, the saved class arrives before the
 *      recount, and exactly one UpdateCarCount(pVehicle, false) follows it.
 *      An edit that drops the recount, duplicates it, moves it before the
 *      class, puts it under a preprocessor conditional or strays a counter
 *      write fails here.
 *
 *   B. It runs the transition as the sources write it: the counter deltas
 *      are parsed from CCarCtrl::UpdateCarCount and the order from
 *      LoadVehiclePool, and the counters are followed through the restore,
 *      a mixed saved set and a carried save/load soak. The counters after
 *      every load equal the counters before the save (the acceptance of the
 *      ticket); the constructed set destroyed through the parsed remove
 *      deltas returns the counters to zero. The recorded pre-fix sequence
 *      is run side by side: it nets zero per vehicle, so it loses the whole
 *      restored set on the first load.
 *
 * What the program cannot measure: the counters of the running game across
 * a real save and load. That live soak is the acceptance of the live rig
 * (the owner's run, or a read of the running process as
 * utils/caps-audit/live-counters.py does); the tool prints NumMissionCars
 * and the other counters NumCarsInUse sums - NumRandomCars,
 * NumLawEnforcerCars, NumParkedCars and the two emergency counters - so the
 * operator reads them before the save and after the load. NumPermanentCars
 * is printed too and stays outside the sum. The call sites of
 * UpdateCarCount outside the restore (the replay player, the far layer's
 * class switch) run on live vehicles, not on a restored one.
 *
 * The checks read C++ as text, so they rest on these shapes of the files:
 *   - a function body closes with a brace at column 0 (FindBody);
 *   - UpdateCarCount splits at a line that is exactly "else{" into the
 *     remove and the create switch;
 *   - LoadVehiclePool's branch table is one #ifdef COMPATIBLE_SAVES, one
 *     #else and the body's last #endif; an #if 0 block of a branch is
 *     skipped, and a preprocessor line between the recount and the branch
 *     end fails the check;
 *   - a count op is a line with "++Num" or "--Num"; its guard is the text
 *     between it and the previous op (the law count is the one op under
 *     the pVehicle->bIsLawEnforcer guard, and no op may sit under a class
 *     guard of its own);
 *   - the calls of the transition are recognized by their exact text.
 * A structural edit that does not fit fails the read (a missing function,
 * class, op or counter is reported), never passes silently.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -O2 -o /tmp/save-vehicle-count utils/save-vehicle-count/evidence.cpp
 *	/tmp/save-vehicle-count
 *
 * The exit code is 0 when every check passes. The output goes into the
 * ticket.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

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

/* --------------------------------------------------------------- the files */

struct SSource
{
	std::vector<std::string> lines;
};

static bool ReadSource(const char *path, SSource &src)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("cannot read %s (run the program from the repository root)\n", path);
		return false;
	}
	std::string text;
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	fclose(f);

	size_t start = 0;
	while (start <= text.size()) {
		size_t nl = text.find('\n', start);
		std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);
		src.lines.push_back(line);
		if (nl == std::string::npos)
			break;
		start = nl + 1;
	}
	return true;
}

static bool Contains(const std::string &s, const char *needle)
{
	return s.find(needle) != std::string::npos;
}

static std::string Trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t");
	return s.substr(a, b - a + 1);
}

static bool IsPreprocessorLine(const std::string &line)
{
	std::string t = Trim(line);
	return !t.empty() && t[0] == '#';
}

static int FindLine(const SSource &src, const char *needle, int from)
{
	for (int i = from; i < (int)src.lines.size(); i++)
		if (Contains(src.lines[i], needle))
			return i;
	return -1;
}

/*
 * The function bodies of this code base close with a brace at column 0, so
 * the end of a body is the first such brace after the signature. The body
 * itself is the lines (begin, end).
 */
static bool FindBody(const SSource &src, const char *signature, int &begin, int &end)
{
	begin = FindLine(src, signature, 0);
	if (begin < 0)
		return false;
	for (int i = begin + 1; i < (int)src.lines.size(); i++) {
		if (src.lines[i] == "}") {
			end = i;
			return true;
		}
	}
	return false;
}

static int FindInBody(const SSource &src, int begin, int end, const char *needle, int from)
{
	if (from < begin + 1)
		from = begin + 1;
	for (int i = from; i < end; i++)
		if (Contains(src.lines[i], needle))
			return i;
	return -1;
}

static int CountText(const SSource &src, int first, int last, const char *needle)
{
	int n = 0;
	for (int i = first; i <= last; i++) {
		size_t at = 0;
		while ((at = src.lines[i].find(needle, at)) != std::string::npos) {
			n++;
			at += strlen(needle);
		}
	}
	return n;
}

/* --------------------------------------------- A. the deltas of the count */

/* One counter the count moves, with the line that moves it. */
struct SCountOp
{
	std::string counter;	/* NumRandomCars, NumLawEnforcerCars, ... */
	int sign;		/* +1 (++) or -1 (--) */
	bool whenLaw;		/* the op sits under the bIsLawEnforcer guard */
	bool classGuard;	/* the op sits under a guard on the created-by class */
	int line;		/* 1-based */
};

struct SClassOps
{
	std::string cls;	/* RANDOM_VEHICLE, ... */
	std::vector<SCountOp> ops;
};

/* The five classes of the count, in the order of the enum (Vehicle.h). */
static const char *aClassNames[] = {
	"RANDOM_VEHICLE", "MISSION_VEHICLE", "PARKED_VEHICLE",
	"PERMANENT_VEHICLE", "FAR_TRAFFIC_VEHICLE"
};
static const int nClassNames = sizeof(aClassNames) / sizeof(aClassNames[0]);

/* The four class counters CCarCtrl::NumCarsInUse sums, in the order of its
 * expression; the model's SCounters::InUse holds the same list. */
static const char *aInUseClass[] = {
	"NumRandomCars", "NumLawEnforcerCars", "NumMissionCars", "NumParkedCars"
};

static bool IsCountOpLine(const std::string &line)
{
	return Contains(line, "++Num") || Contains(line, "--Num");
}

static std::vector<SClassOps> ParseSwitch(const SSource &src, int startLine, int endLine)
{
	std::vector<SClassOps> classes;
	for (int i = startLine + 1; i < endLine; i++) {
		std::string t = Trim(src.lines[i]);
		if (t.size() > 5 && t.compare(0, 5, "case ") == 0) {
			SClassOps c;
			size_t colon = t.find(':');
			c.cls = t.substr(5, colon - 5);
			classes.push_back(c);
			continue;
		}
		if (classes.empty())
			continue;
		size_t at = src.lines[i].find("++Num");
		int sign = +1;
		if (at == std::string::npos) {
			at = src.lines[i].find("--Num");
			sign = -1;
		}
		if (at == std::string::npos)
			continue;
		size_t opStart = at + 2;
		size_t opEnd = opStart;
		while (opEnd < src.lines[i].size() &&
			(isalnum((unsigned char)src.lines[i][opEnd]) || src.lines[i][opEnd] == '_'))
			opEnd++;
		SCountOp op;
		op.counter = src.lines[i].substr(opStart, opEnd - opStart);
		op.sign = sign;
		op.line = i + 1;
		/* The guard of the op: the text between it and the previous count
		 * op (or the case line). The law count is the one op under the
		 * bIsLawEnforcer guard; a counter may not sit under a guard on the
		 * created-by class of its own. */
		op.whenLaw = false;
		op.classGuard = false;
		for (int j = i - 1; j > startLine; j--) {
			std::string p = Trim(src.lines[j]);
			if (p.size() > 5 && p.compare(0, 5, "case ") == 0)
				break;
			if (IsCountOpLine(src.lines[j]))
				break;
			if (Contains(src.lines[j], "bIsLawEnforcer"))
				op.whenLaw = true;
			if (Contains(src.lines[j], "VehicleCreatedBy"))
				op.classGuard = true;
		}
		classes.back().ops.push_back(op);
	}
	return classes;
}

static const SClassOps *FindClass(const std::vector<SClassOps> &classes, const char *name)
{
	for (size_t i = 0; i < classes.size(); i++)
		if (classes[i].cls == name)
			return &classes[i];
	return NULL;
}

/* ------------------------------------------- the restore of LoadVehiclePool */

enum EEvent
{
	EV_CTOR_RANDOM,	/* new(slot) C...(model, RANDOM_VEHICLE) */
	EV_DEC_RANDOM,	/* --CCarCtrl::NumRandomCars */
	EV_LOAD,	/* pVehicle->Load(buf) */
	EV_SET_CLASS,	/* pVehicle->VehicleCreatedBy = the buffer's */
	EV_SET_LAW,	/* pVehicle->bIsLawEnforcer = the buffer's */
	EV_RECOUNT	/* CCarCtrl::UpdateCarCount(pVehicle, false) */
};

struct SEvent
{
	EEvent kind;
	int line;	/* 1-based */
};

/*
 * The counter-affecting lines of one branch, in order. A preprocessor
 * conditional is tracked by depth: an #if 0 block is skipped whole (the
 * alternate branch of its #else is live again), and a nested #ifdef of a
 * branch (the direct branch's FIX_BUGS copy) stays in the scan.
 */
static std::vector<SEvent> ScanBranch(const SSource &src, int first, int last)
{
	std::vector<SEvent> events;
	int depth = 0;
	int deadDepth = -1;	/* the depth of the #if 0 block, -1 when live */
	for (int i = first; i <= last; i++) {
		std::string t = Trim(src.lines[i]);
		if (t.compare(0, 3, "#if") == 0) {
			if (deadDepth < 0 && (t == "#if 0" ||
				(t.size() > 5 && t.compare(0, 5, "#if 0") == 0 &&
				 (t[5] == ' ' || t[5] == '\t' || t[5] == '/'))))
				deadDepth = depth;
			depth++;
			continue;
		}
		if (t.compare(0, 5, "#else") == 0 || t.compare(0, 5, "#elif") == 0) {
			if (deadDepth >= 0 && depth == deadDepth + 1)
				deadDepth = -1;	/* the alternate branch of the #if 0 is live */
			continue;
		}
		if (t.compare(0, 6, "#endif") == 0) {
			depth--;
			if (deadDepth >= 0 && depth == deadDepth)
				deadDepth = -1;
			continue;
		}
		if (deadDepth >= 0)
			continue;

		const std::string &s = src.lines[i];
		SEvent e;
		if (Contains(s, "new(slot)") && Contains(s, "RANDOM_VEHICLE")) {
			e.kind = EV_CTOR_RANDOM; e.line = i + 1; events.push_back(e);
		}
		if (Contains(s, "--CCarCtrl::NumRandomCars") || Contains(s, "CCarCtrl::NumRandomCars--")) {
			e.kind = EV_DEC_RANDOM; e.line = i + 1; events.push_back(e);
		}
		if (Contains(s, "pVehicle->Load(buf)")) {
			e.kind = EV_LOAD; e.line = i + 1; events.push_back(e);
		}
		if (Contains(s, "pVehicle->VehicleCreatedBy = pBufferVehicle->VehicleCreatedBy")) {
			e.kind = EV_SET_CLASS; e.line = i + 1; events.push_back(e);
		}
		if (Contains(s, "pVehicle->bIsLawEnforcer = pBufferVehicle->bIsLawEnforcer")) {
			e.kind = EV_SET_LAW; e.line = i + 1; events.push_back(e);
		}
		if (Contains(s, "CCarCtrl::UpdateCarCount(pVehicle, false)")) {
			e.kind = EV_RECOUNT; e.line = i + 1; events.push_back(e);
		}
	}
	return events;
}

static int CountEvents(const std::vector<SEvent> &events, EEvent kind)
{
	int n = 0;
	for (size_t i = 0; i < events.size(); i++)
		if (events[i].kind == kind)
			n++;
	return n;
}

static int FirstEvent(const std::vector<SEvent> &events, EEvent kind)
{
	for (size_t i = 0; i < events.size(); i++)
		if (events[i].kind == kind)
			return events[i].line;
	return -1;
}

static int LastEvent(const std::vector<SEvent> &events, EEvent kind)
{
	int line = -1;
	for (size_t i = 0; i < events.size(); i++)
		if (events[i].kind == kind)
			line = events[i].line;
	return line;
}

/* A count that must stay unconditional: no #if... between it and the end of
 * the branch that holds it. */
static bool ConditionalAfter(const SSource &src, int countLine, int last)
{
	for (int i = countLine; i <= last; i++)	/* countLine is 1-based: the next line is its index */
		if (IsPreprocessorLine(src.lines[i]))
			return true;
	return false;
}

/* ------------------------------------------------- the shape of the restore */

static void CheckCompatibleBranch(const SSource &src, int first, int last, const std::vector<SEvent> &ev)
{
	const int ctors = CountEvents(ev, EV_CTOR_RANDOM);
	const int decs = CountEvents(ev, EV_DEC_RANDOM);
	const int loads = CountEvents(ev, EV_LOAD);
	const int recounts = CountEvents(ev, EV_RECOUNT);

	printf("  #ifdef COMPATIBLE_SAVES (the branch VICE-163 fixes):\n");
	printf("    %d constructions with RANDOM_VEHICLE, %d NumRandomCars decrement, %d Load, %d recount\n",
		ctors, decs, loads, recounts);
	Check(ctors == 3, "the compatible branch constructs with RANDOM_VEHICLE at the boat, the car and the bike");
	Check(decs == 1, "the compatible branch takes the constructor's count back exactly once");
	Check(loads == 1, "the compatible branch loads the saved vehicle exactly once");
	Check(recounts == 1, "the compatible branch counts the restored vehicle exactly once (VICE-163)");
	Check(CountEvents(ev, EV_SET_CLASS) == 0 && CountEvents(ev, EV_SET_LAW) == 0,
		"the compatible branch takes the class from Load, not from a direct copy");
	Check(CountText(src, first, last, "CCarCtrl::Num") == decs,
		"no other counter write lives in the compatible branch");

	if (ctors == 3 && decs == 1 && loads == 1 && recounts == 1) {
		const int lastCtor = LastEvent(ev, EV_CTOR_RANDOM);
		const int dec = FirstEvent(ev, EV_DEC_RANDOM);
		const int load = FirstEvent(ev, EV_LOAD);
		const int recount = FirstEvent(ev, EV_RECOUNT);
		printf("    the order: construct (line %d) -> decrement (%d) -> Load (%d) -> recount (%d)\n",
			lastCtor, dec, load, recount);
		Check(lastCtor < dec, "the decrement takes the count of the construction");
		Check(dec < load, "the decrement runs before Load sets the saved class");
		Check(load < recount, "the recount runs after Load has set the saved class");
		Check(!ConditionalAfter(src, recount, last),
			"a preprocessor line sits between the recount and the branch end");
	}
}

static void CheckDirectBranch(const SSource &src, int first, int last, const std::vector<SEvent> &ev)
{
	const int ctors = CountEvents(ev, EV_CTOR_RANDOM);
	const int decs = CountEvents(ev, EV_DEC_RANDOM);
	const int setClass = CountEvents(ev, EV_SET_CLASS);
	const int setLaw = CountEvents(ev, EV_SET_LAW);
	const int recounts = CountEvents(ev, EV_RECOUNT);

	printf("  #else (the direct branch):\n");
	printf("    %d constructions with RANDOM_VEHICLE, %d NumRandomCars decrements, %d class copy, %d recount\n",
		ctors, decs, setClass, recounts);
	Check(ctors == 3, "the direct branch constructs with RANDOM_VEHICLE at the boat, the car and the bike");
	Check(decs == 3, "the direct branch takes the constructor's count back at every site");
	Check(setClass == 1 && setLaw == 1, "the direct branch copies the saved class and the law flag");
	Check(recounts == 1, "the direct branch counts the restored vehicle exactly once");
	Check(CountEvents(ev, EV_LOAD) == 0, "the direct branch takes the fields from the buffer, not from Load");
	Check(CountText(src, first, last, "CCarCtrl::Num") == decs,
		"no other counter write lives in the direct branch");

	if (ctors == 3 && decs == 3 && setClass == 1 && recounts == 1) {
		/* Every construction has its own decrement. */
		std::vector<int> ctorLines, decLines;
		for (size_t i = 0; i < ev.size(); i++) {
			if (ev[i].kind == EV_CTOR_RANDOM)
				ctorLines.push_back(ev[i].line);
			if (ev[i].kind == EV_DEC_RANDOM)
				decLines.push_back(ev[i].line);
		}
		bool paired = ctorLines.size() == decLines.size();
		for (size_t i = 0; paired && i < ctorLines.size(); i++)
			if (ctorLines[i] > decLines[i])
				paired = false;
		Check(paired, "every construction of the direct branch is followed by its decrement");
		const int dec = LastEvent(ev, EV_DEC_RANDOM);
		const int cls = FirstEvent(ev, EV_SET_CLASS);
		const int law = FirstEvent(ev, EV_SET_LAW);
		const int recount = FirstEvent(ev, EV_RECOUNT);
		printf("    the tail: last decrement (%d) -> class (%d) -> law flag (%d) -> recount (%d)\n",
			dec, cls, law, recount);
		Check(dec < cls && cls < recount && law < recount,
			"the recount runs after the saved class is in place");
		Check(!ConditionalAfter(src, recount, last),
			"a preprocessor line sits between the recount and the branch end");
	}
}

/* The guard text of every parsed op (the law count under bIsLawEnforcer, the
 * rest under none), on both sides of the count. */
static void CheckGuards(const std::vector<SClassOps> &create, const std::vector<SClassOps> &remove)
{
	static const char *aSide[] = { "create", "remove" };
	for (int side = 0; side < 2; side++) {
		const std::vector<SClassOps> &table = side == 0 ? create : remove;
		for (size_t c = 0; c < table.size(); c++) {
			for (size_t j = 0; j < table[c].ops.size(); j++) {
				const SCountOp &op = table[c].ops[j];
				char msg[200];
				if (op.counter == "NumLawEnforcerCars") {
					sprintf(msg, "%s: %s is not under the pVehicle->bIsLawEnforcer guard (%s side)",
						table[c].cls.c_str(), op.counter.c_str(), aSide[side]);
					Check(op.whenLaw, msg);
				} else {
					sprintf(msg, "%s: %s is under the law guard (%s side)",
						table[c].cls.c_str(), op.counter.c_str(), aSide[side]);
					Check(!op.whenLaw, msg);
					sprintf(msg, "%s: %s is under a class guard of its own (%s side)",
						table[c].cls.c_str(), op.counter.c_str(), aSide[side]);
					Check(!op.classGuard, msg);
				}
			}
		}
	}
}

/* The sum expression of CCarCtrl::NumCarsInUse, the identifiers it adds. */
static std::vector<std::string> ParseSumIdentifiers(const SSource &src, int begin, int end)
{
	std::vector<std::string> names;
	std::string expr;
	bool inReturn = false;
	for (int i = begin + 1; i < end; i++) {
		if (!inReturn) {
			if (!Contains(src.lines[i], "return "))
				continue;
			inReturn = true;
		}
		expr += src.lines[i];
		expr += " ";
		if (Contains(src.lines[i], ";"))
			break;
	}
	for (size_t i = 0; i < expr.size(); ) {
		if (isalpha((unsigned char)expr[i]) || expr[i] == '_') {
			size_t j = i;
			while (j < expr.size() && (isalnum((unsigned char)expr[j]) || expr[j] == '_'))
				j++;
			if (j - i > 3 && expr.compare(i, 3, "Num") == 0)
				names.push_back(expr.substr(i, j - i));
			i = j;
		} else
			i++;
	}
	return names;
}

static bool HasName(const std::vector<std::string> &names, const char *name)
{
	for (size_t i = 0; i < names.size(); i++)
		if (names[i] == name)
			return true;
	return false;
}

static bool CheckSources(SSource &pools, SSource &carCtrl, SSource &vehicle, SSource &game, SSource &storage,
	std::vector<SClassOps> &createOut, std::vector<SClassOps> &removeOut, bool &recountInPlace)
{
	int begin, end, split, endif;

	printf("A. the shape of the restore and of the count (the sources themselves)\n\n");

	/* --- CPools::LoadVehiclePool --- */
	if (!FindBody(pools, "CPools::LoadVehiclePool(uint8* buf, uint32 size)", begin, end)) {
		Fail("Pools.cpp: CPools::LoadVehiclePool is not found");
		return false;
	}
	const int ifdef = FindInBody(pools, begin, end, "#ifdef COMPATIBLE_SAVES", begin);
	if (ifdef < 0) {
		Fail("Pools.cpp: the COMPATIBLE_SAVES branch of LoadVehiclePool is not found");
		return false;
	}
	split = FindInBody(pools, ifdef, end, "#else", ifdef);
	if (split < 0) {
		Fail("Pools.cpp: the #else branch of LoadVehiclePool is not found");
		return false;
	}
	/* The last #endif of the body closes the branch table (the bike block
	 * of the direct branch has a nested #ifdef FIX_BUGS of its own). */
	endif = -1;
	for (int i = split; i < end; i++)
		if (Trim(pools.lines[i]) == "#endif")
			endif = i;
	if (endif < 0) {
		Fail("Pools.cpp: the #endif of the branch table is not found");
		return false;
	}
	printf("  src/core/Pools.cpp: LoadVehiclePool, lines %d..%d\n", begin + 1, end + 1);
	printf("    the compatible branch, lines %d..%d; the direct branch, lines %d..%d\n",
		ifdef + 2, split, split + 2, endif);

	std::vector<SEvent> compatible = ScanBranch(pools, ifdef + 1, split - 1);
	std::vector<SEvent> direct = ScanBranch(pools, split + 1, endif - 1);
	CheckCompatibleBranch(pools, ifdef + 1, split - 1, compatible);
	CheckDirectBranch(pools, split + 1, endif - 1, direct);
	/* The transition the program runs below is the one of the live branch;
	 * COMPATIBLE_SAVES selects it. A branch without the recount runs
	 * without it there too, so the acceptance fails on the pre-fix code. */
	recountInPlace = CountEvents(compatible, EV_RECOUNT) == 1 &&
		CountEvents(direct, EV_RECOUNT) == 1;
	printf("    the source writes the recount in both branches: %s\n", recountInPlace ? "yes" : "NO");
	SSource config;
	if (ReadSource("src/core/config.h", config)) {
		const int def = FindLine(config, "#define COMPATIBLE_SAVES", 0);
		Check(def >= 0, "config.h: COMPATIBLE_SAVES is the live branch");
		if (def >= 0)
			printf("    src/core/config.h line %d: #define COMPATIBLE_SAVES (the live branch)\n", def + 1);
	} else
		Fail("config.h cannot be read");
	printf("\n");

	/* --- IsVehicleSaved: what a save carries --- */
	if (FindBody(pools, "IsVehicleSaved(CVehicle", begin, end)) {
		const int force = FindInBody(pools, begin, end,
			"m_pMyVehicle == pVehicle && IsQuickSave", begin);
		const int forced = FindInBody(pools, begin, end, "if (bForceSaving)", begin);
		const int ret = FindInBody(pools, begin, end,
			"return pVehicle->VehicleCreatedBy == MISSION_VEHICLE;", begin);
		printf("  src/core/Pools.cpp: IsVehicleSaved (lines %d..%d) carries the mission vehicles;\n",
			begin + 1, end + 1);
		printf("    a mission-replay quicksave adds the player's car, whatever its class\n");
		Check(force >= 0 && forced >= 0, "IsVehicleSaved still forces the quicksave's player car");
		Check(ret >= 0, "IsVehicleSaved still carries the MISSION_VEHICLE class");
	} else
		Fail("Pools.cpp: IsVehicleSaved is not found");
	if (FindBody(storage, "bool SaveGameForPause", begin, end) ||
	    FindBody(storage, "SaveGameForPause(", begin, end)) {
		const int set = FindInBody(storage, begin, end, "IsQuickSave = type;", begin);
		printf("  src/save/GenericGameStorage.cpp: the quicksave sets IsQuickSave (%d)\n", set + 1);
		Check(set >= 0, "the mission-replay quicksave still sets IsQuickSave");
	} else
		Fail("GenericGameStorage.cpp: SaveGameForPause is not found");
	printf("\n");

	/* --- CVehicle: the constructor, Load, Save, the destructor --- */
	if (!FindBody(vehicle, "CVehicle::CVehicle(uint8 CreatedBy)", begin, end)) {
		Fail("Vehicle.cpp: the CVehicle constructor is not found");
		return false;
	}
	const int setClass = FindInBody(vehicle, begin, end, "VehicleCreatedBy = CreatedBy;", begin);
	const int lawFalse = FindInBody(vehicle, begin, end, "bIsLawEnforcer = false;", begin);
	const int ctorCount = FindInBody(vehicle, begin, end, "CCarCtrl::UpdateCarCount(this, false);", begin);
	const int ctorCounts = CountText(vehicle, begin, end, "CCarCtrl::UpdateCarCount(this, false);");
	printf("  src/vehicles/Vehicle.cpp: the constructor (lines %d..%d)\n", begin + 1, end + 1);
	printf("    the class (line %d) and the law flag (line %d) are set before the count (line %d)\n",
		setClass + 1, lawFalse + 1, ctorCount + 1);
	Check(setClass >= 0 && lawFalse >= 0 && ctorCount >= 0,
		"the constructor still sets the class, the law flag and the count");
	Check(ctorCounts == 1, "the constructor counts the vehicle exactly once");
	Check(setClass >= 0 && setClass < ctorCount, "the constructor counts with the class it was given");
	Check(lawFalse >= 0 && lawFalse < ctorCount,
		"the constructor counts a non-law-enforcer, so the count is the random one");

	if (!FindBody(vehicle, "CVehicle::Load(uint8*& buf)", begin, end)) {
		Fail("Vehicle.cpp: CVehicle::Load is not found");
		return false;
	}
	const int loadClass = FindInBody(vehicle, begin, end, "ReadSaveBuf(&VehicleCreatedBy, buf);", begin);
	const int loadLaw = FindInBody(vehicle, begin, end, "bIsLawEnforcer = !!(flags & BIT(0));", begin);
	printf("  src/vehicles/Vehicle.cpp: CVehicle::Load (lines %d..%d) reads the class (line %d) and the law flag (line %d)\n",
		begin + 1, end + 1, loadClass + 1, loadLaw + 1);
	Check(loadClass >= 0 && loadLaw >= 0, "Load still reads the class and the law flag the recount needs");

	if (FindBody(vehicle, "CVehicle::Save(uint8*& buf)", begin, end)) {
		const int saveClass = FindInBody(vehicle, begin, end, "WriteSaveBuf(buf, VehicleCreatedBy);", begin);
		Check(saveClass >= 0, "Save still writes the class the restore counts");
	} else
		Fail("Vehicle.cpp: CVehicle::Save is not found");

	if (FindBody(vehicle, "CVehicle::~CVehicle()", begin, end)) {
		const int dtor = FindInBody(vehicle, begin, end, "CCarCtrl::UpdateCarCount(this, true);", begin);
		const int dtorCounts = CountText(vehicle, begin, end, "CCarCtrl::UpdateCarCount(this, true);");
		Check(dtor >= 0, "the destructor takes the count back with the class of the vehicle");
		Check(dtorCounts == 1, "the destructor takes the count back exactly once");
	} else
		Fail("Vehicle.cpp: the CVehicle destructor is not found");

	/* --- CarCtrl: the counters the count moves --- */
	if (!FindBody(carCtrl, "CCarCtrl::UpdateCarCount(CVehicle* pVehicle, bool remove)", begin, end)) {
		Fail("CarCtrl.cpp: CCarCtrl::UpdateCarCount is not found");
		return false;
	}
	split = -1;
	for (int i = begin + 1; i < end; i++)
		if (Trim(carCtrl.lines[i]) == "else{") {
			split = i;
			break;
		}
	if (split < 0) {
		Fail("CarCtrl.cpp: the remove/create split of UpdateCarCount is not found");
		return false;
	}
	std::vector<SClassOps> remove = ParseSwitch(carCtrl, begin, split);
	std::vector<SClassOps> create = ParseSwitch(carCtrl, split, end);
	printf("  src/control/CarCtrl.cpp: UpdateCarCount (lines %d..%d)\n", begin + 1, end + 1);

	Check((int)create.size() == nClassNames, "UpdateCarCount covers the five classes on the create side");
	Check((int)remove.size() == nClassNames, "UpdateCarCount covers the five classes on the remove side");

	CheckGuards(create, remove);

	/* The deltas, class by class, against the table the program runs. */
	static const SClassOps aExpected[] = {
		/* RANDOM: a law enforcer is counted twice, as the live path counts it. */
		{ "RANDOM_VEHICLE",      { { "NumLawEnforcerCars", +1, true, false, 0 }, { "NumRandomCars", +1, false, false, 0 } } },
		{ "MISSION_VEHICLE",     { { "NumMissionCars", +1, false, false, 0 } } },
		{ "PARKED_VEHICLE",      { { "NumParkedCars", +1, false, false, 0 } } },
		{ "PERMANENT_VEHICLE",   { { "NumPermanentCars", +1, false, false, 0 } } },
		{ "FAR_TRAFFIC_VEHICLE", { } },
	};
	char msg[240];
	for (int i = 0; i < nClassNames; i++) {
		const SClassOps *got = FindClass(create, aExpected[i].cls.c_str());
		sprintf(msg, "the create table has no %s class", aExpected[i].cls.c_str());
		Check(got != NULL, msg);
		if (got) {
			sprintf(msg, "%s: the create table has %d deltas, expected %d",
				aExpected[i].cls.c_str(), (int)got->ops.size(), (int)aExpected[i].ops.size());
			Check(got->ops.size() == aExpected[i].ops.size(), msg);
			for (size_t j = 0; j < got->ops.size() && j < aExpected[i].ops.size(); j++) {
				bool same = got->ops[j].counter == aExpected[i].ops[j].counter &&
					got->ops[j].sign == aExpected[i].ops[j].sign &&
					got->ops[j].whenLaw == aExpected[i].ops[j].whenLaw;
				sprintf(msg, "%s: create delta %d is %s %+d (law %d), expected %s %+d (law %d)",
					aExpected[i].cls.c_str(), (int)j,
					got->ops[j].counter.c_str(), got->ops[j].sign, got->ops[j].whenLaw,
					aExpected[i].ops[j].counter.c_str(), aExpected[i].ops[j].sign,
					aExpected[i].ops[j].whenLaw);
				Check(same, msg);
			}
		}
		/* the remove side is the mirror of the create side */
		const SClassOps *gone = FindClass(remove, aExpected[i].cls.c_str());
		sprintf(msg, "the remove table has no %s class", aExpected[i].cls.c_str());
		Check(gone != NULL, msg);
		if (got && gone) {
			sprintf(msg, "%s: the remove table has %d deltas, expected %d (the mirror)",
				aExpected[i].cls.c_str(), (int)gone->ops.size(), (int)got->ops.size());
			Check(gone->ops.size() == got->ops.size(), msg);
			for (size_t j = 0; j < gone->ops.size() && j < got->ops.size(); j++) {
				bool mirror = gone->ops[j].counter == got->ops[j].counter &&
					gone->ops[j].sign == -got->ops[j].sign &&
					gone->ops[j].whenLaw == got->ops[j].whenLaw;
				sprintf(msg, "%s: remove delta %d is %s %+d (law %d), expected %s %+d (law %d)",
					aExpected[i].cls.c_str(), (int)j,
					gone->ops[j].counter.c_str(), gone->ops[j].sign, gone->ops[j].whenLaw,
					got->ops[j].counter.c_str(), -got->ops[j].sign, got->ops[j].whenLaw);
				Check(mirror, msg);
			}
		}
	}
	printf("    the five classes: RANDOM +1 random (+1 law for a law enforcer), MISSION +1 mission,\n");
	printf("    PARKED +1 parked, PERMANENT +1 permanent, FAR_TRAFFIC nothing; the remove side mirrors it\n");

	/* The load path clears the counters before the restore builds on them. */
	static const char *aCounters[] = {
		"NumRandomCars", "NumLawEnforcerCars", "NumMissionCars", "NumParkedCars", "NumPermanentCars"
	};
	if (FindBody(carCtrl, "CCarCtrl::ReInit(void)", begin, end)) {
		for (int i = 0; i < 5; i++) {
			char want[64];
			sprintf(want, "%s = 0;", aCounters[i]);
			sprintf(msg, "ReInit does not zero %s (the load clears before the restore)", aCounters[i]);
			Check(FindInBody(carCtrl, begin, end, want, begin) >= 0, msg);
		}
		printf("    CCarCtrl::ReInit (line %d) zeroes every class counter\n", begin + 1);
	} else
		Fail("CarCtrl.cpp: CCarCtrl::ReInit is not found");

	if (FindBody(carCtrl, "CCarCtrl::NumCarsInUse(void)", begin, end)) {
		const std::vector<std::string> sum = ParseSumIdentifiers(carCtrl, begin, end);
		printf("    CCarCtrl::NumCarsInUse (line %d) sums:", begin + 1);
		for (size_t i = 0; i < sum.size(); i++)
			printf(" %s", sum[i].c_str());
		printf("\n");
		sprintf(msg, "NumCarsInUse sums %d counters, expected the four class counters and the two emergency counters",
			(int)sum.size());
		Check(sum.size() == 6, msg);
		static const char *aDuty[] = { "NumFiretrucksOnDuty", "NumAmbulancesOnDuty" };
		for (int i = 0; i < 4; i++) {
			sprintf(msg, "NumCarsInUse does not sum %s (SCounters::InUse counts it)", aInUseClass[i]);
			Check(HasName(sum, aInUseClass[i]), msg);
		}
		for (int i = 0; i < 2; i++) {
			sprintf(msg, "NumCarsInUse does not sum %s", aDuty[i]);
			Check(HasName(sum, aDuty[i]), msg);
		}
		Check(!HasName(sum, "NumPermanentCars"),
			"NumCarsInUse sums NumPermanentCars (SCounters::InUse keeps it outside the budget)");
		printf("    the four class counters of the sum are SCounters::InUse's list; the two emergency\n");
		printf("    counters are zero through a restore (the save carries no duty flags)\n");
	} else
		Fail("CarCtrl.cpp: CCarCtrl::NumCarsInUse is not found");

	/* The caller of ReInit on the load path. */
	if (FindBody(game, "CGame::ReInitGameObjectVariables(void)", begin, end)) {
		Check(FindInBody(game, begin, end, "CCarCtrl::ReInit();", begin) >= 0,
			"ReInitGameObjectVariables still clears the counters on the load path");
		printf("    the load path: CGame::ReInitGameObjectVariables (Game.cpp line %d) calls CCarCtrl::ReInit\n",
			begin + 1);
	} else
		Fail("Game.cpp: CGame::ReInitGameObjectVariables is not found");
	printf("\n");

	createOut = create;
	removeOut = remove;
	return true;
}

/* ------------------------------------------- B. the transition, run offline */

struct SCounters
{
	int random, law, mission, parked, permanent;
	SCounters() : random(0), law(0), mission(0), parked(0), permanent(0) {}
	/* CCarCtrl::NumCarsInUse: the counters of aInUseClass (random, law,
	 * mission, parked) and the two emergency counters. The save carries no
	 * duty flags, so a restore leaves the emergency counters at zero; the
	 * permanent class stays outside the budget. */
	int InUse() const { return random + law + mission + parked; }
};

static bool SameCounters(const SCounters &a, const SCounters &b)
{
	return a.random == b.random && a.law == b.law && a.mission == b.mission &&
		a.parked == b.parked && a.permanent == b.permanent;
}

static bool IsZero(const SCounters &c)
{
	return c.random == 0 && c.law == 0 && c.mission == 0 && c.parked == 0 && c.permanent == 0;
}

static int *CounterSlot(SCounters &c, const std::string &name)
{
	if (name == "NumRandomCars") return &c.random;
	if (name == "NumLawEnforcerCars") return &c.law;
	if (name == "NumMissionCars") return &c.mission;
	if (name == "NumParkedCars") return &c.parked;
	if (name == "NumPermanentCars") return &c.permanent;
	return NULL;
}

static void ApplyOps(SCounters &c, const SClassOps *ops, bool law)
{
	if (!ops)
		return;
	for (size_t i = 0; i < ops->ops.size(); i++) {
		if (ops->ops[i].whenLaw && !law)
			continue;
		int *slot = CounterSlot(c, ops->ops[i].counter);
		if (slot)
			*slot += ops->ops[i].sign;
	}
}

/* The create count of one class, the way CCarCtrl::UpdateCarCount applies it. */
static void ApplyCreate(SCounters &c, const std::vector<SClassOps> &create, const char *cls, bool law)
{
	ApplyOps(c, FindClass(create, cls), law);
}

/* The remove count of one class, the destructor's CCarCtrl::UpdateCarCount. */
static void ApplyRemove(SCounters &c, const std::vector<SClassOps> &remove, const char *cls, bool law)
{
	ApplyOps(c, FindClass(remove, cls), law);
}

static void AddInto(SCounters &into, const SCounters &one)
{
	into.random += one.random;
	into.law += one.law;
	into.mission += one.mission;
	into.parked += one.parked;
	into.permanent += one.permanent;
}

/*
 * One restored vehicle, built the way the branch writes it: the constructor
 * counts it as a random car, the branch takes that count back, the saved
 * class arrives (Load in the compatible branch, the copy in the direct
 * branch), and the branch counts the vehicle again - or does not, on the
 * pre-fix code (`recount` false), which is the defect of the ticket.
 */
static SCounters RestoreOne(const std::vector<SClassOps> &create, const char *cls, bool law, bool recount)
{
	SCounters c;	/* CCarCtrl::ReInit: every counter starts at zero */
	ApplyCreate(c, create, "RANDOM_VEHICLE", false);	/* the constructor */
	c.random--;						/* --CCarCtrl::NumRandomCars */
	if (recount)
		ApplyCreate(c, create, cls, law);		/* CCarCtrl::UpdateCarCount(pVehicle, false) */
	return c;
}

/* One saved car: the class the save carries and the law flag. */
struct SSavedCar
{
	const char *cls;
	bool law;
};

static void RunTransition(const std::vector<SClassOps> &create, const std::vector<SClassOps> &remove,
	bool recountInPlace)
{
	printf("B. the transition itself, run with the deltas parsed above\n\n");

	printf("  one restored vehicle, by the class the save carries, the counters after\n");
	printf("  (random/law/mission/parked/permanent):\n");
	printf("  %-22s %-16s %s\n", "saved class", "after the restore", "the count it must hold");
	for (int i = 0; i < nClassNames; i++) {
		for (int l = 0; l < 2; l++) {
			const bool law = l == 1;
			if (strcmp(aClassNames[i], "RANDOM_VEHICLE") != 0 && law)
				continue;
			SCounters got = RestoreOne(create, aClassNames[i], law, recountInPlace);
			SCounters want;
			ApplyCreate(want, create, aClassNames[i], law);
			char after[40], wanted[40];
			sprintf(after, "%d/%d/%d/%d/%d", got.random, got.law, got.mission, got.parked, got.permanent);
			sprintf(wanted, "%d/%d/%d/%d/%d", want.random, want.law, want.mission, want.parked, want.permanent);
			printf("  %-22s %-16s %s\n", law ? "RANDOM_VEHICLE + law" : aClassNames[i], after, wanted);
			Check(SameCounters(got, want), "the restore does not count the class the save carries");
		}
	}
	printf("  the constructor's count and the decrement cancel, so the vehicle holds exactly one\n");
	printf("  count: the one of the class the save carries (plus the law count of a law car).\n\n");

	/* The mixed set: the classes a save can carry, one restore and one
	 * destroy. The law car counts twice in the budget, as a live one does
	 * (PopulationBudget.h). */
	static const SSavedCar aMixed[] = {
		{ "MISSION_VEHICLE", false }, { "MISSION_VEHICLE", false }, { "MISSION_VEHICLE", false },
		{ "PARKED_VEHICLE", false }, { "RANDOM_VEHICLE", true }
	};
	const int nMixed = sizeof(aMixed) / sizeof(aMixed[0]);
	SCounters wantMixed, gotMixed;
	for (int i = 0; i < nMixed; i++) {
		ApplyCreate(wantMixed, create, aMixed[i].cls, aMixed[i].law);
		SCounters one = RestoreOne(create, aMixed[i].cls, aMixed[i].law, recountInPlace);
		AddInto(gotMixed, one);
	}
	printf("  the mixed set: 3 mission + 1 parked + 1 random law car\n");
	printf("    after the restore   %d/%d/%d/%d/%d\n", gotMixed.random, gotMixed.law, gotMixed.mission,
		gotMixed.parked, gotMixed.permanent);
	printf("    the create deltas   %d/%d/%d/%d/%d\n", wantMixed.random, wantMixed.law, wantMixed.mission,
		wantMixed.parked, wantMixed.permanent);
	printf("    NumCarsInUse %d (the law car counts in NumRandomCars and NumLawEnforcerCars, its documented\n",
		gotMixed.InUse());
	printf("    double count in PopulationBudget.h)\n");
	Check(SameCounters(gotMixed, wantMixed), "the mixed set does not hold the sum of its create deltas");
	SCounters destroyed = gotMixed;
	for (int i = 0; i < nMixed; i++)
		ApplyRemove(destroyed, remove, aMixed[i].cls, aMixed[i].law);
	printf("    destroyed           %d/%d/%d/%d/%d\n", destroyed.random, destroyed.law, destroyed.mission,
		destroyed.parked, destroyed.permanent);
	Check(IsZero(destroyed), "the destroyed mixed set does not return the counters to zero");
	printf("\n");

	/* The carried soak: the counters before a save are the counters of the
	 * live set (the normal creation paths count it); the load clears them
	 * and restores the same set, and the next round carries the state it
	 * left. The equality is a real one: a short count fails on the round it
	 * happens. */
	const int rounds = 10;
	const int savedMission = 7;
	SCounters live;
	for (int i = 0; i < savedMission; i++)
		ApplyCreate(live, create, "MISSION_VEHICLE", false);
	printf("  the carried soak: %d rounds of save -> CCarCtrl::ReInit -> restore -> save again,\n",
		rounds);
	printf("  %d saved mission cars\n", savedMission);
	printf("  %-6s %-30s %s\n", "round", "before the save", "after the load");
	SCounters lastAfter;
	bool acceptance = true;
	for (int r = 0; r < rounds; r++) {
		SCounters after;
		for (int i = 0; i < savedMission; i++) {
			SCounters one = RestoreOne(create, "MISSION_VEHICLE", false, recountInPlace);
			AddInto(after, one);
		}
		if (!SameCounters(after, live))
			acceptance = false;
		if (r == 0 || r == rounds - 1) {
			char before[48], a[48];
			sprintf(before, "mission %d, cars in use %d", live.mission, live.InUse());
			sprintf(a, "mission %d, cars in use %d", after.mission, after.InUse());
			printf("  %-6d %-30s %s\n", r + 1, before, a);
		}
		live = after;	/* the next round saves the set the load left */
		lastAfter = after;
	}
	printf("  every round: the counters after the load equal the counters before the save;\n");
	printf("  the whole set is carried, so a lost count fails on its round\n");
	Check(acceptance, "the counters after the load do not equal the counters before the save");

	/* The recorded pre-fix sequence, for the contrast: the decrement and
	 * no recount lose the whole restored set on the first load. */
	SCounters preBefore;
	for (int i = 0; i < savedMission; i++)
		ApplyCreate(preBefore, create, "MISSION_VEHICLE", false);
	SCounters preAfter;
	for (int i = 0; i < savedMission; i++) {
		SCounters one = RestoreOne(create, "MISSION_VEHICLE", false, false);
		AddInto(preAfter, one);
	}
	printf("  the pre-fix sequence: before the first save mission %d, after the load mission %d,\n",
		preBefore.mission, preAfter.mission);
	printf("  cars in use %d -> %d (the set is lost; later rounds carry the zero)\n",
		preBefore.InUse(), preAfter.InUse());
	Check(preAfter.mission == 0 && !SameCounters(preAfter, preBefore),
		"the recorded pre-fix sequence is expected to lose the restored set");

	printf("  the acceptance: the counters after the load equal the counters before the save -\n");
	printf("  NumMissionCars before the save is %d (the saved set of the round), the load restores\n", savedMission);
	printf("  %d, and after it the counter reads %d.\n", savedMission, lastAfter.mission);
	printf("  The live soak (the running game, a real save and load) is the acceptance of the live\n");
	printf("  rig; utils/caps-audit/live-counters.py prints NumMissionCars and the other counters\n");
	printf("  NumCarsInUse sums (NumRandomCars, NumLawEnforcerCars, NumParkedCars and the two\n");
	printf("  emergency counters; NumPermanentCars is printed too and stays outside the sum).\n\n");
}

int main(void)
{
	printf("reVC save-load vehicle counter evidence (VICE-163)\n");
	printf("the restore builds with RANDOM_VEHICLE, takes that count back and gives the one\n");
	printf("count of the vehicle to the class the save carries; this program reads the sources\n");
	printf("that own the transition and runs it as they write it.\n\n");

	SSource pools, carCtrl, vehicle, game, storage;
	std::vector<SClassOps> create, remove;
	bool recountInPlace = false;
	if (!ReadSource("src/core/Pools.cpp", pools) ||
	    !ReadSource("src/control/CarCtrl.cpp", carCtrl) ||
	    !ReadSource("src/vehicles/Vehicle.cpp", vehicle) ||
	    !ReadSource("src/core/Game.cpp", game) ||
	    !ReadSource("src/save/GenericGameStorage.cpp", storage)) {
		Fail("the sources cannot be read (run the program from the repository root)");
	} else if (CheckSources(pools, carCtrl, vehicle, game, storage, create, remove, recountInPlace)) {
		RunTransition(create, remove, recountInPlace);
	}

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  the restore counts every restored vehicle once in the class the save carries;\n");
		printf("  the counters after a load equal the counters before the save, the mixed set and\n");
		printf("  the destroyed set close, and the pre-fix sequence is shown short by the restored\n");
		printf("  set. Run the live soak for the full acceptance.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
