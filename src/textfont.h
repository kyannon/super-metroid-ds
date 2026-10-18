/* UI text in the Super Metroid pixel font (assets/fonts, via stb_truetype):
 * measuring and rasterizing UTF-8 strings. ui.c turns the results into
 * textures. Sizes snap to whole screen pixels per font dot (capitals are 11
 * dots tall), so the size drawn can be a little off the one asked for. */
#ifndef SMDS_TEXTFONT_H
#define SMDS_TEXTFONT_H

#include <stdbool.h>
#include <stdint.h>

/* TF_BOLD and TF_BODY are the same font (kept for the callers), drawn with
 * whole pixels per dot; TF_SMOOTH is drawn at exactly the size asked for,
 * smoothed (for text that has to fit a box the crisp sizes miss). */
enum { TF_BOLD, TF_BODY, TF_SMOOTH, TF_COUNT };

/* Screen pixels per font dot, and the capital height drawn, for a capPx asked for. */
int tf_dot(int capPx);
int tf_cap(int capPx);

/* Advance width in pixels of text whose capital letters are capPx tall. */
int tf_width(int font, int capPx, const char* text);

/* Rasterized text: 8-bit coverage, w * h. The baseline is `baseline` pixels
 * from the top and the text starts `left` pixels from the left edge (italic
 * and overhanging glyphs can reach a little either side of the advance). */
typedef struct {
	uint8_t* alpha;
	int w, h, baseline, left;
} TfBitmap;
bool tf_render(int font, int capPx, const char* text, TfBitmap* out);
void tf_free(TfBitmap* b);

#endif
