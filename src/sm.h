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

/* Takes the HUD off the game screen (the room shows through), and has the
 * game keep the room drawn under it. Returns how many of the patches went
 * in (sm_patch_count() when all did). Goes with emu_set_hud_hdma. */
int sm_patch_hud(uint8_t* rom, size_t size);
int sm_patch_count(void);
/* The pause screen without its map (it's on the second screen): pausing
 * goes straight to the equipment screen and L / R don't switch. Not for the
 * hidden copy that captures the map. */
bool sm_patch_pause(uint8_t* rom, size_t size);
/* The pause screen is on the game screen (fading in, up, fading out). */
bool sm_pause_screen_up(const uint8_t* wram);
/* Blanks its L MAP and R boxes in a frame (SNES_W x SNES_H). */
void sm_blank_pause_buttons(uint32_t* fb);
/* The HUD (SNES_W x HUD_H, ABGR8888) from video RAM and the palette, the
 * mini-map left out. */
void sm_draw_hud(const uint8_t* vram, const uint16_t* cgram, uint32_t* out);

#endif
