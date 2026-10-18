/* Tiny 5x7 bitmap font for on-screen messages. */
#ifndef SMDS_FONT_H
#define SMDS_FONT_H

#include <stdint.h>

/* Returns 7 rows, 5 bits each (bit 4 = leftmost), for an ASCII char.
 * Lowercase is drawn as uppercase; unknown chars are blank. */
const uint8_t* font_glyph(char c);

#endif
