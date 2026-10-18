/* Super Metroid specifics: the HUD patch and drawing the HUD on its own. */
#include "sm.h"

#include <stdio.h>
#include <string.h>

/* The game draws its HUD in the top 31 lines with an interrupt at line 0
 * that switches the screen to the HUD's settings (BG3 only, the HUD's
 * tilemap) and another at line 31 that switches back to the room's. Each
 * "start of HUD" handler in bank $80's interrupt table is replaced by a jump
 * to its "end of HUD" partner, so the room's settings hold from line 0 and the
 * room shows through where the HUD was (the game keeps that strip of the
 * room drawn: the camera covers the whole 224 lines). The HUD's tiles are
 * still drawn into video RAM, from where sm_draw_hud copies them. */
static const struct {
	unsigned from, to; /* bank $80 addresses */
	uint8_t was[4];    /* the bytes expected at `from` */
} hudPatches[] = {
	{ 0x968B, 0x96A9, { 0xE2, 0x20, 0xA9, 0x5A } }, /* main gameplay */
	{ 0x96D3, 0x96F1, { 0xE2, 0x20, 0xA9, 0x5A } }, /* start of a door transition */
	{ 0x971A, 0x9733, { 0xE2, 0x20, 0xA9, 0x04 } }, /* Draygon's room */
	{ 0x9758, 0x9771, { 0xE2, 0x20, 0xA9, 0x04 } }, /* vertical door transition */
	{ 0x97C1, 0x97DA, { 0xE2, 0x20, 0xA9, 0x04 } }, /* horizontal door transition */
};

int sm_patch_hud(uint8_t* rom, size_t size) {
	int n = 0;
	for (size_t i = 0; i < sizeof(hudPatches) / sizeof(*hudPatches); ++i) {
		size_t off = hudPatches[i].from - 0x8000; /* LoROM bank $80 */
		if (off + 4 > size) continue;
		uint8_t jmp[3] = { 0x4C, (uint8_t) (hudPatches[i].to & 0xFF), (uint8_t) (hudPatches[i].to >> 8) };
		if (!memcmp(rom + off, jmp, 3)) {
			++n; /* already */
			continue;
		}
		if (memcmp(rom + off, hudPatches[i].was, 4)) {
			fprintf(stderr, "HUD patch %u: unexpected bytes at $80:%04X, left alone\n", (unsigned) i, hudPatches[i].from);
			continue;
		}
		memcpy(rom + off, jmp, 3);
		++n;
	}
	return n;
}

int sm_patch_count(void) {
	return (int) (sizeof(hudPatches) / sizeof(*hudPatches));
}

static inline uint32_t bgr555(uint16_t c) {
	uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
	return 0xFF000000u | (b << 3 | b >> 2) << 16 | (g << 3 | g >> 2) << 8 | (r << 3 | r >> 2);
}

void sm_draw_hud(const uint8_t* vram, const uint16_t* cgram, uint32_t* out) {
	/* BG3 in the HUD's settings: 2 bits a pixel, tiles at word $4000,
	 * tilemap 32 wide at word $5800, palettes 0-7 of 4 colours */
	const uint8_t* map = vram + 0x5800 * 2;
	const uint8_t* chr = vram + 0x4000 * 2;
	for (int ty = 0; ty < HUD_H / 8; ++ty) {
		for (int tx = 0; tx < SNES_W / 8; ++tx) {
			uint16_t e = (uint16_t) (map[(ty * 32 + tx) * 2] | map[(ty * 32 + tx) * 2 + 1] << 8);
			int tile = e & 0x3FF, pal = (e >> 10) & 7;
			bool hf = e & 0x4000, vf = e & 0x8000;
			const uint8_t* t = chr + (size_t) tile * 16;
			for (int y = 0; y < 8; ++y) {
				int sy = vf ? 7 - y : y;
				uint8_t p0 = t[sy * 2], p1 = t[sy * 2 + 1];
				uint32_t* row = out + (ty * 8 + y) * SNES_W + tx * 8;
				for (int x = 0; x < 8; ++x) {
					int bit = hf ? x : 7 - x;
					int c = ((p0 >> bit) & 1) | ((p1 >> bit) & 1) << 1;
					row[x] = c ? bgr555(cgram[pal * 4 + c]) : 0xFF000000u;
				}
			}
		}
	}
	/* line 31 is the room's already (the interrupt comes mid-line) */
	for (int x = 0; x < SNES_W; ++x) out[(HUD_H - 1) * SNES_W + x] = 0xFF000000u;
	/* the mini-map gives way to the settings button */
	for (int y = HUD_MINIMAP_Y0; y <= HUD_MINIMAP_Y1; ++y) {
		for (int x = HUD_MINIMAP_X0; x <= HUD_MINIMAP_X1; ++x) out[y * SNES_W + x] = 0xFF000000u;
	}
}

bool sm_strip_unusable(Emu* e) {
	/* Ceres station's rooms don't draw the strip under the HUD (and can't
	 * be paused): it stays black there, as the game shows it */
	return emu_read8(e, 0x079F) == 6 && emu_read8(e, 0x0998) >= 0x08 && emu_read8(e, 0x0998) <= 0x25;
}

void sm_blank_strip(uint32_t* fb) {
	for (int i = 0; i < SNES_W * (HUD_H - 1); ++i) fb[i] = 0xFF000000u;
}
