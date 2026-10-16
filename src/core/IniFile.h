#ifndef __GTA_INIFILE_H__
#define __GTA_INIFILE_H__

/* The density slider units and the hard tops of the budgets live in the cap
 * table (VICE-123), with the pools they protect. */
#include "PopulationCaps.h"

class CIniFile
{
public:
	static void LoadIniFile();
	static void ApplyPopulationCaps();

	static float PedNumberMultiplier;
	static float CarNumberMultiplier;
};

#endif // __GTA_INIFILE_H__
