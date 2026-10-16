#include "common.h"
#include "Settings.h"

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
//
// 1.2f is CRenderer::ms_lodDistScale, the stock draw distance.
static const tSettingsEntry gSettings[] = {
#ifdef IMPROVED_VIDEOMODE
	// Windowed mode is a custom frontend option; re3.cpp reads and writes it
	// with the other options.
	{ "VideoMode", "Width", SETTINGS_INT32, 0, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_nPrefsWidth },
	{ "VideoMode", "Height", SETTINGS_INT32, 0, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_nPrefsHeight },
	{ "VideoMode", "Depth", SETTINGS_INT32, 0, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_nPrefsDepth },
	{ "VideoMode", "Subsystem", SETTINGS_INT32, 0, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_nPrefsSubsystem },
#else
	{ "Graphics", "VideoMode", SETTINGS_INT32, 0, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_nDisplayVideoMode },
#endif
	{ "Controller", "HeadBob1stPerson", SETTINGS_BOOL, false, SETTINGS_NO_BOUND, 0, &TheCamera.m_bHeadBob },
	{ "Controller", "HorizantalMouseSens", SETTINGS_FLOAT, 0.0025f, SETTINGS_NO_BOUND, 0, &TheCamera.m_fMouseAccelHorzntl },
	{ "Controller", "InvertMouseVertically", SETTINGS_BOOL, true, SETTINGS_NO_BOUND, 0, &MousePointerStateHelper.bInvertVertically },
	{ "Controller", "DisableMouseSteering", SETTINGS_BOOL, true, SETTINGS_NO_BOUND, 0, &CVehicle::m_bDisableMouseSteering },
	{ "Controller", "Vibration", SETTINGS_BOOL, false, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsUseVibration },
	{ "Audio", "SfxVolume", SETTINGS_INT8, 49, 0, 65, 0, &FrontEndMenuManager.m_PrefsSfxVolume },
	{ "Audio", "MusicVolume", SETTINGS_INT8, 49, 0, 65, 0, &FrontEndMenuManager.m_PrefsMusicVolume },
	{ "Audio", "MP3BoostVolume", SETTINGS_INT8, 0, 0, 65, 0, &FrontEndMenuManager.m_PrefsMP3BoostVolume },
	// A stale radio station is picked anew by Frontend.cpp, not clamped.
	{ "Audio", "Radio", SETTINGS_INT8, 0, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsRadioStation },
#ifdef EXTERNAL_3D_SOUND
	{ "Audio", "SpeakerType", SETTINGS_INT8, 0, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsSpeakers },
	{ "Audio", "Provider", SETTINGS_INT8, AUDIO_PROVIDER_NOT_DETERMINED, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_nPrefsAudio3DProviderIndex },
#endif
	{ "Audio", "DynamicAcoustics", SETTINGS_INT8, 1, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsDMA },
	{ "Display", "Brightness", SETTINGS_INT32, 256, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsBrightness },
	{ "Display", "DrawDistance", SETTINGS_FLOAT, 1.2f, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsLOD },
	{ "Display", "Subtitles", SETTINGS_INT8, 0, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsShowSubtitles },
	{ "Graphics", "AspectRatio", SETTINGS_INT8, AR_AUTO, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsUseWideScreen },
#ifdef LEGACY_MENU_OPTIONS
	{ "Graphics", "VSync", SETTINGS_INT8, 1, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsVsyncDisp },
	{ "Graphics", "Trails", SETTINGS_BOOL, false, SETTINGS_NO_BOUND, 0, &CMBlur::BlurOn },
#endif
	{ "Graphics", "FrameLimiter", SETTINGS_INT8, 1, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsFrameLimiter },
	{ "General", "SkinFile", SETTINGS_STRING, 0, SETTINGS_NO_BOUND, 256, FrontEndMenuManager.m_PrefsSkinFile },
	{ "Controller", "Method", SETTINGS_INT8, CONTROL_STANDARD, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_ControlMethod },
	{ "General", "Language", SETTINGS_INT32, 0, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsLanguage },
	{ "Display", "ShowHud", SETTINGS_BOOL, true, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsShowHud },
	{ "Display", "RadarMode", SETTINGS_INT32, 0, 0, 2, 0, &FrontEndMenuManager.m_PrefsRadarMode },
	{ "Display", "ShowLegends", SETTINGS_INT8, 1, SETTINGS_NO_BOUND, 0, &FrontEndMenuManager.m_PrefsShowLegends },
#ifdef EXTENDED_COLOURFILTER
	{ "CustomPipesValues", "PostFXIntensity", SETTINGS_FLOAT, 1.0f, SETTINGS_NO_BOUND, 0, &CPostFX::Intensity },
#endif
#ifdef EXTENDED_PIPELINES
	{ "CustomPipesValues", "NeoVehicleShininess", SETTINGS_FLOAT, 1.0f, SETTINGS_NO_BOUND, 0, &CustomPipes::VehicleShininess },
	{ "CustomPipesValues", "NeoVehicleSpecularity", SETTINGS_FLOAT, 1.0f, SETTINGS_NO_BOUND, 0, &CustomPipes::VehicleSpecularity },
	{ "CustomPipesValues", "RimlightMult", SETTINGS_FLOAT, 1.0f, SETTINGS_NO_BOUND, 0, &CustomPipes::RimlightMult },
	{ "CustomPipesValues", "LightmapMult", SETTINGS_FLOAT, 1.0f, SETTINGS_NO_BOUND, 0, &CustomPipes::LightmapMult },
	{ "CustomPipesValues", "GlossMult", SETTINGS_FLOAT, 1.0f, SETTINGS_NO_BOUND, 0, &CustomPipes::GlossMult },
#endif
	{ "Rendering", "BackfaceCulling", SETTINGS_BOOL, true, SETTINGS_NO_BOUND, 0, &gBackfaceCulling },
#ifdef NEW_RENDERER
	{ "Rendering", "NewRenderer", SETTINGS_BOOL, false, SETTINGS_NO_BOUND, 0, &gbNewRenderer },
#endif
	{ "Drift", "Enabled", SETTINGS_BOOL, false, SETTINGS_NO_BOUND, 0, &gDriftEnabled },
	{ "Drift", "Strength", SETTINGS_FLOAT, 1.0f, 0.0f, 2.0f, 0, &gDriftStrength },
	{ "Drift", "Assist", SETTINGS_FLOAT, 1.0f, 0.0f, 2.0f, 0, &gDriftAssist },
	{ "Drift", "Debug", SETTINGS_BOOL, false, SETTINGS_NO_BOUND, 0, &gDriftDebug },
	{ "Weapons", "ActiveReload", SETTINGS_BOOL, true, SETTINGS_NO_BOUND, 0, &gActiveReloadEnabled },
#ifdef PROPER_SCALING
	{ "Draw", "ProperScaling", SETTINGS_BOOL, true, SETTINGS_NO_BOUND, 0, &CDraw::ms_bProperScaling },
#endif
#ifdef FIX_RADAR
	{ "Draw", "FixRadar", SETTINGS_BOOL, true, SETTINGS_NO_BOUND, 0, &CDraw::ms_bFixRadar },
#endif
#ifdef FIX_SPRITES
	{ "Draw", "FixSprites", SETTINGS_BOOL, true, SETTINGS_NO_BOUND, 0, &CDraw::ms_bFixSprites },
#endif
#ifdef DRAW_GAME_VERSION_TEXT
	{ "General", "DrawVersionText", SETTINGS_BOOL, false, SETTINGS_NO_BOUND, 0, &gbDrawVersionText },
#endif
#ifdef NO_MOVIES
	{ "General", "NoMovies", SETTINGS_BOOL, false, SETTINGS_NO_BOUND, 0, &gbNoMovies },
#endif
};

// The bounds of a row. A row without bounds passes its value through: the
// pair of SETTINGS_NO_BOUND lies outside every value of the table types.
static double
SettingsClamped(double value, const tSettingsEntry &entry)
{
	return Clamp(value, entry.min, entry.max);
}

static bool
SettingsLoadValue(mINI::INIStructure &cfg, const tSettingsEntry &entry)
{
	mINI::INIMap<std::string> section = cfg.get(entry.section);
	if (!section.has(entry.key))
		return false;

	std::string text = section.get(entry.key);
	char *endPtr;

	// Every parse is the call the old ReadIniIfExists overload for the type
	// made, into the same type; the clamp is the only step on top of it.
	switch (entry.type) {
	case SETTINGS_UINT32:
		{
			uint32 v = strtoul(text.c_str(), &endPtr, 0);
			*(uint32*)entry.value = (uint32)SettingsClamped(v, entry);
		}
		break;
	case SETTINGS_UINT8:
		{
			uint8 v = strtoul(text.c_str(), &endPtr, 0);
			*(uint8*)entry.value = (uint8)SettingsClamped(v, entry);
		}
		break;
	case SETTINGS_BOOL:
		*(bool*)entry.value = strtoul(text.c_str(), &endPtr, 0) != 0;
		break;
	case SETTINGS_INT32:
		{
			int32 v = strtol(text.c_str(), &endPtr, 0);
			*(int32*)entry.value = (int32)SettingsClamped(v, entry);
		}
		break;
	case SETTINGS_INT8:
		{
			int8 v = strtol(text.c_str(), &endPtr, 0);
			*(int8*)entry.value = (int8)SettingsClamped(v, entry);
		}
		break;
	case SETTINGS_FLOAT:
		{
			float v = strtof(text.c_str(), &endPtr);
			*(float*)entry.value = (float)SettingsClamped(v, entry);
		}
		break;
	case SETTINGS_STRING:
		strncpy((char*)entry.value, text.c_str(), entry.size - 1);
		((char*)entry.value)[entry.size - 1] = '\0';
		break;
	}
	return true;
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
	for (uint32 i = 0; i < ARRAY_SIZE(gSettings); i++)
		SettingsLoadValue(cfg, gSettings[i]);
}

void
SettingsSaveAll(mINI::INIStructure &cfg)
{
	for (uint32 i = 0; i < ARRAY_SIZE(gSettings); i++)
		SettingsSaveValue(cfg, gSettings[i]);
}
