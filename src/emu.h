/* Thin wrapper around a libretro SNES core (snes9x), loaded at run time.
 *
 * Two copies of the core can run side by side (the game and the hidden copy
 * that captures the pause map): each slot loads its own copy of the core
 * library (sm-core-a.so / sm-core-b.so), so their globals don't meet. */
#ifndef SMDS_EMU_H
#define SMDS_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNES_W 256
#define SNES_H 224
#define SNES_FPS (21477272.0 / 357366.0) /* NTSC: 60.0988 frames a second */

/* SNES pad bits (libretro joypad ids) */
enum {
	K_B = 1 << 0,
	K_Y = 1 << 1,
	K_SELECT = 1 << 2,
	K_START = 1 << 3,
	K_UP = 1 << 4,
	K_DOWN = 1 << 5,
	K_LEFT = 1 << 6,
	K_RIGHT = 1 << 7,
	K_A = 1 << 8,
	K_X = 1 << 9,
	K_L = 1 << 10,
	K_R = 1 << 11,
};

enum { EMU_SLOT_GAME, EMU_SLOT_SHADOW, EMU_SLOTS };

typedef struct EmuCore EmuCore;

typedef struct Emu {
	EmuCore* core;
	int slot;
	uint32_t* fb; /* SNES_W * SNES_H, ABGR8888 (bytes R, G, B, A) */
	bool audio;
	char savePath[1024];
} Emu;

/* The folder the core libraries are in (default: next to the program). */
void emu_set_core_dir(const char* dir);
/* romPath: the .sfc / .smc (a copier header is skipped); savePath: the
 * battery save (.srm), NULL for none (the shadow copy never writes it). */
bool emu_open(Emu* e, int slot, const char* romPath, const char* savePath, bool audio);
/* The same, from a ROM already in memory (the shadow copy). */
bool emu_open_mem(Emu* e, int slot, const uint8_t* rom, size_t size, bool audio);
void emu_close(Emu* e);
void emu_frame(Emu* e, uint32_t keys);
void emu_reset(Emu* e);
/* Skip drawing frames (still emulated): the shadow copy, between captures. */
void emu_set_render(Emu* e, bool on);
/* The HUD's band (the first 31 or 32 lines) of each HDMA table the game sets
 * up takes the next band's values (core/snes9x.patch): with the HUD patched
 * off (sm.c), the room under it scrolls and is drawn like the rest. */
void emu_set_hud_hdma(Emu* e, bool on);
/* Layer 3's tiles from the HUD's tilemap rows are left out of the HUD's lines
 * (while the game would have the HUD there: set each frame). */
void emu_set_hud_bg3(Emu* e, bool on);
/* Layers left out of the picture: bits 0-3 BG1-4, bit 4 sprites. */
void emu_set_layers_off(Emu* e, unsigned mask);

/* The ROM as loaded (no header); writes take effect at once (patches). */
uint8_t* emu_rom(Emu* e, size_t* size);
/* A ROM file, header removed, malloc'd. */
uint8_t* emu_rom_read(const char* path, size_t* size);
/* True for a Super Metroid ROM (internal title). */
bool emu_rom_is_sm(const uint8_t* rom, size_t size);

size_t emu_state_size(Emu* e);
bool emu_state_save(Emu* e, void* buf);
bool emu_state_load(Emu* e, const void* buf);
bool emu_state_save_file(Emu* e, const char* path);
bool emu_state_load_file(Emu* e, const char* path);
/* Writes the battery save if it changed since the last write. */
void emu_save_flush(Emu* e);

/* Work RAM ($7E0000-$7FFFFF, 128 KB), video RAM (64 KB), palette (256 BGR555). */
uint8_t* emu_wram(Emu* e);
uint8_t* emu_vram(Emu* e);
const uint16_t* emu_cgram(Emu* e);
static inline uint8_t emu_read8(Emu* e, uint32_t a) { return emu_wram(e)[a & 0x1FFFF]; }
static inline uint16_t emu_read16(Emu* e, uint32_t a) {
	const uint8_t* w = emu_wram(e);
	return (uint16_t) (w[a & 0x1FFFF] | w[(a + 1) & 0x1FFFF] << 8);
}

/* Pull up to maxFrames stereo frames of audio; returns frames read. */
size_t emu_audio_read(Emu* e, int16_t* out, size_t maxFrames);
unsigned emu_audio_rate(Emu* e);

#endif
