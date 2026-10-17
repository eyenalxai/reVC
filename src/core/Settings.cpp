// The case of the keys of reVC.ini, and with it the inline bodies of ini.h
// (through Settings.h). It must be set before the first include of that header.
#define MINI_CASE_SENSITIVE
#include "common.h"
#include "Settings.h"

#include "DrawDistance.h"
#include "Frontend.h"
#include "Camera.h"
#include "Pad.h"
#include "Vehicle.h"
#include "MBlur.h"
#include "postfx.h"
#include "custompipes.h"
#include "DMAudio.h"
#include "Draw.h"
#include "RwHelper.h"
#include "main.h"

// Every key of reVC.ini that is not a custom frontend option, one row each, in
// the order SaveINISettings writes them. That order is what a fresh file gets,
// so it is the order the old write calls had; the reader walks the same rows
// and does not care about their order.
//
// The bounds are the ones the menus already keep the values inside, so a stale
// value from the file cannot reach a place the menu cannot: the volumes (the
// sliders in Frontend.cpp clamp to 0..65) and the radar mode (the menu keeps
// it 0..2). The drift strength and assist are multipliers with 1.0 for the
// stock feel: 0 makes the layer inert, 2 is twice the stock. The clamp is only
// the outer bound; the grip cut already saturates at 1.32 (DriftControl.cpp)
// and the curve rate near 1.35 (docs/adr/0004).
static const tSettingsEntry aSettings[] = {
#ifdef IMPROVED_VIDEOMODE
	// Windowed mode is a custom frontend option; re3.cpp reads and writes it
	// with the other options.
	{ "VideoMode", "Width", SETTINGS_INT32, SETTINGS_NO_BOUND, sizeof(int32), &FrontEndMenuManager.m_nPrefsWidth },
	{ "VideoMode", "Height", SETTINGS_INT32, SETTINGS_NO_BOUND, sizeof(int32), &FrontEndMenuManager.m_nPrefsHeight },
	{ "VideoMode", "Depth", SETTINGS_INT32, SETTINGS_NO_BOUND, sizeof(int32), &FrontEndMenuManager.m_nPrefsDepth },
	{ "VideoMode", "Subsystem", SETTINGS_INT32, SETTINGS_NO_BOUND, sizeof(int32), &FrontEndMenuManager.m_nPrefsSubsystem },
#else
	{ "Graphics", "VideoMode", SETTINGS_INT32, SETTINGS_NO_BOUND, sizeof(int32), &FrontEndMenuManager.m_nDisplayVideoMode },
#endif
	{ "Controller", "HeadBob1stPerson", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &TheCamera.m_bHeadBob },
	{ "Controller", "HorizantalMouseSens", SETTINGS_FLOAT, SETTINGS_NO_BOUND, sizeof(float), &TheCamera.m_fMouseAccelHorzntl },
	{ "Controller", "InvertMouseVertically", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &MousePointerStateHelper.bInvertVertically },
	{ "Controller", "DisableMouseSteering", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &CVehicle::m_bDisableMouseSteering },
	{ "Controller", "Vibration", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &FrontEndMenuManager.m_PrefsUseVibration },
	{ "Audio", "SfxVolume", SETTINGS_INT8, 0, 65, sizeof(int8), &FrontEndMenuManager.m_PrefsSfxVolume },
	{ "Audio", "MusicVolume", SETTINGS_INT8, 0, 65, sizeof(int8), &FrontEndMenuManager.m_PrefsMusicVolume },
	{ "Audio", "MP3BoostVolume", SETTINGS_INT8, 0, 65, sizeof(int8), &FrontEndMenuManager.m_PrefsMP3BoostVolume },
	// A stale radio station is picked anew by Frontend.cpp, not clamped.
	{ "Audio", "Radio", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_PrefsRadioStation },
#ifdef EXTERNAL_3D_SOUND
	{ "Audio", "SpeakerType", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_PrefsSpeakers },
	{ "Audio", "Provider", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_nPrefsAudio3DProviderIndex },
#endif
	{ "Audio", "DynamicAcoustics", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_PrefsDMA },
	{ "Display", "Brightness", SETTINGS_INT32, SETTINGS_NO_BOUND, sizeof(int32), &FrontEndMenuManager.m_PrefsBrightness },
	// The draw distance is the one range control (TrafficRings.h), so a stale
	// file cannot put it outside the slider: the bounds are the slider's own
	// (DrawDistance.h), and the old spawn distance key maps inside them too.
	{ "Display", "DrawDistance", SETTINGS_FLOAT, DRAWDIST_MIN, DRAWDIST_MAX, sizeof(float), &FrontEndMenuManager.m_PrefsLOD },
	{ "Display", "Subtitles", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_PrefsShowSubtitles },
	{ "Graphics", "AspectRatio", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_PrefsUseWideScreen },
#ifdef LEGACY_MENU_OPTIONS
	{ "Graphics", "VSync", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_PrefsVsyncDisp },
	{ "Graphics", "Trails", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &CMBlur::BlurOn },
#endif
	{ "Graphics", "FrameLimiter", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_PrefsFrameLimiter },
	{ "General", "SkinFile", SETTINGS_STRING, SETTINGS_NO_BOUND, 256, FrontEndMenuManager.m_PrefsSkinFile },
	{ "Controller", "Method", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_ControlMethod },
	{ "General", "Language", SETTINGS_INT32, SETTINGS_NO_BOUND, sizeof(int32), &FrontEndMenuManager.m_PrefsLanguage },
	{ "Display", "ShowHud", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &FrontEndMenuManager.m_PrefsShowHud },
	{ "Display", "RadarMode", SETTINGS_INT32, 0, 2, sizeof(int32), &FrontEndMenuManager.m_PrefsRadarMode },
	{ "Display", "ShowLegends", SETTINGS_INT8, SETTINGS_NO_BOUND, sizeof(int8), &FrontEndMenuManager.m_PrefsShowLegends },
#ifdef EXTENDED_COLOURFILTER
	{ "CustomPipesValues", "PostFXIntensity", SETTINGS_FLOAT, SETTINGS_NO_BOUND, sizeof(float), &CPostFX::Intensity },
#endif
#ifdef EXTENDED_PIPELINES
	{ "CustomPipesValues", "NeoVehicleShininess", SETTINGS_FLOAT, SETTINGS_NO_BOUND, sizeof(float), &CustomPipes::VehicleShininess },
	{ "CustomPipesValues", "NeoVehicleSpecularity", SETTINGS_FLOAT, SETTINGS_NO_BOUND, sizeof(float), &CustomPipes::VehicleSpecularity },
	{ "CustomPipesValues", "RimlightMult", SETTINGS_FLOAT, SETTINGS_NO_BOUND, sizeof(float), &CustomPipes::RimlightMult },
	{ "CustomPipesValues", "LightmapMult", SETTINGS_FLOAT, SETTINGS_NO_BOUND, sizeof(float), &CustomPipes::LightmapMult },
	{ "CustomPipesValues", "GlossMult", SETTINGS_FLOAT, SETTINGS_NO_BOUND, sizeof(float), &CustomPipes::GlossMult },
#endif
	{ "Rendering", "BackfaceCulling", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &gBackfaceCulling },
#ifdef NEW_RENDERER
	{ "Rendering", "NewRenderer", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &gbNewRenderer },
#endif
	{ "Drift", "Enabled", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &gDriftEnabled },
	{ "Drift", "Strength", SETTINGS_FLOAT, 0.0f, 2.0f, sizeof(float), &gDriftStrength },
	{ "Drift", "Assist", SETTINGS_FLOAT, 0.0f, 2.0f, sizeof(float), &gDriftAssist },
	{ "Drift", "Debug", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &gDriftDebug },
	// The movement telemetry flag (MovementTelemetry.h): the movement debug
	// readout and the movement-telemetry.csv record
	{ "Movement", "Debug", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &gMovementDebug },
	{ "Weapons", "ActiveReload", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &gActiveReloadEnabled },
#ifdef PROPER_SCALING
	{ "Draw", "ProperScaling", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &CDraw::ms_bProperScaling },
#endif
#ifdef FIX_RADAR
	{ "Draw", "FixRadar", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &CDraw::ms_bFixRadar },
#endif
#ifdef FIX_SPRITES
	{ "Draw", "FixSprites", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &CDraw::ms_bFixSprites },
#endif
#ifdef DRAW_GAME_VERSION_TEXT
	{ "General", "DrawVersionText", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &gbDrawVersionText },
#endif
#ifdef NO_MOVIES
	{ "General", "NoMovies", SETTINGS_BOOL, SETTINGS_NO_BOUND, sizeof(bool), &gbNoMovies },
#endif
};

// The SkinFile row names the room of the buffer it points at.
static_assert(sizeof(FrontEndMenuManager.m_PrefsSkinFile) == 256, "the SkinFile row must name the room of m_PrefsSkinFile");

// The room of a row must hold the type it names, or a read or a write would
// touch the wrong number of bytes. The rows are static, so one pass is enough;
// the first of the two passes that runs makes it.
static void
SettingsCheckTable(void)
{
	static bool checked = false;
	if (checked)
		return;
	checked = true;

	for (uint32 i = 0; i < ARRAY_SIZE(aSettings); i++) {
		int32 width = 0;
		switch (aSettings[i].type) {
		case SETTINGS_UINT32: width = sizeof(uint32); break;
		case SETTINGS_INT32: width = sizeof(int32); break;
		case SETTINGS_UINT8: width = sizeof(uint8); break;
		case SETTINGS_INT8: width = sizeof(int8); break;
		case SETTINGS_BOOL: width = sizeof(bool); break;
		case SETTINGS_FLOAT: width = sizeof(float); break;
		case SETTINGS_STRING: width = 0; break;	// the room of the buffer, not a width
		}
		if (width != 0 && width != aSettings[i].size) {
			Error("settings: %s/%s names %d bytes for a %d byte type\n", aSettings[i].section, aSettings[i].key, aSettings[i].size, width);
			assert(false);
		}
	}
}

static void
SettingsLoadValue(mINI::INIStructure &cfg, const tSettingsEntry &entry)
{
	mINI::INIMap<std::string> section = cfg.get(entry.section);
	if (!section.has(entry.key))
		return;

	std::string text = section.get(entry.key);
	char *endPtr;

	// Every parse is the call the old ReadIniIfExists overload for the type
	// made, into the same type; the clamp is the only step on top of it.
	switch (entry.type) {
	case SETTINGS_UINT32:
		{
			uint32 v = strtoul(text.c_str(), &endPtr, 0);
			*(uint32*)entry.value = (uint32)Clamp(v, entry.min, entry.max);
		}
		break;
	case SETTINGS_UINT8:
		{
			uint8 v = strtoul(text.c_str(), &endPtr, 0);
			*(uint8*)entry.value = (uint8)Clamp(v, entry.min, entry.max);
		}
		break;
	case SETTINGS_BOOL:
		*(bool*)entry.value = strtoul(text.c_str(), &endPtr, 0) != 0;
		break;
	case SETTINGS_INT32:
		{
			int32 v = strtol(text.c_str(), &endPtr, 0);
			*(int32*)entry.value = (int32)Clamp(v, entry.min, entry.max);
		}
		break;
	case SETTINGS_INT8:
		{
			int8 v = strtol(text.c_str(), &endPtr, 0);
			*(int8*)entry.value = (int8)Clamp(v, entry.min, entry.max);
		}
		break;
	case SETTINGS_FLOAT:
		{
			float v = strtof(text.c_str(), &endPtr);
			*(float*)entry.value = (float)Clamp(v, entry.min, entry.max);
		}
		break;
	case SETTINGS_STRING:
		strncpy((char*)entry.value, text.c_str(), entry.size - 1);
		((char*)entry.value)[entry.size - 1] = '\0';
		break;
	}
}

static void
SettingsSaveValue(mINI::INIStructure &cfg, const tSettingsEntry &entry)
{
	char temp[50];

	switch (entry.type) {
	case SETTINGS_UINT32:
		sprintf(temp, "%u", *(uint32*)entry.value);
		break;
	case SETTINGS_UINT8:
		sprintf(temp, "%u", *(uint8*)entry.value);
		break;
	case SETTINGS_BOOL:
		// A bool holds 0 or 1, so the "%u" of the tag prints the same bytes
		// the old write call printed with the "%d" it resolved to.
		sprintf(temp, "%u", *(bool*)entry.value ? 1 : 0);
		break;
	case SETTINGS_INT32:
		sprintf(temp, "%d", *(int32*)entry.value);
		break;
	case SETTINGS_INT8:
		sprintf(temp, "%d", *(int8*)entry.value);
		break;
	case SETTINGS_FLOAT:
		// the vararg call promoted the float to a double, as the old write
		// call did
		sprintf(temp, "%f", (double)*(float*)entry.value);
		break;
	case SETTINGS_STRING:
		cfg[entry.section][entry.key] = (char*)entry.value;
		return;
	}
	cfg[entry.section][entry.key] = temp;
}

void
SettingsLoadAll(mINI::INIStructure &cfg)
{
	SettingsCheckTable();
	for (uint32 i = 0; i < ARRAY_SIZE(aSettings); i++)
		SettingsLoadValue(cfg, aSettings[i]);
}

void
SettingsSaveAll(mINI::INIStructure &cfg)
{
	SettingsCheckTable();
	for (uint32 i = 0; i < ARRAY_SIZE(aSettings); i++)
		SettingsSaveValue(cfg, aSettings[i]);
}
