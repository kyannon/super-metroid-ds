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

/* Rows of the room's tilemaps: "update BG graphics when scrolling" ($80:A3DF)
 * draws, when the screen scrolls up, the block row one below the top of the
 * screen (LDX #$0001: the top row was under the HUD), so the top row kept
 * whatever was in that part of the tilemap before (the room's bottom rows).
 * Drawing the top row itself (LDX #$0000) keeps the strip correct; the
 * tilemaps have room for it (16 block rows, the screen shows at most 15). */
static const struct {
	unsigned at;
	uint8_t was[3], now[3];
} rowPatches[] = {
	{ 0xA44E, { 0xA2, 0x01, 0x00 }, { 0xA2, 0x00, 0x00 } }, /* layer 1 (the room) */
	{ 0xA486, { 0xA2, 0x01, 0x00 }, { 0xA2, 0x00, 0x00 } }, /* layer 2 (its background) */
};

/* Sprites below the screen: an enemy's spritemap without the "off-screen"
 * handling ($81:8AB8, called from $A0:94EE for every enemy drawn from a plain
 * spritemap) keeps only the low byte of each piece's Y, so a piece that hangs
 * below the bottom of the screen (Y 256 and on) comes back at the top: the
 * underside of Samus's ship, when the camera rises as she jumps from it. The
 * HUD hid those lines. The call goes instead to a copy of that routine (in
 * free space at the end of bank $A0) that hides (Y $F0) a piece whose full Y
 * is below the screen (224 on: from there its low byte would also pass for
 * one just above the screen, which the core draws) or more than 32 lines
 * above it; the rest is unchanged. */
static const uint8_t cullCopy[] = {
	0x5A, 0xB9, 0x00, 0x00, 0xF0, 0x7A, 0xC8, 0xC8, 0x85, 0x18, 0xAE, 0x90,
	0x05, 0x18, 0xB9, 0x00, 0x00, 0x65, 0x14, 0x9D, 0x70, 0x03, 0x29, 0x00,
	0x01, 0xF0, 0x0E, 0xBF, 0x9F, 0x85, 0x81, 0x85, 0x16, 0xB2, 0x16, 0x1F,
	0x9F, 0x83, 0x81, 0x92, 0x16, 0xB9, 0x00, 0x00, 0x10, 0x0E, 0xBF, 0x9F,
	0x85, 0x81, 0x85, 0x16, 0xB2, 0x16, 0x1F, 0xA1, 0x83, 0x81, 0x92, 0x16,
	0xB9, 0x02, 0x00, 0x29, 0xFF, 0x00, 0x89, 0x80, 0x00, 0xF0, 0x03, 0x09,
	0x00, 0xFF, 0x18, 0x65, 0x12, 0x48, 0x18, 0x69, 0x20, 0x00, 0xC9, 0x00,
	0x01, 0x68, 0x90, 0x03, 0xA9, 0xF0, 0x00, 0xE2, 0x20, 0x9D, 0x71, 0x03,
	0xC2, 0x21, 0xB9, 0x03, 0x00, 0x65, 0x00, 0x05, 0x03, 0x9D, 0x72, 0x03,
	0x98, 0x69, 0x05, 0x00, 0xA8, 0x8A, 0x69, 0x04, 0x00, 0x29, 0xFF, 0x01,
	0xAA, 0xC6, 0x18, 0xD0, 0x90, 0x8E, 0x90, 0x05, 0x7A, 0x6B,
};
#define CULL_AT 0xA0F800u
#define CULL_CALL 0xA094EEu

static size_t lorom(unsigned a) {
	return (size_t) ((a >> 16) & 0x7F) * 0x8000 + (a & 0x7FFF);
}

static bool patch_cull(uint8_t* rom, size_t size) {
	size_t at = lorom(CULL_AT), call = lorom(CULL_CALL);
	if (at + sizeof(cullCopy) > size || call + 4 > size) return false;
	static const uint8_t was[4] = { 0x22, 0xB8, 0x8A, 0x81 }; /* JSL $818AB8 */
	const uint8_t now[4] = { 0x22, CULL_AT & 0xFF, (CULL_AT >> 8) & 0xFF, CULL_AT >> 16 };
	if (!memcmp(rom + call, now, 4) && !memcmp(rom + at, cullCopy, sizeof(cullCopy))) return true;
	if (memcmp(rom + call, was, 4)) {
		fprintf(stderr, "sprite patch: unexpected bytes at $A0:94EE, left alone\n");
		return false;
	}
	for (size_t i = 0; i < sizeof(cullCopy); ++i) {
		if (rom[at + i] != 0xFF) {
			fprintf(stderr, "sprite patch: $A0:F800 is in use, left alone\n");
			return false;
		}
	}
	memcpy(rom + at, cullCopy, sizeof(cullCopy));
	memcpy(rom + call, now, 4);
	return true;
}

/* Sprites just above the screen: the spritemap routines with "off-screen"
 * handling ($81:8B22, $81:8B96, $81:8C10, $81:8C85: Ridley's, among others)
 * hide (Y $F0) a piece whose full Y is above the top of the screen, even
 * when most of it hangs down onto the screen: the HUD covered those lines.
 * Each one's Y test (17 bytes) calls instead a routine in free space at the
 * end of bank $81 with the same test that keeps a piece up to 32 lines above
 * the top (Y $E0-$FF; the core draws its rows that wrap onto the screen),
 * and hides one whose Y is below the bottom (Y $E0-$FF from a base on the
 * screen: the gunship's underside as Samus jumps off it, which would wrap
 * onto the top just the same). */
static const uint8_t topAbove[] = { /* the base above the screen: carry keeps a piece below its line */
	0x30, 0x06, 0x65, 0x12, 0xB0, 0x0C, 0x80, 0x04, 0x65, 0x12, 0x90, 0x06,
	0xC9, 0xE0, 0xB0, 0x02, 0xA9, 0xF0, 0x9D, 0x71, 0x03, 0x60,
};
static const uint8_t topOn[] = { /* the base on the screen: hides a piece below its bottom (Y $E0 on) too */
	0x30, 0x0A, 0x65, 0x12, 0xB0, 0x0E, 0xC9, 0xE0, 0x90, 0x0C, 0x80, 0x08, 0x65, 0x12,
	0xB0, 0x06, 0xC9, 0xE0, 0xB0, 0x02, 0xA9, 0xF0, 0x9D, 0x71, 0x03, 0x60,
};
#define TOP_ABOVE_AT 0x81FF60u
#define TOP_ON_AT 0x81FF80u
static const struct {
	unsigned at;
	bool above;
} topSites[] = { { 0x818B63, false }, { 0x818BD7, true }, { 0x818C3A, false }, { 0x818CAF, true } };

static bool patch_top(uint8_t* rom, size_t size) {
	size_t a = lorom(TOP_ABOVE_AT), o = lorom(TOP_ON_AT);
	if (o + sizeof(topOn) > size) return false;
	bool in = !memcmp(rom + a, topAbove, sizeof(topAbove)) && !memcmp(rom + o, topOn, sizeof(topOn));
	if (!in) {
		for (size_t i = 0; i < sizeof(topOn); ++i) {
			if ((i < sizeof(topAbove) && rom[a + i] != 0xFF) || rom[o + i] != 0xFF) {
				fprintf(stderr, "top sprite patch: $81:FF60 is in use, left alone\n");
				return false;
			}
		}
	}
	/* every site as expected (or done) before any changes */
	uint8_t now[4][17];
	for (int k = 0; k < 4; ++k) {
		size_t at = lorom(topSites[k].at);
		unsigned dst = topSites[k].above ? TOP_ABOVE_AT : TOP_ON_AT;
		uint8_t* n = now[k];
		memset(n, 0xEA, 17);
		n[0] = 0x20, n[1] = dst & 0xFF, n[2] = (dst >> 8) & 0xFF, n[3] = 0x80, n[4] = 12; /* JSR; BRA past */
		bool done = !memcmp(rom + at, n, 17);
		/* the original: BMI; ADC $12; (keep) +8; (hide) +4; ADC $12; (keep) +2; LDA #$F0; STA $0371,x */
		uint8_t keep = topSites[k].above ? 0xB0 : 0x90, hide = keep ^ 0x20;
		const uint8_t orig[17] = { 0x30, 0x06, 0x65, 0x12, keep, 0x08, hide, 0x04, 0x65, 0x12, hide, 0x02, 0xA9, 0xF0, 0x9D, 0x71, 0x03 };
		bool was = !memcmp(rom + at, orig, 17);
		if (!done && !was) {
			fprintf(stderr, "top sprite patch: unexpected bytes at $81:%04X, left alone\n", topSites[k].at & 0xFFFF);
			return false;
		}
	}
	memcpy(rom + a, topAbove, sizeof(topAbove));
	memcpy(rom + o, topOn, sizeof(topOn));
	for (int k = 0; k < 4; ++k) memcpy(rom + lorom(topSites[k].at), now[k], 17);
	return true;
}

/* Free-space routines called from the game in place of a few bytes of
 * its own; each goes in only if the game's bytes there are as expected and
 * the space is free (or it's in already). */
typedef struct {
	const char* what;
	unsigned at;                /* the routine's address */
	const uint8_t* code;
	size_t codeLen;
	unsigned site;              /* the call's address */
	const uint8_t* was;         /* the bytes there before */
	const uint8_t* now;
	size_t siteLen;
} Hook;

static bool patch_hook(uint8_t* rom, size_t size, const Hook* h) {
	size_t at = lorom(h->at), site = lorom(h->site);
	if (at + h->codeLen > size || site + h->siteLen > size) return false;
	if (!memcmp(rom + site, h->now, h->siteLen) && !memcmp(rom + at, h->code, h->codeLen)) return true;
	if (memcmp(rom + site, h->was, h->siteLen)) {
		fprintf(stderr, "%s patch: unexpected bytes at $%06X, left alone\n", h->what, h->site);
		return false;
	}
	for (size_t i = 0; i < h->codeLen; ++i) {
		if (rom[at + i] != 0xFF && rom[at + i] != h->code[i]) {
			fprintf(stderr, "%s patch: $%06X is in use, left alone\n", h->what, h->at);
			return false;
		}
	}
	memcpy(rom + at, h->code, h->codeLen);
	memcpy(rom + site, h->now, h->siteLen);
	return true;
}

/* Scrolling up, the room's top row is drawn (the row patches), not the one
 * below it (row 1) as the game did with the HUD over row 0. An upward door
 * transition, though, draws the whole new screen that way, a row a step,
 * and with row 0 its bottom row was never drawn (another room's tiles
 * across the bottom after an elevator ride up). So the row patches' LDX
 * calls a routine at $80:F000 instead: row 0, but row 1 during a door
 * transition (game state $0B), as the game had it; the top row it leaves
 * stale then is redrawn as the transition ends (game.c pokes the last-drawn
 * row so the game draws it). The game uploads one row a layer a frame, so
 * drawing both rows in one go isn't possible. */
static const uint8_t rowPick[] = {
	0xA2, 0x00, 0x00, 0xAD, 0x98, 0x09, 0xC9, 0x0B, 0x00, 0xD0, 0x03, 0xA2, 0x01, 0x00, 0x60,
};
static const uint8_t rowPickWas[3] = { 0xA2, 0x00, 0x00 }, rowPickNow[3] = { 0x20, 0x00, 0xF0 };

/* Samus below the screen: her spritemap routine ($81:89AE) keeps only the
 * low byte of each piece's Y, so a piece below the bottom (as she rises
 * into a room from below on an elevator) has a Y the core takes for just
 * above the top, and her feet showed at the top of the screen. Its Y store
 * ($81:8A11) calls a routine at $81:FFA0 that hides (Y $F0) a piece whose
 * full Y is below the screen or more than 32 lines above it (when $12, the
 * screen Y, looks like a real 16-bit value). */
static const uint8_t samusY[] = {
	/* LDA $12 / CLC / ADC #$80 / CMP #$200 / BCS plain: $12 holds junk in
	 * its high byte for some poses (spin jump), so only trust it near the
	 * screen and otherwise store the low byte like the game does */
	0xA5, 0x12, 0x18, 0x69, 0x80, 0x00, 0xC9, 0x00, 0x02, 0xB0, 0x23,
	0xB9, 0x02, 0x00, 0x29, 0xFF, 0x00, 0x89, 0x80, 0x00, 0xF0, 0x03, 0x09,
	0x00, 0xFF, 0x18, 0x65, 0x12, 0x48, 0x18, 0x69, 0x20, 0x00, 0xC9, 0x00,
	0x01, 0x68, 0x90, 0x03, 0xA9, 0xF0, 0x00, 0x9D, 0x71, 0x03, 0x60,
	/* plain: */
	0xB9, 0x02, 0x00, 0x18, 0x65, 0x12, 0x9D, 0x71, 0x03, 0x60,
};
static const uint8_t samusWas[9] = { 0xB9, 0x02, 0x00, 0x18, 0x65, 0x12, 0x9D, 0x71, 0x03 };
static const uint8_t samusNow[9] = { 0x20, 0xA0, 0xFF, 0x80, 0x04, 0xEA, 0xEA, 0xEA, 0xEA };

/* The sprite drawn over Samus when she faces the screen (pose 0, as on an
 * elevator: $90:8686) takes its Y straight from hers with no test at all, so
 * below the screen its low byte wraps it onto the top lines. Its Y store
 * calls a routine at $90:FF00 that hides it (Y $F0) when it is below the
 * screen or more than 32 lines above it. */
static const uint8_t frontY[] = {
	0xAD, 0xFA, 0x0A, 0x38, 0xE9, 0x11, 0x00, 0x38, 0xED, 0x15, 0x09, /* Samus Y - $11 - screen Y */
	0xC9, 0xE0, 0x00, 0x90, 0x08, 0xC9, 0xE0, 0xFF, 0xB0, 0x03, 0xA9, 0xF0, 0x00,
	0x9D, 0x71, 0x03, 0x60,
};
static const uint8_t frontWas[14] = { 0xAD, 0xFA, 0x0A, 0x38, 0xE9, 0x11, 0x00, 0x38, 0xED, 0x15, 0x09, 0x9D, 0x71, 0x03 };
static const uint8_t frontNow[14] = { 0x20, 0x00, 0xFF, 0x80, 0x09, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA };

static const Hook hooks[] = {
	{ "row", 0x80F000, rowPick, sizeof(rowPick), 0x80A44E, rowPickWas, rowPickNow, 3 },
	{ "row", 0x80F000, rowPick, sizeof(rowPick), 0x80A486, rowPickWas, rowPickNow, 3 },
	{ "Samus sprite", 0x81FFA0, samusY, sizeof(samusY), 0x818A11, samusWas, samusNow, 9 },
	{ "sprite", 0x81FFA0, samusY, sizeof(samusY), 0x818982, samusWas, samusNow, 9 },
	{ "sprite", 0x81FFA0, samusY, sizeof(samusY), 0x818A91, samusWas, samusNow, 9 },
	{ "Samus front", 0x90FF00, frontY, sizeof(frontY), 0x9086A8, frontWas, frontNow, 14 },
};
#define N_HOOKS (sizeof(hooks) / sizeof(*hooks))

/* Samus's arm cannon ($90:C6F9) and her speed echoes ($90:8AD3) hide a piece
 * only once its Y reaches 256, so one at Y 224-255 (rising into a room from
 * below on an elevator) has its bottom rows wrap onto the top lines, which
 * the HUD covered. Their CMP #$0100 becomes CMP #$00E0. */
static const unsigned yCuts[] = { 0x90C71E, 0x908AF3 };
#define N_YCUTS (sizeof(yCuts) / sizeof(*yCuts))

static bool patch_ycut(uint8_t* rom, size_t size, unsigned a) {
	static const uint8_t was[3] = { 0xC9, 0x00, 0x01 }, now[3] = { 0xC9, 0xE0, 0x00 };
	size_t at = lorom(a);
	if (at + 3 > size) return false;
	if (!memcmp(rom + at, now, 3)) return true;
	if (memcmp(rom + at, was, 3)) {
		fprintf(stderr, "sprite Y patch: unexpected bytes at $%06X, left alone\n", a);
		return false;
	}
	memcpy(rom + at, now, 3);
	return true;
}

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
	for (size_t i = 0; i < sizeof(rowPatches) / sizeof(*rowPatches); ++i) {
		size_t off = rowPatches[i].at - 0x8000;
		if (off + 3 > size) continue;
		if (!memcmp(rom + off, rowPatches[i].now, 3)) {
			++n;
		} else if (!memcmp(rom + off, rowPatches[i].was, 3)) {
			memcpy(rom + off, rowPatches[i].now, 3);
			++n;
		} else {
			fprintf(stderr, "row patch %u: unexpected bytes at $80:%04X, left alone\n", (unsigned) i, rowPatches[i].at);
		}
	}
	if (patch_cull(rom, size)) ++n;
	if (patch_top(rom, size)) ++n;
	for (size_t i = 0; i < N_HOOKS; ++i) n += patch_hook(rom, size, &hooks[i]);
	for (size_t i = 0; i < N_YCUTS; ++i) n += patch_ycut(rom, size, yCuts[i]);
	return n;
}

int sm_patch_count(void) {
	return (int) (sizeof(hudPatches) / sizeof(*hudPatches) + sizeof(rowPatches) / sizeof(*rowPatches)) + 2 + (int) (N_HOOKS + N_YCUTS);
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


void sm_draw_bg(const uint8_t* vram, const uint16_t* cgram, const EmuBg* bg, int y0, int y1, unsigned palMask, uint32_t* out) {
	/* mode 1's layer 1 or 2: 4 bits a pixel, 8 x 8 tiles; tilemaps of 32 x 32
	 * entries, 1-4 of them (mapSize: 1 two across, 2 two down, 3 both) */
	const uint16_t* vw = (const uint16_t*) vram;
	for (int y = y0; y < y1; ++y) {
		uint32_t* row = out + (y - y0) * SNES_W;
		for (int x = 0; x < SNES_W; ++x) {
			int bx = (x + bg->hofs) & 0x3FF, by = (y + bg->vofs) & 0x3FF;
			int tx = (bx >> 3) & 63, ty = (by >> 3) & 63;
			int sc = 0;
			if (tx >= 32 && (bg->mapSize & 1)) sc += 1;
			if (ty >= 32 && (bg->mapSize & 2)) sc += bg->mapSize == 3 ? 2 : 1;
			int addr = (bg->mapBase + sc * 0x400 + (ty & 31) * 32 + (tx & 31)) & 0x7FFF;
			uint16_t e = vw[addr];
			int tile = e & 0x3FF, pal = (e >> 10) & 7;
			if (!(palMask >> pal & 1)) {
				row[x] = 0;
				continue;
			}
			int px = bx & 7, py = by & 7;
			if (e & 0x4000) px = 7 - px;
			if (e & 0x8000) py = 7 - py;
			const uint8_t* t = vram + (((bg->tileBase + tile * 16) & 0x7FFF) * 2);
			int bit = 7 - px;
			int c = ((t[py * 2] >> bit) & 1) | ((t[py * 2 + 1] >> bit) & 1) << 1 | ((t[16 + py * 2] >> bit) & 1) << 2 |
			        ((t[16 + py * 2 + 1] >> bit) & 1) << 3;
			row[x] = c ? bgr555(cgram[pal * 16 + c]) : 0;
		}
	}
}

/* The pause screen without its map: pausing opens the equipment screen
 * (SAMUS) straight away, and L / R don't switch. Only the hidden copy uses
 * it now, to capture the equipment screen for the second screen.
 *  - state Dh's loader ($82:8CEF), just before it finishes, calls a routine
 *    in free space at $82:F800 instead of its LDA #$0001 ($82:8D2F): what the
 *    map-to-equipment switch's "load equipment screen" step does ($82:91AB:
 *    JSL $82BB30, JSR $AB47, JSL $82AC22, $0763 = 1), the screen index
 *    $0727 = 1 (equipment) and $0753 = 2 (the button lit: SAMUS), the
 *    buttons' palettes (JSR $A615), then the LDA #$0001 it replaced
 *  - the L / R handler ($82:A50C) returns at once */
static const uint8_t pauseRoutine[] = {
	0x22, 0x30, 0xBB, 0x82, 0x20, 0x47, 0xAB, 0x22, 0x22, 0xAC, 0x82, 0xA9, 0x01, 0x00, 0x8D, 0x63, 0x07,
	0x8D, 0x27, 0x07, 0xA9, 0x02, 0x00, 0x8D, 0x53, 0x07, 0x20, 0x15, 0xA6, 0xA9, 0x01, 0x00, 0x60,
};
#define PAUSE_HOOK 0x10D2F    /* $82:8D2F */
#define PAUSE_ROUTINE 0x17800 /* $82:F800 */
#define PAUSE_LR 0x1250C      /* $82:A50C */
static const uint8_t pauseHookWas[3] = { 0xA9, 0x01, 0x00 }, pauseHookNow[3] = { 0x20, 0x00, 0xF8 };

bool sm_set_pause_patch(uint8_t* rom, size_t size, bool on) {
	if (PAUSE_ROUTINE + sizeof(pauseRoutine) > size) return false;
	bool hookIn = !memcmp(rom + PAUSE_HOOK, pauseHookNow, 3) && rom[PAUSE_LR] == 0x60;
	bool routineIn = !memcmp(rom + PAUSE_ROUTINE, pauseRoutine, sizeof(pauseRoutine));
	if (!on) {
		if (hookIn) memcpy(rom + PAUSE_HOOK, pauseHookWas, 3), rom[PAUSE_LR] = 0x08;
		return true;
	}
	if (hookIn && routineIn) return true;
	if (memcmp(rom + PAUSE_HOOK, pauseHookWas, 3) || rom[PAUSE_LR] != 0x08) {
		fprintf(stderr, "pause patch: unexpected bytes, left alone\n");
		return false;
	}
	if (!routineIn) {
		for (size_t i = 0; i < sizeof(pauseRoutine); ++i) {
			if (rom[PAUSE_ROUTINE + i] != 0xFF) {
				fprintf(stderr, "pause patch: $82:F800 is in use, left alone\n");
				return false;
			}
		}
		memcpy(rom + PAUSE_ROUTINE, pauseRoutine, sizeof(pauseRoutine));
	}
	memcpy(rom + PAUSE_HOOK, pauseHookNow, 3);
	rom[PAUSE_LR] = 0x60; /* RTS */
	return true;
}

/* Equipment switched from the second screen, while the game runs: main
 * gameplay's handler in the game state table ($82:8991, state 8: $8B44)
 * goes through $82:F840 first, which, when the app has set $7E:FFF0, clears
 * it and does what unpausing does after the equipment screen: Samus's suit
 * palette ($91:DEBA), the beam's tiles and palette ($90:AC8D), her pose for
 * the new equipment ($91:E633), and with reserve tanks the HUD's AUTO sign
 * ($82:AEFD on, $82:AF33 off, by $09C0); then on to $8B44. */
static const uint8_t equipHook[] = {
	0xAF, 0xF0, 0xFF, 0x7E, 0xF0, 0x2C, 0xA9, 0x00, 0x00, 0x8F, 0xF0, 0xFF, 0x7E, 0x22, 0xBA, 0xDE, 0x91, 0x22,
	0x8D, 0xAC, 0x90, 0x22, 0x33, 0xE6, 0x91, 0xAD, 0xD4, 0x09, 0xF0, 0x14, 0x8B, 0x4B, 0xAB, 0xAD, 0xC0, 0x09,
	0xC9, 0x01, 0x00, 0xD0, 0x05, 0x20, 0xFD, 0xAE, 0x80, 0x03, 0x20, 0x33, 0xAF, 0xAB, 0x4C, 0x44, 0x8B,
};
#define EQUIP_HOOK 0x17840  /* $82:F840 */
#define EQUIP_TABLE 0x10991 /* $82:8991 */

bool sm_patch_equip(uint8_t* rom, size_t size) {
	if (EQUIP_HOOK + sizeof(equipHook) > size) return false;
	if (rom[EQUIP_TABLE] == 0x40 && rom[EQUIP_TABLE + 1] == 0xF8 && !memcmp(rom + EQUIP_HOOK, equipHook, sizeof(equipHook))) return true;
	if (rom[EQUIP_TABLE] != 0x44 || rom[EQUIP_TABLE + 1] != 0x8B) {
		fprintf(stderr, "equipment patch: unexpected bytes, left alone\n");
		return false;
	}
	for (size_t i = 0; i < sizeof(equipHook); ++i) {
		if (rom[EQUIP_HOOK + i] != 0xFF) {
			fprintf(stderr, "equipment patch: $82:F840 is in use, left alone\n");
			return false;
		}
	}
	memcpy(rom + EQUIP_HOOK, equipHook, sizeof(equipHook));
	rom[EQUIP_TABLE] = 0x40, rom[EQUIP_TABLE + 1] = 0xF8;
	return true;
}

/* The equipment screen's rows (SNES pixels, the whole 256 x 224 screen) */
static const SmEquipRow equipRows[SM_EQUIP_N] = {
	[SM_EQ_VARIA] = { 166, 70, 242, 79, 0x09A2, 0x0001 },
	[SM_EQ_GRAVITY] = { 166, 79, 242, 88, 0x09A2, 0x0020 },
	[SM_EQ_MORPH] = { 166, 102, 242, 111, 0x09A2, 0x0004 },
	[SM_EQ_BOMB] = { 166, 111, 242, 119, 0x09A2, 0x1000 },
	[SM_EQ_SPRING] = { 166, 119, 242, 127, 0x09A2, 0x0002 },
	[SM_EQ_SCREW] = { 166, 127, 242, 136, 0x09A2, 0x0008 },
	[SM_EQ_HIJUMP] = { 166, 150, 242, 159, 0x09A2, 0x0100 },
	[SM_EQ_SPACE] = { 166, 159, 242, 167, 0x09A2, 0x0200 },
	[SM_EQ_SPEED] = { 166, 167, 242, 176, 0x09A2, 0x2000 },
	[SM_EQ_CHARGE] = { 28, 126, 84, 135, 0x09A6, 0x1000 },
	[SM_EQ_ICE] = { 28, 135, 84, 143, 0x09A6, 0x0002 },
	[SM_EQ_WAVE] = { 28, 143, 84, 151, 0x09A6, 0x0001 },
	[SM_EQ_SPAZER] = { 28, 151, 84, 159, 0x09A6, 0x0004 },
	[SM_EQ_PLASMA] = { 28, 159, 84, 168, 0x09A6, 0x0008 },
	[SM_EQ_MODE] = { 16, 74, 92, 90, 0x09C0, 0 },
};

const SmEquipRow* sm_equip_row(int item) {
	return item >= 0 && item < SM_EQUIP_N ? &equipRows[item] : NULL;
}

int sm_equip_at(int x, int y) {
	/* a little slack around each box, rows split where they meet */
	for (int i = 0; i < SM_EQUIP_N; ++i) {
		const SmEquipRow* r = &equipRows[i];
		int y0 = r->y0, y1 = r->y1;
		if (i == SM_EQ_VARIA || i == SM_EQ_MORPH || i == SM_EQ_HIJUMP || i == SM_EQ_CHARGE) y0 -= 3;
		if (i == SM_EQ_GRAVITY || i == SM_EQ_SCREW || i == SM_EQ_SPEED || i == SM_EQ_PLASMA) y1 += 3;
		if (x >= r->x0 - 4 && x < r->x1 + 4 && y >= y0 && y < y1) return i;
	}
	return -1;
}

static uint16_t rd16(const uint8_t* w, unsigned a) {
	return (uint16_t) (w[a] | w[a + 1] << 8);
}
static void wr16(uint8_t* w, unsigned a, uint16_t v) {
	w[a] = (uint8_t) v, w[a + 1] = (uint8_t) (v >> 8);
}

bool sm_equip_has(const uint8_t* wram, int item) {
	const SmEquipRow* r = sm_equip_row(item);
	if (!r) return false;
	if (item == SM_EQ_MODE) return rd16(wram, 0x09D4) > 0; /* reserve tanks */
	return rd16(wram, r->addr + 2) & r->bit;               /* collected */
}

bool sm_equip_on(const uint8_t* wram, int item) {
	const SmEquipRow* r = sm_equip_row(item);
	if (!r) return false;
	if (item == SM_EQ_MODE) return rd16(wram, 0x09C0) == 1; /* AUTO */
	return rd16(wram, r->addr) & r->bit;
}

bool sm_equip_toggle(uint8_t* wram, int item) {
	if (!sm_equip_has(wram, item)) return false;
	const SmEquipRow* r = sm_equip_row(item);
	if (item == SM_EQ_MODE) {
		wr16(wram, 0x09C0, rd16(wram, 0x09C0) == 1 ? 2 : 1);
	} else {
		uint16_t v = rd16(wram, r->addr) ^ r->bit;
		/* spazer and plasma don't go together (the game's own rule) */
		if (item == SM_EQ_SPAZER && (v & 0x0004)) v &= (uint16_t) ~0x0008;
		if (item == SM_EQ_PLASMA && (v & 0x0008)) v &= (uint16_t) ~0x0004;
		wr16(wram, r->addr, v);
	}
	wram[0xFFF0] = 1, wram[0xFFF1] = 0; /* the hook ($82:F840) brings Samus up to date */
	return true;
}
