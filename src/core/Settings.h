#ifndef __GTA_SETTINGS_H__
#define __GTA_SETTINGS_H__
#include "common.h"

// reVC.ini holds one key per setting. Settings.cpp keeps them in one table, so
// the loader and the writer walk the same list. The row order is what a fresh
// file gets, and the type of a row picks the parse and the format of its value.

// mINI picks its case handling, and with it the inline bodies of the header,
// from MINI_CASE_SENSITIVE. Define it before this include; the game keeps the
// case of the keys.
#include "ini.h"

// The conversion a key gets. The reader and the writer pick the call the old
// per-key overloads picked:
//   uint32, uint8, bool: strtoul and "%u"
//   int32, int8:         strtol and "%d"
//   float:               strtof and "%f"
enum eSettingsType
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
//   min, max:     the bounds of the value, or SETTINGS_NO_BOUND
//   size:         the room of the pointee in bytes; the table check keeps it
//                 equal to the width of the type
//   value:        the setting the key speaks for
struct tSettingsEntry
{
	const char *section;
	const char *key;
	eSettingsType type;
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
