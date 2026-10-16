#include "common.h"

#include "IniFile.h"

#include "CarCtrl.h"
#include "FileMgr.h"
#include "main.h"
#include "Population.h"

float CIniFile::PedNumberMultiplier = 1.0f;
float CIniFile::CarNumberMultiplier = 2.0f;
float CIniFile::SpawnDistanceMultiplier = 1.0f;

void CIniFile::LoadIniFile()
{
	CFileMgr::SetDir("");
	int f = CFileMgr::OpenFile("gta3.ini", "r");
	if (f){
		CFileMgr::ReadLine(f, gString, 200);
		sscanf(gString, "%f", &PedNumberMultiplier);
		PedNumberMultiplier = Min(3.4f, Max(0.2f, PedNumberMultiplier));
		CFileMgr::ReadLine(f, gString, 200);
		sscanf(gString, "%f", &CarNumberMultiplier);
		CarNumberMultiplier = Min(3.4f, Max(0.2f, CarNumberMultiplier));
		CFileMgr::CloseFile(f);
	}
	ApplyPopulationCaps();
}

void CIniFile::ApplyPopulationCaps()
{
	// The ring area grows with the square of the spawn distance, so the budgets scale with it too.
	CPopulation::MaxNumberOfPedsInUse = Min(DEFAULT_MAX_NUMBER_OF_PEDS * PedNumberMultiplier * SQR(SpawnDistanceMultiplier), (float)MAX_PEDS_IN_USE);
	// The ring does not apply indoors.
	CPopulation::MaxNumberOfPedsInUseInterior = DEFAULT_MAX_NUMBER_OF_PEDS_INTERIOR * PedNumberMultiplier;
	CCarCtrl::MaxNumberOfCarsInUse = Min(DEFAULT_MAX_NUMBER_OF_CARS * CarNumberMultiplier * SQR(SpawnDistanceMultiplier), (float)MAX_CARS_IN_USE);
}
