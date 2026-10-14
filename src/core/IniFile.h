#ifndef __GTA_INIFILE_H__
#define __GTA_INIFILE_H__

#define DEFAULT_MAX_NUMBER_OF_PEDS 30.0f
#define DEFAULT_MAX_NUMBER_OF_PEDS_INTERIOR 40.0f
#define DEFAULT_MAX_NUMBER_OF_CARS 16.0f

class CIniFile
{
public:
	static void LoadIniFile();

	static float PedNumberMultiplier;
	static float CarNumberMultiplier;
};

#endif // __GTA_INIFILE_H__
