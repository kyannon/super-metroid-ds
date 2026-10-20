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
#include "dma.h"
// Development (tools/probe): the I/O registers as last written, and a DMA
// channel's settings (control, B address, A bank:address, indirect bank).
EXPORT uint8_t* smds_io(void) { return Memory.FillRAM; }
EXPORT void smds_dma(int c, unsigned* ctl, unsigned* b, unsigned* a, unsigned* ind) {
	SDMA* d = &DMA[c & 7];
	*ctl = (d->ReverseTransfer << 7) | (d->HDMAIndirectAddressing << 6) | (d->AAddressDecrement << 4) | (d->AAddressFixed << 3) | d->TransferMode;
	*b = d->BAddress;
	*a = (unsigned) d->ABank << 16 | d->AAddress;
	*ind = d->IndirectBank;
}

// The HUD's band of the game's scroll HDMA tables gives way to the band
// below it (core/snes9x.patch, dma.cpp).
extern bool8 smds_hud_hdma;
EXPORT void smds_set_hud_hdma(int on) { smds_hud_hdma = on ? TRUE : FALSE; }
extern bool8 smds_hud_bg3;
EXPORT void smds_set_hud_bg3(int on) { smds_hud_bg3 = on ? TRUE : FALSE; }
// Development (tools/probe): the PPU's sprite table (544 bytes) and OBSEL.
EXPORT uint8_t* smds_oam(void) { return PPU.OAMData; }
EXPORT int smds_obsel(void) { return Memory.FillRAM[0x2101]; }
// A background layer's settings: tilemap and tile (word) addresses,
// scroll, tilemap size (0-3), 16x16 tiles, and the BG mode.
EXPORT void smds_bg(int bg, int* out) {
	const auto& b = PPU.BG[bg & 3];
	out[0] = b.SCBase, out[1] = b.NameBase, out[2] = b.HOffset, out[3] = b.VOffset;
	out[4] = b.SCSize, out[5] = b.BGSize, out[6] = PPU.BGMode;
}
