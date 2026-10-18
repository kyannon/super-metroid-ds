/* Super Metroid specifics: the HUD patch and drawing the HUD on its own. */
#ifndef SMDS_SM_H
#define SMDS_SM_H

#include "emu.h"

#define HUD_H 32
/* the HUD's mini-map (inclusive, SNES pixels): the settings button goes here */
#define HUD_MINIMAP_X0 208
#define HUD_MINIMAP_X1 248
#define HUD_MINIMAP_Y0 6
#define HUD_MINIMAP_Y1 30

/* Takes the HUD off the game screen (the room shows through). Returns how
 * many of the patches went in (sm_patch_count() when all did). */
int sm_patch_hud(uint8_t* rom, size_t size);
int sm_patch_count(void);
/* The HUD (SNES_W x HUD_H, ABGR8888) from video RAM and the palette, the
 * mini-map left out. */
void sm_draw_hud(const uint8_t* vram, const uint16_t* cgram, uint32_t* out);

/* The room behind the HUD's strip isn't drawn here (Ceres): blank it. */
bool sm_strip_unusable(Emu* e);
void sm_blank_strip(uint32_t* fb);

#endif
