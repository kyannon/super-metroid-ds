/* Pause-map capture on a hidden copy of the game.
 *
 * A second core (emu slot EMU_SLOT_SHADOW) is loaded with a snapshot of the
 * live game, START is pressed, and once the pause screen's map is up and
 * fully faded in, the map part of the screen (rows MAP_Y0..MAP_Y0+MAP_ROWS-1,
 * the area name and frame; not the button prompts below it, nor the HUD
 * above) is copied out, a few frames of it for the blinking markers. The live
 * game is never paused or touched. Captures run on a background thread.
 * The equipment screen (MAPCAP_SAMUS) is captured the same way, the hidden
 * copy's pause screen opening straight onto it (sm_set_pause_patch).
 */
#ifndef SMDS_MAPCAP_H
#define SMDS_MAPCAP_H

#include "emu.h"

#define MAP_Y0 32
#define MAP_ROWS 167 /* down to the bottom of the map's frame (row 198) */
#define MAP_MAX_FRAMES 32

/* what a capture is of: the pause screen's map, or its equipment screen */
enum { MAPCAP_MAP, MAPCAP_SAMUS };

typedef struct MapCapture MapCapture;

typedef struct MapResult {
	bool ok;
	const char* why;             /* failure reason when !ok */
	int nFrames;                 /* > 1 when the map has blinking markers */
	int frameStep;               /* emulated frames between animation frames */
	uint32_t* frames[MAP_MAX_FRAMES]; /* SNES_W * MAP_ROWS each */
	uint32_t serial;             /* increments with each successful capture */
	bool scrolled;               /* only the map scrolled (its arrows) */
} MapResult;

/* rom: the game as loaded (patched like the live one), copied. */
MapCapture* mapcap_create(const uint8_t* rom, size_t size);
void mapcap_destroy(MapCapture* mc);

/* Starts a capture (of mode) from the live game's current state if the
 * worker is idle; true if it started. */
bool mapcap_request(MapCapture* mc, Emu* live, int mode);
bool mapcap_busy(MapCapture* mc);
/* Scrolls the map by steps (8 pixels each; x right, y down) as its arrows
 * would: shown soon from the copy kept at the last map, and kept for the
 * next captures. */
void mapcap_scroll(MapCapture* mc, int dx, int dy);
/* Back to where the pause screen puts the map (Samus in another room). */
void mapcap_reset_scroll(MapCapture* mc);
/* Copies the newest successful result of mode into *out (buffers managed
 * here) if it is newer than lastSerial. */
bool mapcap_poll(MapCapture* mc, int mode, uint32_t lastSerial, MapResult* out);
/* Consecutive failed captures since the last success (and the last reason). */
int mapcap_failures(MapCapture* mc, const char** why);
/* Synchronous capture (tools). */
MapResult mapcap_capture_sync(MapCapture* mc, Emu* live);

/* ---- what the live game is doing ($7E0998, the game state) */
/* In normal play: a moment a map capture makes sense. */
bool sm_can_pause(Emu* live);
/* The game would pause if START were pressed now (its own test). */
bool sm_pause_available(Emu* live);
/* Title, file select, intro, cutscenes, demos: no map belongs on screen. */
bool sm_no_map_here(Emu* live);
/* The HUD (energy, ammo) is up. */
bool sm_hud_shown(Emu* live);
/* Changes when Samus enters another room. */
uint32_t sm_room_key(Emu* live);

#endif
