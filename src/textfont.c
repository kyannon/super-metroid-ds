/* UI text in the Super Metroid font (an OpenType/CFF pixel font, via
 * stb_truetype). The font is drawn on a grid of "dots" (125 font units): its
 * capitals are 11 dots tall and every character advances 8. Text is drawn
 * with whole screen pixels per dot and no smoothing, so it stays as crisp as
 * the game's own lettering. Characters the font doesn't have (- / , ! ? ( )
 * + < >) come from small bitmaps in the same style. */
#include "textfont.h"
#include "fonts_data.h"

#pragma GCC diagnostic ignored "-Wunused-function" /* stb_truetype has many we don't call */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DOT_UNITS 125
#define CAP_DOTS 11
#define ADV_DOTS 8

static stbtt_fontinfo font;
static bool ready;
static int ascentDots = 13, descentDots = 1;
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* stand-ins, CAP_DOTS rows from the top of the capitals (+2 below the baseline) */
typedef struct {
	int cp;
	const char* rows[13];
} Fallback;
static const Fallback fallbacks[] = {
	{ '-', { "", "", "", "", "", ".#####", ".#####", "", "", "", "", "", "" } },
	{ '_', { "", "", "", "", "", "", "", "", "", "", "", "#######", "#######" } },
	{ '/', { ".....##", ".....##", "....##.", "....##.", "...##..", "...##..", "..##...", "..##...", ".##....", ".##....", "##.....", "", "" } },
	{ ',', { "", "", "", "", "", "", "", "", "", "..##", "..##", ".##", "" } },
	{ '!', { "..##", "..##", "..##", "..##", "..##", "..##", "..##", "", "", "..##", "..##", "", "" } },
	{ '?', { ".####.", "##..##", "##..##", "....##", "...##.", "..##..", "..##..", "", "", "..##..", "..##..", "", "" } },
	{ '(', { "...##", "..##", ".##", ".##", "##", "##", "##", ".##", ".##", "..##", "...##", "", "" } },
	{ ')', { "##", ".##", "..##", "..##", "...##", "...##", "...##", "..##", "..##", ".##", "##", "", "" } },
	{ '+', { "", "", "...##", "...##", "...##", "########", "########", "...##", "...##", "...##", "", "", "" } },
	{ '>', { "", ".##", "..##", "...##", "....##", ".....##", "....##", "...##", "..##", ".##", "", "", "" } },
	{ '<', { "", ".....##", "....##", "...##", "..##", ".##", "..##", "...##", "....##", ".....##", "", "", "" } },
};

static const Fallback* fallback_for(int cp) {
	for (size_t i = 0; i < sizeof(fallbacks) / sizeof(*fallbacks); ++i) {
		if (fallbacks[i].cp == cp) return &fallbacks[i];
	}
	return NULL;
}

/* the small font: dots of 250 units (half-dot strokes, 125) */
#define SMALL_DOT_UNITS 250
#define SMALL_CAP_DOTS 5
#define SMALL_ADV_DOTS 8
static stbtt_fontinfo smallFont;
static bool smallReady;

static void init(void) {
	{
		const EmbeddedFont* e = &embeddedFonts[EMBEDDED_SM_SMALL_FONT];
		smallReady = stbtt_InitFont(&smallFont, e->ttf, stbtt_GetFontOffsetForIndex(e->ttf, 0));
	}
	const EmbeddedFont* e = &embeddedFonts[EMBEDDED_SM_FONT];
	if (!stbtt_InitFont(&font, e->ttf, stbtt_GetFontOffsetForIndex(e->ttf, 0))) return;
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
	ascentDots = (asc + DOT_UNITS - 1) / DOT_UNITS;
	descentDots = (-desc + DOT_UNITS - 1) / DOT_UNITS;
	if (descentDots < 2) descentDots = 2; /* room for the stand-ins' tails */
	ready = true;
}

/* Next code point from UTF-8 (invalid bytes come out as themselves). */
static int next_cp(const char** s) {
	const unsigned char* p = (const unsigned char*) *s;
	int c = *p++;
	int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	if (extra) {
		c &= 0x3F >> extra;
		for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i) c = (c << 6) | (*p++ & 0x3F);
	}
	*s = (const char*) p;
	return c;
}

int tf_dot(int capPx) {
	int d = (capPx + CAP_DOTS / 3) / CAP_DOTS;
	return d < 1 ? 1 : d;
}

int tf_cap(int capPx) {
	return tf_dot(capPx) * CAP_DOTS;
}

static int small_dot(int capPx) {
	int d = (capPx + SMALL_CAP_DOTS / 2) / SMALL_CAP_DOTS;
	return d < 1 ? 1 : d;
}

int tf_font_cap(int fontId, int capPx) {
	if (fontId == TF_SMALL) return small_dot(capPx) * SMALL_CAP_DOTS;
	if (fontId == TF_SMOOTH) return capPx;
	return tf_cap(capPx);
}

static int count_chars(const char* text) {
	int n = 0;
	for (const char* p = text; *p;) {
		next_cp(&p);
		++n;
	}
	return n;
}

static bool render_smooth(int capPx, const char* text, TfBitmap* out);

/* The small font on a grid of half dots (125 units), then k screen pixels a
 * dot (a half-dot stroke is k / 2 pixels; at one pixel a dot, the left half
 * of each dot decides). */
static bool render_small(int capPx, const char* text, TfBitmap* out) {
	if (!smallReady) return false;
	int k = small_dot(capPx);
	int n = count_chars(text);
	const int S = 2; /* half dots a dot */
	int ascS = SMALL_CAP_DOTS * S, descS = 1 * S;
	int ws = n * SMALL_ADV_DOTS * S, hs = ascS + descS;
	uint8_t* sub = calloc((size_t) ws * hs, 1);
	if (!sub) return false;
	float sc = (float) S / SMALL_DOT_UNITS;
	int x = 0;
	for (const char* p = text; *p;) {
		int cp = next_cp(&p);
		int g = stbtt_FindGlyphIndex(&smallFont, cp);
		if (!g && cp == '.') {
			/* not in the font: a dot on the baseline, as wide as a stroke */
			for (int ty = ascS - S; ty < ascS; ++ty) {
				for (int tx = x + 2 * S; tx < x + 4 * S && tx < ws; ++tx) sub[ty * ws + tx] = 255;
			}
		}
		int ix0, iy0, ix1, iy1;
		stbtt_GetGlyphBitmapBox(&smallFont, g, sc, sc, &ix0, &iy0, &ix1, &iy1);
		int gw = ix1 - ix0, gh = iy1 - iy0;
		if (g && gw > 0 && gh > 0) {
			uint8_t* gb = malloc((size_t) gw * gh);
			if (gb) {
				stbtt_MakeGlyphBitmap(&smallFont, gb, gw, gh, gw, sc, sc, g);
				for (int yy = 0; yy < gh; ++yy) {
					int ty = ascS + iy0 + yy;
					if (ty < 0 || ty >= hs) continue;
					for (int xx = 0; xx < gw; ++xx) {
						int tx = x + ix0 + xx;
						if (tx >= 0 && tx < ws && gb[yy * gw + xx] >= 128) sub[ty * ws + tx] = 255;
					}
				}
				free(gb);
			}
		}
		x += SMALL_ADV_DOTS * S;
	}
	int w = n * SMALL_ADV_DOTS * k, h = (SMALL_CAP_DOTS + 1) * k;
	uint8_t* a = malloc((size_t) w * h);
	if (!a) {
		free(sub);
		return false;
	}
	for (int y = 0; y < h; ++y) {
		for (int xx = 0; xx < w; ++xx) a[y * w + xx] = sub[(y * S / k) * ws + xx * S / k];
	}
	free(sub);
	out->alpha = a;
	out->w = w;
	out->h = h;
	out->baseline = SMALL_CAP_DOTS * k;
	out->left = k;
	return true;
}

int tf_width(int fontId, int capPx, const char* text) {
	pthread_once(&once, init);
	if (!text) return 0;
	int n = count_chars(text);
	if (fontId == TF_SMOOTH) return n ? (int) ceilf((n * ADV_DOTS - 1) * (float) capPx / CAP_DOTS) : 0;
	/* the small font's glyphs are 6 dots of the 8, a dot in from the left */
	if (fontId == TF_SMALL) return n ? (n * SMALL_ADV_DOTS - 2) * small_dot(capPx) : 0;
	/* the last character's empty right-hand column doesn't count */
	return n ? (n * ADV_DOTS - 1) * tf_dot(capPx) : 0;
}

bool tf_render(int fontId, int capPx, const char* text, TfBitmap* out) {
	pthread_once(&once, init);
	memset(out, 0, sizeof(*out));
	if (!ready || !text || capPx <= 0) return false;
	if (fontId == TF_SMOOTH) return render_smooth(capPx, text, out);
	if (fontId == TF_SMALL) return render_small(capPx, text, out);
	int k = tf_dot(capPx);
	int n = count_chars(text);
	int wd = n * ADV_DOTS + 2, hd = ascentDots + descentDots + 1;
	uint8_t* dots = calloc((size_t) wd * hd, 1);
	if (!dots) return false;
	float sc = 1.0f / DOT_UNITS;
	int base = ascentDots; /* baseline row (dots) */
	int x = 1;
	for (const char* p = text; *p;) {
		int cp = next_cp(&p);
		const Fallback* fb = stbtt_FindGlyphIndex(&font, cp) ? NULL : fallback_for(cp);
		if (fb) {
			for (int r = 0; r < 13; ++r) {
				int ty = base - CAP_DOTS + r;
				for (int c = 0; fb->rows[r][c]; ++c) {
					if (fb->rows[r][c] == '#' && ty >= 0 && ty < hd && x + c < wd) dots[ty * wd + x + c] = 255;
				}
			}
		} else {
			int g = stbtt_FindGlyphIndex(&font, cp);
			int ix0, iy0, ix1, iy1;
			stbtt_GetGlyphBitmapBox(&font, g, sc, sc, &ix0, &iy0, &ix1, &iy1);
			int gw = ix1 - ix0, gh = iy1 - iy0;
			if (gw > 0 && gh > 0) {
				uint8_t* gb = malloc((size_t) gw * gh);
				if (gb) {
					stbtt_MakeGlyphBitmap(&font, gb, gw, gh, gw, sc, sc, g);
					for (int yy = 0; yy < gh; ++yy) {
						int ty = base + iy0 + yy;
						if (ty < 0 || ty >= hd) continue;
						for (int xx = 0; xx < gw; ++xx) {
							int tx = x + ix0 + xx;
							if (tx >= 0 && tx < wd && gb[yy * gw + xx] >= 128) dots[ty * wd + tx] = 255;
						}
					}
					free(gb);
				}
			}
		}
		x += ADV_DOTS;
	}
	int w = wd * k, h = hd * k;
	uint8_t* a = malloc((size_t) w * h);
	if (!a) {
		free(dots);
		return false;
	}
	for (int y = 0; y < h; ++y) {
		for (int xx = 0; xx < w; ++xx) a[y * w + xx] = dots[(y / k) * wd + xx / k];
	}
	free(dots);
	out->alpha = a;
	out->w = w;
	out->h = h;
	out->baseline = base * k;
	out->left = k;
	return true;
}

/* Rendered big (8 pixels a dot) and box-filtered down to the exact size. */
static bool render_smooth(int capPx, const char* text, TfBitmap* out) {
	TfBitmap big;
	const int K = 8;
	if (!tf_render(TF_BOLD, K * CAP_DOTS, text, &big)) return false;
	float sc = (float) capPx / (K * CAP_DOTS);
	int w = (int) ceilf(big.w * sc), h = (int) ceilf(big.h * sc);
	uint8_t* a = calloc((size_t) w * h, 1);
	if (!a) {
		tf_free(&big);
		return false;
	}
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			/* the source area this pixel covers */
			float sx0 = x / sc, sx1 = (x + 1) / sc, sy0 = y / sc, sy1 = (y + 1) / sc;
			float sum = 0, area = 0;
			for (int yy = (int) sy0; yy < (int) ceilf(sy1) && yy < big.h; ++yy) {
				float wy = fminf(sy1, yy + 1) - fmaxf(sy0, yy);
				for (int xx = (int) sx0; xx < (int) ceilf(sx1) && xx < big.w; ++xx) {
					float wx = fminf(sx1, xx + 1) - fmaxf(sx0, xx);
					sum += big.alpha[yy * big.w + xx] * wx * wy;
					area += wx * wy;
				}
			}
			a[y * w + x] = area > 0 ? (uint8_t) fminf(255, sum / area + 0.5f) : 0;
		}
	}
	out->alpha = a;
	out->w = w;
	out->h = h;
	out->baseline = (int) lroundf(big.baseline * sc);
	out->left = (int) lroundf(big.left * sc);
	tf_free(&big);
	return true;
}

void tf_free(TfBitmap* b) {
	free(b->alpha);
	b->alpha = NULL;
}
