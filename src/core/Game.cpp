#include "common.h"
#include "platform.h"

#include "Game.h"
#include "main.h"
#include "RwHelper.h"
#include "Accident.h"
#include "Antennas.h"
#include "Bridge.h"
#include "CarCtrl.h"
#include "CarGen.h"
#include "CdStream.h"
#include "Clock.h"
#include "Clouds.h"
#include "Collision.h"
#include "Console.h"
#include "Coronas.h"
#include "Cranes.h"
#include "Credits.h"
#include "CutsceneMgr.h"
#include "DMAudio.h"
#include "Darkel.h"
#include "Debug.h"
#include "DriftControl.h"
#include "DriftFeedback.h"
#include "EventList.h"
#include "FileLoader.h"
#include "FileMgr.h"
#include "Fire.h"
#include "Fluff.h"
#include "Font.h"
#include "Frontend.h"
#include "frontendoption.h"
#include "GameLogic.h"
#include "Garages.h"
#include "GenericGameStorage.h"
#include "Glass.h"
#include "HandlingMgr.h"
#include "Heli.h"
#include "Hud.h"
#include "IniFile.h"
#include "Lights.h"
#include "MBlur.h"
#include "Messages.h"
#include "MemoryCard.h"
#include "MemoryHeap.h"
#include "Pad.h"
#include "Particle.h"
#include "ParticleObject.h"
#include "PedRoutes.h"
#include "Phones.h"
#include "Pickups.h"
#include "Plane.h"
#include "PlayerInfo.h"
#include "PlayerSkin.h"
#include "Population.h"
#include "Radar.h"
#include "Record.h"
#include "References.h"
#include "Renderer.h"
#include "Replay.h"
#include "Restart.h"
#include "RoadBlocks.h"
#include "Rubbish.h"
#include "SceneEdit.h"
#include "Script.h"
#include "Shadows.h"
#include "Skidmarks.h"
#include "SetPieces.h"
#include "SpawnOccupancy.h"
#include "SpawnReadiness.h"
#include "SpecialFX.h"
#include "SpawnSites.h"
#include "Stats.h"
#include "Streaming.h"
#include "SurfaceTable.h"
#include "TempColModels.h"
#include "Timecycle.h"
#include "TrafficLights.h"
#include "Train.h"
#include "TxdStore.h"
#include "User.h"
#include "VisibilityPlugins.h"
#include "WaterCannon.h"
#include "WaterLevel.h"
#include "Weapon.h"
#include "WeaponEffects.h"
#include "Weather.h"
#include "World.h"
#include "ZoneCull.h"
#include "Zones.h"
#include "Occlusion.h"
#include "debugmenu.h"
#include "Ropes.h"
#include "WindModifiers.h"
#include "WaterCreatures.h"
#include "postfx.h"
#include "custompipes.h"
#include "screendroplets.h"
#include "VarConsole.h"

eLevelName CGame::currLevel;
int32 CGame::currArea;
bool CGame::bDemoMode = true;
bool CGame::nastyGame = true;
bool CGame::frenchGame;
bool CGame::germanGame;
bool CGame::noProstitutes;
bool CGame::playingIntro;
char CGame::aDatFile[32];
#ifdef MORE_LANGUAGES
bool CGame::russianGame = false;
bool CGame::japaneseGame = false;
#endif
#ifndef MASTER
CVector CGame::PlayerCoords;
bool8 CGame::VarUpdatePlayerCoords;
#endif

int gameTxdSlot;

#ifdef SECUROM
uint8 gameProcessPirateCheck = 0;
#endif

bool DoRWStuffStartOfFrame(int16 TopRed, int16 TopGreen, int16 TopBlue, int16 BottomRed, int16 BottomGreen, int16 BottomBlue, int16 Alpha);
void DoRWStuffEndOfFrame(void);
#ifdef PS2_MENU
void MessageScreen(char *msg)
{
	//TODO: stretch_screen
	
	CRect rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	CRGBA color(255, 255, 255, 255);

	DoRWStuffStartOfFrame(50, 50, 50, 0, 0, 0, 255);
	
	CSprite2d::InitPerFrame();
	CFont::InitPerFrame();
	DefinedState();

	CSprite2d *splash = LoadSplash(NULL);
	splash->Draw(rect, color, color, color, color);
	splash->DrawRect(CRect(SCREEN_SCALE_X(20.0f), SCREEN_SCALE_Y(110.0f), SCREEN_SCALE_X(620.0f), SCREEN_SCALE_Y(300.0f)), CRGBA(50, 50, 50, 192));
	
	CFont::SetFontStyle(FONT_BANK);
	CFont::SetBackgroundOff();
	CFont::SetWrapx(SCREEN_SCALE_FROM_RIGHT(190.0f)); // 450.0f
	CFont::SetScale(SCREEN_SCALE_X(1.0f), SCREEN_SCALE_Y(1.0f));
	CFont::SetCentreOn();
	CFont::SetCentreSize(SCREEN_SCALE_X(450.0f));
	CFont::SetJustifyOff();
	CFont::SetColor(CRGBA(255, 255, 255, 255));
	CFont::SetDropColor(CRGBA(32, 32, 32, 255));
	CFont::SetDropShadowPosition(3);
	CFont::SetPropOn();
	CFont::PrintString(SCREEN_SCALE_X(320.0f), SCREEN_SCALE_Y(130.0f), TheText.Get(msg));
	CFont::DrawFonts();
	
	DoRWStuffEndOfFrame();
}
#endif

bool
CGame::InitialiseOnceBeforeRW(void)
{
	CFileMgr::Initialise();
	CdStreamInit(MAX_CDCHANNELS);
	debug("size of matrix %d\n", sizeof(CMatrix));
	debug("size of placeable %d\n", sizeof(CPlaceable));
	debug("size of entity %d\n", sizeof(CEntity));
	debug("size of building %d\n", sizeof(CBuilding));
	debug("size of dummy %d\n", sizeof(CDummy));
#ifdef EXTENDED_COLOURFILTER
	CPostFX::InitOnce();
#endif
#ifdef CUSTOM_FRONTEND_OPTIONS
	// Not needed here but may be needed in future
	// if (numCustomFrontendOptions == 0 && numCustomFrontendScreens == 0)
	CustomFrontendOptionsPopulate();
#endif
	return true;
}

bool
CGame::InitialiseRenderWare(void)
{
	CFileMgr::SetDir("");
	ValidateVersion();

	CTxdStore::Initialise();
	CVisibilityPlugins::Initialise();

#ifdef GTA_PS2
	RpSkySelectTrueTSClipper(TRUE);
	RpSkySelectTrueTLClipper(TRUE);

	// PS2ManagerApplyDirectionalLightingCB() uploads the GTA lights
	// directly without going through RpWorld and all that
	SetupPS2ManagerDefaultLightingCallback();
	PreAllocateRwObjects();
#endif

	/* Create camera */
	Scene.camera = CameraCreate(SCREEN_WIDTH, SCREEN_HEIGHT, TRUE);
	ASSERT(Scene.camera != nil);
	if (!Scene.camera)
	{
		return (false);
	}
	
	RwCameraSetFarClipPlane(Scene.camera, 2000.0f);
	RwCameraSetNearClipPlane(Scene.camera, 0.9f);
	
	CameraSize(Scene.camera, nil, DEFAULT_VIEWWINDOW, DEFAULT_ASPECT_RATIO);
	
	/* Create a world */
	RwBBox  bbox;
	
	bbox.sup.x = bbox.sup.y = bbox.sup.z = 10000.0f;
	bbox.inf.x = bbox.inf.y = bbox.inf.z = -10000.0f;

	Scene.world = RpWorldCreate(&bbox);
	ASSERT(Scene.world != nil);
	if (!Scene.world)
	{
		CameraDestroy(Scene.camera);
		Scene.camera = nil;
		return (false);
	}
	
	/* Add the camera to the world */
	RpWorldAddCamera(Scene.world, Scene.camera);
	LightsCreate(Scene.world);

	CreateDebugFont();

#ifdef PS2_MATFX
	rw::MatFX::envMapApplyLight = true;
	rw::MatFX::envMapUseMatColor = true;
	rw::MatFX::envMapFlipU = true;
#else
	rw::MatFX::envMapApplyLight = false;
	rw::MatFX::envMapUseMatColor = false;
	rw::MatFX::envMapFlipU = false;
#endif
	rw::RGBA envcol = { 64, 64, 64, 255 };
	rw::MatFX::envMapColor = envcol;

	PUSH_MEMID(MEMID_TEXTURES);
	CFont::Initialise();
	CHud::Initialise();
	CPlayerSkin::Initialise();
	POP_MEMID();

#ifdef EXTENDED_PIPELINES
	CustomPipes::CustomPipeInit();	// need Scene.world for this
#endif
#ifdef SCREEN_DROPLETS
	ScreenDroplets::InitDraw();
#endif

	return (true);
}

void CGame::ShutdownRenderWare(void)
{
#ifdef SCREEN_DROPLETS
	ScreenDroplets::Shutdown();
#endif
#ifdef EXTENDED_PIPELINES
	CustomPipes::CustomPipeShutdown();
#endif

	DestroySplashScreen();
	CHud::Shutdown();
	CFont::Shutdown();
	
	for ( int32 i = 0; i < NUMPLAYERS; i++ )
		CWorld::Players[i].DeletePlayerSkin();

	CPlayerSkin::Shutdown();
	
	DestroyDebugFont();
	
	/* Destroy world */
	LightsDestroy(Scene.world);
	RpWorldRemoveCamera(Scene.world, Scene.camera);
	RpWorldDestroy(Scene.world);
	
	/* destroy camera */
	CameraDestroy(Scene.camera);
	
	Scene.world = nil;
	Scene.camera = nil;
	
	CVisibilityPlugins::Shutdown();
	
}

bool CGame::InitialiseOnceAfterRW(void)
{
	TheText.Load();
	CTimer::Initialise();
	CTempColModels::Initialise();
	mod_HandlingManager.Initialise();
	CSurfaceTable::Initialise("DATA\\SURFACE.DAT");
	CPedStats::Initialise();
	CTimeCycle::Initialise();
#ifdef GTA_PS2
	LoadingScreen("Loading the Game", "Initialising audio", GetRandomSplashScreen());
#endif
	DMAudio.Initialise();

#ifndef GTA_PS2
#ifdef EXTERNAL_3D_SOUND
	if ( DMAudio.GetNum3DProvidersAvailable() == 0 )
		FrontEndMenuManager.m_nPrefsAudio3DProviderIndex = NO_AUDIO_PROVIDER;

	if ( FrontEndMenuManager.m_nPrefsAudio3DProviderIndex == AUDIO_PROVIDER_NOT_DETERMINED || FrontEndMenuManager.m_nPrefsAudio3DProviderIndex == -2 )
	{
		FrontEndMenuManager.m_PrefsSpeakers = 0;
		FrontEndMenuManager.m_nPrefsAudio3DProviderIndex = DMAudio.AutoDetect3DProviders();
	}

	DMAudio.SetCurrent3DProvider(FrontEndMenuManager.m_nPrefsAudio3DProviderIndex);
	DMAudio.SetSpeakerConfig(FrontEndMenuManager.m_PrefsSpeakers);
#endif
	DMAudio.SetDynamicAcousticModelingStatus(FrontEndMenuManager.m_PrefsDMA);
	DMAudio.SetMusicMasterVolume(FrontEndMenuManager.m_PrefsMusicVolume);
	DMAudio.SetEffectsMasterVolume(FrontEndMenuManager.m_PrefsSfxVolume);
	DMAudio.SetEffectsFadeVol(127);
	DMAudio.SetMusicFadeVol(127);
#endif
	return true;
}

void
CGame::FinalShutdown(void)
{	
	CTxdStore::Shutdown();
	CPedStats::Shutdown();
	CdStreamShutdown();
}

bool CGame::Initialise(const char* datFile)
{
	ResetLoadingScreenBar();
	strcpy(aDatFile, datFile);

#ifdef GTA_PS2
	// TODO: upload VU0 collision code here
#endif

	CPools::Initialise();
	/* The store's one true rebuild at boot (VICE-149): the pools are live
	 * and empty, so the records start clean whatever the static memory
	 * held. */
	CSpawnOccupancy::Resync();

#ifndef GTA_PS2
#ifdef PED_CAR_DENSITY_SLIDERS
	// Load density values from gta3.ini only if our reVC.ini have them 1.0f/2.0f
	if (CIniFile::PedNumberMultiplier == 1.0f && CIniFile::CarNumberMultiplier == 2.0f)
#endif
		CIniFile::LoadIniFile();
#endif
	currLevel = LEVEL_BEACH;
	currArea = AREA_MAIN_MAP;

	PUSH_MEMID(MEMID_TEXTURES);
	LoadingScreen("Loading the Game", "Loading generic textures", GetRandomSplashScreen());
	gameTxdSlot = CTxdStore::AddTxdSlot("generic");
	CTxdStore::Create(gameTxdSlot);
	CTxdStore::AddRef(gameTxdSlot);

#ifdef EXTENDED_PIPELINES
	// for generic fallback
	CustomPipes::SetTxdFindCallback();
#endif

	LoadingScreen("Loading the Game", "Loading particles", nil);
	int particleTxdSlot = CTxdStore::AddTxdSlot("particle");
	CTxdStore::LoadTxd(particleTxdSlot, "MODELS/PARTICLE.TXD");
	CTxdStore::AddRef(particleTxdSlot);
	CTxdStore::SetCurrentTxd(gameTxdSlot);
	LoadingScreen("Loading the Game", "Setup game variables", nil);
	POP_MEMID();

#ifdef GTA_PS2
	CDma::SyncChannel(0, true);
#endif

	CGameLogic::InitAtStartOfGame();
	CReferences::Init();
	TheCamera.Init();
	TheCamera.SetRwCamera(Scene.camera);
	CDebug::DebugInitTextBuffer();
	ThePaths.Init();
	ThePaths.AllocatePathFindInfoMem(4500);
	CScriptPaths::Init();
	CWeather::Init();
	CCullZones::Init();
	COcclusion::Init();
	CCollision::Init();
	CSetPieces::Init();
	CTheZones::Init();
	CUserDisplay::Init();
	CMessages::Init();
	CMessages::ClearAllMessagesDisplayedByGame();
	CRecordDataForGame::Init();
	CRestart::Initialise();

	PUSH_MEMID(MEMID_WORLD);
	CWorld::Initialise();
	POP_MEMID();

	PUSH_MEMID(MEMID_TEXTURES);
	CParticle::Initialise();
	POP_MEMID();

	PUSH_MEMID(MEMID_ANIMATION);
	CAnimManager::Initialise();
	CCutsceneMgr::Initialise();
	POP_MEMID();

	PUSH_MEMID(MEMID_CARS);
	CCarCtrl::Init();
	POP_MEMID();

	PUSH_MEMID(MEMID_DEF_MODELS);
	InitModelIndices();
	CModelInfo::Initialise();
	CPickups::Init();
	CTheCarGenerators::Init();

	CdStreamAddImage("MODELS\\GTA3.IMG");

	CFileLoader::LoadLevel("DATA\\DEFAULT.DAT");
	CFileLoader::LoadLevel(datFile);

	LoadingScreen("Loading the Game", "Add Particles", nil);
	CWorld::AddParticles();
	CVehicleModelInfo::LoadVehicleColours();
	CVehicleModelInfo::LoadEnvironmentMaps();
	CTheZones::PostZoneCreation();
	POP_MEMID();

	LoadingScreen("Loading the Game", "Setup paths", nil);
	ThePaths.PreparePathData();
	/* The far traffic layer's path snapshot (VICE-133): the worker's copy of
	 * the graph, taken here on the main thread, right after the graph is
	 * prepared. The grid is built here too, deliberately: it used to be built
	 * lazily by the first FindNodeClosestToCoors, i.e. by the first spawn of
	 * the first played frame (one array allocation and the node walk), and the
	 * ticket's contract puts the snapshot after PreparePathData and
	 * BuildNodeGrid. Forcing it keeps the first frame free of it, under the
	 * loading screen, and the placements read a grid that was already there.
	 * The worker thread itself is created here, once. */
	ThePaths.BuildNodeGrid();
	CFarTraffic::BuildWorldSnapshot();
	/* The spawn sites (VICE-148): the index over the car path connections,
	 * built here with the graph prepared and the zones created above
	 * (PostZoneCreation) and with the collision world not resident yet - a
	 * site starts unvalidated and the ground is validated lazily per site.
	 * The spawn service's planner reads it since VICE-150. */
	CSpawnSites::Build();
	for (int i = 0; i < NUMPLAYERS; i++)
		CWorld::Players[i].Clear();
	CWorld::Players[0].LoadPlayerSkin();
	TestModelIndices();

	LoadingScreen("Loading the Game", "Setup water", nil);
	CWaterLevel::Initialise("DATA\\WATER.DAT");
	TheConsole.Init();
	CDraw::SetFOV(120.0f);
	CDraw::ms_fLODDistance = 500.0f;

	LoadingScreen("Loading the Game", "Setup streaming", nil);
	CStreaming::LoadInitialVehicles();
	CStreaming::LoadInitialPeds();
	CStreaming::RequestBigBuildings(LEVEL_GENERIC);
	CStreaming::LoadAllRequestedModels(false);
	CStreaming::RemoveIslandsNotUsed(currLevel);
	printf("Streaming uses %zuK of its memory", CStreaming::ms_memoryUsed / 1024); // original modifier was %d

	LoadingScreen("Loading the Game", "Load animations", GetRandomSplashScreen());
	PUSH_MEMID(MEMID_ANIMATION);
	CAnimManager::LoadAnimFiles();
	POP_MEMID();

	CStreaming::LoadInitialWeapons();
	CStreaming::LoadAllRequestedModels(0);
	CPed::Initialise();
	CRouteNode::Initialise();
	CEventList::Initialise();
#ifdef SCREEN_DROPLETS
	ScreenDroplets::Initialise();
#endif
	LoadingScreen("Loading the Game", "Find big buildings", nil);
	CRenderer::Init();

	LoadingScreen("Loading the Game", "Setup game variables", nil);
	CRadar::Initialise();
	CRadar::LoadTextures();
	CWeapon::InitialiseWeapons();

	LoadingScreen("Loading the Game", "Setup traffic lights", nil);
	CTrafficLights::ScanForLightsOnMap();
	CRoadBlocks::Init();

	LoadingScreen("Loading the Game", "Setup game variables", nil);
	CPopulation::Initialise();
	CWorld::PlayerInFocus = 0;
	CCoronas::Init();
	CShadows::Init();
	CWeaponEffects::Init();
	CSkidmarks::Init();
	CAntennas::Init();
	CGlass::Init();
	gPhoneInfo.Initialise();
#ifdef GTA_SCENE_EDIT
	CSceneEdit::Initialise();
#endif

	LoadingScreen("Loading the Game", "Load scripts", nil);
	PUSH_MEMID(MEMID_SCRIPT);
	CTheScripts::Init();
	CGangs::Initialise();
	POP_MEMID();

	LoadingScreen("Loading the Game", "Setup game variables", nil);
	CClock::Initialise(1000);
	CHeli::InitHelis();
	CCranes::InitCranes();
	CMovingThings::Init();
	CDarkel::Init();
	CStats::Init();
	CPacManPickups::Init();
	CRubbish::Init();
	CClouds::Init();
	CSpecialFX::Init();
	CRopes::Init();
	CWaterCannons::Init();
	CBridge::Init();
	CGarages::Init();

	LoadingScreen("Loading the Game", "Position dynamic objects", nil);
	LoadingScreen("Loading the Game", "Initialise vehicle paths", nil);

	CTrain::InitTrains();
	CPlane::InitPlanes();
	CCredits::Init();
	CRecordDataForChase::Init();
	CReplay::Init();

	LoadingScreen("Loading the Game", "Start script", nil);
#ifdef PS2_MENU
	if ( !TheMemoryCard.m_bWantToLoad )
#endif
	{
		CTheScripts::StartTestScript();
		CTheScripts::Process();
		TheCamera.Process();
	}

	LoadingScreen("Loading the Game", "Load scene", nil);
	CCollision::ms_collisionInMemory = currLevel;
	for (int i = 0; i < MAX_PADS; i++)
		CPad::GetPad(i)->Clear(true);

	/* The spawn readiness (VICE-151): the boot is a loading-screen host. The
	 * car class set is global and requested once; the first zone set is the
	 * player's own. The prewarm takes the blocking path - the screen covers
	 * it - and the pins stay for the session (a save load rebuilds them).
	 * The explicit call is here, not in the drawing function: LoadingScreen
	 * draws, it does not load. */
	if (FindPlayerPed())
		CSpawnReadiness::PrewarmInCover(FindPlayerCoors());

#ifndef MASTER
	PlayerCoords = FindPlayerCoors();
	VarConsole.Add("X PLAYER COORD", &PlayerCoords.x, 10.0f, -10000.0f, 10000.0f, true);
	VarConsole.Add("Y PLAYER COORD", &PlayerCoords.y, 10.0f, -10000.0f, 10000.0f, true);
	VarConsole.Add("Z PLAYER COORD", &PlayerCoords.z, 10.0f, -10000.0f, 10000.0f, true);
	VarConsole.Add("UPDATE PLAYER COORD", &VarUpdatePlayerCoords, true);
#endif


	DMAudio.SetStartingTrackPositions(TRUE);
	DMAudio.ChangeMusicMode(MUSICMODE_GAME);
	return true;
}

bool CGame::ShutDown(void)
{
	/* The far traffic layer's worker (VICE-133): its thread is joined here, so
	 * nothing of the layer survives the world it simulated. */
	CFarTraffic::ShutDown();

	CReplay::FinishPlayback();
	CReplay::EmptyReplayBuffer();
	CPlane::Shutdown();
	CTrain::Shutdown();
	CScriptPaths::Shutdown();
	CWaterCreatures::RemoveAll();
	CSpecialFX::Shutdown();
	CGarages::Shutdown();
	CMovingThings::Shutdown();
	gPhoneInfo.Shutdown();
	CWeapon::ShutdownWeapons();
	CPedType::Shutdown();
	
	for (int32 i = 0; i < NUMPLAYERS; i++)
	{
		if ( CWorld::Players[i].m_pPed )
		{
			CWorld::Remove(CWorld::Players[i].m_pPed);
			delete CWorld::Players[i].m_pPed;
			CWorld::Players[i].m_pPed = nil;
		}
		
		CWorld::Players[i].Clear();
	}
	
	CRenderer::Shutdown();
	CWorld::ShutDown();
	DMAudio.DestroyAllGameCreatedEntities();
	CModelInfo::ShutDown();
	CAnimManager::Shutdown();
	CCutsceneMgr::Shutdown();
	CVehicleModelInfo::DeleteVehicleColourTextures();
	CVehicleModelInfo::ShutdownEnvironmentMaps();
	CRadar::Shutdown();
	CStreaming::Shutdown();
	CTxdStore::GameShutdown();
	CCollision::Shutdown();
	CWaterLevel::Shutdown();
	CRubbish::Shutdown();
	CClouds::Shutdown();
	CShadows::Shutdown();
	CCoronas::Shutdown();
	CSkidmarks::Shutdown();
	CWeaponEffects::Shutdown();
	CParticle::Shutdown();
	CPools::ShutDown();
	CHud::ReInitialise();
	CTxdStore::RemoveTxdSlot(gameTxdSlot);
	CMBlur::MotionBlurClose();
	CdStreamRemoveImages();
	return true;
}

void CGame::ReInitGameObjectVariables(void)
{
	CGameLogic::InitAtStartOfGame();
#ifdef PS2_MENU
	if ( !TheMemoryCard.m_bWantToLoad )
#endif
	{
		TheCamera.Init();
		TheCamera.SetRwCamera(Scene.camera);
	}
	CDebug::DebugInitTextBuffer();
	CWeather::Init();
	CUserDisplay::Init();
	CMessages::Init();
	CRestart::Initialise();
	CWorld::bDoingCarCollisions = false;
	CHud::ReInitialise();
	CRadar::Initialise();
	CCarCtrl::ReInit();
	CTimeCycle::Initialise();
	CDraw::SetFOV(120.0f);
	CDraw::ms_fLODDistance = 500.0f;
	CStreaming::RequestBigBuildings(LEVEL_GENERIC);
	CStreaming::RemoveIslandsNotUsed(LEVEL_BEACH);
	CStreaming::RemoveIslandsNotUsed(LEVEL_MAINLAND);
	/* The spawn readiness (VICE-151): a restart and a save load rebuild the
	 * world, so the prewarm state is dropped here and rebuilt by the load's
	 * own host (DoGameSpecificStuffAfterSucessLoad) or by the boot-like
	 * frames of a plain restart. */
	CSpawnReadiness::Reset();
	CStreaming::LoadAllRequestedModels(false);
	currArea = AREA_MAIN_MAP;
	CPed::Initialise();
	CEventList::Initialise();
#ifdef SCREEN_DROPLETS
	ScreenDroplets::Initialise();
#endif
	CWeapon::InitialiseWeapons();
	CPopulation::Initialise();
	
	for (int i = 0; i < NUMPLAYERS; i++)
		CWorld::Players[i].Clear();
	
	CWorld::PlayerInFocus = 0;
	CAntennas::Init();
	CGlass::Init();
	gPhoneInfo.Initialise();

	PUSH_MEMID(MEMID_SCRIPT);
	CTheScripts::Init();
	CGangs::Initialise();
	POP_MEMID();

	CTimer::Initialise();
	CClock::Initialise(1000);
	CTheCarGenerators::Init();
	CHeli::InitHelis();
	CMovingThings::Init();
	CDarkel::Init();
	CStats::Init();
	CPickups::Init();
	CPacManPickups::Init();
	CGarages::Init();
	CSpecialFX::Init();
	CRopes::Init();
	CWaterCannons::Init();
	CScriptPaths::Init();
	CParticle::ReloadConfig();

#ifdef PS2_MENU
	if ( !TheMemoryCard.m_bWantToLoad )
#else
	if ( !FrontEndMenuManager.m_bWantToLoad )
#endif
	{
		CCranes::InitCranes();
		CTheScripts::StartTestScript();
		CTheScripts::Process();
		TheCamera.Process();
		CTrain::InitTrains();
		CPlane::InitPlanes();
	}
	
	for (int32 i = 0; i < MAX_PADS; i++)
		CPad::GetPad(i)->Clear(true);
}

void CGame::ReloadIPLs(void)
{
	// Empty and unused
}

void CGame::ShutDownForRestart(void)
{
	CReplay::FinishPlayback();
	CReplay::EmptyReplayBuffer();
	DMAudio.DestroyAllGameCreatedEntities();
	CMovingThings::Shutdown();
	
	for (int i = 0; i < NUMPLAYERS; i++)
		CWorld::Players[i].Clear();

	CGarages::SetAllDoorsBackToOriginalHeight();
	CTheScripts::UndoBuildingSwaps();
	CTheScripts::UndoEntityInvisibilitySettings();
	CWorld::ClearForRestart();
	CGameLogic::ClearShortCut();
	CTimer::Shutdown();
	/* The spawn readiness (VICE-151): the pins are released before the
	 * streamer's own sweep, so ReInit's removal sees the flags already
	 * gone and its tracker sweep is clean. */
	CSpawnReadiness::Reset();
	CStreaming::ReInit();
	CRadar::RemoveRadarSections();
	FrontEndMenuManager.UnloadTextures();
	CParticleObject::RemoveAllExpireableParticleObjects();
	CWaterCreatures::RemoveAll(); 
	CSetPieces::Init();
	CPedType::Shutdown();
	CSpecialFX::Shutdown();
}

void CGame::InitialiseWhenRestarting(void)
{
	CRect rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	CRGBA color(255, 255, 255, 255);
	
	CTimer::Initialise();
	CSprite2d::SetRecipNearClip();

	if (b_FoundRecentSavedGameWantToLoad || FrontEndMenuManager.m_bWantToLoad)
	{
		LoadSplash("splash1");
#ifndef XBOX_MESSAGE_SCREEN
		if (FrontEndMenuManager.m_bWantToLoad)
			FrontEndMenuManager.MessageScreen("FELD_WR", true);
#endif
	}

	b_FoundRecentSavedGameWantToLoad = false;
	
	TheCamera.Init();
	
	if ( FrontEndMenuManager.m_bWantToLoad == true )
	{
#ifdef XBOX_MESSAGE_SCREEN
		FrontEndMenuManager.SetDialogTimer(1000);
		DoRWStuffStartOfFrame(0, 0, 0, 0, 0, 0, 0);
		CSprite2d::InitPerFrame();
		CFont::InitPerFrame();
		FrontEndMenuManager.DrawOverlays();
		DoRWStuffEndOfFrame();
#endif
		RestoreForStartLoad();
	}
	
	ReInitGameObjectVariables();
	
	if ( FrontEndMenuManager.m_bWantToLoad == true )
	{
		FrontEndMenuManager.m_bWantToLoad = false;
		InitRadioStationPositionList();
		if ( GenericLoad() == true )
		{
			DMAudio.ResetTimers(CTimer::GetTimeInMilliseconds());
			CTrain::InitTrains();
			CPlane::InitPlanes();
		}
		else
		{
			for ( int32 i = 0; i < 50; i++ )
			{
				HandleExit();
				FrontEndMenuManager.MessageScreen("FED_LFL", true); // Loading save game has failed. The game will restart now. 
			}
			
			TheCamera.SetFadeColour(0, 0, 0);
			ShutDownForRestart();
			CTimer::Stop();
			CTimer::Initialise();
			FrontEndMenuManager.m_bWantToLoad = false;
			ReInitGameObjectVariables();
			currLevel = LEVEL_GENERIC;
			CCollision::SortOutCollisionAfterLoad();
		}
#ifdef XBOX_MESSAGE_SCREEN
		FrontEndMenuManager.ProcessDialogTimer();
#endif
	}
	
	CTimer::Update();
	
	DMAudio.ChangeMusicMode(MUSICMODE_GAME);
}

void CGame::Process(void) 
{
	/* The far traffic layer's worker (VICE-133): its cycle of the previous
	 * frame is complete here and its output is published, so the worker is idle
	 * for the whole of this frame - in particular during CWorld::Process below
	 * and the render after it (the contract in FarTrafficWorker.h; a debug
	 * build asserts it). The cycle of this frame is handed back at the end of
	 * Idle, after the render. */
	CFarTraffic::JoinWorker();

	CPad::UpdatePads();
#ifdef USE_CUSTOM_ALLOCATOR
	ProcessTidyUpMemory();
#endif
#ifdef DEBUGMENU
	DebugMenuProcess();
#endif
	CCutsceneMgr::Update();

	if (!CCutsceneMgr::IsCutsceneProcessing() && !CTimer::GetIsCodePaused())
		FrontEndMenuManager.Process();

	CTheZones::Update();
#ifdef SECUROM
	if (CTimer::GetTimeInMilliseconds() >= (35 * 60 * 1000) && gameProcessPirateCheck == 0){
		// if game not pirated
		// gameProcessPirateCheck = 1;
		// else
		gameProcessPirateCheck = 2;
	}
#endif
	uint32 startTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();
	CStreaming::Update();
	uint32 processTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond() - startTime;
	CWindModifiers::Number = 0;
	if (!CTimer::GetIsPaused())
	{
		// The population caps follow the ring, and the ring follows the draw
		// distance and the weather (TrafficRings.h). The menu above is where the
		// range control moves, so refresh the caps here, once a frame: before the
		// population and the traffic of this frame read them, and with the draw
		// distance of this frame (CurrentTrafficRings composes it live; the
		// camera runs later in the frame). The far clip is the one of the last
		// time cycle, one frame old, exactly as it is for every reader of the
		// frame.
		CIniFile::ApplyPopulationCaps();
#ifndef MASTER
		if (VarUpdatePlayerCoords) {
			FindPlayerPed()->Teleport(PlayerCoords);
			VarUpdatePlayerCoords = false;
		}
#endif
		CSprite2d::SetRecipNearClip();
		CSprite2d::InitPerFrame();
		CFont::InitPerFrame();
		CRecordDataForGame::SaveOrRetrieveDataForThisFrame();
		CRecordDataForChase::SaveOrRetrieveDataForThisFrame();
		CPad::DoCheats();
		CClock::Update();
		CWeather::Update();

		PUSH_MEMID(MEMID_SCRIPT);
		CTheScripts::Process();
		POP_MEMID();

		CCollision::Update();
		CScriptPaths::Update();
		CTrain::UpdateTrains();
		CPlane::UpdatePlanes();
		CHeli::UpdateHelis();
		CDarkel::Update();
		CSkidmarks::Update();
		CAntennas::Update();
		CGlass::Update();
#ifdef GTA_SCENE_EDIT
		CSceneEdit::Update();
#endif
		CSetPieces::Update();
		CEventList::Update();
		CParticle::Update();
		gFireManager.Update();

		// Otherwise even on 30 fps most probably you won't see any peds around Ocean View Hospital
#if defined FIX_BUGS && !defined SQUEEZE_PERFORMANCE
		if (processTime > 2) {
#else
		if (processTime >= 2) {
#endif
			CPopulation::Update(false);
		} else {
			CPopulation::Update(true);
		}
		CWeapon::UpdateWeapons();
		if (!CCutsceneMgr::IsRunning())
			CTheCarGenerators::Process();
		if (!CReplay::IsPlayingBack())
			CCranes::UpdateCranes();
		CClouds::Update();
		CMovingThings::Update();
		CWaterCannons::Update();
		CUserDisplay::Process();
		CReplay::Update();

		PUSH_MEMID(MEMID_WORLD);
		CWorld::Process();
		POP_MEMID();

		gAccidentManager.Update();
		CPacManPickups::Update();
		CPickups::Update();
		CGarages::Update();
		CRubbish::Update();
		CSpecialFX::Update();
		CRopes::Update();
		CTimeCycle::Update();

		// The drift feedback is stepped once a frame here: after the physics
		// step wrote the drift state, and before the camera below reads the
		// strength (see DriftFeedback.h and ADR 0012). IsDriftCar keeps the
		// step on the player's car; the enable flag is not tested here, so
		// the module's own check still runs every frame.
		CVehicle *playerVeh = FindPlayerVehicle();
		if(IsDriftCar(playerVeh))
			CDriftFeedback::Update((CAutomobile*)playerVeh, CTimer::GetTimeStepInSeconds());

		if (CReplay::ShouldStandardCameraBeProcessed())
			TheCamera.Process();
		CCullZones::Update();
		if (!CReplay::IsPlayingBack())
			CGameLogic::Update();
		CBridge::Update();
		CCoronas::DoSunAndMoon();
		CCoronas::Update();
		CShadows::UpdateStaticShadows();
		CShadows::UpdatePermanentShadows();
		gPhoneInfo.Update();
		if (!CReplay::IsPlayingBack())
		{
			PUSH_MEMID(MEMID_CARS);
			CCarCtrl::GenerateRandomCars();
			CRoadBlocks::GenerateRoadBlocks();
			CCarCtrl::RemoveDistantCars();
			CCarCtrl::RemoveCarsIfThePoolGetsFull();
			/* The far traffic layer (VICE-128) runs last in the car block:
			 * the near spawner and the culls have had their say about the
			 * pool, the frame's bubble budget and the near fill, and the
			 * layer fills the band between the near traffic and the fog with
			 * its own cheap cars, promotes what the player has come near and
			 * takes over what the cull has just handed it. */
			CFarTraffic::Process();
			/* The spawn sites' flags (VICE-148): the script's road toggles
			 * and a save load move the graph's node flags, so the sites
			 * re-derive their own flags here - never a rebuild. The version
			 * check makes a frame with no toggle one comparison. */
			CSpawnSites::RefreshFlags();
			POP_MEMID();
		}
	}
#ifdef GTA_PS2
	CMemCheck::DoTest();
#endif
}

#ifdef USE_CUSTOM_ALLOCATOR

// TODO(MIAMI)

int32 gNumMemMoved;

bool
MoveMem(void** ptr)
{
	if (*ptr) {
		gNumMemMoved++;
		void* newPtr = gMainHeap.MoveMemory(*ptr);
		if (*ptr != newPtr) {
			*ptr = newPtr;
			return true;
		}
	}
	return false;
}

// Some convenience structs
struct SkyDataPrefix
{
	uint32 pktSize1;
	uint32 data;	// pointer to data as read from TXD
	uint32 pktSize2;
	uint32 unused;
};

struct DMAGIFUpload
{
	uint32 tag1_qwc, tag1_addr;	// dmaref
	uint32 nop1, vif_direct1;

	uint32 giftag[4];
	uint32 gs_bitbltbuf[4];

	uint32 tag2_qwc, tag2_addr;	// dmaref
	uint32 nop2, vif_direct2;
};

// This is very scary. it depends on the exact memory layout of the DMA chains and whatnot
RwTexture*
MoveTextureMemoryCB(RwTexture* texture, void* pData)
{
#ifdef GTA_PS2
	bool* pRet = (bool*)pData;
	RwRaster* raster = RwTextureGetRaster(texture);
	_SkyRasterExt* rasterExt = RASTEREXTFROMRASTER(raster);
	if (raster->originalPixels == nil ||	// the raw data
		raster->cpPixels == raster->originalPixels ||	// old format, can't handle it
		rasterExt->dmaRefCount != 0 && rasterExt->dmaClrCount != 0)
		return texture;

	// this is the allocated pointer we will move
	SkyDataPrefix* prefix = (SkyDataPrefix*)raster->originalPixels;
	DMAGIFUpload* uploads = (DMAGIFUpload*)(prefix + 1);

	// We have 4qw for each upload,
	// i.e. for each buffer width of mip levels,
	// and the palette if there is one.
	// NB: this code does NOT support mipmaps!
	// so we assume two uploads (pixels and palette)
	//
	// each upload looks like this:
	//    (DMAcnt; NOP; VIF DIRECT(2))
	//     giftag (1, A+D)
	//      GS_BITBLTBUF
	//    (DMAref->pixel data; NOP; VIF DIRECT(5))
	// the DMArefs are what we have to adjust
	uintptr dataDiff, upload1Diff, upload2Diff, pixelDiff, paletteDiff;
	dataDiff = prefix->data - (uintptr)raster->originalPixels;
	upload1Diff = uploads[0].tag2_addr - (uintptr)raster->originalPixels;
	if (raster->palette)
		upload2Diff = uploads[1].tag2_addr - (uintptr)raster->originalPixels;
	pixelDiff = (uintptr)raster->cpPixels - (uintptr)raster->originalPixels;
	if (raster->palette)
		paletteDiff = (uintptr)raster->palette - (uintptr)raster->originalPixels;
	uint8* newptr = (uint8*)gMainHeap.MoveMemory(raster->originalPixels);
	if (newptr != raster->originalPixels) {
		// adjust everything
		prefix->data = (uintptr)newptr + dataDiff;
		uploads[0].tag2_addr = (uintptr)newptr + upload1Diff;
		if (raster->palette)
			uploads[1].tag2_addr = (uintptr)newptr + upload2Diff;
		raster->originalPixels = newptr;
		raster->cpPixels = newptr + pixelDiff;
		if (raster->palette)
			raster->palette = newptr + paletteDiff;

		if (pRet) {
			*pRet = true;
			return nil;
		}
	}
#else
	// nothing to do here really, everything should be in videomemory
#endif
	return texture;
}

bool
MoveAtomicMemory(RpAtomic* atomic, bool onlyOne)
{
	RpGeometry* geo = RpAtomicGetGeometry(atomic);

#if THIS_IS_COMPATIBLE_WITH_GTA3_RW31
	if (MoveMem((void**)&geo->triangles) && onlyOne)
		return true;
	if (MoveMem((void**)&geo->matList.materials) && onlyOne)
		return true;
	if (MoveMem((void**)&geo->preLitLum) && onlyOne)
		return true;
	if (MoveMem((void**)&geo->texCoords[0]) && onlyOne)
		return true;
	if (MoveMem((void**)&geo->texCoords[1]) && onlyOne)
		return true;

	// verts and normals of morph target are allocated together
	int vertDiff;
	if (geo->morphTarget->normals)
		vertDiff = geo->morphTarget->normals - geo->morphTarget->verts;
	if (MoveMem((void**)&geo->morphTarget->verts)) {
		if (geo->morphTarget->normals)
			geo->morphTarget->normals = geo->morphTarget->verts + vertDiff;
		if (onlyOne)
			return true;
	}

	RpMeshHeader* oldmesh = geo->mesh;
	if (MoveMem((void**)&geo->mesh)) {
		// index pointers are allocated together with meshes,
		// have to relocate those too
		RpMesh* mesh = (RpMesh*)(geo->mesh + 1);
		uintptr reloc = (uintptr)geo->mesh - (uintptr)oldmesh;
		for (int i = 0; i < geo->mesh->numMeshes; i++)
			mesh[i].indices = (RxVertexIndex*)((uintptr)mesh[i].indices + reloc);
		if (onlyOne)
			return true;
	}
#else
	// we could do something in librw here
#endif
	return false;
}

bool
MoveColModelMemory(CColModel& colModel, bool onlyOne)
{
#if GTA_VERSION >= GTA3_PS2_160
	// hm...should probably only do this if ownsCollisionVolumes
	// but it doesn't exist on PS2...
	if (!colModel.ownsCollisionVolumes)
		return false;
#endif

	if (MoveMem((void**)&colModel.spheres) && onlyOne)
		return true;
	if (MoveMem((void**)&colModel.lines) && onlyOne)
		return true;
	if (MoveMem((void**)&colModel.boxes) && onlyOne)
		return true;
	if (MoveMem((void**)&colModel.vertices) && onlyOne)
		return true;
	if (MoveMem((void**)&colModel.triangles) && onlyOne)
		return true;
	if (MoveMem((void**)&colModel.trianglePlanes) && onlyOne)
		return true;
	if (MoveMem((void**)&colModel.triangleBasis) && onlyOne)
		return true;
	if (MoveMem((void**)&colModel.triangleAccel) && onlyOne)
		return true;
	return false;
}

RpAtomic*
MoveAtomicMemoryCB(RpAtomic* atomic, void* pData)
{
	bool* pRet = (bool*)pData;
	if (pRet == nil)
		MoveAtomicMemory(atomic, false);
	else if (MoveAtomicMemory(atomic, true)) {
		*pRet = true;
		return nil;
	}
	return atomic;
}

bool
TidyUpModelInfo(CBaseModelInfo* modelInfo, bool onlyone)
{
	if (modelInfo->GetColModel() && modelInfo->DoesOwnColModel())
		if (MoveColModelMemory(*modelInfo->GetColModel(), onlyone))
			return true;

	RwObject* rwobj = modelInfo->GetRwObject();
	if(rwobj){
		if (RwObjectGetType(rwobj) == rpATOMIC)
			if (MoveAtomicMemory((RpAtomic*)rwobj, onlyone))
				return true;
		if (RwObjectGetType(rwobj) == rpCLUMP) {
			bool ret = false;
			if (onlyone)
				RpClumpForAllAtomics((RpClump*)rwobj, MoveAtomicMemoryCB, &ret);
			else
				RpClumpForAllAtomics((RpClump*)rwobj, MoveAtomicMemoryCB, nil);
			if (ret)
				return true;
		}
	}

	if (modelInfo->GetModelType() == MITYPE_PED && ((CPedModelInfo*)modelInfo)->m_hitColModel)
		if (MoveColModelMemory(*((CPedModelInfo*)modelInfo)->m_hitColModel, onlyone))
			return true;

	return false;
}
#endif


void CGame::DrasticTidyUpMemory(bool flushDraw)
{
#ifdef USE_CUSTOM_ALLOCATOR
	bool removedCol = false;

	TidyUpMemory(true, flushDraw);

	if (gMainHeap.GetLargestFreeBlock() < 200000 && !playingIntro) {
		CStreaming::RemoveIslandsNotUsed(LEVEL_INDUSTRIAL);
		CStreaming::RemoveIslandsNotUsed(LEVEL_COMMERCIAL);
		CStreaming::RemoveIslandsNotUsed(LEVEL_SUBURBAN);
		TidyUpMemory(true, flushDraw);
	}

	if (gMainHeap.GetLargestFreeBlock() < 200000 && !playingIntro) {
		CModelInfo::RemoveColModelsFromOtherLevels(LEVEL_GENERIC);
		TidyUpMemory(true, flushDraw);
		removedCol = true;
	}

	if (gMainHeap.GetLargestFreeBlock() < 200000 && !playingIntro) {
		CStreaming::RemoveBigBuildings(LEVEL_INDUSTRIAL);
		CStreaming::RemoveBigBuildings(LEVEL_COMMERCIAL);
		CStreaming::RemoveBigBuildings(LEVEL_SUBURBAN);
		TidyUpMemory(true, flushDraw);
	}

	if (removedCol) {
		// different on PS2
		CFileLoader::LoadCollisionFromDatFile(CCollision::ms_collisionInMemory);
	}

	if (!playingIntro)
		CStreaming::RequestBigBuildings(currLevel);

	CStreaming::LoadAllRequestedModels(true);
#endif
}

void CGame::TidyUpMemory(bool moveTextures, bool flushDraw)
{
#ifdef USE_CUSTOM_ALLOCATOR
	printf("Largest free block before tidy %d\n", gMainHeap.GetLargestFreeBlock());

	if (moveTextures) {
		if (flushDraw) {
#ifdef GTA_PS2
			for (int i = 0; i < sweMaxFlips + 1; i++) {
#else
			for (int i = 0; i < 5; i++) {	// probably more than needed
#endif
				RwCameraBeginUpdate(Scene.camera);
				RwCameraEndUpdate(Scene.camera);
				RwCameraShowRaster(Scene.camera, nil, 0);
			}
			}
		int fontSlot = CTxdStore::FindTxdSlot("fonts");

		for (int i = 0; i < TXDSTORESIZE; i++) {
			if (i == fontSlot ||
				CTxdStore::GetSlot(i) == nil)
				continue;
			RwTexDictionary* txd = CTxdStore::GetSlot(i)->texDict;
			if (txd)
				RwTexDictionaryForAllTextures(txd, MoveTextureMemoryCB, nil);
		}
		}

	// animations
	for (int i = 0; i < NUMANIMATIONS; i++) {
		CAnimBlendHierarchy* anim = CAnimManager::GetAnimation(i);
		if (anim == nil)
			continue;	// cannot happen
		anim->MoveMemory();
	}

	// model info
	for (int i = 0; i < MODELINFOSIZE; i++) {
		CBaseModelInfo* mi = CModelInfo::GetModelInfo(i);
		if (mi == nil)
			continue;
		TidyUpModelInfo(mi, false);
	}

	printf("Largest free block after tidy %d\n", gMainHeap.GetLargestFreeBlock());
#endif
	}

void CGame::ProcessTidyUpMemory(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	static int32 modelIndex = 0;
	static int32 animIndex = 0;
	static int32 txdIndex = 0;
	bool txdReturn = false;
	RwTexDictionary* txd = nil;
	gNumMemMoved = 0;

	// model infos
	for (int numCleanedUp = 0; numCleanedUp < 10; numCleanedUp++) {
		CBaseModelInfo* mi;
		do {
			mi = CModelInfo::GetModelInfo(modelIndex);
			modelIndex++;
			if (modelIndex >= MODELINFOSIZE)
				modelIndex = 0;
		} while (mi == nil);

		if (TidyUpModelInfo(mi, true))
			return;
	}

	// tex dicts
	for (int numCleanedUp = 0; numCleanedUp < 3; numCleanedUp++) {
		if (gNumMemMoved > 80)
			break;

		do {
#ifdef FIX_BUGS
			txd = nil;
#endif
			if (CTxdStore::GetSlot(txdIndex))
				txd = CTxdStore::GetSlot(txdIndex)->texDict;
			txdIndex++;
			if (txdIndex >= TXDSTORESIZE)
				txdIndex = 0;
		} while (txd == nil);

		RwTexDictionaryForAllTextures(txd, MoveTextureMemoryCB, &txdReturn);
		if (txdReturn)
			return;
		}

	// animations
	CAnimBlendHierarchy* anim;
	do {
		anim = CAnimManager::GetAnimation(animIndex);
		animIndex++;
		if (animIndex >= NUMANIMATIONS)
			animIndex = 0;
	} while (anim == nil);	// always != nil
	anim->MoveMemory(true);
#endif
}

void
CGame::InitAfterFocusLoss()
{
	FrontEndMenuManager.m_nPrefsAudio3DProviderIndex = FrontEndMenuManager.m_lastWorking3DAudioProvider;
	DMAudio.SetCurrent3DProvider(FrontEndMenuManager.m_lastWorking3DAudioProvider);

	if (!FrontEndMenuManager.m_bGameNotLoaded && !FrontEndMenuManager.m_bMenuActive)
		FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
}

bool
CGame::CanSeeWaterFromCurrArea(void)
{
	return currArea == AREA_MAIN_MAP || currArea == AREA_MANSION
		|| currArea == AREA_HOTEL;
}

/*
 * The writer of the visible area for the script and the restart (VICE-126).
 *
 * The script's COMMAND_SET_AREA_VISIBLE comes through here, so the area change
 * that *leaves an interior* is one place: the player steps out into the street
 * and the population and the traffic fill at once (ArmFillBursts).
 *
 * It is not the only writer of CGame::currArea, and the ADR says so: the boot
 * (CGame::Initialise), the reset (ReInitGameObjectVariables), the save load
 * (CGenericGameStorage reads the field out of the save file) and the replay
 * player (CReplay::Process and its restore) write the field directly. None of
 * them arms a burst: the boot and the reset arm through the countdowns of the
 * two populations, and a replay is a recording, not a fill. A loaded save that
 * restores an interior therefore arms nothing; the exit from that interior
 * comes back through here and arms the burst, which is the moment the street
 * appears.
 *
 * The fill of an interior *entry* is not armed: an interior is small, its
 * budget is the flat one, and the interior transition usually hides the world
 * behind walls, not behind a fade.
 */
void
CGame::SetCurrArea(int32 area)
{
	bool leftInterior = IsInInterior() && area == AREA_MAIN_MAP;

	currArea = area;

	if (leftInterior) {
		ArmFillBursts(false);
		/* The spawn readiness (VICE-151): the interior exit is a host - the
		 * street's zone set is requested here, so the fill burst's own gate
		 * finds the models on their way. The requests ride the ordinary
		 * streaming; the street appears behind walls, not behind a fade. */
		CSpawnReadiness::OnInteriorExit();
	}
}

/*
 * Give the fill burst to both populations (VICE-126): after a transition that
 * hides the world - an interior exit, a cutscene end - the street fills at once
 * instead of trickling up. The burst itself, its bounds and its gates live in
 * FillBurst.h; the triggers live here, in CPopulation::Update and
 * CCarCtrl::GenerateRandomCars (the boot and restart countdowns) and in
 * CCutsceneMgr::DeleteCutsceneData (the end of a cutscene).
 *
 * `withParkedCars` is the generator bypass of the car burst: the shipped arms
 * (the countdowns, the restart, the cutscene end) had it, the interior exit
 * does not. CCarCtrl::ArmFillBurst documents what it does.
 *
 * A replay playback rebuilds the world from the recording: the fill is under
 * the same gate as the population and the streaming (Game.cpp), so the burst
 * of a transition is not armed there.
 */
void
CGame::ArmFillBursts(bool withParkedCars)
{
	if (CReplay::IsPlayingBack())
		return;
	CPopulation::ArmFillBurst();
	CCarCtrl::ArmFillBurst(withParkedCars);
}

bool
CGame::CanSeeOutSideFromCurrArea(void)
{
	return currArea == AREA_MAIN_MAP || currArea == AREA_MALL ||
		currArea == AREA_MANSION || currArea == AREA_HOTEL;
}
