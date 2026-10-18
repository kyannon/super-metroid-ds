// Super Metroid DS additions to the snes9x libretro core: what the app
// needs beyond the libretro API (the HUD is drawn from video RAM, the map
// copy skips layers, the HUD patch goes into the loaded ROM).
#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "gfx.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT void smds_set_layers(unsigned disabledMask) { Settings.BG_Forced = disabledMask & 0x1F; }
EXPORT uint8_t* smds_vram(void) { return Memory.VRAM; }
EXPORT uint16_t* smds_cgram(void) { return PPU.CGDATA; }
EXPORT uint8_t* smds_rom(void) { return Memory.ROM; }
EXPORT size_t smds_rom_size(void) { return Memory.CalculatedSize; }
