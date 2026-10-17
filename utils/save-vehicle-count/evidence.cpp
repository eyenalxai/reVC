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
 *      branches of LoadVehiclePool), Vehicle.cpp (the constructor, Load and
 *      Save) and CarCtrl.cpp (UpdateCarCount, Init/ReInit) - and checks the
 *      shape of the transition in both branches: every construction uses
 *      RANDOM_VEHICLE, one manual NumRandomCars decrement takes the
 *      constructor's count back, the saved class arrives before the
 *      recount, and exactly one UpdateCarCount(pVehicle, false) follows it.
 *      An edit that drops the recount, duplicates it, moves it before the
 *      class or strays a counter write fails here.
 *
 *   B. It runs the transition as the sources write it: the counter deltas
 *      are parsed from CCarCtrl::UpdateCarCount and the order from
 *      LoadVehiclePool, and the counters are followed through a save/load
 *      soak. The counters after every load equal the counters of the saved
 *      set (the acceptance of the ticket), for every class a save can carry
 *      and for a law enforcer, which counts once as a random car and once
 *      as a law enforcer, the same as a live one. The recorded pre-fix
 *      sequence is run side by side: it nets zero per vehicle, so the
 *      soak shows NumMissionCars at zero and NumCarsInUse short by the
 *      restored set every round.
 *
 * What the program cannot measure: the counters of the running game across
 * a real save and load. That live soak is the acceptance of the live rig
 * (the owner's run, or a read of the running process as
 * utils/caps-audit/live-counters.py does for the traffic counters); this
 * program is the offline half. The call sites of UpdateCarCount outside the
 * restore (the replay player, the far layer's class switch) run on live
 * vehicles, not on a restored one.
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
	std::string path;
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

	src.path = path;
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

/* --------------------------------------------- A. the deltas of the count */

/* One counter the count moves, with the line that moves it. */
struct SCountOp
{
	std::string counter;	/* NumRandomCars, NumLawEnforcerCars, ... */
	int sign;		/* +1 (++) or -1 (--) */
	bool whenLaw;		/* the op runs only for a law enforcer */
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
		/* The law count is the one conditional op of the function: the
		 * random case adds it under `if (pVehicle->bIsLawEnforcer)`. */
		op.whenLaw = op.counter == "NumLawEnforcerCars";
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

static std::vector<SEvent> ScanBranch(const SSource &src, int first, int last)
{
	std::vector<SEvent> events;
	for (int i = first; i <= last; i++) {
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
	}
}

static bool CheckSources(SSource &pools, SSource &carCtrl, SSource &vehicle, std::vector<SClassOps> &createOut,
	bool &recountInPlace)
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
		const int ret = FindInBody(pools, begin, end,
			"return pVehicle->VehicleCreatedBy == MISSION_VEHICLE;", begin);
		printf("  src/core/Pools.cpp: IsVehicleSaved (lines %d..%d) carries only mission vehicles\n",
			begin + 1, end + 1);
		Check(ret >= 0, "IsVehicleSaved returns true only for a MISSION_VEHICLE");
	} else
		Fail("Pools.cpp: IsVehicleSaved is not found");
	printf("\n");

	/* --- CVehicle: the constructor, Load, Save, the destructor --- */
	if (!FindBody(vehicle, "CVehicle::CVehicle(uint8 CreatedBy)", begin, end)) {
		Fail("Vehicle.cpp: the CVehicle constructor is not found");
		return false;
	}
	const int setClass = FindInBody(vehicle, begin, end, "VehicleCreatedBy = CreatedBy;", begin);
	const int lawFalse = FindInBody(vehicle, begin, end, "bIsLawEnforcer = false;", begin);
	const int ctorCount = FindInBody(vehicle, begin, end, "CCarCtrl::UpdateCarCount(this, false);", begin);
	printf("  src/vehicles/Vehicle.cpp: the constructor (lines %d..%d)\n", begin + 1, end + 1);
	printf("    the class (line %d) and the law flag (line %d) are set before the count (line %d)\n",
		setClass + 1, lawFalse + 1, ctorCount + 1);
	Check(setClass >= 0 && lawFalse >= 0 && ctorCount >= 0,
		"the constructor still sets the class, the law flag and the count");
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
		Check(dtor >= 0, "the destructor takes the count back with the class of the vehicle");
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

	/* The deltas, class by class, against the table the program runs. */
	static const SClassOps aExpected[] = {
		/* RANDOM: a law enforcer is counted twice, as the live path counts it. */
		{ "RANDOM_VEHICLE",      { { "NumLawEnforcerCars", +1, true, 0 }, { "NumRandomCars", +1, false, 0 } } },
		{ "MISSION_VEHICLE",     { { "NumMissionCars", +1, false, 0 } } },
		{ "PARKED_VEHICLE",      { { "NumParkedCars", +1, false, 0 } } },
		{ "PERMANENT_VEHICLE",   { { "NumPermanentCars", +1, false, 0 } } },
		{ "FAR_TRAFFIC_VEHICLE", { } },
	};
	for (int i = 0; i < nClassNames; i++) {
		const SClassOps *got = FindClass(create, aExpected[i].cls.c_str());
		Check(got != NULL, "a class of the count table is missing");
		if (got) {
			Check(got->ops.size() == aExpected[i].ops.size(),
				"a create delta of the count table is missing");
			for (size_t j = 0; j < got->ops.size() && j < aExpected[i].ops.size(); j++)
				Check(got->ops[j].counter == aExpected[i].ops[j].counter &&
					got->ops[j].sign == aExpected[i].ops[j].sign &&
					got->ops[j].whenLaw == aExpected[i].ops[j].whenLaw,
					"a create delta of the count table is not the expected one");
		}
		/* the remove side is the mirror of the create side */
		const SClassOps *gone = FindClass(remove, aExpected[i].cls.c_str());
		Check(gone != NULL, "a class of the remove table is missing");
		if (got && gone) {
			Check(gone->ops.size() == got->ops.size(), "the remove table does not mirror the create table");
			for (size_t j = 0; j < gone->ops.size() && j < got->ops.size(); j++)
				Check(gone->ops[j].counter == got->ops[j].counter &&
					gone->ops[j].sign == -got->ops[j].sign &&
					gone->ops[j].whenLaw == got->ops[j].whenLaw,
					"a remove delta is not the mirror of the create delta");
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
			Check(FindInBody(carCtrl, begin, end, want, begin) >= 0,
				"ReInit still zeroes every class counter (the load clears before the restore)");
		}
		printf("    CCarCtrl::ReInit (line %d) zeroes every class counter\n", begin + 1);
	} else
		Fail("CarCtrl.cpp: CCarCtrl::ReInit is not found");

	if (FindBody(carCtrl, "CCarCtrl::NumCarsInUse(void)", begin, end)) {
		/* The budget of the car spawner: the four class counters of the count
		 * plus the two emergency counters. The permanent class stays outside
		 * it. */
		static const char *aSummed[] = {
			"NumFiretrucksOnDuty", "NumAmbulancesOnDuty", "NumParkedCars",
			"NumMissionCars", "NumLawEnforcerCars", "NumRandomCars"
		};
		for (int i = 0; i < 6; i++)
			Check(FindInBody(carCtrl, begin, end, aSummed[i], begin) >= 0,
				"NumCarsInUse still sums the counters a restored car must reach");
		printf("    CCarCtrl::NumCarsInUse (line %d) sums the four class counters and the two\n", begin + 1);
		printf("    emergency counters; a restored mission car reaches the car budget through it\n");
	} else
		Fail("CarCtrl.cpp: CCarCtrl::NumCarsInUse is not found");

	/* The caller of ReInit on the load path. */
	SSource game;
	if (ReadSource("src/core/Game.cpp", game)) {
		if (FindBody(game, "CGame::ReInitGameObjectVariables(void)", begin, end)) {
			Check(FindInBody(game, begin, end, "CCarCtrl::ReInit();", begin) >= 0,
				"ReInitGameObjectVariables still clears the counters on the load path");
			printf("    the load path: CGame::ReInitGameObjectVariables (Game.cpp line %d) calls CCarCtrl::ReInit\n",
				begin + 1);
		} else
			Fail("Game.cpp: CGame::ReInitGameObjectVariables is not found");
	} else
		Fail("Game.cpp cannot be read");
	printf("\n");

	createOut = create;
	return true;
}

/* ------------------------------------------- B. the transition, run offline */

struct SCounters
{
	int random, law, mission, parked, permanent;
	SCounters() : random(0), law(0), mission(0), parked(0), permanent(0) {}
	/* CCarCtrl::NumCarsInUse: the four class counters it sums (the
	 * permanent class stays outside the budget; the two emergency counters
	 * are zero through a restore - the save carries no duty flags). */
	int InUse() const { return random + law + mission + parked; }
};

static bool SameCounters(const SCounters &a, const SCounters &b)
{
	return a.random == b.random && a.law == b.law && a.mission == b.mission &&
		a.parked == b.parked && a.permanent == b.permanent;
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

/* The create count of one class, the way CCarCtrl::UpdateCarCount applies it. */
static void ApplyCreate(SCounters &c, const std::vector<SClassOps> &create, const char *cls, bool law)
{
	const SClassOps *ops = FindClass(create, cls);
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

static void RunTransition(const std::vector<SClassOps> &create, bool recountInPlace)
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

	/* The soak: save S mission cars, clear the counters, restore, repeat. */
	const int rounds = 10;
	const int savedMission = 7;
	printf("  the soak: %d rounds of save -> CCarCtrl::ReInit -> restore, %d saved mission cars\n",
		rounds, savedMission);
	printf("  %-6s %-30s %s\n", "round", recountInPlace ? "after the fix" : "as written (no recount)",
		"the pre-fix sequence");
	SCounters lastAfter, lastPre;
	bool acceptance = true, stable = true;
	for (int r = 0; r < rounds; r++) {
		SCounters after, pre;
		for (int i = 0; i < savedMission; i++) {
			SCounters one = RestoreOne(create, "MISSION_VEHICLE", false, recountInPlace);
			AddInto(after, one);
			one = RestoreOne(create, "MISSION_VEHICLE", false, false);
			AddInto(pre, one);
		}
		if (r > 0 && !SameCounters(after, lastAfter))
			stable = false;
		lastAfter = after;
		lastPre = pre;
		if (r == 0 || r == rounds - 1) {
			char a[48], p[48];
			sprintf(a, "mission %d, cars in use %d", after.mission, after.InUse());
			sprintf(p, "mission %d, cars in use %d", pre.mission, pre.InUse());
			printf("  %-6d %-30s %s\n", r + 1, a, p);
		}
		if (after.mission != savedMission || after.InUse() != savedMission)
			acceptance = false;
	}
	printf("  every round of the restore: NumMissionCars %d of %d, NumCarsInUse %d of %d (one count per\n",
		lastAfter.mission, savedMission, lastAfter.InUse(), savedMission);
	printf("  restored car); the pre-fix sequence: NumMissionCars %d, NumCarsInUse %d every round\n",
		lastPre.mission, lastPre.InUse());
	Check(acceptance, "the counters after the load do not equal the counters of the saved set");
	Check(stable, "the counters of the soak drift between rounds (the fix)");
	Check(lastPre.mission == 0, "the recorded pre-fix sequence is expected to leave the mission count at zero");

	printf("  the acceptance: the counters after the load equal the counters of the saved set -\n");
	printf("  NumMissionCars before the save is %d (the saved set of the round), the load restores\n", savedMission);
	printf("  %d, and after it the counter reads %d. The live soak (the running game, a real save\n",
		savedMission, lastAfter.mission);
	printf("  and load) is the acceptance of the live rig; the counters it reads are\n");
	printf("  CCarCtrl::NumMissionCars and CCarCtrl::NumCarsInUse.\n\n");
}

int main(void)
{
	printf("reVC save-load vehicle counter evidence (VICE-163)\n");
	printf("the restore builds with RANDOM_VEHICLE, takes that count back and gives the one\n");
	printf("count of the vehicle to the class the save carries; this program reads the three\n");
	printf("sources that own the transition and runs it as they write it.\n\n");

	SSource pools, carCtrl, vehicle;
	std::vector<SClassOps> create;
	bool recountInPlace = false;
	if (!ReadSource("src/core/Pools.cpp", pools) ||
	    !ReadSource("src/control/CarCtrl.cpp", carCtrl) ||
	    !ReadSource("src/vehicles/Vehicle.cpp", vehicle)) {
		Fail("the sources cannot be read (run the program from the repository root)");
	} else if (CheckSources(pools, carCtrl, vehicle, create, recountInPlace)) {
		RunTransition(create, recountInPlace);
	}

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  the restore counts every restored vehicle once in the class the save carries;\n");
		printf("  the counters after a load equal the counters of the saved set, and the pre-fix\n");
		printf("  sequence is shown short by the restored set. Run the live soak for the full\n");
		printf("  acceptance.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
