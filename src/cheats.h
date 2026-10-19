/* Cheats from RetroArch .cht files (libretro-database layout), as in GBA DS:
 * every .cht in the cheats folder (and its subfolders) in one list. */
#ifndef SMDS_CHEATS_H
#define SMDS_CHEATS_H

#include "emu.h"

#include <stdbool.h>

#define CHEAT_MAX_FILES 32

typedef struct {
	char desc[128];
	char* code;
	bool on;
	int file;
} Cheat;

typedef struct {
	char files[CHEAT_MAX_FILES][1024];
	int nFiles;
	Cheat* items;
	int n;
	char prefs[1024]; /* where the on/off choices are kept */
} Cheats;

/* Reads every .cht in dir (two levels of subfolders too; file names in
 * order) into one list, and restores the saved choices from prefsPath (or
 * the files' own cheatN_enable when nothing was saved yet). */
void cheats_load(Cheats* c, const char* dir, const char* prefsPath);
/* Puts the switched-on cheats into the emulator. */
void cheats_apply(const Cheats* c, Emu* e);
void cheats_save(const Cheats* c);
int cheats_on(const Cheats* c);
void cheats_free(Cheats* c);

#endif
