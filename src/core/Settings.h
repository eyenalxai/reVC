#ifndef __GTA_SETTINGS_H__
#define __GTA_SETTINGS_H__
#include "common.h"

// reVC.ini holds one key per setting. Settings.cpp keeps them in one table, so
// the loader and the writer walk the same list. The row order is what a fresh
// file gets, and the type of a row picks the parse and the format of its value.

#define MINI_CASE_SENSITIVE
#include "ini.h"

// The conversion a key gets. The reader and the writer pick the call the old
// per-key overloads picked:
//   uint32, uint8, bool: strtoul and "%u"
//   int32, int8:         strtol and "%d"
//   float:               strtof and "%f"
enum tSettingsType
{
	SETTINGS_UINT32,
	SETTINGS_INT32,
	SETTINGS_UINT8,
	SETTINGS_INT8,
	SETTINGS_BOOL,
	SETTINGS_FLOAT,
	SETTINGS_STRING,
};

// One key:
//   section, key: where the value sits in the file
//   type:         how the reader parses the value and the writer prints it
//   def:          the stock default, the value the game starts with
//   min, max:     the bounds of the value, or SETTINGS_NO_BOUND
//   size:         the room of a string setting, unused by the numeric types
//   value:        the setting the key speaks for
struct tSettingsEntry
{
	const char *section;
	const char *key;
	tSettingsType type;
	double def;
	double min;
	double max;
	int32 size;
	void *value;
};

// The bounds of a key the reader takes as it parses it. The pair lies outside
// every value of the table types, so a clamp with it changes nothing.
#define SETTINGS_NO_BOUND	(-HUGE_VAL), (HUGE_VAL)

// Read every key of the table that the file holds. A key that is not there
// keeps the value the game has (the gta_vc.set value), as it always did.
void SettingsLoadAll(mINI::INIStructure &cfg);
// Write the current value of every key of the table into the structure. The
// keys of the user that the table does not know stay in it.
void SettingsSaveAll(mINI::INIStructure &cfg);

#endif // __GTA_SETTINGS_H__
