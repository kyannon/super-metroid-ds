#ifndef SMDS_CONFIG_H
#define SMDS_CONFIG_H

#include <stdbool.h>

typedef struct Config {
	char layout[16];   /* auto | span-h | span-v | dual | single */
	bool swap;         /* the map on the first (left/top) screen instead of the second */
	char scale[16];    /* game: integer | fit | zoom | stretch */
	char mapScale[16]; /* second screen: the same */
	bool smooth;       /* plain linear filtering when scaling (blurry) */
	int shader;        /* over both screens: 0 none, 1 a pixel grid, 2 scanlines */
	int lcdStrength;   /* grid darkness, 0..100 */
	int scanStrength;  /* scanline darkness, 0..100 */
	bool vsync;
	bool frameBuffer;  /* emulate a frame ahead on a second thread: smoother, a frame more input delay */
	bool pinCores;     /* Android: the game thread on a core of its own */
	bool map;          /* the second screen (HUD and pause map) */
	bool mapAnim;      /* the map's blinking markers (off: one still frame) */
	bool matchDisplay; /* run at the display's refresh rate (if close to the SNES's) */
	int mapInterval;   /* frames between map refreshes */
	bool abSwap;       /* SNES A on the bottom face button instead of the right one */
	char hotkey[16];   /* guide | back */
	char menuCombo[16]; /* select+start | none */
	int homeButton;    /* raw joystick button number that acts as HOME (-1 = off) */
	bool showFps;
	bool sway;         /* ask sway to fullscreen the window across all outputs */
	int audioLatency;  /* ms */
	int ffSpeed;       /* fast-forward multiplier */
	bool rewind;       /* hold L2 to go back in time */
	int rewindMb;      /* memory kept for it */
	int rewindStep;    /* frames between the points it can go back to */
	char saveDir[512]; /* battery saves; default: next to the ROM */
	char stateDir[512]; /* save states, prefs; default: <app>/states */
	char romDir[1024]; /* where to look for the ROM, ':'-separated */
	char screenshotDir[512]; /* Take Screenshot saves here; default: <app>/screenshots */
	char coreDir[512]; /* the SNES core libraries; default: next to the program */
	char touchScreen[16]; /* first | second: the screen with the touch panel */
	char touchMap[16];    /* window | screen: how touch coordinates map */
	bool autoResume;      /* save state on exit, load it on launch */
	char rom[512];        /* the ROM (else searched for in rom_dir) */

	/* testing / automation */
	char loadState[512];
	int quitAfter; /* UI ticks */
	char inputScript[512];
} Config;

void config_defaults(Config* c);
/* Loads key=value lines; a missing file is not an error. */
void config_load_file(Config* c, const char* path);
/* --key=value / --flag arguments and an optional ROM path. False on error. */
bool config_parse_args(Config* c, int argc, char** argv);
void config_usage(const char* argv0);

#endif
