/* The UI font, embedded (tools/embed_fonts.py). */
#ifndef SMDS_FONTS_DATA_H
#define SMDS_FONTS_DATA_H

#include <stddef.h>

enum { EMBEDDED_SM_FONT, EMBEDDED_FONT_COUNT };

typedef struct {
	const unsigned char* ttf;
	size_t size;
} EmbeddedFont;

extern const EmbeddedFont embeddedFonts[EMBEDDED_FONT_COUNT];

#endif
