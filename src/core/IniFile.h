#ifndef __GTA_INIFILE_H__
#define __GTA_INIFILE_H__

#define DEFAULT_MAX_NUMBER_OF_PEDS 100.0f
#define DEFAULT_MAX_NUMBER_OF_PEDS_INTERIOR 60.0f
#define DEFAULT_MAX_NUMBER_OF_CARS 45.0f

// Hard tops for the budgets: the pool minus the interior peds, the drivers and a margin.
#define MAX_PEDS_IN_USE 900
#define MAX_CARS_IN_USE 256

class CIniFile
{
public:
	static void LoadIniFile();
	static void ApplyPopulationCaps();

	static float PedNumberMultiplier;
	static float CarNumberMultiplier;
	static float SpawnDistanceMultiplier;
};

#endif // __GTA_INIFILE_H__
