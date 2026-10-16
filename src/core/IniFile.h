#ifndef __GTA_INIFILE_H__
#define __GTA_INIFILE_H__

#define DEFAULT_MAX_NUMBER_OF_PEDS 100.0f
#define DEFAULT_MAX_NUMBER_OF_PEDS_INTERIOR 60.0f
#define DEFAULT_MAX_NUMBER_OF_CARS 45.0f

class CIniFile
{
public:
	static void LoadIniFile();

	static float PedNumberMultiplier;
	static float CarNumberMultiplier;
	static float SpawnDistanceMultiplier;
};

#endif // __GTA_INIFILE_H__
