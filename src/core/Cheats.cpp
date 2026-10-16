#include "common.h"
#include "Cheats.h"
#include "Pad.h"
#include "Timer.h"
#include "Hud.h"
#include "Text.h"
#include "World.h"
#include "Vehicle.h"
#include "Pools.h"
#include "Ped.h"
#include "PlayerPed.h"
#include "Population.h"
#include "Weather.h"
#include "Streaming.h"
#include "PathFind.h"
#include "Wanted.h"
#include "WaterLevel.h"
#include "General.h"
#include "Fluff.h"
#include "Gangs.h"
#include "Stats.h"
#include "CarCtrl.h"
#include "TrafficLights.h"

_TODO("gbFastTime");
extern bool gbFastTime;

#ifdef WALLCLIMB_CHEAT
extern bool gGravityCheat;
#endif

void SpecialCarCheats()
{
	if ( !CVehicle::bCheat9 )
	{
		((CVehicleModelInfo*)CModelInfo::GetModelInfo(MI_INFERNUS))->m_wheelScale *= 1.3f;
		((CVehicleModelInfo*)CModelInfo::GetModelInfo(MI_CHEETAH))->m_wheelScale *= 1.3f;
		((CVehicleModelInfo*)CModelInfo::GetModelInfo(MI_PHEONIX))->m_wheelScale *= 1.3f;
		((CVehicleModelInfo*)CModelInfo::GetModelInfo(MI_DELUXO))->m_wheelScale *= 1.3f;
		mod_HandlingManager.GetHandlingData(HANDLING_LANDSTAL)->Transmission.fEngineAcceleration *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_PATRIOT)->Transmission.fEngineAcceleration *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_BOBCAT)->Transmission.fEngineAcceleration *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_BFINJECT)->Transmission.fEngineAcceleration *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_RANCHER)->Transmission.fEngineAcceleration *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_DESPERAD)->Transmission.fEngineAcceleration *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_DELUXO)->Transmission.fEngineAcceleration *= 2.0f;

		mod_HandlingManager.GetHandlingData(HANDLING_BAGGAGE)->Transmission.fEngineAcceleration *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_BAGGAGE)->Transmission.fMaxVelocity *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_BAGGAGE)->Transmission.InitGearRatios();

		mod_HandlingManager.GetHandlingData(HANDLING_GOLFCART)->Transmission.fEngineAcceleration *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_GOLFCART)->Transmission.fMaxVelocity *= 2.0f;
		mod_HandlingManager.GetHandlingData(HANDLING_GOLFCART)->Transmission.InitGearRatios();

		mod_HandlingManager.GetHandlingData(HANDLING_STINGER)->fCollisionDamageMultiplier *= 0.25f;
		mod_HandlingManager.GetHandlingData(HANDLING_STINGER)->fMass *= 2.5f;
		mod_HandlingManager.GetHandlingData(HANDLING_STINGER)->fTurnMass *= 4.0f;

		mod_HandlingManager.GetHandlingData(HANDLING_BANSHEE)->fCollisionDamageMultiplier *= 0.25f;
		mod_HandlingManager.GetHandlingData(HANDLING_BANSHEE)->fMass *= 2.5f;
		mod_HandlingManager.GetHandlingData(HANDLING_BANSHEE)->fTurnMass *= 4.0f;

		mod_HandlingManager.GetHandlingData(HANDLING_SABRETUR)->fCollisionDamageMultiplier *= 0.25f;
		mod_HandlingManager.GetHandlingData(HANDLING_SABRETUR)->fMass *= 2.5f;
		mod_HandlingManager.GetHandlingData(HANDLING_SABRETUR)->fTurnMass *= 4.0f;

		mod_HandlingManager.GetHandlingData(HANDLING_COMET)->fCollisionDamageMultiplier *= 0.25f;
		mod_HandlingManager.GetHandlingData(HANDLING_COMET)->fMass *= 2.5f;
		mod_HandlingManager.GetHandlingData(HANDLING_COMET)->fTurnMass *= 4.0f;

		mod_HandlingManager.GetHandlingData(HANDLING_DELUXO)->fCollisionDamageMultiplier *= 0.25f;
		mod_HandlingManager.GetHandlingData(HANDLING_DELUXO)->fMass *= 2.5f;
		mod_HandlingManager.GetHandlingData(HANDLING_DELUXO)->fTurnMass *= 4.0f;
		CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
		CVehicle::bCheat9 = true;
		CPad::bHasPlayerCheated = true;
	}
}

void PickUpChicksCheat()
{
	if ( FindPlayerVehicle() && (FindPlayerVehicle()->IsCar() || FindPlayerVehicle()->IsBike()) )
	{
		CVehicle *vehicle = FindPlayerVehicle();
		if ( FindPlayerVehicle()->IsBike() )
		{
			if ( vehicle->pPassengers[0] )
				vehicle->pPassengers[0]->SetObjective(OBJECTIVE_LEAVE_CAR, vehicle);
		}
		CPed* someoneElse = (CPed*)CWorld::TestSphereAgainstWorld(vehicle->GetPosition(), 6.0f, FindPlayerPed(), false, false, true, false, false, false);
		if ( someoneElse && someoneElse->m_nPedState != PED_DRIVING )
		{
			CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
			someoneElse->SetObjective(OBJECTIVE_ENTER_CAR_AS_PASSENGER, vehicle);
		}
	}
}

void WeaponCheat1()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT2"), true);

	CStreaming::RequestModel(MI_BRASS_KNUCKLES, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_BASEBALL_BAT, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_MOLOTOV, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_COLT45, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_SHOTGUN, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_TEC9, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_RUGER, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_SNIPERRIFLE, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_FLAMETHROWER, STREAMFLAGS_DONT_REMOVE);
	CStreaming::LoadAllRequestedModels(false);

	FindPlayerPed()->GiveWeapon(WEAPONTYPE_BRASSKNUCKLE, 1);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_BASEBALLBAT, 1);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_MOLOTOV, 10);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_COLT45, 100);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_SHOTGUN, 50);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_TEC9, 150);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_RUGER, 120);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_SNIPERRIFLE, 25);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_FLAMETHROWER, 200);

	CStreaming::SetModelIsDeletable(MI_BRASS_KNUCKLES);
	CStreaming::SetModelIsDeletable(MI_BASEBALL_BAT);
	CStreaming::SetModelIsDeletable(MI_MOLOTOV);
	CStreaming::SetModelIsDeletable(MI_COLT45);
	CStreaming::SetModelIsDeletable(MI_SHOTGUN);
	CStreaming::SetModelIsDeletable(MI_TEC9);
	CStreaming::SetModelIsDeletable(MI_RUGER);
	CStreaming::SetModelIsDeletable(MI_SNIPERRIFLE);
	CStreaming::SetModelIsDeletable(MI_FLAMETHROWER);
#ifdef MOBILE_IMPROVEMENTS
	if (FindPlayerVehicle()) {
		FindPlayerPed()->RemoveWeaponWhenEnteringVehicle();
	}
#endif
}

void WeaponCheat2()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT2"), true);

	CStreaming::RequestModel(MI_KATANA, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_GRENADE, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_BOMB, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_PYTHON, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_STUBBY_SHOTGUN, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_SILENCEDINGRAM, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_M4, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_LASERSCOPE, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_ROCKETLAUNCHER, STREAMFLAGS_DONT_REMOVE);
	CStreaming::LoadAllRequestedModels(false);

#ifdef FIX_BUGS
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_KATANA, 1);
#else
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_KATANA, 0);
#endif
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_DETONATOR_GRENADE, 10);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_PYTHON, 40);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_STUBBY_SHOTGUN, 25);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_SILENCED_INGRAM, 100);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_M4, 150);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_LASERSCOPE, 21);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_ROCKETLAUNCHER, 5);

	CStreaming::SetModelIsDeletable(MI_KATANA);
	CStreaming::SetModelIsDeletable(MI_GRENADE);
	CStreaming::SetModelIsDeletable(MI_BOMB);
	CStreaming::SetModelIsDeletable(MI_PYTHON);
	CStreaming::SetModelIsDeletable(MI_STUBBY_SHOTGUN);
	CStreaming::SetModelIsDeletable(MI_SILENCEDINGRAM);
	CStreaming::SetModelIsDeletable(MI_M4);
	CStreaming::SetModelIsDeletable(MI_LASERSCOPE);
	CStreaming::SetModelIsDeletable(MI_ROCKETLAUNCHER);
#ifdef MOBILE_IMPROVEMENTS
	if (FindPlayerVehicle()) {
		FindPlayerPed()->RemoveWeaponWhenEnteringVehicle();
	}
#endif
}

void WeaponCheat3()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT2"), true);

	CStreaming::RequestModel(MI_CHAINSAW, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_GRENADE, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_PYTHON, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_SPAS12_SHOTGUN, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_MP5, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_M4, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_LASERSCOPE, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_MINIGUN, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_MINIGUN2, STREAMFLAGS_DONT_REMOVE);
	CStreaming::LoadAllRequestedModels(false);

#ifdef FIX_BUGS
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_CHAINSAW, 1);
#else
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_CHAINSAW, 0);
#endif
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_GRENADE, 10);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_PYTHON, 40);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_SPAS12_SHOTGUN, 30);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_MP5, 100);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_M4, 150);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_LASERSCOPE, 21);
	FindPlayerPed()->GiveWeapon(WEAPONTYPE_MINIGUN, 500);

	CStreaming::SetModelIsDeletable(MI_CHAINSAW);
	CStreaming::SetModelIsDeletable(MI_GRENADE);
	CStreaming::SetModelIsDeletable(MI_PYTHON);
	CStreaming::SetModelIsDeletable(MI_SPAS12_SHOTGUN);
	CStreaming::SetModelIsDeletable(MI_MP5);
	CStreaming::SetModelIsDeletable(MI_M4);
	CStreaming::SetModelIsDeletable(MI_LASERSCOPE);
	CStreaming::SetModelIsDeletable(MI_MINIGUN);
	CStreaming::SetModelIsDeletable(MI_MINIGUN2);

#ifdef MOBILE_IMPROVEMENTS
	if (FindPlayerVehicle()) {
		FindPlayerPed()->RemoveWeaponWhenEnteringVehicle();
	}
#endif
}

void HealthCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT3"), true);
	FindPlayerPed()->m_fHealth = CWorld::Players[0].m_nMaxHealth;
	if (FindPlayerVehicle()) {
		FindPlayerVehicle()->m_fHealth = 1000.0f;
		if (FindPlayerVehicle()->m_vehType == VEHICLE_TYPE_CAR) {
			((CAutomobile*)FindPlayerVehicle())->Damage.SetEngineStatus(0);
			for (int32 i = 0; i < 4; i++)
				((CAutomobile*)FindPlayerVehicle())->Damage.SetWheelStatus(i, WHEEL_STATUS_OK);
		}
	}
}

// TODO(Miami): this is HELLA different on mobile, although it mostly has debug oriented things like player exiting it's current car and enters spawned one etc.
void VehicleCheat(int model)
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CStreaming::RequestModel(model, STREAMFLAGS_DONT_REMOVE);
	CStreaming::LoadAllRequestedModels(false);
	if (CStreaming::ms_aInfoForModel[model].m_loadState == STREAMSTATE_LOADED) {
		CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);

		if (!(CStreaming::ms_aInfoForModel[model].m_loadState & STREAMFLAGS_DONT_REMOVE)) {
			CStreaming::SetModelIsDeletable(model);
			CStreaming::SetModelTxdIsDeletable(model);
		}

		int32 node = ThePaths.FindNodeClosestToCoors(FindPlayerCoors(), PATH_CAR, 100.0f);
		if (node < 0) return;

#ifdef FIX_BUGS
		CAutomobile* vehicle = new CAutomobile(model, RANDOM_VEHICLE);
#else
		CAutomobile* vehicle = new CAutomobile(model, MISSION_VEHICLE);
#endif
		if (vehicle != nil) {
			CVector pos = ThePaths.m_pathNodes[node].GetPosition();
			pos.z += 4.0f;
			vehicle->SetPosition(pos);
			vehicle->SetOrientation(0.0f, 0.0f, DEGTORAD(200.0f));

			vehicle->SetStatus(STATUS_ABANDONED);
			vehicle->m_nDoorLock = CARLOCK_UNLOCKED;
			CWorld::Add(vehicle);
		}
	}
	CStats::CheatedCount += 1000;
	CPad::bHasPlayerCheated = true;
}


void BlowUpCarsCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);

	int i = CPools::GetVehiclePool()->GetSize();
	while (i-- > 0) {
		if (CVehicle *veh = CPools::GetVehiclePool()->GetSlot(i))
			veh->BlowUpCar(nil);
	}
}

void ChangePlayerCheat()
{
	// I don't know wtf is going on in here...
	int modelId, anotherModelId;

	if (FindPlayerPed()->IsPedInControl() && CModelInfo::GetModelInfo("player", nil)) {
		CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
		CPlayerPed *ped = FindPlayerPed();
		AssocGroupId AnimGrp = ped->m_animGroup;
		do
		{
			do {
				modelId = CGeneral::GetRandomNumberInRange(0, MI_PGA);
				anotherModelId = modelId+1;
			} while (!CModelInfo::GetModelInfo(anotherModelId));
		} while (anotherModelId >= MI_SPECIAL01 && anotherModelId <= MI_SPECIAL04 || modelId == MI_TAXI_D);

		uint8 flags = CStreaming::ms_aInfoForModel[anotherModelId].m_flags;
		ped->DeleteRwObject();
		CStreaming::RequestModel(anotherModelId, STREAMFLAGS_DEPENDENCY| STREAMFLAGS_DONT_REMOVE);
		CStreaming::LoadAllRequestedModels(false);
		ped->m_modelIndex = -1;
		ped->SetModelIndex(anotherModelId);
		ped->m_animGroup = AnimGrp;
		if (modelId != -1) {
			if (!(flags & STREAMFLAGS_DONT_REMOVE))
				CStreaming::SetModelIsDeletable(anotherModelId);
		}
	}
}

void ChangePlayerModel(const char* name) {
	if (!FindPlayerVehicle()) {
		FindPlayerPed()->Undress(name);
		CStreaming::LoadAllRequestedModels(0);
		FindPlayerPed()->Dress();
	}
}

void MayhemCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	for (int i = PEDTYPE_CIVMALE; i < PEDTYPE_SPECIAL; i++)
		CPedType::SetThreats(i, PED_FLAG_PLAYER1 | PED_FLAG_PLAYER2 | PED_FLAG_PLAYER3 | PED_FLAG_PLAYER4 |
			PED_FLAG_CIVMALE | PED_FLAG_CIVFEMALE | PED_FLAG_COP | PED_FLAG_GANG1 |
			PED_FLAG_GANG2 | PED_FLAG_GANG3 | PED_FLAG_GANG4 | PED_FLAG_GANG5 |
			PED_FLAG_GANG6 | PED_FLAG_GANG7 | PED_FLAG_GANG8 | PED_FLAG_GANG9 |
			PED_FLAG_EMERGENCY | PED_FLAG_PROSTITUTE | PED_FLAG_CRIMINAL | PED_FLAG_SPECIAL );
	CStats::CheatedCount += 1000;
	CPad::bHasPlayerCheated = true;
}

void EverybodyAttacksPlayerCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	for (int i = PEDTYPE_CIVMALE; i < PEDTYPE_SPECIAL; i++)
		CPedType::AddThreat(i, PED_FLAG_PLAYER1);

	CStats::CheatedCount += 1000;
	CPad::bHasPlayerCheated = true;
}

void WeaponsForAllCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CPopulation::ms_bGivePedsWeapons = !CPopulation::ms_bGivePedsWeapons;
	CStats::CheatedCount += 1000;
	CPad::bHasPlayerCheated = true;
}

void FastTimeCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	if (CTimer::GetTimeScale() < 4.0f)
		CTimer::SetTimeScale(CTimer::GetTimeScale() * 2.0f);
}

void SlowTimeCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	if (CTimer::GetTimeScale() > 0.25f)
		CTimer::SetTimeScale(CTimer::GetTimeScale() * 0.5f);
}

void MoneyCheat()
{
	CWorld::Players[CWorld::PlayerInFocus].m_nMoney += 250000;
	CHud::SetHelpMessage(TheText.Get("CHEAT6"), true);
}

void ArmourCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT4"), true);
	FindPlayerPed()->m_fArmour = CWorld::Players[0].m_nMaxArmour;
}

void WantedLevelUpCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT5"), true);
	FindPlayerPed()->m_pWanted->CheatWantedLevel(Min(FindPlayerPed()->m_pWanted->GetWantedLevel() + 2, 6));
}

void WantedLevelDownCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT5"), true);
	FindPlayerPed()->m_pWanted->CheatWantedLevel(0);
}

void SunnyWeatherCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT7"), true);
	CWeather::ForceWeatherNow(WEATHER_SUNNY);
}

void ExtraSunnyWeatherCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT7"), true);
	CWeather::ForceWeatherNow(WEATHER_EXTRA_SUNNY);
}

void CloudyWeatherCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT7"), true);
	CWeather::ForceWeatherNow(WEATHER_CLOUDY);
}

void RainyWeatherCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT7"), true);
	CWeather::ForceWeatherNow(WEATHER_RAINY);
}

void FoggyWeatherCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT7"), true);
	CWeather::ForceWeatherNow(WEATHER_FOGGY);
}

void FastWeatherCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	gbFastTime = !gbFastTime;
}

void OnlyRenderWheelsCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CVehicle::bWheelsOnlyCheat = !CVehicle::bWheelsOnlyCheat;
	CStats::CheatedCount += 1000;
	CPad::bHasPlayerCheated = true;
}

void ChittyChittyBangBangCheat()
{
#ifdef BETTER_ALLCARSAREDODO_CHEAT
	CHud::SetHelpMessage(TheText.Get(!CVehicle::bAllDodosCheat ? "CHEAT1" : "CHEATOF"), true);
#else
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
#endif
	CVehicle::bAllDodosCheat = !CVehicle::bAllDodosCheat;
	CStats::CheatedCount += 1000;
	CPad::bHasPlayerCheated = true;
}

void StrongGripCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CVehicle::bCheat3 = !CVehicle::bCheat3;
	CStats::CheatedCount += 1000;
	CPad::bHasPlayerCheated = true;
}

void FannyMagnetCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CPed::bFannyMagnetCheat = !CPed::bFannyMagnetCheat;
	CPad::bHasPlayerCheated = true;
}

void BlackCarsCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	gbBlackCars = true;
	gbPinkCars = false;
}

void PinkCarsCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	gbBlackCars = false;
	gbPinkCars = true;
}

void TrafficLightsCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CTrafficLights::bGreenLightsCheat = true;
}

void MadCarsCheat()
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CCarCtrl::bMadDriversCheat = true;
}

void NoSeaBedCheat(void)
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CWaterLevel::m_bRenderSeaBed = !CWaterLevel::m_bRenderSeaBed;
}

void RenderWaterLayersCheat(void)
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	if ( ++CWaterLevel::m_nRenderWaterLayers > 5 )
			CWaterLevel::m_nRenderWaterLayers = 0;
}

void BackToTheFuture(void)
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CVehicle::bHoverCheat = !CVehicle::bHoverCheat;
	CPad::bHasPlayerCheated = true;
}

void SuicideCheat(void) {
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	FindPlayerPed()->InflictDamage(nil, WEAPONTYPE_UNARMED, 1000.0f, PEDPIECE_TORSO, 0);
}

void DoChicksWithGunsCheat(void) {
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CStreaming::SetModelIsDeletable(CGangs::GetGangPedModel1(GANG_PLAYER));
	CStreaming::SetModelIsDeletable(CGangs::GetGangPedModel2(GANG_PLAYER));
	CStreaming::SetModelTxdIsDeletable(CGangs::GetGangPedModel1(GANG_PLAYER));
	CStreaming::SetModelTxdIsDeletable(CGangs::GetGangPedModel2(GANG_PLAYER));
	CStreaming::RemoveCurrentZonesModels();
	CGangs::SetGangPedModels(GANG_PLAYER, MI_HFYBE, MI_WFYBE);
	CGangs::SetGangWeapons(GANG_PLAYER, WEAPONTYPE_M4, WEAPONTYPE_M4);
	CStats::CheatedCount += 1000;
	CPad::bHasPlayerCheated = true;
}

//////////////////////////////////////////////////////////////////////////

#ifdef KANGAROO_CHEAT
void KangarooCheat()
{
	wchar *string;
	CPed *playerPed = FindPlayerPed();
	int m_fMass;

	if (playerPed->m_ped_flagI80) {
		string = TheText.Get("CHEATOF");
		m_fMass = 70.0f;
	} else {
		string = TheText.Get("CHEAT1");
		m_fMass = 15.0f;
	}
	CHud::SetHelpMessage(string, true);
	playerPed->m_ped_flagI80 = !playerPed->m_ped_flagI80;

	playerPed->m_fMass = m_fMass;
	playerPed->m_fAirResistance = 0.4f / m_fMass;
}
#endif

#ifdef RESTORE_ALLCARSHELI_CHEAT
void AllCarsHeliCheat(void)
{
	wchar* string;
	if (bAllCarCheat) {
		string = TheText.Get("CHEATOF");
		bAllCarCheat = false;
	}
	else {
		string = TheText.Get("CHEAT1");
		bAllCarCheat = true;
	}
	CHud::SetHelpMessage(string, true);
}
#endif

#ifdef WALLCLIMB_CHEAT
void WallClimbingCheat(void)
{
	wchar* string;
	if (gGravityCheat) {
		string = TheText.Get("CHEATOF");
		gGravityCheat = false;
	}
	else {
		string = TheText.Get("CHEAT1");
		gGravityCheat = true;
	}
	CHud::SetHelpMessage(string, true);
}
#endif

void FlyingFishCheat(void)
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CVehicle::bCheat8 = !CVehicle::bCheat8;
	CPad::bHasPlayerCheated = true;
}

void DoShowChaseStatCheat(void) {
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	CStats::ShowChaseStatOnScreen = 1;
}

int Cheat_strncmp(char* sourceStr, char* origCheatStr)
{
#define ccmp(n) if((uint8)sourceStr[i] != (uint8)origCheatStr[i] - n) return 1;
	int i = 0;
	while(origCheatStr[i])
	{
		switch(i)
		{
		case 0: ccmp(3); break;
		case 1: ccmp(5); break;
		case 2: ccmp(7); break;
		case 3: ccmp(1); break;
		case 4: ccmp(13); break;
		case 5: ccmp(27); break;
		case 6: ccmp(3); break;
		case 7: ccmp(7); break;
		case 8: ccmp(1); break;
		case 9: ccmp(11); break;
		case 10: ccmp(13); break;
		case 11: ccmp(8); break;
		case 12: ccmp(7); break;
		case 13: ccmp(32); break;
		case 14: ccmp(13); break;
		case 15: ccmp(6); break;
		case 16: ccmp(28); break;
		case 17: ccmp(19); break;
		case 18: ccmp(10); break;
		case 19: ccmp(3); break;
		case 20: ccmp(3); break;
		case 21: ccmp(5); break;
		case 22: ccmp(7); break;
		case 23: ccmp(1); break;
		case 24: ccmp(13); break;
		case 25: ccmp(27); break;
		case 26: ccmp(3); break;
		case 27: ccmp(7); break;
		default: return 1;
		}
		i++;
	}
	return 0;
#undef ccmp
}

#ifdef GTA_PC_CONTROLS
// TODO(Miami): Mobile has changed some of the cheats to include debugging things
void CPad::AddToPCCheatString(char c)
{
	for (int32 i = ARRAY_SIZE(KeyBoardCheatString) - 2; i >= 0; i--)
		KeyBoardCheatString[i + 1] = KeyBoardCheatString[i];

	KeyBoardCheatString[0] = c;

#define _CHEATCMP(str) strncmp(str, KeyBoardCheatString, sizeof(str)-1)

	// "THUGSTOOLS"
	if (!Cheat_strncmp(KeyBoardCheatString, "VQVPanJ\\I_")) {
		KeyBoardCheatString[0] = ' ';
		WeaponCheat1();
	}
	// "PROFESSIONALTOOLS"
	else if (!Cheat_strncmp(KeyBoardCheatString, "VQVPagDUPT`[Lf\\Xl")) {
		KeyBoardCheatString[0] = ' ';
		WeaponCheat2();
	}
	// "NUTTERTOOLS"
	else if (!Cheat_strncmp(KeyBoardCheatString, "VQVPamH[U`[")) {
		KeyBoardCheatString[0] = ' ';
		WeaponCheat3();
	}
	// "PRECIOUSPROTECTION"
	else if (!Cheat_strncmp(KeyBoardCheatString, "QTPUP`WVS[`]ViPKnc")) {
		KeyBoardCheatString[0] = ' ';
		ArmourCheat();
	}
	// "ASPIRINE"
	else if (!Cheat_strncmp(KeyBoardCheatString, "HSPSVkVH")) {
		KeyBoardCheatString[0] = ' ';
		HealthCheat();
	}
	// "YOUWONTTAKEMEALIVE"
	else if (!Cheat_strncmp(KeyBoardCheatString, "H[PMN`PLLLa\\Uod[kl")) {
		KeyBoardCheatString[0] = ' ';
		WantedLevelUpCheat();
	}
	// "LEAVEMEALONE"
	else if (!Cheat_strncmp(KeyBoardCheatString, "HSVMN`PLWLRT")) {
		KeyBoardCheatString[0] = ' ';
		WantedLevelDownCheat();
	}
	// "APLEASANTDAY"
	else if (!Cheat_strncmp(KeyBoardCheatString, "\\FKU[\\VHFW]I")) {
		KeyBoardCheatString[0] = ' ';
		SunnyWeatherCheat();
	}
	// "ALOVELYDAY"
	else if (!Cheat_strncmp(KeyBoardCheatString, "\\FKZY`YVML")) {
		KeyBoardCheatString[0] = ' ';
		ExtraSunnyWeatherCheat();
	}
	// "ABITDRIEG"
	else if (!Cheat_strncmp(KeyBoardCheatString, "JJPSQoLIB")) {
		KeyBoardCheatString[0] = ' ';
		CloudyWeatherCheat();
	}
	// "CATSANDDOGS"
	else if (!Cheat_strncmp(KeyBoardCheatString, "VLVEQiDZULP")) {
		KeyBoardCheatString[0] = ' ';
		RainyWeatherCheat();
	}
	// "CANTSEEATHING"
	else if (!Cheat_strncmp(KeyBoardCheatString, "JSPIa\\HLT_[IJ")) {
		KeyBoardCheatString[0] = ' ';
		FoggyWeatherCheat();
	}
	// "PANZER"
	else if (!Cheat_strncmp(KeyBoardCheatString, "UJaONk")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_RHINO);
	}
	// "TAKETOTHESKIES"
	else if (!Cheat_strncmp(KeyBoardCheatString, "VJPL``K[P_RSHt")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_MAVERICK);
	}
	// "LIFEISPASSINGMEBY"
	else if (!Cheat_strncmp(KeyBoardCheatString, "\\GLNTiLZTL][PeSOh")) {
		KeyBoardCheatString[0] = ' ';
		FastWeatherCheat();
	}
	// "BIGBANG"
	else if (!Cheat_strncmp(KeyBoardCheatString, "JSHCTdE")) {
		KeyBoardCheatString[0] = ' ';
		BlowUpCarsCheat();
	}
	// "STILLLIKEDRESSINGUP"
	else if (!Cheat_strncmp(KeyBoardCheatString, "SZNOVnVLSORSPlYReg]")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerCheat();
	}
	// "FIGHTFIGHTFIGHT"
	else if (!Cheat_strncmp(KeyBoardCheatString, "WMNJSoKNJQaPNiS")) {
		KeyBoardCheatString[0] = ' ';
		MayhemCheat();
	}
	// "NOBODYLIKESME"
	else if (!Cheat_strncmp(KeyBoardCheatString, "HRZFXdO`EZOWU")) {
		KeyBoardCheatString[0] = ' ';
		EverybodyAttacksPlayerCheat();
	}
	// "OURGODGIVENRIGHTTOBEARARMS"
	else if (!Cheat_strncmp(KeyBoardCheatString, "VRYB_\\HIP_aPNi_TaiSJGTNSbj")) {
		KeyBoardCheatString[0] = ' ';
		WeaponsForAllCheat();
	}
	// "ONSPEED"
	else if (!Cheat_strncmp(KeyBoardCheatString, "GJLQ`iR")) {
		KeyBoardCheatString[0] = ' ';
		FastTimeCheat();
	}
	// "BOOOOOORING"
	else if (!Cheat_strncmp(KeyBoardCheatString, "JSPS\\jRVPZO")) {
		KeyBoardCheatString[0] = ' ';
		SlowTimeCheat();
	}
	// "WHEELSAREALLINEED"
	else if (!Cheat_strncmp(KeyBoardCheatString, "GJLOVgOHF]N[SeRNs")) {
		KeyBoardCheatString[0] = ' ';
		OnlyRenderWheelsCheat();
	}
	//COMEFLYWITHME
	else if (!Cheat_strncmp(KeyBoardCheatString, "HROUVr\\SGPZWJ")) {
		KeyBoardCheatString[0] = ' ';
		ChittyChittyBangBangCheat();
	}
	// "GRIPISEVERYTHING"
	else if (!Cheat_strncmp(KeyBoardCheatString, "JSPIatULWP`QWi_M")) {
		KeyBoardCheatString[0] = ' ';
		StrongGripCheat();
	}
	// "CHASESTAT"
	else if (!Cheat_strncmp(KeyBoardCheatString, "WF[TRnDOD")) {
		KeyBoardCheatString[0] = ' ';
		DoShowChaseStatCheat();
	}
	// "CHICKSWITHGUNS"
	else if (!Cheat_strncmp(KeyBoardCheatString, "VS\\HUoL^TVPQOc")) {
		KeyBoardCheatString[0] = ' ';
		DoChicksWithGunsCheat();
	}
	// "ICANTTAKEITANYMORE"
	else if (!Cheat_strncmp(KeyBoardCheatString, "HWVNfiD[JPXI[t[G_\\")) {
		KeyBoardCheatString[0] = ' ';
		SuicideCheat();
	}
	// "GREENLIGHT"
	else if (!Cheat_strncmp(KeyBoardCheatString, "WMNJYiHLSR")) {
		KeyBoardCheatString[0] = ' ';
		TrafficLightsCheat();
	}
	// "MIAMITRAFFIC"
	else if (!Cheat_strncmp(KeyBoardCheatString, "FNMGNmWPNLVU")) {
		KeyBoardCheatString[0] = ' ';
		MadCarsCheat();
	}
	// "AHAIRDRESSERSCAR"
	else if (!Cheat_strncmp(KeyBoardCheatString, "UFJT_`VZF]QZPaUG")) {
		KeyBoardCheatString[0] = ' ';
		PinkCarsCheat();
	}
	// "IWANTITPAINTEDBLACK"
	else if (!Cheat_strncmp(KeyBoardCheatString, "NHHMO_H[OTNX[iaT]jS")) {
		KeyBoardCheatString[0] = ' ';
		BlackCarsCheat();
	}
	// "TRAVELINSTYLE"
	else if (!Cheat_strncmp(KeyBoardCheatString, "HQ`U`iLSFaNZ[")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_BLOODRA);
	}
	// "THELASTRIDE"
	else if (!Cheat_strncmp(KeyBoardCheatString, "HIPSanDSFSa")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_ROMERO);
	}
	// "ROCKANDROLLCAR"
	else if (!Cheat_strncmp(KeyBoardCheatString, "UFJMYjUKOLXKVr")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_LOVEFIST);
	}
	// "RUBBISHCAR"
	else if (!Cheat_strncmp(KeyBoardCheatString, "UFJI`dEIV]")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_TRASH);
	}
	// "GETTHEREQUICKLY"
	else if (!Cheat_strncmp(KeyBoardCheatString, "\\QRDVpTLSPU\\[eT")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_BLOODRB);
	}
	// "GETTHEREFAST"
	else if (!Cheat_strncmp(KeyBoardCheatString, "WXHGRmHOU_RO")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_SABRETUR);
	}
	// "BETTERTHANWALKING"
	else if (!Cheat_strncmp(KeyBoardCheatString, "JSPLY\\ZUBSaZLtaK^")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_CADDY);
	}
	// "GETTHEREFASTINDEED"
	else if (!Cheat_strncmp(KeyBoardCheatString, "GJLE[dWZBQfZLvRXa[^WHL")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_HOTRINA);
	}
	// "GETTHEREAMAZINGLYFAST"
	else if (!Cheat_strncmp(KeyBoardCheatString, "WXHGfgJUJeNUHe_Kdg^HJ")) {
		KeyBoardCheatString[0] = ' ';
		VehicleCheat(MI_HOTRINB);
	}
	// LOOKLIKELANCE
	else if (!Cheat_strncmp(KeyBoardCheatString, "HHUBY`NPMV\\WS")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("igbuddy");
	}
	// IWANTBIGTITS
	else if (!Cheat_strncmp(KeyBoardCheatString, "VYPUTdE[OLdQ")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("igcandy");
	}
	// MYSONISALAWYER
	else if (!Cheat_strncmp(KeyBoardCheatString, "UJ`XNgDZJY\\[`m")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("igken");
	}
	// ILOOKLIKEHILARY
	else if (!Cheat_strncmp(KeyBoardCheatString, "\\WHMVcHRJWXWVlV")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("ighlary");
	}
	// ROCKANDROLLMAN
	else if (!Cheat_strncmp(KeyBoardCheatString, "QFTMYjUKOLXKVr")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("igjezz");
	}
	// ONEARMEDBANDIT
	else if (!Cheat_strncmp(KeyBoardCheatString, "WNKON]GLN]NMUo")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("igphil");
	}
	// IDONTHAVETHEMONEYSONNY
	else if (!Cheat_strncmp(KeyBoardCheatString, "\\SUP`tHUPXRP[ecGdgXRGN")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("igsonny");
	}
	// FOXYLITTLETHING
	else if (!Cheat_strncmp(KeyBoardCheatString, "JSPIa`O[UTYa_oS")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("igmerc");
	}
	// WELOVEOURDICK
	else if (!Cheat_strncmp(KeyBoardCheatString, "NHPE_pRLWZYM^")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("igdick");
	}
	// CHEATSHAVEBEENCRACKED
	else if (!Cheat_strncmp(KeyBoardCheatString, "GJRDNmFUFPOM]aUYpTOKF")) {
		KeyBoardCheatString[0] = ' ';
		ChangePlayerModel("igdiaz");
	}
	// DEEPFRIEDMARSBARS
	else if (!Cheat_strncmp(KeyBoardCheatString, "VWHC`mDTEPVZMpRK")) {
		KeyBoardCheatString[0] = ' ';
		gfTommyFatness = 0.26f;
	}
	// PROGRAMMER
	else if (!Cheat_strncmp(KeyBoardCheatString, "UJTNNmJVS[")) {
		KeyBoardCheatString[0] = ' ';
		gfTommyFatness = -0.3f;
	}
	// SEAWAYS
	else if (!Cheat_strncmp(KeyBoardCheatString, "V^HXN`V")) {
		KeyBoardCheatString[0] = ' ';
		BackToTheFuture();
	}
	// LOADSOFLITTLETHINGS
	else if (!Cheat_strncmp(KeyBoardCheatString, "VLUJUoHSU_VTMo`J]bV")) {
		KeyBoardCheatString[0] = ' ';
		SpecialCarCheats();
	}
	// HOPINGIRL
	else if (!Cheat_strncmp(KeyBoardCheatString, "OWPH[dSVI")) {
		KeyBoardCheatString[0] = ' ';
		PickUpChicksCheat();
	}
	//CERTAINDEATH
	else if (!Cheat_strncmp(KeyBoardCheatString, "KYHFQiLHU]RK")) {
		KeyBoardCheatString[0] = ' ';
		CSmokeTrails::CigOn = !CSmokeTrails::CigOn;
	}
	//AIRSHIP
	else if (!Cheat_strncmp(KeyBoardCheatString, "SNOT_dD")) {
		KeyBoardCheatString[0] = ' ';
		FlyingFishCheat();
	}
	//FANNYMAGNET
	else if (!Cheat_strncmp(KeyBoardCheatString, "WJUHNh\\UOLS")) {
		KeyBoardCheatString[0] = ' ';
		FannyMagnetCheat();
	}

#ifdef KANGAROO_CHEAT
	// "KANGAROO"
	if (!_CHEATCMP("OORAGNAK"))
		KangarooCheat();
#endif

#ifndef MASTER
	// "PEDDEBUG"
	if (!_CHEATCMP("GUBEDDEP"))
		CPed::SwitchDebugDisplay();
#endif

#ifdef RESTORE_ALLCARSHELI_CHEAT
	// "CARSAREHELI"
	if (!_CHEATCMP("ILEHERASRAC"))
		AllCarsHeliCheat();
#endif

#ifdef WALLCLIMB_CHEAT
	// "SPIDERCAR"
	if (!_CHEATCMP("RACREDIPS"))
		WallClimbingCheat();
#endif

#if !defined(PC_WATER) && defined(WATER_CHEATS)
	// SEABEDCHEAT
	if (!_CHEATCMP("TAEHCDEBAESON"))
		NoSeaBedCheat();

	// WATERLAYERSCHEAT
	if (!_CHEATCMP("TAEHCSREYALRETAW"))
		RenderWaterLayersCheat();
#endif

#undef _CHEATCMP
}
#endif

void LittleTest(void)
{
	static int32 Cunt = 0;

	Cunt++; // ???
}
