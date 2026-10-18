// Stand-ins for snes9x's NTSC filter (filter/snes_ntsc.c, LGPL), which this
// project doesn't use: the libretro core only calls these when its NTSC
// filter option is on, and the app never turns it on. Leaving the filter out
// keeps LGPL code out of the shipped libraries.
#include "filter/snes_ntsc.h"

#include <string.h>

extern "C" {
snes_ntsc_setup_t const snes_ntsc_composite = {};
snes_ntsc_setup_t const snes_ntsc_svideo = {};
snes_ntsc_setup_t const snes_ntsc_rgb = {};
snes_ntsc_setup_t const snes_ntsc_monochrome = {};

void snes_ntsc_init(snes_ntsc_t* ntsc, snes_ntsc_setup_t const* setup) {
	(void) setup;
	if (ntsc) memset(ntsc, 0, sizeof(*ntsc));
}

void snes_ntsc_blit(snes_ntsc_t const*, SNES_NTSC_IN_T const*, long, int, int, int, void*, long) {}
void snes_ntsc_blit_hires(snes_ntsc_t const*, SNES_NTSC_IN_T const*, long, int, int, int, void*, long) {}
}
