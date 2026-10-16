#ifndef __GTA_CHEATS_H__
#define __GTA_CHEATS_H__

// The PC entry point CPad::AddToPCCheatString is declared in Pad.h and defined in Cheats.cpp.

void SpecialCarCheats();
void PickUpChicksCheat();
void WeaponCheat1();
void WeaponCheat2();
void WeaponCheat3();
void HealthCheat();
void VehicleCheat(int model);
void BlowUpCarsCheat();
void ChangePlayerCheat();
void MayhemCheat();
void EverybodyAttacksPlayerCheat();
void WeaponsForAllCheat();
void FastTimeCheat();
void SlowTimeCheat();
void MoneyCheat();
void ArmourCheat();
void WantedLevelUpCheat();
void WantedLevelDownCheat();
void SunnyWeatherCheat();
void CloudyWeatherCheat();
void RainyWeatherCheat();
void FoggyWeatherCheat();
void FastWeatherCheat();
void OnlyRenderWheelsCheat();
void ChittyChittyBangBangCheat();
void StrongGripCheat();
void NoSeaBedCheat(void);
void RenderWaterLayersCheat(void);
#ifdef KANGAROO_CHEAT
void KangarooCheat();
#endif
#ifdef RESTORE_ALLCARSHELI_CHEAT
void AllCarsHeliCheat(void);
#endif
#ifdef WALLCLIMB_CHEAT
void WallClimbingCheat(void);
#endif

#endif // __GTA_CHEATS_H__
