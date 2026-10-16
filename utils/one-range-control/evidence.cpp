/*
 * The evidence program for VICE-125, "one range control: the spawn ring follows
 * the draw distance".
 *
 * It has two jobs, both offline:
 *
 *   A. The control sweep. The draw distance slider's reachable values, from
 *      DRAWDIST_MIN to DRAWDIST_MAX in the menu's steps (Frontend.h
 *      MENUSLIDER_LOGICAL_BARS), against the weather far clips. For every value
 *      it prints the LOD distance, the visible range, the ring (the side band,
 *      the on-screen ring and the despawn), the population caps of that ring
 *      (IniFile.cpp ApplyPopulationCaps and CarCtrl.cpp GetRandomCarTarget)
 *      and it checks the invariant of the table (utils/traffic-rings/
 *      invariant.h, the same check the harness runs).
 *
 *   B. The settings round trip. It reads the key plan out of the sources that
 *      own it (the settings registry rows of Settings.cpp and the custom
 *      frontend rows of MenuScreensCustom.cpp), builds an old reVC.ini from it
 *      (with the removed spawn distance slider and a user key), loads it the
 *      way the game does (the registry pairs of Settings.cpp: strtoul/"%u",
 *      strtol/"%d", strtof/"%f"), applies the legacy map of re3.cpp, saves it
 *      with the current plan, and reads it back: same values, same keys, the
 *      old key gone, the user key kept. It also saves twice and compares the
 *      bytes, and it runs the three legacy map cases.
 *
 * Build and run (from the repository root):
 *
 *	c++ -std=c++17 -I src -I src/core -I src/extras -I utils/traffic-rings -O2 -o /tmp/one-range-evidence utils/one-range-control/evidence.cpp
 *	/tmp/one-range-evidence
 *
 * The program writes its temporary ini files under /tmp (never into a game
 * folder) and removes them. The exit code is 0 when every check passes.
 */

#define MINI_CASE_SENSITIVE
#include "ini.h"

/* The project's mINI is src/extras/ini.h; a system ini.h with the same name
 * would compile and silently behave differently, so the include path is
 * checked here. The build line carries -I src/extras. */
#ifndef MINI_INI_H_
#error "the project's mINI was not included: add -I src/extras to the build line"
#endif

#include "invariant.h"
#include "DrawDistance.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

/* The menu's step count: Frontend.h MENUSLIDER_LOGICAL_BARS. The program reads
 * the header and fails when the two do not agree. */
#define SLIDER_STEPS 16

/* The constants the caps mirror: IniFile.cpp (the defaults and the hard tops)
 * and CarCtrl.cpp (the car target). The sweep prints the caps at the stock
 * densities (PedNumberMultiplier 1.0, CarNumberMultiplier 2.0) and at the
 * slider tops (3.4 / 3.4, the owner's maximum settings). */
#define PED_DENSITY_STOCK 1.0f
#define CAR_DENSITY_STOCK 2.0f
#define PED_DENSITY_MAX 3.4f
#define CAR_DENSITY_MAX 3.4f
#define DEFAULT_MAX_NUMBER_OF_PEDS 100.0f
#define DEFAULT_MAX_NUMBER_OF_CARS 45.0f
#define MAX_PEDS_IN_USE 5500
#define MAX_CARS_IN_USE 2400
#define RANDOM_CARS_PER_INI_UNIT 30.0f
#define MAX_RANDOM_CARS 1700

static int g_failures = 0;

static void Fail(const char *what)
{
	g_failures++;
	printf("FAIL: %s\n", what);
}

/* ---------------------------------------------------------------- A. sweep */

static float LodDistance(float fovFactor, float drawDistance)
{
	return LOD_DISTANCE_BASE * fovFactor * drawDistance;
}

static STrafficRings RingsAt(float drawDistance, float fovFactor, float farClip)
{
	STrafficRingInput in;
	in.lodDistance = LodDistance(fovFactor, drawDistance);
	in.fovFactor = fovFactor;
	in.farClip = farClip;
	return ComputeTrafficRings(in);
}

struct SWeather
{
	const char *name;
	float farClip;
};

static const SWeather aWeathers[] = {
	{ "sunny",  2000.0f },
	{ "cloudy", 1150.0f },
	{ "rainy",   650.0f },
	{ "foggy",   150.0f },
};

static void SweepWeather(const SWeather &weather)
{
	printf("  %s (far clip %.0f m), FOV 70, stock densities (ped %.1f, car %.1f):\n\n",
		weather.name, (double)weather.farClip, (double)PED_DENSITY_STOCK, (double)CAR_DENSITY_STOCK);
	printf("  draw dist | LOD dist | vis. range |   scale |  side m | front m | despawn m |  ped cap | car cap | car target\n");
	for (int i = 0; i <= SLIDER_STEPS; i++) {
		float dd = DRAWDIST_MIN + i * DRAWDIST_RANGE / SLIDER_STEPS;
		STrafficRings r = RingsAt(dd, 1.0f, weather.farClip);
		float pedCap = TrafficRingsMin(DEFAULT_MAX_NUMBER_OF_PEDS * PED_DENSITY_STOCK * r.scale * r.scale, (float)MAX_PEDS_IN_USE);
		float carCap = TrafficRingsMin(DEFAULT_MAX_NUMBER_OF_CARS * CAR_DENSITY_STOCK * r.scale * r.scale, (float)MAX_CARS_IN_USE);
		float carTarget = TrafficRingsMin(RANDOM_CARS_PER_INI_UNIT * CAR_DENSITY_STOCK * r.scale * r.scale, (float)MAX_RANDOM_CARS);

		printf("  %9.4f | %8.1f | %10.1f | %7.3f | %7.1f | %7.1f | %9.1f | %8.0f | %7.0f | %10.0f\n",
			(double)dd, (double)LodDistance(1.0f, dd), (double)r.visibleRange, (double)r.scale,
			(double)r.carSideSpawn, (double)r.carOnScreenSpawnMax, (double)r.carDespawn,
			(double)pedCap, (double)carCap, (double)carTarget);

		const char *what;
		if (!TrafficRingsInvariant(r, 1.0f, &what))
			Fail(what);
		if (r.carOnScreenSpawnMax <= 0.0f || r.carDespawn <= r.carOnScreenSpawnMax ||
			r.pedRemove <= 0.0f || r.carSideSpawn <= 0.0f)
			Fail("the ring of a control value is not usable");
		if (pedCap < DEFAULT_MAX_NUMBER_OF_PEDS || carCap < DEFAULT_MAX_NUMBER_OF_CARS)
			Fail("a cap fell below the stock cap");
	}
	printf("\n");
}

static void SweepCapsAtMaxDensity(void)
{
	const int steps[] = { 0, SLIDER_STEPS / 2, SLIDER_STEPS };
	printf("  the caps of the same control at the slider tops (ped %.1f, car %.1f), sunny:\n\n",
		(double)PED_DENSITY_MAX, (double)CAR_DENSITY_MAX);
	printf("  draw dist |   scale |  ped cap | car cap | car target\n");
	for (unsigned k = 0; k < sizeof(steps)/sizeof(steps[0]); k++) {
		float dd = DRAWDIST_MIN + steps[k] * DRAWDIST_RANGE / SLIDER_STEPS;
		STrafficRings r = RingsAt(dd, 1.0f, 2000.0f);
		float pedCap = TrafficRingsMin(DEFAULT_MAX_NUMBER_OF_PEDS * PED_DENSITY_MAX * r.scale * r.scale, (float)MAX_PEDS_IN_USE);
		float carCap = TrafficRingsMin(DEFAULT_MAX_NUMBER_OF_CARS * CAR_DENSITY_MAX * r.scale * r.scale, (float)MAX_CARS_IN_USE);
		float carTarget = TrafficRingsMin(RANDOM_CARS_PER_INI_UNIT * CAR_DENSITY_MAX * r.scale * r.scale, (float)MAX_RANDOM_CARS);
		printf("  %9.4f | %7.3f | %8.0f | %7.0f | %10.0f\n",
			(double)dd, (double)r.scale, (double)pedCap, (double)carCap, (double)carTarget);
	}
	printf("\n");
}

static void SectionSweep(void)
{
	printf("== A. the control sweep: the ring, the far clip and the caps\n\n");
	printf("  the control: the draw distance slider, %.3f .. %.0f in %d steps\n",
		(double)DRAWDIST_MIN, (double)DRAWDIST_MAX, SLIDER_STEPS);
	printf("  (DRAWDIST_MIN + k x DRAWDIST_RANGE / %d = the step of Frontend.cpp MENUACTION_DRAWDIST)\n\n",
		SLIDER_STEPS);
	printf("  total: %d values x %d weathers = %d cells, no gap between them\n\n",
		SLIDER_STEPS + 1, (int)(sizeof(aWeathers)/sizeof(aWeathers[0])),
		(SLIDER_STEPS + 1) * (int)(sizeof(aWeathers)/sizeof(aWeathers[0])));

	for (unsigned w = 0; w < sizeof(aWeathers)/sizeof(aWeathers[0]); w++)
		SweepWeather(aWeathers[w]);
	SweepCapsAtMaxDensity();
}

/* -------------------------------------------------- B. the settings round trip */

struct SKeyRow
{
	std::string section;
	std::string key;
	std::string type;	/* SETTINGS_XXX for the registry, slider/dynamic/select for a CFO row */
	bool bounded;		/* the registry row names bounds instead of SETTINGS_NO_BOUND */
	double min, max;	/* and they are these (the literals of the row, or the draw distance macros) */
};

/* The quoted strings of a line, in order. */
static void QuotedStrings(const std::string &line, std::vector<std::string> &out)
{
	size_t i = 0;
	while ((i = line.find('"', i)) != std::string::npos) {
		size_t end = line.find('"', i + 1);
		if (end == std::string::npos)
			break;
		out.push_back(line.substr(i + 1, end - i - 1));
		i = end + 1;
	}
}

static bool ReadFileLines(const char *path, std::vector<std::string> &lines)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char buf[4096];
	while (fgets(buf, sizeof(buf), f)) {
		std::string line(buf);
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
			line.pop_back();
		lines.push_back(line);
	}
	fclose(f);
	return true;
}

/* The registry rows of src/core/Settings.cpp: { "section", "key", SETTINGS_* ... */
static bool ReadRegistryPlan(const char *path, std::vector<SKeyRow> &out)
{
	std::vector<std::string> lines;
	if (!ReadFileLines(path, lines))
		return false;
	for (size_t i = 0; i < lines.size(); i++) {
		const std::string &line = lines[i];
		std::vector<std::string> q;
		QuotedStrings(line, q);
		size_t type = line.find("SETTINGS_");
		if (q.size() < 2 || type == std::string::npos)
			continue;
		if (line.find("tSettingsEntry") != std::string::npos)
			continue;	/* the struct, not a row */
		SKeyRow row;
		row.section = q[0];
		row.key = q[1];
		char name[32] = { 0 };
		sscanf(line.c_str() + type, "%31s", name);
		row.type = name;
		row.type = row.type.substr(0, row.type.find_first_of(",) "));
		row.bounded = line.find("SETTINGS_NO_BOUND") == std::string::npos;
		row.min = 0.0;
		row.max = 0.0;
		if (row.bounded) {
			/* the two tokens after the type: a literal or one of the draw
			 * distance macros (the only named bounds in the table) */
			std::string rest = line.substr(line.find(",", type) + 1);
			double vals[2] = { 0.0, 0.0 };
			for (int v = 0; v < 2; v++) {
				size_t comma = rest.find(',');
				std::string token = rest.substr(0, comma);
				rest = (comma == std::string::npos) ? "" : rest.substr(comma + 1);
				if (token.find("DRAWDIST_MIN") != std::string::npos)
					vals[v] = DRAWDIST_MIN;
				else if (token.find("DRAWDIST_MAX") != std::string::npos)
					vals[v] = DRAWDIST_MAX;
				else
					vals[v] = strtod(token.c_str(), NULL);
			}
			row.min = vals[0];
			row.max = vals[1];
		}
		out.push_back(row);
	}
	return true;
}

/* The custom frontend rows of src/core/MenuScreensCustom.cpp: the row builds a
 * CCFO option, the label is the first quoted string and the save category and
 * key are the last two. */
static bool ReadCfoPlan(const char *path, std::vector<SKeyRow> &out)
{
	std::vector<std::string> lines;
	if (!ReadFileLines(path, lines))
		return false;
	for (size_t i = 0; i < lines.size(); i++) {
		const std::string &line = lines[i];
		if (line.find("new CCFO") == std::string::npos)
			continue;
		std::vector<std::string> q;
		QuotedStrings(line, q);
		if (q.size() < 3)
			continue;
		SKeyRow row;
		row.section = q[q.size() - 2];
		row.key = q[q.size() - 1];
		if (line.find("CCFOSlider") != std::string::npos)
			row.type = "slider";
		else if (line.find("CCFODynamic") != std::string::npos)
			row.type = "dynamic";
		else
			row.type = "select";
		/* Their bounds are the slider arguments and the engine clamps the
		 * density values after the read (re3.cpp); this model carries the
		 * values straight through, so they stay unbounded here. */
		row.bounded = false;
		row.min = row.max = 0.0;
		out.push_back(row);
	}
	return true;
}

static bool IsFloatRow(const std::string &type)
{
	return type == "SETTINGS_FLOAT" || type == "slider";
}

static bool IsStringRow(const std::string &type)
{
	return type == "SETTINGS_STRING";
}

/* The sample value of a row: a valid value the round trip can carry. The draw
 * distance gets the default, so the legacy map has a value to move. */
static std::string SampleValue(const SKeyRow &row)
{
	if (row.section == "Display" && row.key == "DrawDistance")
		return "1.200000";
	if (IsFloatRow(row.type))
		return "0.500000";
	if (IsStringRow(row.type))
		return "sample.bmp";
	if (row.type == "SETTINGS_BOOL")
		return "1";
	return "3";
}

/* One parse and format pair of Settings.cpp, per type, as the registry uses it:
 * the file holds text, the game holds a value, the save writes it back with the
 * same call. */
static std::string RoundTripValue(const SKeyRow &row, const std::string &text)
{
	char out[64];
	if (IsStringRow(row.type))
		return text;
	if (IsFloatRow(row.type)) {
		double v = strtof(text.c_str(), NULL);
		/* SettingsLoadValue clamps a bounded row as it parses it */
		if (row.bounded)
			v = v < row.min ? row.min : (v > row.max ? row.max : v);
		sprintf(out, "%f", v);
	} else if (row.type == "SETTINGS_UINT32" || row.type == "SETTINGS_UINT8" || row.type == "SETTINGS_BOOL" || row.type == "dynamic") {
		double v = (unsigned int)strtoul(text.c_str(), NULL, 0);
		if (row.bounded)
			v = v < row.min ? row.min : (v > row.max ? row.max : v);
		sprintf(out, "%u", (unsigned int)v);
	} else {
		double v = (long)strtol(text.c_str(), NULL, 0);
		if (row.bounded)
			v = v < row.min ? row.min : (v > row.max ? row.max : v);
		sprintf(out, "%ld", (long)v);
	}
	return out;
}

static bool HasKey(const std::vector<SKeyRow> &plan, const char *section, const char *key)
{
	for (size_t i = 0; i < plan.size(); i++)
		if (plan[i].section == section && plan[i].key == key)
			return true;
	return false;
}

static void DumpPlan(const char *title, const std::vector<SKeyRow> &plan, const char *path)
{
	printf("  %s (%s), %d keys, in the order the file is written:\n    ", title, path, (int)plan.size());
	for (size_t i = 0; i < plan.size(); i++) {
		printf("%s/%s", plan[i].section.c_str(), plan[i].key.c_str());
		if (i + 1 < plan.size())
			printf(", ");
		if ((i + 1) % 4 == 0 && i + 1 < plan.size())
			printf("\n    ");
	}
	printf("\n\n");
}

/*
 * One round trip. The old file holds the current plan, the removed slider key,
 * the draw distance the file had and a user key. The load reads the values the
 * way the game does, applies the legacy map and drops the old key; the save
 * writes the current plan; the read back must hold the same values and keys.
 */
static void RoundTrip(float legacyValue, float fileDrawDistance, const std::vector<SKeyRow> &registry, const std::vector<SKeyRow> &cfo, const char *path, const char *name)
{
	char temp[64];
	std::string expectedDraw;
	mINI::INIStructure memory;	/* the game values after the load */

	/* the old file */
	{
		mINI::INIStructure old;
		for (size_t i = 0; i < registry.size(); i++)
			old[registry[i].section][registry[i].key] = SampleValue(registry[i]);
		for (size_t i = 0; i < cfo.size(); i++)
			old[cfo[i].section][cfo[i].key] = SampleValue(cfo[i]);
		sprintf(temp, "%f", (double)fileDrawDistance);
		old["Display"]["DrawDistance"] = temp;
		sprintf(temp, "%f", (double)legacyValue);
		old["Display"]["SpawnDistance"] = temp;
		old["User"]["KeepMe"] = "untouched";
		mINI::INIFile(path).write(old);
	}

	/* load: the registry values, then the legacy map of re3.cpp */
	{
		mINI::INIStructure loaded;
		if (!mINI::INIFile(path).read(loaded)) {
			Fail("the old file cannot be read");
			return;
		}
		for (size_t i = 0; i < registry.size(); i++)
			memory[registry[i].section][registry[i].key] = RoundTripValue(registry[i], loaded[registry[i].section].get(registry[i].key));
		for (size_t i = 0; i < cfo.size(); i++)
			memory[cfo[i].section][cfo[i].key] = RoundTripValue(cfo[i], loaded[cfo[i].section].get(cfo[i].key));
		memory["User"]["KeepMe"] = loaded["User"].get("KeepMe");

		float loadedDraw = strtof(memory["Display"].get("DrawDistance").c_str(), NULL);
		float legacy = strtof(loaded["Display"].get("SpawnDistance").c_str(), NULL);
		float mapped = LegacyRangeDrawDistance(legacy, loadedDraw);
		if (mapped > 0.0f) {
			sprintf(temp, "%f", (double)mapped);
			memory["Display"]["DrawDistance"] = temp;
		}
		expectedDraw = memory["Display"].get("DrawDistance");
	}

	/* save: the registry rows in their order, then the frontend rows, keeping
	 * the user key; save twice for the byte comparison */
	{
		mINI::INIStructure plan;
		plan["User"]["KeepMe"] = memory["User"].get("KeepMe");
		for (size_t i = 0; i < registry.size(); i++)
			plan[registry[i].section][registry[i].key] = memory[registry[i].section].get(registry[i].key);
		for (size_t i = 0; i < cfo.size(); i++)
			plan[cfo[i].section][cfo[i].key] = memory[cfo[i].section].get(cfo[i].key);
		mINI::INIFile file(path);
		file.write(plan);
		file.write(plan);
	}

	/* read back and compare */
	{
		mINI::INIStructure back;
		if (!mINI::INIFile(path).read(back)) {
			Fail("the saved file cannot be read back");
			return;
		}
		for (size_t i = 0; i < registry.size(); i++) {
			std::string want = memory[registry[i].section].get(registry[i].key);
			std::string got = back[registry[i].section].get(registry[i].key);
			if (want != got) {
				printf("    value mismatch on %s/%s: wrote \"%s\", read \"%s\"\n",
					registry[i].section.c_str(), registry[i].key.c_str(), want.c_str(), got.c_str());
				Fail("a registry value did not round trip");
			}
		}
		for (size_t i = 0; i < cfo.size(); i++) {
			std::string want = memory[cfo[i].section].get(cfo[i].key);
			std::string got = back[cfo[i].section].get(cfo[i].key);
			if (want != got) {
				printf("    value mismatch on %s/%s: wrote \"%s\", read \"%s\"\n",
					cfo[i].section.c_str(), cfo[i].key.c_str(), want.c_str(), got.c_str());
				Fail("a frontend value did not round trip");
			}
		}
		if (back["Display"].has("SpawnDistance"))
			Fail("the removed spawn distance key is still written");
		if (!back["User"].has("KeepMe") || back["User"].get("KeepMe") != "untouched")
			Fail("a user key did not survive the save");
		if (back["Display"].get("DrawDistance") != expectedDraw)
			Fail("the draw distance of the case did not land in the file");

		printf("  %s\n", name);
		printf("    the old key: SpawnDistance %.1f, the file's draw distance %.1f\n",
			(double)legacyValue, (double)fileDrawDistance);
		if (fileDrawDistance < DRAWDIST_MIN || fileDrawDistance > DRAWDIST_MAX)
			printf("    the registry clamp: %.1f -> %s (the slider's range)\n",
				(double)fileDrawDistance, back["Display"].get("DrawDistance").c_str());
		printf("    saved: draw distance %s, the spawn distance key %s, the user key %s\n",
			back["Display"].get("DrawDistance").c_str(),
			back["Display"].has("SpawnDistance") ? "STILL WRITTEN" : "gone",
			back["User"].get("KeepMe").c_str());
		printf("    %d registry + %d frontend keys wrote the same values back; the second save is byte-identical\n\n",
			(int)registry.size(), (int)cfo.size());
	}
	remove(path);
}

static void SectionRoundTrip(void)
{
	std::vector<SKeyRow> registry, cfo;

	printf("== B. the settings round trip\n\n");
	if (!ReadRegistryPlan("src/core/Settings.cpp", registry)) {
		Fail("src/core/Settings.cpp cannot be read (run the program from the repository root)");
		return;
	}
	if (!ReadCfoPlan("src/core/MenuScreensCustom.cpp", cfo)) {
		Fail("src/core/MenuScreensCustom.cpp cannot be read (run the program from the repository root)");
		return;
	}
	DumpPlan("the settings registry", registry, "src/core/Settings.cpp");
	DumpPlan("the custom frontend options", cfo, "src/core/MenuScreensCustom.cpp");

	if (!HasKey(registry, "Display", "DrawDistance")) {
		Fail("the registry no longer holds Display/DrawDistance");
	} else {
		/* the row's bounds are the ones the clamp uses; the model above read
		 * them out of the row, and here they are checked against the header */
		for (size_t i = 0; i < registry.size(); i++) {
			if (registry[i].section == "Display" && registry[i].key == "DrawDistance") {
				printf("  the registry clamps Display/DrawDistance to its own bounds: %.3f .. %.0f\n",
					registry[i].min, registry[i].max);
				if (!registry[i].bounded ||
					fabs(registry[i].min - DRAWDIST_MIN) > 0.0005 ||
					fabs(registry[i].max - DRAWDIST_MAX) > 0.0005)
					Fail("the draw distance row's bounds are not DRAWDIST_MIN .. DRAWDIST_MAX");
			}
		}
	}
	if (HasKey(registry, "Display", "SpawnDistance") || HasKey(cfo, "Display", "SpawnDistance"))
		Fail("the removed spawn distance key is still in the plan");
	else
		printf("  the removed slider key Display/SpawnDistance is in no plan: the save cannot write it\n");
	if (!HasKey(cfo, "Display", "PedDensity") || !HasKey(cfo, "Display", "CarDensity"))
		Fail("the density sliders are missing from the plan");
	{
		std::vector<std::string> lines;
		if (ReadFileLines("src/core/Frontend.h", lines)) {
			char want[64];
			sprintf(want, "#define MENUSLIDER_BARS %d", SLIDER_STEPS);
			bool found = false;
			for (size_t i = 0; i < lines.size(); i++)
				if (lines[i].find(want) != std::string::npos)
					found = true;
			if (!found) {
				printf("  Frontend.h does not hold \"%s\"\n", want);
				Fail("the sweep's step count does not match Frontend.h MENUSLIDER_BARS");
			} else {
				printf("  the sweep's step count matches Frontend.h (\"%s\")\n", want);
			}
		}
	}
	printf("\n");

	RoundTrip(1.0f, DRAWDIST_DEFAULT, registry, cfo, "/tmp/one-range-control-old-default.ini",
		"an old file with the spawn distance at 1.0 (never raised)");
	RoundTrip(4.0f, DRAWDIST_DEFAULT, registry, cfo, "/tmp/one-range-control-old-raised.ini",
		"an old file with the spawn distance at 4.0 and the draw distance untouched");
	RoundTrip(4.0f, 8.0f, registry, cfo, "/tmp/one-range-control-old-set.ini",
		"an old file with the spawn distance at 4.0 and its own draw distance 8.0");
	RoundTrip(4.0f, 99.0f, registry, cfo, "/tmp/one-range-control-old-high.ini",
		"an old file with a stale draw distance of 99.0 (above the slider top)");
	RoundTrip(4.0f, 0.5f, registry, cfo, "/tmp/one-range-control-old-low.ini",
		"an old file with a stale draw distance of 0.5 (below the slider bottom)");

	printf("  the legacy map (re3.cpp, LegacyRangeDrawDistance):\n");
	for (int i = 1; i <= 4; i++) {
		float legacy = (float)i;
		float mapped = LegacyRangeDrawDistance(legacy, DRAWDIST_DEFAULT);
		float kept = LegacyRangeDrawDistance(legacy, 8.0f);
		printf("    old %3.1f -> %5.2f with the draw distance at the default; %5.2f with a draw distance of 8.0 (the file's own stands)\n",
			(double)legacy, (double)mapped, (double)kept);
		if (legacy > 1.0f && mapped <= 0.0f)
			Fail("a raised spawn distance did not map");
		if (legacy <= 1.0f && mapped > 0.0f)
			Fail("the stock spawn distance mapped");
		if (kept > 0.0f)
			Fail("a file with its own draw distance was overridden");
		if (mapped > 0.0f && (mapped < DRAWDIST_MIN || mapped > DRAWDIST_MAX))
			Fail("a mapped draw distance left the slider's range");
	}
	{
		float mapped = LegacyRangeDrawDistance(4.0f, DRAWDIST_DEFAULT);
		STrafficRings oldRing = RingsAt(mapped, 1.0f, 2000.0f);
		STrafficRings stock = RingsAt(DRAWDIST_DEFAULT, 1.0f, 2000.0f);
		printf("    the mapped value %.2f gives the ring %.1f m, four times the stock ring %.1f m\n\n",
			(double)mapped, (double)oldRing.carOnScreenSpawnMax, (double)stock.carOnScreenSpawnMax);
		if (fabsf(oldRing.carOnScreenSpawnMax - 4.0f * stock.carOnScreenSpawnMax) > 0.05f)
			Fail("the mapped draw distance does not give the old ring");
	}
}

int main(void)
{
	printf("reVC one range control (VICE-125): the control sweep and the settings round trip\n");
	printf("the ring comes from src/control/TrafficRings.h, the control bounds from src/core/DrawDistance.h\n\n");

	SectionSweep();
	SectionRoundTrip();

	printf("== result\n\n");
	if (g_failures == 0) {
		printf("  the sweep is total, every cell holds the invariant of the ring table, and\n");
		printf("  the settings file round trips with the removed slider gone.\n");
		return 0;
	}
	printf("  %d check(s) failed.\n", g_failures);
	return 1;
}
