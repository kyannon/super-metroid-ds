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
 * is below the screen or more than 32 lines above it; the rest is unchanged. */
static const uint8_t cullCopy[] = {
	0x5A, 0xB9, 0x00, 0x00, 0xF0, 0x7A, 0xC8, 0xC8, 0x85, 0x18, 0xAE, 0x90,
	0x05, 0x18, 0xB9, 0x00, 0x00, 0x65, 0x14, 0x9D, 0x70, 0x03, 0x29, 0x00,
	0x01, 0xF0, 0x0E, 0xBF, 0x9F, 0x85, 0x81, 0x85, 0x16, 0xB2, 0x16, 0x1F,
	0x9F, 0x83, 0x81, 0x92, 0x16, 0xB9, 0x00, 0x00, 0x10, 0x0E, 0xBF, 0x9F,
	0x85, 0x81, 0x85, 0x16, 0xB2, 0x16, 0x1F, 0xA1, 0x83, 0x81, 0x92, 0x16,
	0xB9, 0x02, 0x00, 0x29, 0xFF, 0x00, 0x89, 0x80, 0x00, 0xF0, 0x03, 0x09,
	0x00, 0xFF, 0x18, 0x65, 0x12, 0x48, 0x18, 0x69, 0x20, 0x00, 0xC9, 0x20,
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
	return n;
}

int sm_patch_count(void) {
	return (int) (sizeof(hudPatches) / sizeof(*hudPatches) + sizeof(rowPatches) / sizeof(*rowPatches)) + 1;
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


void sm_draw_bg(const uint8_t* vram, const uint16_t* cgram, const EmuBg* bg, int y0, int y1, uint32_t* out) {
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
