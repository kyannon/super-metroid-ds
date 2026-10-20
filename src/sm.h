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
/* The pause screen without its map (on the hidden copy, for the second
 * screen's SAMUS page): pausing goes straight to the equipment screen and
 * L / R don't switch. Off puts the game's own pause back. */
bool sm_set_pause_patch(uint8_t* rom, size_t size, bool on);
/* Equipment switched from outside (sm_equip_toggle) takes effect in the
 * running game at its next frame of play. */
bool sm_patch_equip(uint8_t* rom, size_t size);

/* The equipment screen's rows */
enum {
	SM_EQ_VARIA, SM_EQ_GRAVITY, SM_EQ_MORPH, SM_EQ_BOMB, SM_EQ_SPRING, SM_EQ_SCREW, SM_EQ_HIJUMP, SM_EQ_SPACE,
	SM_EQ_SPEED, SM_EQ_CHARGE, SM_EQ_ICE, SM_EQ_WAVE, SM_EQ_SPAZER, SM_EQ_PLASMA, SM_EQ_MODE, SM_EQUIP_N
};
typedef struct {
	int x0, y0, x1, y1; /* SNES pixels on the 256 x 224 screen, x1 / y1 exclusive */
	unsigned addr;      /* equipped bits (collected at addr + 2); $09C0 for MODE */
	uint16_t bit;
} SmEquipRow;
const SmEquipRow* sm_equip_row(int item);
/* The row at a point of the equipment screen, or -1. */
int sm_equip_at(int x, int y);
/* Samus has it (collected; MODE: reserve tanks). */
bool sm_equip_has(const uint8_t* wram, int item);
/* It's equipped (MODE: AUTO). */
bool sm_equip_on(const uint8_t* wram, int item);
/* Switches it in the game's RAM (needs sm_patch_equip): false if Samus
 * doesn't have it. */
bool sm_equip_toggle(uint8_t* wram, int item);
/* Screen rows y0..y1-1 of a 4-bit background layer (mode 1's 1 or 2) from
 * video RAM, SNES_W a row; 0 where it's clear. */
void sm_draw_bg(const uint8_t* vram, const uint16_t* cgram, const EmuBg* bg, int y0, int y1, uint32_t* out);
/* The HUD (SNES_W x HUD_H, ABGR8888) from video RAM and the palette, the
 * mini-map left out. */
void sm_draw_hud(const uint8_t* vram, const uint16_t* cgram, uint32_t* out);

#endif
