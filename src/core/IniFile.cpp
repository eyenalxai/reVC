#include "common.h"

#include "IniFile.h"

#include "CarCtrl.h"
#include "FileMgr.h"
#include "main.h"
#include "Population.h"
#include "TrafficRings.h"

float CIniFile::PedNumberMultiplier = 1.0f;
float CIniFile::CarNumberMultiplier = 2.0f;

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
	// The cap follows the ring, and the ring follows the visible range
	// (TrafficRings.h). The ring area grows with the square of the scale, so the
	// budgets scale with it too. Call this when a slider or the draw distance
	// moves; the game also calls it once a frame, because the weather moves the
	// visible range.
	// The pedestrian cap is the count ON FOOT: the drivers and the passengers of
	// the cars are added on top of it where they are compared with the count
	// (Population.cpp AddToPopulation); the pool arithmetic is in PopulationCaps.h.
	float ringScale = CurrentTrafficRings().scale;
	CPopulation::MaxNumberOfPedsInUse = Min(DEFAULT_MAX_NUMBER_OF_PEDS * PedNumberMultiplier * SQR(ringScale), (float)MAX_PEDS_IN_USE);
	// The ring does not apply indoors.
	CPopulation::MaxNumberOfPedsInUseInterior = DEFAULT_MAX_NUMBER_OF_PEDS_INTERIOR * PedNumberMultiplier;
	CCarCtrl::MaxNumberOfCarsInUse = Min(DEFAULT_MAX_NUMBER_OF_CARS * CarNumberMultiplier * SQR(ringScale), (float)MAX_CARS_IN_USE);
}
