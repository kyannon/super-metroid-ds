/* The play session: the game screen, the second screen (the HUD over the
 * pause screen's map or equipment screen, or "MAP NOT AVAILABLE"), the
 * settings menu. */
#include "game.h"
#include "android.h"
#include "emu.h"
#include "mapcap.h"
#include "paths.h"
#include "cheats.h"
#include "rewind.h"
#include "sm.h"
#include "textfont.h"
#include "ui.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>

static Config* cfg;
static Emu emu;
static bool hudPatched;

/* One frame of the game; while the game would show its HUD, layer 3 leaves
 * the HUD's tiles out of the HUD's lines (a room whose layer 3 shares the
 * HUD's tilemap would show them there for a frame or two at a door). */
static void game_frame(uint32_t keys) {
	if (hudPatched) {
		/* the strip under the old HUD gets the room's line-31 HDMA values
		 * only while there's a HUD: in a cutscene (the colony exploding, the
		 * intro) the game's own top lines are drawn as they are */
		bool h = sm_hud_shown(&emu);
		emu_set_hud_bg3(&emu, h);
		emu_set_hud_hdma(&emu, h);
	}
	emu_frame(&emu, keys);
	/* a door transition just ended: have the game draw the screen's top row
	 * (sm.c's row patch left it to the transition's end) by marking the
	 * last-drawn block rows one below where they are, as if it had just
	 * scrolled up */
	static int lastState;
	int st = emu_read8(&emu, 0x0998);
	if (hudPatched && lastState == 0x0B && st != 0x0B) {
		uint8_t* w = emu_wram(&emu);
		uint16_t y1 = (uint16_t) (w[0x08F9] | w[0x08FA] << 8) + 1, y2 = (uint16_t) (w[0x08FD] | w[0x08FE] << 8) + 1;
		w[0x0901] = (uint8_t) y1, w[0x0902] = (uint8_t) (y1 >> 8);
		w[0x0905] = (uint8_t) y2, w[0x0906] = (uint8_t) (y2 >> 8);
	}
	lastState = st;
}
static MapCapture* mc;
static GamePaths gp;
static bool debug;

/* ------------------------------------------------- frame buffer (option)
 * With frame_buffer on, the next frame is emulated on a worker thread while
 * the loop draws and shows the one before (one frame more input delay; a
 * frame that runs long no longer misses the display's refresh). Everything
 * else that touches the core runs while the worker is idle. */
static struct {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool started, busy, quit;
	uint32_t keys;
} ahead = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };
static bool aheadInFlight, aheadReady;

/* rewinding (L2): a state every cfg->rewindStep frames */
static Rewind* rw;
static uint8_t* rwState;
static int rwSince;
static bool rewinding;

static void rewind_setup(void) {
	rewind_destroy(rw);
	free(rwState);
	rw = NULL, rwState = NULL;
	if (!cfg->rewind) return;
	size_t n = emu_state_size(&emu);
	rwState = n ? malloc(n) : NULL;
	rw = rwState ? rewind_create(n, (size_t) SDL_max(4, cfg->rewindMb) << 20) : NULL;
	if (!rw) free(rwState), rwState = NULL;
	rwSince = 0;
}
static uint32_t aheadFb[SNES_W * SNES_H];

static void* ahead_worker(void* arg) {
	(void) arg;
#ifdef __ANDROID__
	android_raise_thread();
#endif
	pthread_mutex_lock(&ahead.lock);
	for (;;) {
		while (!ahead.busy && !ahead.quit) pthread_cond_wait(&ahead.cond, &ahead.lock);
		if (ahead.quit) break;
		uint32_t keys = ahead.keys;
		pthread_mutex_unlock(&ahead.lock);
		game_frame(keys);
		pthread_mutex_lock(&ahead.lock);
		ahead.busy = false;
		pthread_cond_broadcast(&ahead.cond);
	}
	pthread_mutex_unlock(&ahead.lock);
	return NULL;
}

static bool ahead_start(uint32_t keys) {
	pthread_mutex_lock(&ahead.lock);
	if (!ahead.started) {
		ahead.quit = false;
		ahead.started = pthread_create(&ahead.thread, NULL, ahead_worker, NULL) == 0;
	}
	bool ok = ahead.started;
	if (ok) {
		ahead.keys = keys;
		ahead.busy = true;
		pthread_cond_broadcast(&ahead.cond);
	}
	pthread_mutex_unlock(&ahead.lock);
	return ok;
}

static void ahead_sync(void) {
	if (!aheadInFlight) return;
	pthread_mutex_lock(&ahead.lock);
	while (ahead.busy) pthread_cond_wait(&ahead.cond, &ahead.lock);
	pthread_mutex_unlock(&ahead.lock);
	aheadInFlight = false;
	aheadReady = true;
}

static void ahead_stop(void) {
	ahead_sync();
	aheadReady = false;
	pthread_mutex_lock(&ahead.lock);
	bool started = ahead.started;
	ahead.quit = true;
	pthread_cond_broadcast(&ahead.cond);
	pthread_mutex_unlock(&ahead.lock);
	if (started) pthread_join(ahead.thread, NULL);
	ahead.started = false;
}

/* ------------------------------------------------- the second screen */

static SDL_Texture* gameTex[2];
static SDL_Texture* secTex[2];
static uint32_t secPx[SNES_W * SNES_H]; /* HUD on top, the map below */
static MapResult mapRes;
static bool haveMap;      /* a map of this session is up */
static int mapShownIdx = -1;
static bool mapDirty = true;
static uint64_t mapAnimStart, frameCount;
static bool singleShowsMap;
static double fps, speedPct;

/* The second screen's pages, MAP, SAMUS and MENU (the pause screen's map
 * and its equipment screen, captured like the map, and the settings), with
 * a button each below them; switching fades to black and back. While the
 * game couldn't pause (a door, Ceres, a cutscene) MAP and SAMUS are greyed
 * and their page dimmed; MENU is always there. */
static MapResult samRes;
static bool haveSam;
static int samShownIdx = -1;
static uint64_t samAnimStart;
#define TAB_MENU 2
#define N_TABS 3
static int secTab = MAPCAP_MAP, tabNext = MAPCAP_MAP;
enum { FADE_NONE, FADE_OUT, FADE_WAIT, FADE_IN };
#define FADE_STEPS 8
#define FADE_WAIT_MAX 45 /* frames to wait for the new page's capture */
#define DIM_STEPS 6
static int fadePhase, fadeTicks;
static uint32_t fadeSerial;
static bool capForce[2];
static int tabPressed = -1, eqPressed = -1;
static bool tabsOn;   /* the game could pause now */
static int dimLevel;  /* 0..DIM_STEPS */
/* START during play (two screens): the game stops, dimmed, the pages stay */
static bool appPaused;
static const SDL_Rect tabRects[N_TABS] = { { 14, 202, 72, 18 }, { 92, 202, 72, 18 }, { 170, 202, 72, 18 } };
static const char* tabLabels[N_TABS] = { "MAP", "SAMUS", "MENU" };

#define PX_RGB(r, g, b) (0xFF000000u | (uint32_t) (b) << 16 | (uint32_t) (g) << 8 | (uint32_t) (r))
/* the pause screen's colours */
#define PX_BLACK PX_RGB(0, 0, 0)
#define PX_WHITE PX_RGB(255, 255, 255)
#define PX_ORANGE PX_RGB(255, 182, 0)      /* an equipped item's row */
#define PX_GREY PX_RGB(148, 150, 148)      /* an item that's off */
#define PX_GREY_TEXT PX_RGB(57, 56, 57)    /* its letters */
#define PX_GREY_DOT PX_RGB(189, 190, 189)  /* its bullet */
#define PX_FRAME_DARK PX_RGB(99, 97, 99)
#define PX_FRAME_LIGHT PX_RGB(189, 190, 189)
#define PX_LAVENDER PX_RGB(214, 158, 222)
#define PX_PURPLE PX_RGB(66, 0, 140)       /* the backdrop's grid */
#define PX_RED PX_RGB(255, 0, 0)
#define PX_CURSOR PX_RGB(0, 0, 230)

static void px_fill(uint32_t* px, int x, int y, int w, int h, uint32_t c) {
	for (int j = y; j < y + h; ++j) {
		if (j < 0 || j >= SNES_H) continue;
		for (int i = x; i < x + w; ++i) {
			if (i >= 0 && i < SNES_W) px[j * SNES_W + i] = c;
		}
	}
}

static void px_set(uint32_t* px, int x, int y, uint32_t c) {
	if (x >= 0 && x < SNES_W && y >= 0 && y < SNES_H) px[y * SNES_W + x] = c;
}

/* rows y0..y1-1 at num / den brightness */
static void px_scale(uint32_t* px, int y0, int y1, int num, int den) {
	for (uint32_t* q = px + y0 * SNES_W; q < px + y1 * SNES_W; ++q) {
		uint32_t c = *q;
		uint32_t r = (c & 0xFF) * num / den, g = (c >> 8 & 0xFF) * num / den, b = (c >> 16 & 0xFF) * num / den;
		*q = 0xFF000000u | b << 16 | g << 8 | r;
	}
}

/* Text in the game's pixels: TF_BOLD (capitals 11 tall) or TF_SMALL (5),
 * its capitals centred on cy; edge (if not 0) a pixel round it, like the
 * prologue's lettering. Rendered once, kept. */
static int px_cap(int font) {
	return font == TF_SMALL ? 5 : 11;
}
static const TfBitmap* px_glyphs(int font, const char* text) {
	static struct {
		int font;
		char text[128];
		TfBitmap b;
		bool ok;
	} cache[48];
	static int next;
	for (int i = 0; i < 48; ++i) {
		if (cache[i].ok && cache[i].font == font && !strcmp(cache[i].text, text)) return &cache[i].b;
	}
	int slot = next++ % 48;
	tf_free(&cache[slot].b);
	cache[slot].ok = tf_render(font, px_cap(font), text, &cache[slot].b);
	cache[slot].font = font;
	snprintf(cache[slot].text, sizeof(cache[slot].text), "%s", text);
	return cache[slot].ok ? &cache[slot].b : NULL;
}
static int px_text_w(int font, const char* text) {
	return tf_width(font, px_cap(font), text);
}
static void px_text(uint32_t* px, int font, int x, int cy, const char* text, uint32_t fill, uint32_t edge, int align) {
	if (!text || !text[0]) return;
	const TfBitmap* b = px_glyphs(font, text);
	if (!b) return;
	int cap = px_cap(font), w = px_text_w(font, text);
	int left = align < 0 ? x : align > 0 ? x - w : x - w / 2;
	int ox = left - b->left, oy = cy - cap / 2 - (b->baseline - cap);
	for (int pass = edge ? 0 : 1; pass < 2; ++pass) {
		for (int j = 0; j < b->h; ++j) {
			for (int i = 0; i < b->w; ++i) {
				if (b->alpha[j * b->w + i] < 128) continue;
				if (pass == 1) px_set(px, ox + i, oy + j, fill);
				else {
					for (int dy = -1; dy <= 1; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) px_set(px, ox + i + dx, oy + j + dy, edge);
					}
				}
			}
		}
	}
}

/* The equipment screen's bullet before an item, 5 x 4 from (x, cy - 2). */
static void px_bullet(uint32_t* px, int x, int cy, bool on, uint32_t bg) {
	static const char* rows[4] = { ".RRS.", "RWRRS", "RRRRS", ".RRS." };
	uint32_t dot = on ? PX_RED : PX_GREY_DOT, shine = on ? PX_WHITE : bg, shade = on ? PX_BLACK : PX_GREY_TEXT;
	for (int j = 0; j < 4; ++j) {
		for (int i = 0; i < 5; ++i) {
			char c = rows[j][i];
			if (c != '.') px_set(px, x + i, cy - 2 + j, c == 'R' ? dot : c == 'W' ? shine : shade);
		}
	}
}

/* The pause screen's frame (x 2-253, y 41-196: four lines, rounded
 * corners) round its dotted purple grid, the title in a gap of the top
 * edge between three short bars a side, as the map's and SAMUS's are. */
#define FRAME_X0 2
#define FRAME_X1 253
#define FRAME_Y0 41
#define FRAME_Y1 196
static void px_pause_frame(uint32_t* px, const char* title) {
	px_fill(px, 0, MAP_Y0, SNES_W, MAP_ROWS, PX_BLACK);
	for (int y = FRAME_Y0 + 4; y <= FRAME_Y1 - 4; ++y) {
		for (int x = FRAME_X0 + 4; x <= FRAME_X1 - 4; ++x) {
			if (!(y & 1) && (!(x & 7) || ((y & 7) == 6 && !(x & 1)))) px_set(px, x, y, PX_PURPLE);
		}
	}
	/* the four lines, outside in: dark, light, white, dark */
	const uint32_t line[4] = { PX_FRAME_DARK, PX_FRAME_LIGHT, PX_WHITE, PX_FRAME_DARK };
	for (int k = 0; k < 4; ++k) {
		px_fill(px, FRAME_X0 + 5, FRAME_Y0 + k, FRAME_X1 - FRAME_X0 - 9, 1, line[k]);
		px_fill(px, FRAME_X0 + 5, FRAME_Y1 - k, FRAME_X1 - FRAME_X0 - 9, 1, line[k]);
		px_fill(px, FRAME_X0 + k, FRAME_Y0 + 5, 1, FRAME_Y1 - FRAME_Y0 - 9, line[k]);
		px_fill(px, FRAME_X1 - k, FRAME_Y0 + 5, 1, FRAME_Y1 - FRAME_Y0 - 9, line[k]);
	}
	/* the corners (top left as drawn; the others mirrored) */
	static const char* corner[5] = { "..DDD", ".DDLL", "DDLLW", "DLLWD", "DLWDD" };
	for (int j = 0; j < 5; ++j) {
		for (int i = 0; i < 5; ++i) {
			char c = corner[j][i];
			if (c == '.') continue;
			uint32_t col = c == 'D' ? PX_FRAME_DARK : c == 'L' ? PX_FRAME_LIGHT : PX_WHITE;
			px_set(px, FRAME_X0 + i, FRAME_Y0 + j, col);
			px_set(px, FRAME_X1 - i, FRAME_Y0 + j, col);
			px_set(px, FRAME_X0 + i, FRAME_Y1 - j, col);
			px_set(px, FRAME_X1 - i, FRAME_Y1 - j, col);
		}
	}
	if (!title || !title[0]) return;
	/* the gap: the title and 8 pixels either side, then | | | */
	int tw = px_text_w(TF_BOLD, title);
	int l = SNES_W / 2 - tw / 2 - 8, r = SNES_W / 2 + (tw - tw / 2) + 8;
	px_fill(px, l - 6, FRAME_Y0 - 4, r - l + 12, 12, PX_BLACK);
	for (int k = 0; k < 4; ++k) {
		for (int bar = 0; bar < 3; ++bar) {
			px_set(px, l - 1 - bar * 2, FRAME_Y0 + k, line[k]);
			px_set(px, r + bar * 2, FRAME_Y0 + k, line[k]);
		}
	}
	px_text(px, TF_BOLD, SNES_W / 2, FRAME_Y0 + 1, title, PX_WHITE, 0, 0);
}

/* The map's left / right arrows (6 x 11 at x 13 / 237, y 114), in the
 * yellow of their pulse's peak (the game's pulse: dark for 15 frames, then
 * up to yellow and down again, 3 frames a step, 54 in all). */
#define ARROW_Y0 114
#define ARROW_COLOUR PX_RGB(255, 231, 0)
static void px_arrow(uint32_t* px, bool right) {
	static const uint8_t lv[8][3] = { { 0, 0, 0 }, { 49, 40, 0 }, { 82, 73, 0 }, { 115, 97, 0 },
		                              { 148, 134, 0 }, { 181, 166, 0 }, { 214, 190, 0 }, { 255, 231, 0 } };
	(void) lv;
	uint32_t c = ARROW_COLOUR; /* steady, as the map's are now (map_steady_arrows) */
	for (int i = 0; i < 11; ++i) {
		int w = 6 - abs(i - 5);
		for (int j = 0; j < w; ++j) px_set(px, right ? 237 + j : 18 - j, ARROW_Y0 + i, c);
	}
}

static void draw_tab(uint32_t* px, int t) {
	SDL_Rect r = tabRects[t];
	bool on = t == (fadePhase == FADE_NONE ? secTab : tabNext);
	bool pressed = t == tabPressed;
	bool usable = t == TAB_MENU || tabsOn;
	uint32_t edge = PX_FRAME_DARK, fill, text;
	if (!usable) fill = PX_GREY_TEXT, text = PX_FRAME_DARK;
	else if (on) fill = PX_ORANGE, text = PX_BLACK, edge = PX_LAVENDER;
	else fill = pressed ? PX_GREY_DOT : PX_GREY, text = PX_GREY_TEXT;
	/* a rounded frame like the pause screen's boxes */
	px_fill(px, r.x + 1, r.y, r.w - 2, r.h, edge);
	px_fill(px, r.x, r.y + 1, r.w, r.h - 2, edge);
	px_fill(px, r.x + 2, r.y + 2, r.w - 4, r.h - 4, fill);
	if (on && usable) px_fill(px, r.x + 3, r.y + 2, r.w - 6, 1, PX_RGB(255, 231, 140)); /* a highlight */
	int off = pressed ? 1 : 0;
	px_text(px, TF_BOLD, r.x + r.w / 2 + off, r.y + r.h / 2 + off, tabLabels[t], text, 0, 0);
}

static int tab_hit(int sx, int sy) {
	for (int t = 0; t < N_TABS; ++t) {
		SDL_Rect r = tabRects[t];
		if (sx >= r.x - 3 && sx < r.x + r.w + 3 && sy >= r.y - 3 && sy < SNES_H) return t;
	}
	return -1;
}

/* the page's brightness in FADE_STEPS */
static int fade_level(void) {
	switch (fadePhase) {
	case FADE_OUT: return FADE_STEPS - fadeTicks;
	case FADE_WAIT: return 0;
	case FADE_IN: return fadeTicks;
	default: return FADE_STEPS;
	}
}

static int going_tab(void) {
	return fadePhase == FADE_NONE ? secTab : tabNext;
}

static void switch_tab(int t) {
	if (t == going_tab()) return;
	tabNext = t;
	if (fadePhase == FADE_IN) fadeTicks = FADE_STEPS - fadeTicks, fadePhase = FADE_OUT;
	else if (fadePhase == FADE_NONE) fadeTicks = 0, fadePhase = FADE_OUT;
	if (t != TAB_MENU) {
		fadeSerial = t == MAPCAP_SAMUS ? samRes.serial : mapRes.serial;
		capForce[t] = true;
	}
	mapDirty = true;
}

/* The pause map's scroll arrows, steady: wherever one shows in any frame
 * of a capture (in its pulse's colours), it's drawn in every frame at the
 * pulse's peak. Up / down: 11 x 6 at x 122, y 52 / 180; left / right: 6 x
 * 11 at x 13 / 237, y 114. */
static bool arrow_pixel(int a, int x, int y) {
	switch (a) {
	case 0: return y >= 52 && y < 58 && abs(x - 127) <= y - 52;   /* up */
	case 1: return y >= 180 && y < 186 && abs(x - 127) <= 185 - y; /* down */
	case 2: return y >= ARROW_Y0 && y < ARROW_Y0 + 11 && x <= 18 && x > 18 - (6 - abs(y - ARROW_Y0 - 5)); /* left */
	default: return y >= ARROW_Y0 && y < ARROW_Y0 + 11 && x >= 237 && x < 237 + (6 - abs(y - ARROW_Y0 - 5)); /* right */
	}
}
static void map_steady_arrows(MapResult* r) {
	static const uint32_t pulse[7] = { PX_RGB(49, 40, 0), PX_RGB(82, 73, 0), PX_RGB(115, 97, 0), PX_RGB(148, 134, 0),
		                               PX_RGB(181, 166, 0), PX_RGB(214, 190, 0), PX_RGB(255, 231, 0) };
	static const int box[4][4] = { { 122, 52, 133, 58 }, { 122, 180, 133, 186 }, { 13, ARROW_Y0, 19, ARROW_Y0 + 11 }, { 237, ARROW_Y0, 243, ARROW_Y0 + 11 } };
	for (int a = 0; a < 4; ++a) {
		bool shown = false;
		for (int f = 0; f < r->nFrames && !shown; ++f) {
			const uint32_t* px = r->frames[f];
			for (int y = box[a][1]; y < box[a][3] && !shown; ++y) {
				for (int x = box[a][0]; x < box[a][2] && !shown; ++x) {
					if (!arrow_pixel(a, x, y)) continue;
					uint32_t c = px[(y - MAP_Y0) * SNES_W + x];
					for (int k = 0; k < 7; ++k) shown |= c == pulse[k];
				}
			}
		}
		if (!shown) continue;
		for (int f = 0; f < r->nFrames; ++f) {
			uint32_t* px = r->frames[f];
			for (int y = box[a][1]; y < box[a][3]; ++y) {
				for (int x = box[a][0]; x < box[a][2]; ++x) {
					if (arrow_pixel(a, x, y)) px[(y - MAP_Y0) * SNES_W + x] = ARROW_COLOUR;
				}
			}
		}
	}
}

/* A new map (not just scrolled) crossfades in over the one shown, e.g. a
 * map station's rooms appearing */
#define XFADE_FRAMES 24
static uint32_t xfOld[SNES_W * MAP_ROWS];
static int xfTick = -1;

/* The equipment screen's look of a switched item at once (its rows' orange
 * and grey swapped in the frames shown), until the next capture shows it. */
static void sam_recolor(void) {
	const uint8_t* w = emu_wram(&emu);
	const uint32_t orange = PX_ORANGE, grey = PX_GREY;
	const uint32_t red = PX_RED, greyDot = PX_GREY_DOT;
	for (int it = 0; it < SM_EQUIP_N; ++it) {
		if (it == SM_EQ_MODE || !sm_equip_has(w, it)) continue;
		const SmEquipRow* r = sm_equip_row(it);
		bool on = sm_equip_on(w, it);
		for (int f = 0; f < samRes.nFrames; ++f) {
			uint32_t* px = samRes.frames[f];
			if (!px) continue;
			for (int y = r->y0; y < r->y1; ++y) {
				if (y < MAP_Y0 || y >= MAP_Y0 + MAP_ROWS) continue;
				uint32_t* row = px + (y - MAP_Y0) * SNES_W;
				for (int x = r->x0; x < r->x1; ++x) {
					if (on && row[x] == grey) row[x] = orange;
					else if (on && row[x] == greyDot) row[x] = red;
					else if (!on && row[x] == orange) row[x] = grey;
					else if (!on && row[x] == red) row[x] = greyDot;
				}
			}
		}
	}
	mapDirty = true;
}

/* ui_image_pixels' lcd for the Shader setting: a cell a pixel, or scanlines */
static int shader_lcd(void) {
	return cfg->shader == 1 ? 1 : cfg->shader == 2 ? UI_SCANLINES : 0;
}

/* The game on the first (top) screen; the second has the touch panel and
 * the pages tapped on it, so they don't swap. */
static int game_screen(void) {
	return 0;
}

static int map_screen(void) {
	return nScreens < 2 ? 0 : 1 - game_screen();
}

/* Ceres's escape: the EMERGENCY message the game writes on layer 1 of the
 * room after Ridley ($E0B5, screen rows ESC_SRC..+ESC_ROWS), shown on the
 * MAP / SAMUS page instead of AWAITING DATA CONNECTION. as it's written,
 * and kept while the escape's timer runs. */
#define ESC_SRC 40
#define ESC_ROWS 72
#define ESC_Y (MAP_Y0 + (MAP_ROWS - ESC_ROWS) / 2)
static uint32_t escPx[SNES_W * ESC_ROWS];
static bool haveEsc, escShown;

static void escape_update(void) {
	uint16_t area = emu_read16(&emu, 0x079F), room = emu_read16(&emu, 0x079B);
	bool timer = emu_read8(&emu, 0x0943) != 0, hud = sm_hud_shown(&emu);
	bool was = escShown;
	if (area != 6) haveEsc = false;
	bool here = false;
	/* in play only: a door fades the palette (the message would be kept black) */
	if (area == 6 && room == 0xE0B5 && hud && emu_read8(&emu, 0x0998) == 0x08) {
		EmuBg bg;
		emu_bg(&emu, 0, &bg);
		static uint32_t now[SNES_W * ESC_ROWS];
		if (bg.mode == 1) {
			/* the message's letters only (palettes 5 and 6: the room's own
			 * tiles, and Ridley's close-up flying off, are in others) */
			sm_draw_bg(emu_vram(&emu), emu_cgram(&emu), &bg, ESC_SRC, ESC_SRC + ESC_ROWS, 0x60, now);
			for (int i = 0; i < SNES_W * ESC_ROWS && !here; ++i) here = now[i] != 0;
		}
		if (here) {
			if (memcmp(now, escPx, sizeof(now))) mapDirty = true;
			memcpy(escPx, now, sizeof(now));
			haveEsc = true;
		}
	}
	bool inRoom = area == 6 && room == 0xE0B5;
	static bool tested;
	if (!tested && getenv("SMDS_TEST_ESCAPE") && timer) haveEsc = tested = true; /* tests: from a state past Ridley's room */
	/* Samus at the elevator: the ride up (game states $20, $21), the fade to
	 * black, the black, and the colony in the cutscene ($22) until its
	 * explosion flashes white (the timer stops at the black) */
	uint8_t st = emu_read8(&emu, 0x0998);
	bool ending = haveEsc && (st == 0x20 || st == 0x21 || st == 0x22);
	if (ending && st == 0x22) {
		/* the white flash: the whole picture bright in red, green and blue
		 * (the yellow one before it has little blue) */
		uint64_t r = 0, g = 0, b = 0;
		for (int i = 0; i < SNES_W * SNES_H; ++i) {
			uint32_t c = emu.fb[i];
			r += c & 0xFF, g += c >> 8 & 0xFF, b += c >> 16 & 0xFF;
		}
		uint64_t n = (uint64_t) SNES_W * SNES_H * 100;
		if (r > n && g > n && b > n) ending = false, haveEsc = false;
	}
	escShown = haveEsc && (timer || ending || (hud && (here || inRoom)));
	if (!timer && !ending && !here && !(inRoom && st != 0x08)) haveEsc = false;
	if (escShown != was) mapDirty = true;
	if (escShown != was && debug) fprintf(stderr, "frame %llu: escape message %s (have %d timer %d hud %d here %d area %d)\n", (unsigned long long) frameCount, escShown ? "up" : "down", haveEsc, timer, hud, here, area);
	if (debug) {
		static int lastState = -1;
		int st = emu_read8(&emu, 0x0998);
		static int lastBright = -1;
		int br = emu_read8(&emu, 0x0051);
		if (st != lastState || (getenv("SMDS_DEBUG_BRIGHT") && br != lastBright))
			fprintf(stderr, "frame %llu: game state %02x brightness %02x area %d (escape message %s, timer %02x)\n", (unsigned long long) frameCount, st, br, emu_read16(&emu, 0x079F), escShown ? "up" : "down", emu_read8(&emu, 0x0943));
		lastState = st, lastBright = br;
	}
}

/* the MENU page and the EXIT question (with the menu, below) */
static void compose_menu(uint32_t* px);
static void compose_exit(uint32_t* px);

/* The second screen's picture: the live HUD over the page. */
static void compose_secondary(bool hud) {
	static uint32_t hudPx[SNES_W * HUD_H];
	if (hud) {
		sm_draw_hud(emu_vram(&emu), emu_cgram(&emu), hudPx);
		memcpy(secPx, hudPx, sizeof(hudPx));
	} else {
		for (int i = 0; i < SNES_W * HUD_H; ++i) secPx[i] = 0xFF000000u;
	}
	uint32_t* below = secPx + SNES_W * HUD_H;
	int belowN = SNES_W * (SNES_H - HUD_H);
	for (int i = 0; i < belowN; ++i) below[i] = 0xFF000000u;
	if (secTab == TAB_MENU) {
		compose_menu(secPx);
	} else {
		const MapResult* res = secTab == MAPCAP_SAMUS ? &samRes : &mapRes;
		bool have = secTab == MAPCAP_SAMUS ? haveSam : haveMap;
		int idx = secTab == MAPCAP_SAMUS ? samShownIdx : mapShownIdx;
		if (have && idx >= 0 && idx < res->nFrames && res->frames[idx]) {
			memcpy(secPx + SNES_W * MAP_Y0, res->frames[idx], (size_t) SNES_W * MAP_ROWS * 4);
			if (secTab == MAPCAP_MAP && xfTick >= 0) {
				uint32_t* q = secPx + SNES_W * MAP_Y0;
				int t = xfTick, n = XFADE_FRAMES;
				for (int i = 0; i < SNES_W * MAP_ROWS; ++i) {
					uint32_t a = xfOld[i], b = q[i];
					uint32_t r = ((a & 0xFF) * (n - t) + (b & 0xFF) * t) / n;
					uint32_t g = ((a >> 8 & 0xFF) * (n - t) + (b >> 8 & 0xFF) * t) / n;
					uint32_t bl = ((a >> 16 & 0xFF) * (n - t) + (b >> 16 & 0xFF) * t) / n;
					q[i] = 0xFF000000u | bl << 16 | g << 8 | r;
				}
			}
		} else if (fadePhase == FADE_NONE) {
			if (escShown) {
				/* the escape's EMERGENCY message, as the game writes it */
				for (int y = 0; y < ESC_ROWS; ++y) {
					for (int x = 0; x < SNES_W; ++x) {
						uint32_t c = escPx[y * SNES_W + x];
						if (c) secPx[(ESC_Y + y) * SNES_W + x] = c;
					}
				}
			} else {
				/* green with a dark edge, like the prologue's lettering */
				px_text(secPx, TF_SMALL, SNES_W / 2, MAP_Y0 + MAP_ROWS / 2, "AWAITING DATA CONNECTION.", PX_RGB(0, 255, 0), PX_RGB(0, 89, 0), 0);
			}
		}
	}
	int lvl = fade_level();
	if (lvl < FADE_STEPS) px_scale(secPx, MAP_Y0, MAP_Y0 + MAP_ROWS, lvl, FADE_STEPS);
	/* the page dimmed, not the buttons: MENU stays lit, MAP and SAMUS are
	 * drawn greyed themselves */
	if (dimLevel && !escShown) px_scale(secPx, MAP_Y0, MAP_Y0 + MAP_ROWS, DIM_STEPS * 2 - dimLevel, DIM_STEPS * 2);
	for (int t = 0; t < N_TABS; ++t) draw_tab(secPx, t);
	compose_exit(secPx);
}

/* The EXIT button where the HUD's mini-map was, in screen-local
 * coordinates of the second screen. */
static SDL_Rect exit_button_rect(int s) {
	SDL_Rect img = ui_image_rect(s, SNES_W, SNES_H, cfg->mapScale);
	double sx = (double) img.w / SNES_W, sy = (double) img.h / SNES_H;
	int x0 = img.x + (int) lround(HUD_MINIMAP_X0 * sx), x1 = img.x + (int) lround((HUD_MINIMAP_X1 + 1) * sx);
	int y0 = img.y + (int) lround(HUD_MINIMAP_Y0 * sy), y1 = img.y + (int) lround((HUD_MINIMAP_Y1 + 1) * sy);
	return (SDL_Rect) { x0, y0, x1 - x0, y1 - y0 };
}

static bool buttonPressed; /* a finger is down on it */

/* The map's scroll arrows (left, right, up, down) as places to tap, in the
 * second screen's SNES pixels: a band along each edge of the map's frame
 * (rows MAP_Y0..198), the corners left out. */
enum { ARROW_LEFT, ARROW_RIGHT, ARROW_UP, ARROW_DOWN };
static int arrowHeld = -1, arrowFrames;

static int map_arrow_at(int s, int x, int y) {
	SDL_Rect img = ui_image_rect(s, SNES_W, SNES_H, cfg->mapScale);
	if (img.w <= 0 || img.h <= 0) return -1;
	int sx = (int) floor((x - img.x) * (double) SNES_W / img.w), sy = (int) floor((y - img.y) * (double) SNES_H / img.h);
	if (sy < MAP_Y0 + 8 || sy >= MAP_Y0 + MAP_ROWS || sx < 0 || sx >= SNES_W) return -1;
	bool midX = sx >= 64 && sx < SNES_W - 64, midY = sy >= MAP_Y0 + 40 && sy < MAP_Y0 + MAP_ROWS - 40;
	if (sx < 56 && midY) return ARROW_LEFT;
	if (sx >= SNES_W - 56 && midY) return ARROW_RIGHT;
	if (sy < MAP_Y0 + 40 && midX) return ARROW_UP;
	if (sy >= MAP_Y0 + MAP_ROWS - 40 && midX) return ARROW_DOWN;
	return -1;
}

/* The HUD's item icons (missiles, super missiles, power bombs, grapple,
 * X-ray), tapped on the second screen, select that item at once (the
 * selected one again: none). As the game's own SELECT does it: $09D2 (the
 * item) is set to the one before and SELECT pressed for a frame, so the
 * game's switch runs (its checks, sound and icon); if the game didn't take
 * the press (Samus can't switch just then) $09D2 is put back. */
static const struct {
	int x0, x1;
} hudIcons[6] = { { 0, 0 }, { 78, 106 }, { 108, 132 }, { 133, 156 }, { 157, 180 }, { 181, 204 } };
static int hudTap; /* the item asked for, 1..5, or 0 */
static struct {
	bool active;
	uint16_t orig, wrote;
} hudInj;

static int hud_icon_at(int s, int x, int y) {
	SDL_Rect img = ui_image_rect(s, SNES_W, SNES_H, cfg->mapScale);
	if (img.w <= 0 || img.h <= 0) return 0;
	int sx = (int) floor((x - img.x) * (double) SNES_W / img.w), sy = (int) floor((y - img.y) * (double) SNES_H / img.h);
	if (sy < 4 || sy >= HUD_H) return 0;
	for (int i = 1; i <= 5; ++i) {
		if (sx >= hudIcons[i].x0 && sx < hudIcons[i].x1) return i;
	}
	return 0;
}

static bool hud_item_available(int i) {
	switch (i) {
	case 1: return emu_read16(&emu, 0x09C6) > 0;
	case 2: return emu_read16(&emu, 0x09CA) > 0;
	case 3: return emu_read16(&emu, 0x09CE) > 0;
	case 4: return emu_read16(&emu, 0x09A2) & 0x4000;
	case 5: return emu_read16(&emu, 0x09A2) & 0x8000;
	default: return false;
	}
}

static void wram_write16(uint32_t a, uint16_t v) {
	uint8_t* w = emu_wram(&emu);
	w[a] = (uint8_t) v, w[a + 1] = (uint8_t) (v >> 8);
}

/* Before the next emulated frame: a tapped item becomes a SELECT press. */
static void hud_inject(uint32_t* keys) {
	if (!hudTap || hudInj.active) return;
	int want = hudTap;
	hudTap = 0;
	if (emu_read8(&emu, 0x0998) != 0x08 || btnNow[B_SELECT]) return;
	uint16_t cur = emu_read16(&emu, 0x09D2);
	int target = cur == want ? 0 : want;
	if (target && !hud_item_available(target)) return;
	hudInj.orig = cur;
	hudInj.wrote = (uint16_t) (target ? target - 1 : 5);
	hudInj.active = true;
	wram_write16(0x09D2, hudInj.wrote);
	*keys |= K_SELECT;
}

/* After that frame: put the item back if the game didn't take the press. */
static void hud_inject_done(void) {
	if (!hudInj.active) return;
	hudInj.active = false;
	if (emu_read16(&emu, 0x09D2) == hudInj.wrote) wram_write16(0x09D2, hudInj.orig);
}

static void map_scroll_step(int d) {
	mapcap_scroll(mc, d == ARROW_LEFT ? -1 : d == ARROW_RIGHT ? 1 : 0, d == ARROW_UP ? -1 : d == ARROW_DOWN ? 1 : 0);
}

/* EXIT: white on red, in the game's small lettering, whole pixels a dot
 * ("EXIT" is 30 dots wide, 5 tall) */
static bool exitAsk; /* ARE YOU SURE? is up */
static void draw_exit_button(int s) {
	SDL_Rect b = exit_button_rect(s);
	SDL_Rect img = ui_image_rect(s, SNES_W, SNES_H, cfg->mapScale);
	int u = SDL_max(1, img.w / SNES_W);
	ui_sm_button_face(s, b, u, buttonPressed, (SDL_Color) { 206, 0, 0, 255 });
	int innerW = b.w - u * 4, innerH = b.h - u * 4;
	int dot = SDL_max(1, SDL_min(innerW / 30, innerH / 5));
	int off = buttonPressed ? u : 0;
	ui_font_text(s, TF_SMALL, b.x + b.w / 2 + off, b.y + b.h / 2 + off, dot * 5, "EXIT", SM_WHITE, 0);
	/* under the question, dimmed with the rest */
	if (exitAsk) ui_fill(s, b, (SDL_Color) { 0, 0, 0, 128 });
}

static bool in_rect(SDL_Rect r, int x, int y) {
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* ------------------------------------------------- menu */

enum MenuItem {
	M_RESUME, M_RESTART, M_SAVE, M_LOAD, M_SHOT, M_EXIT,
	M_SCALE, M_LCD,
	M_MAPANIM, M_MAPRATE, M_FRAMEBUF, M_MATCH, M_FPS, M_REWIND, M_CHEATS, M_FULLPAUSE,
};
static const char* menuLabels[] = {
	[M_RESUME] = "Resume", [M_RESTART] = "Restart Game", [M_SAVE] = "Save State", [M_LOAD] = "Load State",
	[M_SHOT] = "Take Screenshot", [M_CHEATS] = "Cheats", [M_EXIT] = "Close Super Metroid",
};
#define N_PAGES 2
static int pages[N_PAGES][12], pageLen[N_PAGES], curPage;
static int menuItems[12], nMenuItems;
static bool menuOpen;
static Cheats cheats;
static bool cheatsOpen, cheatDragged;
static int cheatSel, cheatScroll, cheatPress = -1, cheatPressY, cheatPressScroll;
static void open_cheats(void);
static int menuSel, menuPressed = -1;
static bool restartArmed;
static char menuNote[80];
/* buttons still held when the menu closes are kept from the game until released */
static bool suppress[B_COUNT];
#define HELD(b) (btnNow[b] && !suppress[b])
static void suppress_held(void) {
	for (int b = 0; b < B_COUNT; ++b) suppress[b] = btnNow[b];
}

static bool item_enabled(int item) {
	if (item == M_LOAD) return file_exists(gp.state);
	if (item == M_MAPRATE || item == M_MAPANIM) return mc != NULL;
	return true;
}

static void set_page(int page) {
	curPage = page;
	nMenuItems = pageLen[page];
	memcpy(menuItems, pages[page], sizeof(int) * nMenuItems);
	menuSel = 0;
	menuPressed = -1;
	menuNote[0] = 0;
	restartArmed = false;
}

static const char* scale_label(const char* scale) {
	return !strcmp(scale, "fit")     ? "Scale: Full Width" :
	       !strcmp(scale, "zoom")    ? "Scale: Zoom" :
	       !strcmp(scale, "stretch") ? "Scale: Stretch" : "Scale: Pixel Perfect";
}

static const int mapRates[] = { 180, 90, 60, 30 };
#define N_MAP_RATES ((int) (sizeof(mapRates) / sizeof(*mapRates)))

static const char* map_rate_label(int frames) {
	switch (frames) {
	case 180: return "Map Refresh: Every 3 s";
	case 90: return "Map Refresh: Every 1.5 s";
	case 60: return "Map Refresh: Every 1 s";
	case 30: return "Map Refresh: Every 0.5 s";
	default: return "Map Refresh: Custom";
	}
}

static const char* item_label(int item) {
	switch (item) {
	case M_SCALE: return scale_label(cfg->scale);
	case M_MAPRATE: return map_rate_label(cfg->mapInterval);
	case M_LCD: return cfg->shader == 1 ? "Shader: Pixel Grid" : cfg->shader == 2 ? "Shader: Scanlines" : "Shader: Off";
	case M_FPS: return cfg->showFps ? "Show FPS: On" : "Show FPS: Off";
	case M_REWIND: return cfg->rewind ? "Rewind (L2): On" : "Rewind (L2): Off";
	case M_FULLPAUSE: return cfg->fullPause ? "Full Pause Menu: On" : "Full Pause Menu: Off";
	case M_FRAMEBUF: return cfg->frameBuffer ? "Frame Buffer: On" : "Frame Buffer: Off";
	case M_MAPANIM: return cfg->mapAnim ? "Map Animations: On" : "Map Animations: Off";
	case M_MATCH: return cfg->matchDisplay ? "Match Display Rate: On" : "Match Display Rate: Off";
	case M_RESTART: return restartArmed ? "Tap Again to Restart" : menuLabels[item];
	default: return menuLabels[item];
	}
}

/* The MENU page is drawn in the game's pixels like the other pages: the
 * pause screen's frame with the page's name in its top edge, the items in
 * rows of the equipment screen's orange, the map's arrows to the pages
 * either side (swiping does the same). */
typedef struct {
	SDL_Rect items[12];
	int noteY;
	int row[12], col[12], nRows; /* each item's row and place in it */
} MenuGeom;
#define MENU_CAP 11 /* the font's capitals, one pixel a dot */
#define MENU_X 30
#define MENU_W (SNES_W - 2 * MENU_X)
#define MENU_TOP 52
#define MENU_ROWH 15
#define MENU_PITCH 17
static const char* pageTitles[N_PAGES] = { "GAME", "DISPLAY" };

static MenuGeom menu_geom(void) {
	MenuGeom g;
	memset(&g, 0, sizeof(g));
	/* a row an item, but Save State and Load State side by side */
	int row = -1;
	for (int i = 0; i < nMenuItems; ++i) {
		bool second = i > 0 && menuItems[i] == M_LOAD && menuItems[i - 1] == M_SAVE;
		if (!second) ++row;
		g.items[i] = (SDL_Rect) { MENU_X, MENU_TOP + row * MENU_PITCH, MENU_W, MENU_ROWH };
		g.row[i] = row, g.col[i] = second;
		if (second) {
			int half = (MENU_W - 4) / 2;
			g.items[i - 1].w = half;
			g.items[i].x += MENU_W - half, g.items[i].w = half;
		}
	}
	g.nRows = row + 1;
	g.noteY = 170;
	return g;
}

/* the item at a point (the game's pixels), or -1 */
static int item_at(int x, int y) {
	MenuGeom g = menu_geom();
	for (int i = 0; i < nMenuItems; ++i) {
		/* the gap between rows counts too */
		SDL_Rect r = g.items[i];
		r.y -= 1, r.h += 2;
		if (in_rect(r, x, y)) return i;
	}
	return -1;
}

/* the left (0) or right (1) arrow's place to tap, or -1 */
static int page_arrow_at(int x, int y) {
	if (y < ARROW_Y0 - 22 || y > ARROW_Y0 + 32) return -1;
	if (x < MENU_X) return 0;
	if (x >= MENU_X + MENU_W) return 1;
	return -1;
}

/* An item's row: orange (grey when it can't be used), the bullet, the
 * label; the cursor (white and blue) round the one selected. */
static void px_item(uint32_t* px, SDL_Rect r, const char* label, const char* right, bool en, bool sel) {
	if (sel) {
		px_fill(px, r.x - 2, r.y - 2, r.w + 4, r.h + 4, PX_WHITE);
		px_fill(px, r.x - 1, r.y - 1, r.w + 2, r.h + 2, PX_CURSOR);
	}
	uint32_t bg = en ? PX_ORANGE : PX_GREY, ink = en ? PX_BLACK : PX_GREY_TEXT;
	px_fill(px, r.x, r.y, r.w, r.h, bg);
	int cy = r.y + r.h / 2;
	px_bullet(px, r.x + 3, cy, en, bg);
	px_text(px, TF_BOLD, r.x + 11, cy, label, ink, 0, -1);
	if (right) px_text(px, TF_BOLD, r.x + r.w - 4, cy, right, ink, 0, 1);
}

static int battery_percent(void) {
	static int pct = -1;
	static Uint32 last;
	static bool read;
	Uint32 now = SDL_GetTicks();
	if (read && now - last < 5000) return pct;
	read = true;
	last = now;
	pct = -1;
#ifndef __ANDROID__
	DIR* d = opendir("/sys/class/power_supply");
	if (d) {
		struct dirent* de;
		while (pct < 0 && (de = readdir(d))) {
			if (de->d_name[0] == '.') continue;
			char path[512], type[32] = "";
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", de->d_name);
			FILE* f = fopen(path, "r");
			if (!f) continue;
			if (!fgets(type, sizeof(type), f)) type[0] = 0;
			fclose(f);
			if (strncmp(type, "Battery", 7)) continue;
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", de->d_name);
			f = fopen(path, "r");
			if (!f) continue;
			int v;
			if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100) pct = v;
			fclose(f);
		}
		closedir(d);
	}
#endif
	if (pct < 0) {
		int p = -1;
		SDL_PowerState st = SDL_GetPowerInfo(NULL, &p);
		if (st != SDL_POWERSTATE_UNKNOWN && st != SDL_POWERSTATE_NO_BATTERY && p >= 0) pct = p;
	}
	return pct;
}

/* PAUSED at the game's text size, white with a dark grey edge a pixel
 * wide, like the opening's text crawl (u screen pixels a game pixel) */
static void draw_paused_label(int s, int cx, int cy, int u) {
	int cap = tf_cap(u * 11);
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			if (dx || dy) ui_font_text(s, TF_BOLD, cx + dx * u, cy + dy * u, cap, "PAUSED", (SDL_Color) { 82, 81, 82, 255 }, 0);
		}
	}
	ui_font_text(s, TF_BOLD, cx, cy, cap, "PAUSED", SM_WHITE, 0);
}

/* ------------------------------------------------- cheats (on the MENU page)
 * "Cheats" on the Game page: every cheat of the .cht files in cheats/, one
 * list, each with ON / OFF (as in GBA DS). D-pad and A to switch, L / R a
 * page at a time, B back; tap a cheat to switch it, drag to scroll, the
 * left arrow (or a swipe right) to go back. */
#define CHEAT_ROWS 6 /* rows on screen */

static void cheat_keep_visible(void) {
	if (cheatSel < cheatScroll) cheatScroll = cheatSel;
	if (cheatSel >= cheatScroll + CHEAT_ROWS) cheatScroll = cheatSel - CHEAT_ROWS + 1;
	cheatScroll = SDL_clamp(cheatScroll, 0, SDL_max(0, cheats.n - CHEAT_ROWS));
}

static void open_cheats(void) {
	cheatsOpen = true;
	cheatSel = 0;
	cheatScroll = 0;
	cheatPress = -1;
	mapDirty = true;
}

static void toggle_cheat(int i) {
	if (i < 0 || i >= cheats.n) return;
	cheats.items[i].on = !cheats.items[i].on;
	cheats_apply(&cheats, &emu);
	cheats_save(&cheats);
}

static SDL_Rect cheat_row_rect(int row) { /* row on screen, 0..CHEAT_ROWS-1 */
	return (SDL_Rect) { MENU_X, MENU_TOP + row * MENU_PITCH, MENU_W - 6, MENU_ROWH };
}

/* text cut to fit width (the game's pixels), with "..." */
static void fit_text(const char* in, int width, char* out, size_t size) {
	snprintf(out, size, "%s", in);
	if (px_text_w(TF_BOLD, out) <= width) return;
	size_t n = strlen(out);
	while (n > 0) {
		out[--n] = 0;
		char t[160];
		snprintf(t, sizeof(t), "%s...", out);
		if (px_text_w(TF_BOLD, t) <= width) {
			snprintf(out, size, "%s", t);
			return;
		}
	}
}

static void compose_cheats(uint32_t* px) {
	int nameW = MENU_W - 6 - 11 - 34;
	bool cut = false;
	for (int row = 0; row < CHEAT_ROWS; ++row) {
		int i = cheatScroll + row;
		if (i >= cheats.n) break;
		const Cheat* c = &cheats.items[i];
		char name[160];
		fit_text(c->desc, nameW, name, sizeof(name));
		bool sel = i == cheatSel && (menuOpen || cheatPress == i);
		if (i == cheatSel && strcmp(name, c->desc)) cut = true;
		px_item(px, cheat_row_rect(row), name, c->on ? "ON" : "OFF", c->on, sel);
	}
	/* a scroll bar when the list is longer than the screen */
	if (cheats.n > CHEAT_ROWS) {
		int trackY = MENU_TOP, trackH = CHEAT_ROWS * MENU_PITCH - 2;
		int barH = SDL_max(8, trackH * CHEAT_ROWS / cheats.n);
		int barY = trackY + (trackH - barH) * cheatScroll / SDL_max(1, cheats.n - CHEAT_ROWS);
		int x = MENU_X + MENU_W - 3;
		px_fill(px, x, trackY, 3, trackH, PX_FRAME_DARK);
		px_fill(px, x, barY, 3, barH, PX_WHITE);
	}
	/* below: the selected cheat's whole name when its row had to cut it,
	 * else how many are on */
	char foot[160];
	if (cut && cheatSel < cheats.n) fit_text(cheats.items[cheatSel].desc, FRAME_X1 - FRAME_X0 - 16, foot, sizeof(foot));
	else snprintf(foot, sizeof(foot), "%d OF %d ON", cheats_on(&cheats), cheats.n);
	px_text(px, TF_BOLD, SNES_W / 2, menu_geom().noteY, foot, PX_ORANGE, 0, 0);
}

/* The time and the battery in the frame's bottom corners. */
static void compose_status(uint32_t* px) {
	char buf[16];
	int cy = FRAME_Y1 - 11;
	time_t now = time(NULL);
	struct tm tm;
	if (localtime_r(&now, &tm)) {
		snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
		px_text(px, TF_BOLD, FRAME_X0 + 8, cy, buf, PX_WHITE, 0, -1);
	}
	int pct = battery_percent();
	if (pct >= 0) {
		snprintf(buf, sizeof(buf), "%d%%", pct);
		px_text(px, TF_BOLD, FRAME_X1 - 7, cy, buf, PX_WHITE, 0, 1);
	}
}

static void compose_menu(uint32_t* px) {
	if (cheatsOpen) {
		px_pause_frame(px, "CHEATS");
		px_arrow(px, false); /* back */
		compose_cheats(px);
		compose_status(px);
		return;
	}
	px_pause_frame(px, pageTitles[curPage]);
	if (curPage > 0) px_arrow(px, false);
	if (curPage < N_PAGES - 1) px_arrow(px, true);
	MenuGeom g = menu_geom();
	for (int i = 0; i < nMenuItems; ++i) {
		bool sel = i == menuPressed || (menuOpen && i == menuSel);
		px_item(px, g.items[i], item_label(menuItems[i]), NULL, item_enabled(menuItems[i]), sel);
	}
	if (menuNote[0]) px_text(px, TF_BOLD, SNES_W / 2, g.noteY, menuNote, PX_ORANGE, 0, 0);
	compose_status(px);
}

/* ------------------------------------------------- touches on the MENU page */

static struct {
	bool down, swiping;
	int x0, y0, arrow;
} swipe = { .arrow = -1 };

static void set_page_by(int d) {
	int to = curPage + d;
	if (to < 0 || to >= N_PAGES) return;
	set_page(to);
	mapDirty = true;
}

/* A touch on the cheats list: tap a cheat to switch it, drag to scroll,
 * the left arrow or a swipe right to go back. */
static void cheats_pointer(int kind, int x, int y) {
	if (kind == 1) {
		cheatPress = -1;
		cheatDragged = false;
		cheatPressY = y;
		cheatPressScroll = cheatScroll;
		for (int row = 0; row < CHEAT_ROWS; ++row) {
			SDL_Rect r = cheat_row_rect(row);
			r.y -= 1, r.h += 2;
			if (cheatScroll + row < cheats.n && in_rect(r, x, y)) cheatPress = cheatScroll + row, cheatSel = cheatPress;
		}
	} else if (kind == 3) {
		if (abs(y - cheatPressY) > 6 && !swipe.swiping) cheatDragged = true;
		if (cheatDragged && cheats.n > CHEAT_ROWS) {
			cheatScroll = SDL_clamp(cheatPressScroll - (y - cheatPressY) / MENU_PITCH, 0, cheats.n - CHEAT_ROWS);
		}
		if (swipe.swiping) cheatPress = -1;
	} else if (kind == 2) {
		if (swipe.swiping) {
			if (x - swipe.x0 >= 24) cheatsOpen = false;
		} else if (swipe.arrow == 0 && page_arrow_at(x, y) == 0) {
			cheatsOpen = false;
		} else if (!cheatDragged && cheatPress >= 0) {
			SDL_Rect r = cheat_row_rect(cheatPress - cheatScroll);
			r.y -= 1, r.h += 2;
			if (in_rect(r, x, y)) toggle_cheat(cheatPress);
		}
		cheatPress = -1;
	}
	mapDirty = true;
}

static void cheats_buttons(void) {
	if (btn_pressed(B_DOWN) && cheatSel < cheats.n - 1) ++cheatSel;
	if (btn_pressed(B_UP) && cheatSel > 0) --cheatSel;
	if (btn_pressed(B_R)) cheatSel = SDL_min(cheats.n - 1, cheatSel + CHEAT_ROWS);
	if (btn_pressed(B_L)) cheatSel = SDL_max(0, cheatSel - CHEAT_ROWS);
	cheat_keep_visible();
	if (btn_pressed(B_A)) toggle_cheat(cheatSel);
	if (btn_pressed(B_B) || btn_pressed(B_LEFT)) cheatsOpen = false;
}

static bool activate(int item);

/* A touch on the MENU page (the game's pixels): tap an item, an arrow, or
 * swipe left / right for the next / previous page. True to quit. */
static bool menu_pointer(int kind, int x, int y) {
	bool quit = false;
	if (kind == 1) {
		swipe.down = true, swipe.swiping = false;
		swipe.x0 = x, swipe.y0 = y;
		swipe.arrow = page_arrow_at(x, y);
	} else if (kind == 3 && swipe.down && !swipe.swiping) {
		int dx = x - swipe.x0, dy = y - swipe.y0;
		if (abs(dx) > 10 && abs(dx) > abs(dy)) swipe.swiping = true, swipe.arrow = -1, menuPressed = -1;
	}
	if (cheatsOpen) {
		cheats_pointer(kind, x, y);
	} else {
		int it = item_at(x, y);
		if (kind == 1) {
			menuPressed = swipe.arrow >= 0 ? -1 : it;
			if (menuPressed >= 0) menuSel = menuPressed;
		} else if (kind == 3 && menuPressed >= 0 && it != menuPressed) {
			menuPressed = -1;
		} else if (kind == 2) {
			if (swipe.swiping) {
				int dx = x - swipe.x0;
				if (dx <= -24) set_page_by(1);
				else if (dx >= 24) set_page_by(-1);
			} else if (swipe.arrow >= 0 && page_arrow_at(x, y) == swipe.arrow) {
				set_page_by(swipe.arrow ? 1 : -1);
			} else if (menuPressed >= 0 && it == menuPressed) {
				quit = activate(menuItems[it]);
			}
			menuPressed = -1;
		}
		mapDirty = true;
	}
	if (kind == 2) swipe.down = swipe.swiping = false, swipe.arrow = -1;
	return quit;
}

/* ------------------------------------------------- the menu, by buttons
 * HOME (or SELECT + START) brings the MENU page up with the game paused and
 * the D-pad on the menu; B, START or HOME again go back to where it was.
 * The page can be used by touch too, the game running. */
static bool menuWasPaused;
static int menuReturnTab = MAPCAP_MAP;
static int menuLeave; /* set by an item: 1 back to the game, 2 the same at once (a screenshot) */

static void open_menu(void) {
	if (menuOpen) return;
	cheatsOpen = false;
	menuOpen = true;
	menuWasPaused = appPaused;
	appPaused = true;
	if (going_tab() != TAB_MENU) menuReturnTab = going_tab();
	set_page(0);
	switch_tab(TAB_MENU);
	buttonPressed = false;
	mapDirty = true;
}

/* back from the menu: as it was (B / HOME), or playing (an item that's done) */
static void close_menu(bool resume, bool atOnce) {
	menuOpen = false;
	cheatsOpen = false;
	appPaused = resume ? false : menuWasPaused;
	menuWasPaused = false;
	if (atOnce) {
		secTab = tabNext = menuReturnTab;
		fadePhase = FADE_NONE;
	} else {
		switch_tab(menuReturnTab);
	}
	suppress_held();
	mapDirty = true;
}

/* ------------------------------------------------- ARE YOU SURE?
 * EXIT asks first: YES closes Super Metroid, NO (the one selected at
 * first) goes back. The game waits meanwhile. */
static int exitSel, exitPressed = -1;
static bool exitWasPaused;
static const SDL_Rect exitBox = { 40, 78, 176, 66 };
static const SDL_Rect exitBtn[2] = { { 64, 116, 56, 18 }, { 136, 116, 56, 18 } }; /* NO, YES */

static void open_exit(void) {
	exitAsk = true;
	exitSel = 0;
	exitPressed = -1;
	exitWasPaused = appPaused;
	appPaused = true;
	mapDirty = true;
}

static void close_exit(void) {
	exitAsk = false;
	appPaused = exitWasPaused || menuOpen;
	suppress_held();
	mapDirty = true;
}

static void compose_exit(uint32_t* px) {
	if (!exitAsk) return;
	px_scale(px, 0, SNES_H, 1, 2);
	SDL_Rect b = exitBox;
	const uint32_t line[4] = { PX_FRAME_DARK, PX_FRAME_LIGHT, PX_WHITE, PX_FRAME_DARK };
	for (int k = 0; k < 4; ++k) px_fill(px, b.x + k, b.y + k, b.w - 2 * k, b.h - 2 * k, line[k]);
	px_fill(px, b.x + 4, b.y + 4, b.w - 8, b.h - 8, PX_BLACK);
	/* rounded: the outer corners cut */
	for (int c = 0; c < 4; ++c) {
		int x = c & 1 ? b.x + b.w - 1 : b.x, y = c & 2 ? b.y + b.h - 1 : b.y, dx = c & 1 ? -1 : 1, dy = c & 2 ? -1 : 1;
		px_set(px, x, y, PX_BLACK), px_set(px, x + dx, y, PX_BLACK), px_set(px, x, y + dy, PX_BLACK);
	}
	px_text(px, TF_BOLD, b.x + b.w / 2, b.y + 18, "ARE YOU SURE?", PX_WHITE, 0, 0);
	static const char* labels[2] = { "NO", "YES" };
	for (int i = 0; i < 2; ++i) {
		SDL_Rect r = exitBtn[i];
		bool on = i == exitSel, pressed = i == exitPressed;
		uint32_t edge = on ? PX_LAVENDER : PX_FRAME_DARK, fill = on ? PX_ORANGE : pressed ? PX_GREY_DOT : PX_GREY;
		px_fill(px, r.x + 1, r.y, r.w - 2, r.h, edge);
		px_fill(px, r.x, r.y + 1, r.w, r.h - 2, edge);
		px_fill(px, r.x + 2, r.y + 2, r.w - 4, r.h - 4, fill);
		int off = pressed ? 1 : 0;
		px_text(px, TF_BOLD, r.x + r.w / 2 + off, r.y + r.h / 2 + off, labels[i], on ? PX_BLACK : PX_GREY_TEXT, 0, 0);
	}
}

static int exit_btn_at(int x, int y) {
	for (int i = 0; i < 2; ++i) {
		SDL_Rect r = exitBtn[i];
		r.x -= 3, r.y -= 3, r.w += 6, r.h += 6;
		if (in_rect(r, x, y)) return i;
	}
	return -1;
}

/* ------------------------------------------------- settings that stick */

static void save_prefs(void) {
	char prefs[1100];
	snprintf(prefs, sizeof(prefs), "%s/prefs.cfg", cfg->stateDir);
	FILE* f = fopen(prefs, "w");
	if (!f) return;
	fprintf(f, "# written by the settings menu; overrides sm-ds.cfg\n");
	fprintf(f, "scale = %s\nmap_scale = %s\nshader = %s\nshow_fps = %d\nmap_animations = %d\nmap_interval = %d\nframe_buffer = %d\nmatch_display = %d\n",
	        cfg->scale, cfg->mapScale, cfg->shader == 1 ? "grid" : cfg->shader == 2 ? "scanlines" : "off", cfg->showFps ? 1 : 0, cfg->mapAnim ? 1 : 0,
	        cfg->mapInterval, cfg->frameBuffer ? 1 : 0, cfg->matchDisplay ? 1 : 0);
	fprintf(f, "rewind = %d\nfull_pause_menu = %d\n", cfg->rewind ? 1 : 0, cfg->fullPause ? 1 : 0);
	fclose(f);
}

static bool gameTexAll = true;

/* One screen: the game or the HUD and pages (L3, HOME + Y). With two the
 * screens never swap. */
static void toggle_single_view(void) {
	if (nScreens > 1) return;
	gameTexAll = true;
	mapDirty = true;
	singleShowsMap = !singleShowsMap;
}

/* ------------------------------------------------- pacing */

static double frameSec = 1.0 / SNES_FPS;
static double pacedHz;
static double audioSpeed = 1.0;

static void pacing_update(void) {
	double hz = nScreens > 0 ? ui_display_hz(screens[game_screen()].view) : 0;
	pacedHz = 0;
	if (cfg->matchDisplay && hz > 0 && fabs(hz / SNES_FPS - 1) <= 0.015) pacedHz = hz;
	frameSec = 1.0 / (pacedHz > 0 ? pacedHz : SNES_FPS);
	audioSpeed = pacedHz > 0 ? pacedHz / SNES_FPS : 1.0;
	fprintf(stderr, "frame pacing: display %.3f Hz, game %.3f frames a second (%s)\n", hz, 1.0 / frameSec,
	        pacedHz > 0 ? "matched to the display" : cfg->matchDisplay ? "display too far off to match" : "the SNES's rate");
}

/* ------------------------------------------------- states */

static bool state_save(const char* path) {
	return emu_state_save_file(&emu, path);
}

static bool state_load(const char* path) {
	bool ok = emu_state_load_file(&emu, path);
	if (ok) aheadReady = false, rewind_clear(rw), appPaused = false;
	return ok;
}

static void auto_save(void) {
	if (!cfg->autoResume) return;
	fprintf(stderr, state_save(gp.autoState) ? "auto-saved %s\n" : "auto-save to %s failed\n", gp.autoState);
}

static void on_background(void) {
	auto_save();
	emu_save_flush(&emu);
}

static void on_foreground(void) {
	if (!menuOpen) open_menu();
}

static bool shotPending;

static void take_screenshot(void) {
	char dir[600], path[1400];
	snprintf(dir, sizeof(dir), "%s", cfg->screenshotDir);
	mkdirs(dir);
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		snprintf(dir, sizeof(dir), "%s/screenshots", cfg->stateDir); /* (the folder asked for can't be made) */
		mkdirs(dir);
	}
	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
	snprintf(path, sizeof(path), "%s/super-metroid-%s.png", dir, stamp);
	for (int n = 2; file_exists(path) && n < 100; ++n) snprintf(path, sizeof(path), "%s/super-metroid-%s-%d.png", dir, stamp, n);
	ui_osd(ui_screenshot_stacked(path) ? "Screenshot saved" : "Screenshot failed");
}

/* true: leave the game */
static bool activate(int item) {
	if (item != M_RESTART) restartArmed = false;
	switch (item) {
	case M_RESUME:
		menuLeave = 1;
		break;
	case M_SHOT:
		menuLeave = 2;
		shotPending = true;
		break;
	case M_SCALE: {
		const char* next = !strcmp(cfg->scale, "integer") ? "fit" : !strcmp(cfg->scale, "fit") ? "zoom" :
		                   !strcmp(cfg->scale, "zoom")    ? "stretch" : "integer";
		snprintf(cfg->scale, sizeof(cfg->scale), "%s", next);
		snprintf(cfg->mapScale, sizeof(cfg->mapScale), "%s", next);
		save_prefs();
		break;
	}
	case M_MAPRATE: {
		int next = mapRates[0];
		for (int i = 0; i < N_MAP_RATES; ++i) {
			if (mapRates[i] < cfg->mapInterval) {
				next = mapRates[i];
				break;
			}
		}
		cfg->mapInterval = next;
		save_prefs();
		break;
	}
	case M_LCD:
		cfg->shader = (cfg->shader + 1) % 3;
		save_prefs();
		break;
	case M_SAVE:
		if (state_save(gp.state)) {
			ui_osd("State saved");
			menuLeave = 1;
		} else {
			snprintf(menuNote, sizeof(menuNote), "Save failed");
		}
		break;
	case M_LOAD:
		if (!item_enabled(M_LOAD)) {
			snprintf(menuNote, sizeof(menuNote), "No saved state yet");
		} else if (state_load(gp.state)) {
			ui_osd("State loaded");
			menuLeave = 1;
		} else {
			snprintf(menuNote, sizeof(menuNote), "Load failed");
		}
		break;
	case M_FPS:
		cfg->showFps = !cfg->showFps;
		save_prefs();
		break;
	case M_REWIND:
		cfg->rewind = !cfg->rewind;
		rewind_setup();
		if (cfg->rewind && !rw) snprintf(menuNote, sizeof(menuNote), "Not enough memory");
		else snprintf(menuNote, sizeof(menuNote), "%s", cfg->rewind ? "Hold L2 to go back in time" : "L2 does nothing");
		save_prefs();
		break;
	case M_FULLPAUSE:
		cfg->fullPause = !cfg->fullPause;
		snprintf(menuNote, sizeof(menuNote), "%s", cfg->fullPause ? "START opens the pause screen" : "START just pauses");
		save_prefs();
		break;
	case M_FRAMEBUF:
		cfg->frameBuffer = !cfg->frameBuffer;
		snprintf(menuNote, sizeof(menuNote), "%s", cfg->frameBuffer ? "Smoother, one frame more input delay" : "Least input delay");
		save_prefs();
		break;
	case M_MATCH:
		cfg->matchDisplay = !cfg->matchDisplay;
		pacing_update();
		if (!cfg->matchDisplay) snprintf(menuNote, sizeof(menuNote), "The SNES's own %.2f frames a second", SNES_FPS);
		else if (pacedHz > 0) snprintf(menuNote, sizeof(menuNote), "%.2f frames a second", pacedHz);
		else snprintf(menuNote, sizeof(menuNote), "The display's rate is too far off");
		save_prefs();
		break;
	case M_MAPANIM:
		cfg->mapAnim = !cfg->mapAnim;
		mapAnimStart = frameCount;
		mapShownIdx = haveMap ? 0 : -1;
		mapDirty = true;
		save_prefs();
		break;
	case M_RESTART:
		if (!restartArmed) {
			restartArmed = true;
			snprintf(menuNote, sizeof(menuNote), "Unsaved progress will be lost");
			break;
		}
		emu_reset(&emu);
		appPaused = false;
		aheadReady = false;
		rewind_clear(rw);
		haveMap = false;
		mapShownIdx = -1;
		mapDirty = true;
		restartArmed = false;
		menuLeave = 1;
		ui_osd("Game restarted");
		break;
	case M_CHEATS:
		if (!cheats.n) snprintf(menuNote, sizeof(menuNote), "No cheats: put .cht files in cheats/");
		else open_cheats();
		break;
	case M_EXIT:
		return true;
	}
	return false;
}

/* A map station used: once its MAP DATA ACCESS COMPLETED. box (message
 * $14) closes the game opens the pause screen on the map ($0998 = $0C).
 * With the map on the second screen that pause is run through unseen: a
 * few frames a tick, unpaused as soon as its map is up, the game screen
 * kept as it was, until play is back (the game gives Samus back from its
 * unpause: leaving out the pause hangs it). Meanwhile the second screen goes to MAP, captured anew. */
static bool mapSkip;
static int lastState = 8;

static void mapstation_update(void) {
	bool second = nScreens > 1 && mc && !cfg->fullPause;
	int st = emu_read8(&emu, 0x0998);
	if (second && !mapSkip && st == 0x0C && lastState == 0x08 && emu_read8(&emu, 0x1C1F) == 0x14) {
		if (debug) fprintf(stderr, "frame %llu: map station: its pause run unseen, the map on the second screen\n", (unsigned long long) frameCount);
		mapSkip = true;
		aheadReady = false;
		if (menuOpen) close_menu(false, true);
		switch_tab(MAPCAP_MAP);
		capForce[MAPCAP_MAP] = true;
		mapcap_reset_scroll(mc);
	}
	lastState = st;
}

/* Frames of that pause, up to n; false when it's over. */
static bool mapskip_run(int n, int16_t* scratch) {
	for (int i = 0; i < n; ++i) {
		int st = emu_read8(&emu, 0x0998);
		if (st == 0x08) {
			if (debug) fprintf(stderr, "frame %llu: map station: play is back\n", (unsigned long long) frameCount);
			return mapSkip = false;
		}
		if (st == 0x0F) {
			/* its map up: what START does there ($82:A5C2, less the sound
			 * and the button's palette): $0723 / $0725 = 1, $0729 = $0B, on
			 * to state $10 (unpausing) */
			wram_write16(0x0723, 1);
			wram_write16(0x0725, 1);
			wram_write16(0x0729, 0x0B);
			wram_write16(0x0998, 0x10);
		}
		game_frame(0);
		while (emu_audio_read(&emu, scratch, 4096) > 0) {} /* silent */
	}
	lastState = emu_read8(&emu, 0x0998);
	return true;
}

/* ------------------------------------------------- drawing */

static int small_text_px(int s, int gap) {
	return SDL_max(1, SDL_min(SDL_min(ui_px(s) - 1, 3), (gap - 6) / 7));
}

static void draw_osd(void) {
	const char* osd = ui_osd_text();
	if (!osd || menuOpen || shotPending) return;
	int s = ui_touch_screen();
	SDL_Rect r = screens[s].rect;
	int u = SDL_max(1, r.h / SNES_H);
	int cap = tf_cap(SDL_max(11, u * 7));
	int y = r.h - cap - u * 4;
	ui_font_text(s, TF_BOLD, u * 6 + u, y + u, cap, osd, SM_BLACK, -1);
	ui_font_text(s, TF_BOLD, u * 6, y, cap, osd, SM_YELLOW, -1);
}

static void draw_secondary(int s) {
	SDL_Rect r = screens[s].rect;
	ui_fill(s, (SDL_Rect) { 0, 0, r.w, r.h }, SM_BLACK);
	ui_image_pixels(s, secTex, SNES_W, SNES_H, cfg->mapScale, shader_lcd());
	draw_exit_button(s);
}

static void draw_game(void) {
	bool two = nScreens > 1;
	for (int s = 0; s < nScreens; ++s) {
		bool showGame = nScreens == 1 ? !(singleShowsMap || menuOpen || exitAsk) : (s == game_screen() || !two);
		if (!showGame) {
			draw_secondary(s);
			continue;
		}
		if (nScreens == 2 && !two && s != game_screen()) continue;
		ui_image_pixels(s, gameTex, SNES_W, SNES_H, cfg->scale, shader_lcd());
		if (appPaused) {
			/* START: the game stopped, dimmed */
			SDL_Rect img = ui_image_rect(s, SNES_W, SNES_H, cfg->scale);
			ui_fill(s, img, (SDL_Color) { 0, 0, 0, 150 });
			int u = SDL_max(1, img.w / SNES_W);
			draw_paused_label(s, img.x + img.w / 2, img.y + img.h / 2, u);
		}
		if (cfg->showFps) {
			char buf[48];
			snprintf(buf, sizeof(buf), "%.1f FPS  %d%%", fps, (int) (speedPct + 0.5));
			SDL_Rect img = ui_image_rect(s, SNES_W, SNES_H, cfg->scale);
			int fpx = small_text_px(s, img.y);
			int fy = img.y >= 7 * fpx + 2 ? (img.y - 7 * fpx) / 2 : fpx * 2;
			ui_text(s, img.x + fpx * 2 + 1, fy + 1, fpx, buf, SM_BLACK);
			ui_text(s, img.x + fpx * 2, fy, fpx, buf, SM_WHITE);
		}
	}
	draw_osd();
}

/* ------------------------------------------------- the session */

enum GameExit game_run(Config* c, const char* rom) {
	cfg = c;
	debug = getenv("SMDS_DEBUG") != NULL;
	game_paths(cfg, rom, &gp);
	if (!emu_open(&emu, EMU_SLOT_GAME, rom, gp.sav, true)) {
		ui_osd("Couldn't start the game");
		return GAME_QUIT_APP;
	}
	size_t romSize;
	uint8_t* romData = emu_rom(&emu, &romSize);
	int patched = sm_patch_hud(romData, romSize);
	/* the HUD's lines of the game's HDMA tables (scroll, BG mode...) take the room's values */
	if (patched) emu_set_hud_hdma(&emu, true);
	hudPatched = patched > 0;
	fprintf(stderr, "game %s, save %s, HUD patches %d of %d\n", gp.base, gp.sav, patched, sm_patch_count());
	ui_on_background(on_background);
	ui_on_foreground(on_foreground);

	/* equipment switched on the second screen's SAMUS page (the map copy
	 * gets it too, for a switch made just before a capture) */
	if (cfg->map && !sm_patch_equip(romData, romSize)) fprintf(stderr, "equipment switching from the SAMUS page is off\n");
	mc = cfg->map ? mapcap_create(romData, romSize) : NULL;
	memset(&mapRes, 0, sizeof(mapRes));
	memset(&samRes, 0, sizeof(samRes));
	haveMap = haveSam = false;
	mapShownIdx = samShownIdx = -1;
	secTab = tabNext = MAPCAP_MAP;
	fadePhase = FADE_NONE;
	capForce[0] = capForce[1] = false;
	tabPressed = eqPressed = -1;
	tabsOn = false;
	dimLevel = DIM_STEPS;
	appPaused = false;
	frameCount = 0;
	menuOpen = exitAsk = false;
	menuLeave = 0;
	gameTexAll = true;
	for (int i = 0; i < nViews; ++i) {
		gameTex[i] = SDL_CreateTexture(views[i].ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, SNES_W, SNES_H);
		secTex[i] = SDL_CreateTexture(views[i].ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, SNES_W, SNES_H);
		SDL_SetTextureBlendMode(gameTex[i], SDL_BLENDMODE_NONE);
		SDL_SetTextureBlendMode(secTex[i], SDL_BLENDMODE_NONE);
	}
	compose_secondary(false);
	for (int i = 0; i < nViews; ++i) ui_update_texture(secTex[i], secPx, SNES_W * 4);

	memset(pageLen, 0, sizeof(pageLen));
	pages[0][pageLen[0]++] = M_RESUME;
	pages[0][pageLen[0]++] = M_SAVE;
	pages[0][pageLen[0]++] = M_LOAD;
	pages[0][pageLen[0]++] = M_REWIND;
	pages[0][pageLen[0]++] = M_CHEATS;
	pages[0][pageLen[0]++] = M_FULLPAUSE;
	pages[0][pageLen[0]++] = M_RESTART;
	pages[1][pageLen[1]++] = M_SHOT;
	pages[1][pageLen[1]++] = M_SCALE;
	pages[1][pageLen[1]++] = M_LCD;
	pages[1][pageLen[1]++] = M_FPS;
	set_page(0);

	if (cfg->loadState[0]) {
		if (!state_load(cfg->loadState)) fprintf(stderr, "could not load %s\n", cfg->loadState);
		cfg->loadState[0] = 0;
	} else if (cfg->autoResume && file_exists(gp.autoState)) {
		if (state_load(gp.autoState)) ui_osd("Resumed");
	}

	rewind_setup();
	{
		char prefs[1400];
		snprintf(prefs, sizeof(prefs), "%s/%s.cheats", cfg->stateDir, gp.base);
		cheats_load(&cheats, cfg->cheatDir, prefs);
		if (cheats_on(&cheats)) cheats_apply(&cheats, &emu);
	}

	SDL_AudioSpec want = { 0 }, have;
	want.freq = 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	/* the sound card's block: 1024 frames (21 ms) leaves the sound thread room
	 * when every core is busy (512 popped now and then on the RG DS) */
	want.samples = getenv("SMDS_AUDIO_SAMPLES") ? atoi(getenv("SMDS_AUDIO_SAMPLES")) : 1024;
	SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio) SDL_PauseAudioDevice(audio, 0), fprintf(stderr, "sound: %d Hz, blocks of %d frames, queue %d ms\n", have.freq, have.samples, cfg->audioLatency);
	else fprintf(stderr, "no audio: %s\n", SDL_GetError());
	double rsPos = 0, rsAdj = 0; /* resampler: position in the input, ratio adjustment */
	int16_t rsLast[2] = { 0, 0 };
	const int targetQueued = want.freq * cfg->audioLatency / 1000;

	static int16_t samples[4096 * 2];
	uint32_t lastRoomKey = 0;
	int sinceMapRequest = 1 << 20;
	int lastFails = 0;
	bool forceMap = true;
	bool hotkeyBack = !strcmp(cfg->hotkey, "back");
	int modBtn = hotkeyBack ? B_SELECT : B_HOME;
	bool comboUsed = false;
	int comboHold = -1, comboFrames = 0;
	int comboPulse[2] = { 0, 0 };
	int selectPulse = 0;
	unsigned ffPhase = 0;
	bool noPace = getenv("SMDS_NOPACE") != NULL;
	enum GameExit exitTo = GAME_QUIT_APP;
	bool done = false;

	pacing_update();
	const double freq = (double) SDL_GetPerformanceFrequency();
	double next = SDL_GetPerformanceCounter() / freq;
	double fpsT0 = next;
	int fpsFrames = 0;
	/* timing report in the log every 10 s of play: what keeps a frame from
	 * being ready in time (sound crackles when its queue runs dry) */
	struct {
		double t0, last, emuMax, emuSum, drawMax, drawSum, gapMax;
		int frames, late, dry, qMin, mapRuns, refills;
		long made, queued;
	} perf = { 0 };
	perf.qMin = 1 << 30;
#ifdef __ANDROID__
	android_perf_init((int64_t) (frameSec * 0.5 * 1e9));
	android_set_pinning(cfg->pinCores);
	android_pin_game_thread();
#endif
	while (!done) {
		ahead_sync();
		hud_inject_done();
		if (!ui_events()) break;
		ui_tick();
		ui_read_buttons();

		/* ---- touch: the settings button opens the menu; in the menu, taps press items */
		Pointer p;
		while (ui_next_pointer(&p)) {
			bool secShown = nScreens > 1 || singleShowsMap || menuOpen || exitAsk;
			if (nScreens == 1 && !secShown) {
				/* one screen showing the game: any tap opens the menu */
				if (p.kind == 1) open_menu();
				continue;
			}
			if (nScreens > 1 && p.screen != map_screen()) continue;
			int sx, sy;
			{
				SDL_Rect img = ui_image_rect(p.screen, SNES_W, SNES_H, cfg->mapScale);
				sx = img.w > 0 ? (int) floor((p.x - img.x) * (double) SNES_W / img.w) : -1;
				sy = img.h > 0 ? (int) floor((p.y - img.y) * (double) SNES_H / img.h) : -1;
			}
			/* ARE YOU SURE? takes every touch while it's up */
			if (exitAsk) {
				int b = exit_btn_at(sx, sy);
				if (p.kind == 1) exitPressed = b;
				else if (p.kind == 3 && b != exitPressed) exitPressed = -1;
				else if (p.kind == 2) {
					if (exitPressed >= 0 && b == exitPressed) {
						if (b == 1) exitTo = GAME_QUIT_APP, done = true;
						else close_exit();
					}
					exitPressed = -1;
				}
				mapDirty = true;
				continue;
			}
			if (p.kind == 1 && sm_hud_shown(&emu) && !appPaused) {
				int icon = hud_icon_at(p.screen, p.x, p.y);
				if (icon) hudTap = icon;
			}
			bool pageUp = fadePhase == FADE_NONE;
			/* the MAP, SAMUS and MENU buttons (MAP and SAMUS while the game could pause) */
			{
				int t = tab_hit(sx, sy);
				if (t >= 0 && t != TAB_MENU && !tabsOn) t = -1;
				if (p.kind == 1) tabPressed = t;
				else if (p.kind == 3 && t != tabPressed) tabPressed = -1;
				else if (p.kind == 2) {
					if (tabPressed >= 0 && t == tabPressed) {
						/* MENU as HOME: brings the menu up with the game paused, and again
						 * goes back to the page (and the play) it was in */
						if (t == TAB_MENU) {
							if (menuOpen) close_menu(false, false);
							else open_menu();
						} else {
							if (menuOpen) close_menu(false, false);
							switch_tab(t);
						}
					}
					tabPressed = -1;
				}
				mapDirty = true;
			}
			/* the MENU page */
			if (secTab == TAB_MENU && pageUp) {
				if (menu_pointer(p.kind, sx, sy)) done = true;
			}
			/* the SAMUS page: a tap on an item (or MODE) switches it */
			if (pageUp && tabsOn && secTab == MAPCAP_SAMUS && haveSam) {
				int it = sm_equip_at(sx, sy);
				if (p.kind == 1) eqPressed = it;
				else if (p.kind == 2) {
					if (eqPressed >= 0 && it == eqPressed && sm_equip_toggle(emu_wram(&emu), it)) {
						sam_recolor();
						capForce[MAPCAP_SAMUS] = true;
						if (debug) fprintf(stderr, "equipment: switched item %d\n", it);
					}
					eqPressed = -1;
				}
			}
			/* the map's arrows: a tap near an edge of the map scrolls it
			 * that way (a step, then more while held) */
			if (p.kind == 1 || p.kind == 3) {
				int d = haveMap && mc && pageUp && tabsOn && secTab == MAPCAP_MAP ? map_arrow_at(p.screen, p.x, p.y) : -1;
				if (p.kind == 1 && d >= 0) map_scroll_step(d), arrowHeld = d, arrowFrames = 0;
				else if (p.kind == 3 && d != arrowHeld) arrowHeld = -1;
			} else if (p.kind == 2) {
				arrowHeld = -1;
			}
			/* EXIT */
			bool inside = in_rect(exit_button_rect(p.screen), p.x, p.y);
			if (p.kind == 1) buttonPressed = inside;
			else if (p.kind == 3) buttonPressed = buttonPressed && inside;
			else if (p.kind == 2) {
				if (buttonPressed && inside) open_exit();
				buttonPressed = false;
			}
		}
		if (done) break;

		if (arrowHeld >= 0 && (!mc || secTab != MAPCAP_MAP || fadePhase != FADE_NONE || !tabsOn || exitAsk)) arrowHeld = -1;
		if (arrowHeld >= 0 && ++arrowFrames >= 18 && arrowFrames % 6 == 0) map_scroll_step(arrowHeld);

		/* ---- the menu combo (SELECT + START together), when switched on */
		bool comboOpened = false;
		if (!menuOpen && !strcmp(cfg->menuCombo, "select+start")) {
			if (comboHold < 0) {
				if (btn_pressed(B_SELECT) && btn_pressed(B_START)) comboHold = B_START, comboFrames = 0;
				else if (btn_pressed(B_SELECT) && !btnNow[B_START]) comboHold = B_SELECT, comboFrames = 0;
				else if (btn_pressed(B_START) && !btnNow[B_SELECT]) comboHold = B_START, comboFrames = 0;
			}
			if (comboHold >= 0) {
				int other = comboHold == B_SELECT ? B_START : B_SELECT;
				if (btnNow[comboHold] && btnNow[other]) {
					open_menu();
					comboOpened = comboUsed = true;
					comboHold = -1;
				} else if (!btnNow[comboHold]) {
					comboPulse[comboHold == B_START] = 4;
					comboHold = -1;
				} else if (++comboFrames > 5) {
					comboHold = -1;
				}
			}
		} else {
			comboHold = -1;
		}

		/* ---- HOME alone toggles the menu; with a button it's a hotkey */
		if (btnNow[modBtn] && !menuOpen) {
			if (btn_pressed(B_START)) exitTo = GAME_QUIT_APP, done = true, comboUsed = true;
			if (btn_pressed(B_R)) ui_osd(state_save(gp.state) ? "State saved" : "Save failed"), comboUsed = true;
			if (btn_pressed(B_L) && file_exists(gp.state)) state_load(gp.state), ui_osd("State loaded"), forceMap = true, comboUsed = true;
			if (btn_pressed(B_X)) cfg->showFps = !cfg->showFps, comboUsed = true;
			if (btn_pressed(B_Y) && nScreens < 2) toggle_single_view(), comboUsed = true;
		}
		if (btn_released(B_HOME) && !(modBtn == B_HOME && comboUsed)) {
			if (exitAsk) close_exit();
			else if (menuOpen) close_menu(false, false);
			else open_menu();
		}
		if (btn_released(modBtn)) {
			if (comboUsed) suppress_held();
			if (hotkeyBack && !comboUsed && !menuOpen) selectPulse = 4;
			comboUsed = false;
		}
		if (done) break;

		if (exitAsk) {
			/* ARE YOU SURE?: left / right, A; B or START is NO */
			if (btn_pressed(B_LEFT) || btn_pressed(B_RIGHT)) exitSel = btn_pressed(B_RIGHT) ? 1 : 0, mapDirty = true;
			if (btn_pressed(B_B) || btn_pressed(B_START)) close_exit();
			else if (btn_pressed(B_A)) {
				if (exitSel) {
					exitTo = GAME_QUIT_APP;
					break;
				}
				close_exit();
			}
		} else if (menuOpen && !comboOpened && cheatsOpen) {
			cheats_buttons();
			mapDirty = true;
			if (btn_pressed(B_START)) close_menu(false, false);
		} else if (menuOpen && !comboOpened) {
			if (btn_pressed(B_L) && curPage > 0) set_page(curPage - 1), mapDirty = true;
			if (btn_pressed(B_R) && curPage < N_PAGES - 1) set_page(curPage + 1), mapDirty = true;
			if (btn_pressed(B_DOWN) || btn_pressed(B_UP) || btn_pressed(B_LEFT) || btn_pressed(B_RIGHT)) {
				/* rows up and down (the same side of a row of two when it can),
				 * left and right within a row (and to the next page past it) */
				MenuGeom g = menu_geom();
				int r = g.row[menuSel], c = g.col[menuSel];
				if (btn_pressed(B_LEFT) || btn_pressed(B_RIGHT)) {
					int want = btn_pressed(B_RIGHT) ? 1 : 0, found = -1;
					for (int i = 0; i < nMenuItems; ++i) {
						if (g.row[i] == r && g.col[i] == want) found = i;
					}
					if (found >= 0 && found != menuSel) menuSel = found;
					else set_page_by(want ? 1 : -1);
				} else {
					int to = (r + (btn_pressed(B_DOWN) ? 1 : g.nRows - 1)) % g.nRows, pick = -1;
					for (int i = 0; i < nMenuItems; ++i) {
						if (g.row[i] == to && (pick < 0 || g.col[i] == c)) pick = i;
					}
					if (pick >= 0) menuSel = pick;
				}
				mapDirty = true;
			}
			if (btn_pressed(B_B) || btn_pressed(B_START)) close_menu(false, false);
			else if (btn_pressed(B_A) && activate(menuItems[menuSel])) break;
		}
		if (menuLeave) {
			/* an item that's done (Resume, a state saved...): back to the game */
			close_menu(true, menuLeave == 2);
			menuLeave = 0;
			forceMap = true;
		}


		if (btn_pressed(B_L3)) toggle_single_view();

		/* ---- emulate */
		uint32_t keys = 0;
		for (int b = 0; b < B_COUNT; ++b) {
			if (!btnNow[b]) suppress[b] = false;
		}
		if (!btnNow[modBtn] || hotkeyBack) {
			if (HELD(B_A)) keys |= K_A;
			if (HELD(B_B)) keys |= K_B;
			if (HELD(B_X)) keys |= K_X;
			if (HELD(B_Y)) keys |= K_Y;
			if (HELD(B_START) && !(hotkeyBack && btnNow[B_SELECT]) && comboHold != B_START) keys |= K_START;
			if (HELD(B_SELECT) && !hotkeyBack && comboHold != B_SELECT) keys |= K_SELECT;
			if (HELD(B_L)) keys |= K_L;
			if (HELD(B_R)) keys |= K_R;
			if (HELD(B_UP)) keys |= K_UP;
			if (HELD(B_DOWN)) keys |= K_DOWN;
			if (HELD(B_LEFT)) keys |= K_LEFT;
			if (HELD(B_RIGHT)) keys |= K_RIGHT;
			if (hotkeyBack && btnNow[B_SELECT]) keys &= ~(K_L | K_R | K_START);
		}
		if (selectPulse > 0) keys |= K_SELECT, --selectPulse;
		if (comboPulse[0] > 0) keys |= K_SELECT, --comboPulse[0];
		if (comboPulse[1] > 0) keys |= K_START, --comboPulse[1];

		/* START in play, with two screens: the game stops (dimmed) instead
		 * of opening its pause screen, whose pages are on the second screen;
		 * START again goes on */
		{
			static uint32_t lastKeys;
			bool startNew = (keys & K_START) && !(lastKeys & K_START);
			lastKeys = keys;
			if (appPaused) {
				bool start = (btn_pressed(B_START) && !suppress[B_START]) || (startNew && !btnNow[B_START]);
				if (start && !menuOpen && !exitAsk) appPaused = false, suppress[B_START] = btnNow[B_START];
				keys = 0;
				lastKeys = 0;
			} else if (startNew && nScreens > 1 && mc && !cfg->fullPause && sm_pause_available(&emu)) {
				appPaused = true;
				suppress[B_START] = btnNow[B_START];
				keys = 0;
			}
			static bool wasPaused;
			if (appPaused != wasPaused && audio) {
				if (appPaused) {
					SDL_PauseAudioDevice(audio, 1);
					SDL_ClearQueuedAudio(audio);
				} else {
					SDL_PauseAudioDevice(audio, 0);
					next = SDL_GetPerformanceCounter() / freq;
					perf.last = 0;
				}
			}
			if (appPaused != wasPaused) mapDirty = true;
			wasPaused = appPaused;
		}

		/* L2 held: back in time, a stored state a frame (each shown once,
		 * so twice as fast as the game went); at the oldest it holds there */
		rewinding = !appPaused && rw && HELD(B_L2) && !(btnNow[modBtn] && !hotkeyBack);
		bool ff = btnNow[B_R2] && !rewinding && !appPaused;
		int runs = appPaused ? 0 : ff ? SDL_max(1, cfg->ffSpeed) : 1;
		int queuedNow = audio ? (int) (SDL_GetQueuedAudioSize(audio) / 4) : 0;
		bool dropAudio = queuedNow > targetQueued * 3;
		double tEmu0 = SDL_GetPerformanceCounter() / freq;
		if (audio) {
			int q = (int) (SDL_GetQueuedAudioSize(audio) / 4);
			if (q < perf.qMin) perf.qMin = q;
			if (q < (int) have.samples) ++perf.dry;
		}
		if (rewinding) {
			static bool atOldest;
			bool back = rewind_pop(rw, rwState);
			if (back || rewind_current(rw, rwState)) emu_state_load(&emu, rwState);
			if (!back && !atOldest) ui_osd("Rewind: as far back as it goes");
			else if (back) ui_osd("Rewinding");
			atOldest = !back;
			aheadReady = false;
			game_frame(0);
			while (emu_audio_read(&emu, samples, 4096) > 0) {} /* silent */
			rwSince = 0;
			runs = 0;
		}
		if (mapSkip) {
			mapskip_run(12, samples);
			runs = 0;
			rewinding = false;
		}
		for (int i = 0; i < runs; ++i) {
			if (i == 0 && aheadReady) aheadReady = false;
			else {
				uint32_t k = keys;
				if (i == 0) hud_inject(&k);
				game_frame(k);
				hud_inject_done();
			}
			size_t got;
			while ((got = emu_audio_read(&emu, samples, 4096)) > 0) {
				if (!audio || dropAudio) continue;
				if (runs > 1) {
					size_t outN = 0;
					for (size_t f = 0; f < got; ++f) {
						if (ffPhase++ % runs == 0) {
							samples[outN * 2] = samples[f * 2];
							samples[outN * 2 + 1] = samples[f * 2 + 1];
							++outN;
						}
					}
					got = outN;
				}
				if (!got) continue;
				/* The queue is kept near targetQueued: the game runs at the
				 * display's pace, not the sound card's, so the two drift apart.
				 * The resampling ratio follows the queue (at most 0.5% off, a
				 * pitch change nobody hears, and smoothly: dropping or repeating
				 * samples instead made a loud steady sound crackle). Close to
				 * empty (at the start, after the menu, after a stall) it's
				 * topped up with silence once. */
				int q = (int) (SDL_GetQueuedAudioSize(audio) / 4);
				if (q < targetQueued / 4) {
					static const int16_t silence[2048 * 2];
					for (int left = targetQueued - q; left > 0; left -= 2048)
						SDL_QueueAudio(audio, silence, (Uint32) SDL_min(left, 2048) * 4);
					q = targetQueued;
					++perf.refills;
				}
				double want_ = SDL_clamp((double) (targetQueued - q) / targetQueued * 0.01, -0.005, 0.005);
				rsAdj += (want_ - rsAdj) * 0.05;
				double step = emu_audio_rate(&emu) * audioSpeed / (want.freq * (1.0 + rsAdj));
				static int16_t conv[8192 * 2];
				int outN = 0;
				for (;;) {
					int i = (int) rsPos; /* x[0] = the last sample before, x[k] = samples[k - 1] */
					if (i + 1 > (int) got || outN >= 8192) break;
					double f = rsPos - i;
					for (int ch = 0; ch < 2; ++ch) {
						double s0 = i == 0 ? rsLast[ch] : samples[(i - 1) * 2 + ch];
						double s1 = samples[i * 2 + ch];
						conv[outN * 2 + ch] = (int16_t) lrint(s0 + (s1 - s0) * f);
					}
					++outN;
					rsPos += step;
				}
				rsPos -= (double) got;
				if (rsPos < 0) rsPos = 0;
				rsLast[0] = samples[(got - 1) * 2], rsLast[1] = samples[(got - 1) * 2 + 1];
				perf.made += (long) got;
				if (outN) SDL_QueueAudio(audio, conv, (Uint32) outN * 4);
				perf.queued += outN;
			}
		}
		if (rw && !rewinding && !appPaused && ++rwSince >= SDL_max(1, cfg->rewindStep)) {
			if (emu_state_save(&emu, rwState)) rewind_push(rw, rwState);
			rwSince = 0;
		}
		{
			double d = SDL_GetPerformanceCounter() / freq - tEmu0;
			perf.emuSum += d;
			if (d > perf.emuMax) perf.emuMax = d;
			if (debug && d > 0.003) fprintf(stderr, "frame %llu: emulation %.1f ms (game state %02x)\n", (unsigned long long) frameCount, d * 1e3, emu_read8(&emu, 0x0998));
		}
		++frameCount;
		if (frameCount % 120 == 0) emu_save_flush(&emu); /* the game saved at a station: on disk soon after */

		/* ---- the map */
		if (sm_no_map_here(&emu)) {
			if (haveMap && debug) fprintf(stderr, "frame %llu: no map here (game state %02x)\n", (unsigned long long) frameCount, emu_read8(&emu, 0x0998));
			if (haveMap || haveSam) mapDirty = true;
			haveMap = haveSam = false;
			forceMap = true;
		}
		if (mc) {
			++sinceMapRequest;
			uint32_t rk = sm_room_key(&emu);
			bool roomChanged = rk != lastRoomKey;
			const char* why = NULL;
			int fails = mapcap_failures(mc, &why);
			if (fails != lastFails) {
				if (debug && fails) fprintf(stderr, "frame %llu: map capture failed (%d in a row): %s\n", (unsigned long long) frameCount, fails, why ? why : "?");
				lastFails = fails;
			}
			/* the page shown (or being switched to) is the one kept fresh */
			int want = fadePhase != FADE_NONE ? tabNext : secTab;
			int interval = cfg->mapInterval << SDL_min(fails, 3);
			bool forced = want != TAB_MENU && capForce[want];
			bool urgent = forced || (want == MAPCAP_MAP && (forceMap || roomChanged));
			bool due = (!appPaused && sinceMapRequest >= interval) || (urgent && (!fails || sinceMapRequest >= cfg->mapInterval || forced));
			if (want == TAB_MENU) due = false; /* nothing to capture */
			if (due && want == MAPCAP_MAP && roomChanged && sm_can_pause(&emu) && !mapcap_busy(mc)) mapcap_reset_scroll(mc);
			if (due && sm_can_pause(&emu) && !mapcap_busy(mc) && mapcap_request(mc, &emu, want)) {
				sinceMapRequest = 0;
				capForce[want] = false;
				if (want == MAPCAP_MAP) lastRoomKey = rk, forceMap = false;
			}
			bool hadMap = haveMap;
			if (mapcap_poll(mc, MAPCAP_MAP, mapRes.serial, &mapRes)) {
				map_steady_arrows(&mapRes);
				if (hadMap && !mapRes.scrolled && secTab == MAPCAP_MAP && fadePhase == FADE_NONE && !sm_no_map_here(&emu)) {
					/* crossfade from what's on the screen */
					memcpy(xfOld, secPx + SNES_W * MAP_Y0, sizeof(xfOld));
					xfTick = 0;
				}
				if (!sm_no_map_here(&emu)) {
					haveMap = true;
					mapAnimStart = frameCount;
					mapShownIdx = 0;
					mapDirty = true;
					if (debug) fprintf(stderr, "frame %llu: map updated (%d frames)\n", (unsigned long long) frameCount, mapRes.nFrames);
				}
			}
			if (mapcap_poll(mc, MAPCAP_SAMUS, samRes.serial, &samRes)) {
				if (!sm_no_map_here(&emu)) {
					haveSam = true;
					samAnimStart = frameCount;
					samShownIdx = 0;
					mapDirty = true;
					/* switched since that capture began: shown as it is now */
					sam_recolor();
					if (debug) fprintf(stderr, "frame %llu: equipment screen updated (%d frames)\n", (unsigned long long) frameCount, samRes.nFrames);
				}
			}
			/* the blinking markers, the equipment screen's glow */
			if (haveMap && mapRes.nFrames > 1 && cfg->mapAnim) {
				int idx = (int) ((frameCount - mapAnimStart) / SDL_max(1, mapRes.frameStep)) % mapRes.nFrames;
				if (idx != mapShownIdx) mapShownIdx = idx, mapDirty |= secTab == MAPCAP_MAP;
			} else if (haveMap && mapShownIdx != 0) {
				mapShownIdx = 0;
				mapDirty = true;
			}
			if (haveSam && samRes.nFrames > 1 && cfg->mapAnim) {
				int idx = (int) ((frameCount - samAnimStart) / SDL_max(1, samRes.frameStep)) % samRes.nFrames;
				if (idx != samShownIdx) samShownIdx = idx, mapDirty |= secTab == MAPCAP_SAMUS;
			} else if (haveSam && samShownIdx != 0) {
				samShownIdx = 0;
				mapDirty = true;
			}

		}

		escape_update();
		mapstation_update();
		/* ---- the pages: their buttons, switching between them */
		/* the buttons: usable while the game could pause (dimmed when not) */
		bool avail = mc && sm_pause_available(&emu);
		if (avail != tabsOn) tabsOn = avail, mapDirty = true;
		if (!tabsOn) {
			eqPressed = -1;
			if (tabPressed != TAB_MENU) tabPressed = -1; /* MENU is always there */
		}
		int dimTo = tabsOn || going_tab() == TAB_MENU ? 0 : DIM_STEPS;
		if (dimLevel != dimTo) dimLevel += dimLevel < dimTo ? 1 : -1, mapDirty = true;
		/* switching pages: fade out, the new page once captured, fade in */
		if (fadePhase == FADE_OUT && ++fadeTicks >= FADE_STEPS) {
			fadePhase = FADE_WAIT, fadeTicks = 0;
		} else if (fadePhase == FADE_WAIT) {
			uint32_t ser = tabNext == MAPCAP_SAMUS ? samRes.serial : mapRes.serial;
			bool ready = tabNext == TAB_MENU || !mc || (ser != fadeSerial && !capForce[tabNext] && !mapcap_busy(mc));
			if (ready || ++fadeTicks >= FADE_WAIT_MAX) {
				secTab = tabNext;
				fadePhase = FADE_IN, fadeTicks = 0;
			}
		} else if (fadePhase == FADE_IN && ++fadeTicks >= FADE_STEPS) {
			fadePhase = FADE_NONE;
			mapDirty = true; /* the page at full brightness (and what only shows then) */
		}
		if (fadePhase != FADE_NONE) mapDirty = true;
		if (secTab == TAB_MENU && frameCount % 60 == 0) mapDirty = true; /* the clock */
		if (xfTick >= 0) {
			if (++xfTick >= XFADE_FRAMES || secTab != MAPCAP_MAP || fadePhase != FADE_NONE) xfTick = -1;
			mapDirty = true;
		}

		/* ---- the second screen: the HUD changes every frame it's up */
		bool hud = sm_hud_shown(&emu);
		static bool hadHud;
		if ((hud || hadHud || mapDirty) && !mapSkip) {
			compose_secondary(hud);
			int sv = screens[map_screen()].view;
			for (int i = 0; i < nViews; ++i) {
				if (nScreens == 1 || i == sv) ui_update_texture(secTex[i], secPx, SNES_W * 4);
			}
			mapDirty = false;
		}
		hadHud = hud;

		/* ---- draw */
		double tDraw0 = SDL_GetPerformanceCounter() / freq;
		const uint32_t* shownFb = emu.fb;
		if (cfg->frameBuffer && !ff && !rewinding && !appPaused && !mapSkip) {
			memcpy(aheadFb, emu.fb, sizeof(aheadFb));
			uint32_t k = keys;
			hud_inject(&k); /* done after the frame, at the next ahead_sync */
			if (ahead_start(k)) aheadInFlight = true, shownFb = aheadFb;
			else hud_inject_done();
		}
		for (int i = 0; i < nViews; ++i) {
			if (mapSkip) continue; /* the map station's pause: the picture before it stays */
			if (gameTexAll || i == screens[game_screen()].view) ui_update_texture(gameTex[i], shownFb, SNES_W * 4);
		}
		gameTexAll = false;
		ui_sync_view(cfg->vsync || pacedHz > 0 ? screens[game_screen()].view : -1);
		ui_clear();
		draw_game();
		if (shotPending) {
			shotPending = false;
			take_screenshot();
		}
		ui_present();

		/* ---- pacing: real time, nudged by the sound queue */
		double now = SDL_GetPerformanceCounter() / freq;
		{
			double d = now - tDraw0;
			perf.drawSum += d;
			if (d > perf.drawMax) perf.drawMax = d;
			if (perf.last > 0) {
				double gap = now - perf.last;
				if (gap > perf.gapMax) perf.gapMax = gap;
				if (gap > frameSec * 1.5) ++perf.late;
			}
			perf.last = now;
			if (mc && mapcap_busy(mc)) ++perf.mapRuns;
			if (!perf.t0) perf.t0 = now;
			if (++perf.frames && now - perf.t0 >= 10.0) {
				fprintf(stderr, "timing: %d frames in %.1f s, emulation %.1f ms (max %.1f), drawing %.1f ms (max %.1f), "
				        "longest frame %.1f ms, %d late; sound queue min %.0f ms, ran dry %d times, topped up %d times (made %ld, queued %ld at rate %u); map copy busy %d%% of frames; rewind %.0f s kept in %.1f MB\n",
				        perf.frames, now - perf.t0, perf.emuSum / perf.frames * 1e3, perf.emuMax * 1e3,
				        perf.drawSum / perf.frames * 1e3, perf.drawMax * 1e3, perf.gapMax * 1e3, perf.late,
				        audio ? perf.qMin * 1000.0 / have.freq : 0.0, perf.dry, perf.refills, perf.made, perf.queued, emu_audio_rate(&emu), perf.mapRuns * 100 / perf.frames,
				        rewind_count(rw) * SDL_max(1, cfg->rewindStep) / SNES_FPS, rewind_bytes(rw) / 1048576.0);
				memset(&perf, 0, sizeof(perf));
				perf.qMin = 1 << 30;
				perf.last = now;
			}
		}
		fpsFrames += rewinding ? 1 : runs;
		if (now - fpsT0 >= 1.0) {
			fps = fpsFrames / (now - fpsT0);
			speedPct = fps / SNES_FPS * 100.0;
			fpsFrames = 0;
			fpsT0 = now;
		}
		next += pacedHz > 0 ? frameSec / 1.005 : frameSec;
		if (audio) {
			int queued = (int) (SDL_GetQueuedAudioSize(audio) / 4);
			if (queued > targetQueued * 3 / 2) next += frameSec * 0.015;
			else if (queued < targetQueued / 2) next -= frameSec * 0.015;
		}
		if (next < now - 4 * frameSec) next = now;
		if (noPace) next = now;
		double wait = next - now;
		if (wait > 0) {
			struct timespec ts = { 0, (long) (wait * 1e9) };
			nanosleep(&ts, NULL);
		}
	}

	ahead_stop();
	ui_on_background(NULL);
	ui_on_foreground(NULL);
	ui_sync_view(-1);
	fprintf(stderr, "leaving the game after %llu frames\n", (unsigned long long) frameCount);
	auto_save();
	if (audio) SDL_CloseAudioDevice(audio);
	mapcap_destroy(mc);
	mc = NULL;
	cheats_free(&cheats);
	cheatsOpen = false;
	rewind_destroy(rw);
	free(rwState);
	rw = NULL, rwState = NULL;
	emu_close(&emu);
	for (int i = 0; i < MAP_MAX_FRAMES; ++i) free(mapRes.frames[i]), mapRes.frames[i] = NULL;
	for (int i = 0; i < nViews; ++i) {
		SDL_DestroyTexture(gameTex[i]);
		SDL_DestroyTexture(secTex[i]);
		gameTex[i] = secTex[i] = NULL;
	}
	return exitTo;
}
