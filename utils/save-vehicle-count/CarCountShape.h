/*
 * The shape of CCarCtrl::UpdateCarCount, read from the source as text.
 *
 * The one parser for the two offline programs that rest on the count:
 * utils/save-vehicle-count/evidence.cpp (VICE-163, the restore gives the
 * vehicle its one count) and utils/spawn-law-counter/evidence.cpp (VICE-166,
 * the law flag is counted apart from the class). The programs used to carry
 * their own parses and drifted apart when the law count moved to the prefix of
 * both sides; both read this one now (see the build lines of the two
 * programs, -I utils/save-vehicle-count).
 *
 * The shape it reports, per side of the count (remove and create):
 *
 *   - the prefix ops: the count ops before the first case. The law count lives
 *     here in the VICE-166 shape, so it applies to every class the count sees.
 *   - the case ops: the ops of each case block, keyed by the class name of the
 *     case label (SClassOps::index is the 0-based line of the label).
 *   - the far layer: FAR_TRAFFIC_VEHICLE returns before the count (the
 *     VICE-128 no-op) when its test sits above the first op and returns.
 *
 * Every op carries the guard text between it and the previous op or the side's
 * start line: whenLaw (a pVehicle->bIsLawEnforcer test above it) and classGuard
 * (a pVehicle->VehicleCreatedBy test above it). A law op is also reported as
 * an SLawStatement with the guard line itself (guardExact: the guard is
 * exactly "if (pVehicle->bIsLawEnforcer)"), whether a class test sits between
 * the side's start and the guard (classAbove: a class gate around the law),
 * whether the op sits before the switch (inPrefix) and the statement's line
 * span (firstIndex..lastIndex), so a check can move or replay it.
 *
 * The shapes the parser rests on (an edit that breaks them fails the read):
 *   - the body of CCarCtrl::UpdateCarCount closes with a brace at column 0;
 *   - the remove and the create halves split at a line that is exactly "else{";
 *   - a count op is a line with "++Num" or "--Num";
 *   - a case is a line whose trim starts with "case " and it ends the guard
 *     walk of the ops below it;
 *   - a class gate around a law statement is an if (...) test with a
 *     VehicleCreatedBy text between the side's start and the statement's
 *     guard.
 *
 * EffectiveOps() says what the count does for one class: the far layer
 * nothing when its early return is in place, the prefix ops for every other
 * class, then the class's case ops.
 */

#ifndef CAR_COUNT_SHAPE_H
#define CAR_COUNT_SHAPE_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

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

/* --------------------------------------------- the deltas of the count */

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
	int index;		/* 0-based line of the case label */
};

/* A law count op, with the statement that holds it. The indices are 0-based
 * line numbers of the file the shape was read from. */
struct SLawStatement
{
	int create;		/* 1 = the create side, 0 = the remove side */
	SCountOp op;
	bool inPrefix;		/* the op sits before the switch (the VICE-166 shape) */
	int guardIndex;		/* the line of the statement's if (...), -1 when none */
	bool guardExact;	/* that if is exactly if (pVehicle->bIsLawEnforcer) */
	bool classAbove;	/* a VehicleCreatedBy test sits between the side's start and the guard */
	int firstIndex, lastIndex;	/* the statement's own lines */
};

struct SCountShape
{
	int begin, split, end;		/* 0-based: the signature, "else{", the closing brace */
	int removeStart, createStart;	/* 0-based: "if (remove)" and "else{" */
	std::vector<SClassOps> create, remove;
	std::vector<SCountOp> createPrefix, removePrefix;
	std::vector<SLawStatement> law;	/* one entry per law op of the function */
	bool farEarlyReturn;		/* FAR_TRAFFIC_VEHICLE returns before the first op */
	int farTestIndex;		/* 0-based, -1 when the function holds no such test */
};

/* The five classes of the count, in the order of the enum (Vehicle.h). */
static const char *aClassNames[] = {
	"RANDOM_VEHICLE", "MISSION_VEHICLE", "PARKED_VEHICLE",
	"PERMANENT_VEHICLE", "FAR_TRAFFIC_VEHICLE"
};
static const int nClassNames = sizeof(aClassNames) / sizeof(aClassNames[0]);

static bool IsCountOpLine(const std::string &line)
{
	return Contains(line, "++Num") || Contains(line, "--Num");
}

static bool IsIfLine(const std::string &t)
{
	return (t.size() > 3 && t.compare(0, 3, "if ") == 0 && t[3] == '(') ||
		(t.size() > 3 && t.compare(0, 3, "if(") == 0);
}

static SLawStatement ParseLawStatement(const SSource &src, int startLine, int opIndex,
	const SCountOp &op, bool create, bool inPrefix)
{
	SLawStatement ls;
	ls.create = create;
	ls.op = op;
	ls.inPrefix = inPrefix;
	ls.guardIndex = -1;
	ls.guardExact = false;
	ls.classAbove = false;
	ls.firstIndex = -1;
	ls.lastIndex = -1;

	/* The statement's guard: the nearest if (...) above the op, up to the
	 * previous op or the case label. */
	for (int j = opIndex - 1; j > startLine; j--) {
		std::string p = Trim(src.lines[j]);
		if (p.size() > 5 && p.compare(0, 5, "case ") == 0)
			break;
		if (IsCountOpLine(src.lines[j]))
			break;
		if (p.size() > 3 && p.compare(0, 3, "if ") == 0 && p.find(')') != std::string::npos) {
			ls.guardIndex = j;
			break;
		}
	}
	if (ls.guardIndex < 0)
		return ls;

	std::string guard = Trim(src.lines[ls.guardIndex]);
	std::string cond = guard;
	if (!cond.empty() && cond[cond.size() - 1] == '{') {
		cond.erase(cond.size() - 1);
		while (!cond.empty() && (cond[cond.size() - 1] == ' ' || cond[cond.size() - 1] == '\t'))
			cond.erase(cond.size() - 1);
	}
	ls.guardExact = cond == "if (pVehicle->bIsLawEnforcer)";
	for (int j = startLine + 1; j < ls.guardIndex; j++) {
		std::string p = Trim(src.lines[j]);
		if (IsIfLine(p) && Contains(src.lines[j], "VehicleCreatedBy"))
			ls.classAbove = true;
	}

	/* The statement's own lines: the guard and its body, to its closing
	 * brace, or to the semicolon of a brace-less body. */
	if (!guard.empty() && guard[guard.size() - 1] == '{') {
		int depth = 0;
		int j = ls.guardIndex;
		for (; j < (int)src.lines.size(); j++) {
			const std::string &s = src.lines[j];
			for (size_t k = 0; k < s.size(); k++) {
				if (s[k] == '{')
					depth++;
				else if (s[k] == '}') {
					depth--;
					if (depth == 0)
						break;
				}
			}
			if (depth == 0)
				break;
		}
		ls.firstIndex = ls.guardIndex;
		ls.lastIndex = j;
	} else {
		ls.firstIndex = ls.guardIndex;
		int j = ls.guardIndex;
		while (j < (int)src.lines.size() && src.lines[j].find(';') == std::string::npos)
			j++;
		ls.lastIndex = j;
	}
	return ls;
}

/* One side of the count: the case blocks and the ops before the first case.
 * A law op is reported as an SLawStatement as well. */
static void ParseCountSide(const SSource &src, int startLine, int endLine, bool create,
	std::vector<SClassOps> &classes, std::vector<SCountOp> &prefix, std::vector<SLawStatement> &law)
{
	classes.clear();
	prefix.clear();
	for (int i = startLine + 1; i < endLine; i++) {
		std::string t = Trim(src.lines[i]);
		if (t.size() > 5 && t.compare(0, 5, "case ") == 0) {
			SClassOps c;
			size_t colon = t.find(':');
			c.cls = t.substr(5, colon - 5);
			c.index = i;
			classes.push_back(c);
			continue;
		}
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
		op.whenLaw = false;
		op.classGuard = false;
		/* The guard of the op: the text between it and the previous count
		 * op (or the case line or the side's start). The law count is the
		 * one op under the bIsLawEnforcer guard; a counter may not sit
		 * under a guard on the created-by class of its own. */
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
		if (classes.empty())
			prefix.push_back(op);
		else
			classes.back().ops.push_back(op);
		if (op.counter == "NumLawEnforcerCars")
			law.push_back(ParseLawStatement(src, startLine, i, op, create, classes.empty()));
	}
}

/* The shape of CCarCtrl::UpdateCarCount: the body, the else{ split, both
 * sides and the far layer's early return. */
static bool ParseUpdateCarCount(const SSource &src, SCountShape &shape)
{
	shape = SCountShape();
	shape.farTestIndex = -1;
	if (!FindBody(src, "CCarCtrl::UpdateCarCount(CVehicle* pVehicle, bool remove)", shape.begin, shape.end))
		return false;
	for (int i = shape.begin + 1; i < shape.end; i++)
		if (Trim(src.lines[i]) == "else{") {
			shape.split = i;
			break;
		}
	if (shape.split <= 0)
		return false;
	shape.removeStart = FindInBody(src, shape.begin, shape.split, "if (remove)", shape.begin);
	if (shape.removeStart < 0)
		return false;
	shape.createStart = shape.split;
	ParseCountSide(src, shape.removeStart, shape.split, false, shape.remove, shape.removePrefix, shape.law);
	ParseCountSide(src, shape.createStart, shape.end, true, shape.create, shape.createPrefix, shape.law);

	int firstOp = -1;
	for (int i = shape.begin + 1; i < shape.end; i++)
		if (IsCountOpLine(src.lines[i])) {
			firstOp = i;
			break;
		}
	for (int i = shape.begin + 1; i < shape.end; i++)
		if (Contains(src.lines[i], "FAR_TRAFFIC_VEHICLE")) {
			shape.farTestIndex = i;
			break;
		}
	shape.farEarlyReturn = shape.farTestIndex >= 0 && (firstOp < 0 || shape.farTestIndex < firstOp);
	if (shape.farEarlyReturn) {
		bool returns = false;
		for (int j = shape.farTestIndex; j < shape.end && j < shape.farTestIndex + 3; j++)
			if (Contains(src.lines[j], "return"))
				returns = true;
		shape.farEarlyReturn = returns;
	}
	return true;
}

static const SClassOps *FindClass(const std::vector<SClassOps> &classes, const char *name)
{
	for (size_t i = 0; i < classes.size(); i++)
		if (classes[i].cls == name)
			return &classes[i];
	return NULL;
}

/* The ops the count applies for one class: nothing for the far layer when its
 * early return is in place, the prefix ops for every other class, then the
 * class's own case ops. */
static std::vector<SCountOp> EffectiveOps(const SCountShape &shape, bool create, const char *cls)
{
	std::vector<SCountOp> ops;
	if (strcmp(cls, "FAR_TRAFFIC_VEHICLE") == 0 && shape.farEarlyReturn)
		return ops;
	const std::vector<SCountOp> &prefix = create ? shape.createPrefix : shape.removePrefix;
	ops.insert(ops.end(), prefix.begin(), prefix.end());
	const SClassOps *c = FindClass(create ? shape.create : shape.remove, cls);
	if (c)
		ops.insert(ops.end(), c->ops.begin(), c->ops.end());
	return ops;
}

/* The law statement is shaped as VICE-166 requires: its own guard is exactly
 * if (pVehicle->bIsLawEnforcer), and no class test gates it from above. */
static bool LawStatementShapeOk(const SLawStatement &ls)
{
	return ls.guardIndex >= 0 && ls.guardExact && !ls.classAbove;
}

#endif
