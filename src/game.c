/* The play session: the game screen, the second screen (the HUD over the
 * pause map, or "MAP NOT AVAILABLE"), the settings menu. */
#include "game.h"
#include "android.h"
#include "emu.h"
#include "mapcap.h"
#include "paths.h"
#include "sm.h"
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
		emu_frame(&emu, keys);
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

static int game_screen(void) {
	if (nScreens < 2) return 0;
	return cfg->swap ? 1 : 0;
}

static int map_screen(void) {
	return nScreens < 2 ? 0 : 1 - game_screen();
}

/* The second screen's picture: the live HUD over the last map. */
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
	if (haveMap && mapShownIdx >= 0 && mapRes.frames[mapShownIdx]) {
		memcpy(secPx + SNES_W * MAP_Y0, mapRes.frames[mapShownIdx], (size_t) SNES_W * MAP_ROWS * 4);
	}
}

/* The settings button where the HUD's mini-map was, in screen-local
 * coordinates of the second screen. */
static SDL_Rect settings_button_rect(int s) {
	SDL_Rect img = ui_image_rect(s, SNES_W, SNES_H, cfg->mapScale);
	double sx = (double) img.w / SNES_W, sy = (double) img.h / SNES_H;
	int x0 = img.x + (int) lround(HUD_MINIMAP_X0 * sx), x1 = img.x + (int) lround((HUD_MINIMAP_X1 + 1) * sx);
	int y0 = img.y + (int) lround(HUD_MINIMAP_Y0 * sy), y1 = img.y + (int) lround((HUD_MINIMAP_Y1 + 1) * sy);
	return (SDL_Rect) { x0, y0, x1 - x0, y1 - y0 };
}

static bool buttonPressed; /* a finger is down on it */

static void draw_settings_button(int s) {
	SDL_Rect b = settings_button_rect(s);
	SDL_Rect img = ui_image_rect(s, SNES_W, SNES_H, cfg->mapScale);
	int u = SDL_max(1, img.w / SNES_W);
	ui_sm_button(s, b, u, buttonPressed);
	/* two lines, as large as fits inside the face */
	int innerW = b.w - u * 6, innerH = b.h - u * 5;
	/* the font's capitals are 11 dots tall, "SETTINGS" 63 dots wide; whole
	 * pixels a dot when that fills the button, else smoothed to fit */
	double dot = SDL_min(innerW / 63.0, innerH / 25.0);
	int font = TF_BOLD;
	if (dot < 1) dot = 1;
	else if (dot - (int) dot > 0.25) font = TF_SMOOTH;
	else dot = (int) dot;
	int cap = (int) (dot * 11), gap = (int) (dot * 3);
	int cy = b.y + b.h / 2;
	int off = buttonPressed ? u : 0;
	ui_font_text(s, font, b.x + b.w / 2 + off, cy - (cap + gap) / 2 + off, cap, "TAP FOR", SM_TEXT_DARK, 0);
	ui_font_text(s, font, b.x + b.w / 2 + off, cy + (cap + gap) / 2 + off, cap, "SETTINGS", SM_TEXT_DARK, 0);
}

static bool in_rect(SDL_Rect r, int x, int y) {
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* ------------------------------------------------- menu */

enum MenuItem {
	M_RESUME, M_RESTART, M_SAVE, M_LOAD, M_SHOT, M_EXIT,
	M_SWAP, M_SCALE, M_LCD,
	M_MAPANIM, M_MAPRATE, M_FRAMEBUF, M_MATCH, M_FPS,
};
static const char* menuLabels[] = {
	[M_RESUME] = "Resume", [M_RESTART] = "Restart Game", [M_SAVE] = "Save State", [M_LOAD] = "Load State",
	[M_SHOT] = "Take Screenshot", [M_EXIT] = "Close Super Metroid", [M_SWAP] = "Swap Screens",
};
#define N_PAGES 3
static const char* pageNames[N_PAGES] = { "Game", "Display", "System" };
static int pages[N_PAGES][12], pageLen[N_PAGES], curPage;
static int menuItems[12], nMenuItems;
static bool menuOpen;
static int menuSel, menuPressed = -1;
static bool ignorePointerUp, restartArmed;
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
	case M_LCD: return cfg->lcdGrid ? "Pixel Grid: On" : "Pixel Grid: Off";
	case M_FPS: return cfg->showFps ? "Show FPS: On" : "Show FPS: Off";
	case M_FRAMEBUF: return cfg->frameBuffer ? "Frame Buffer: On" : "Frame Buffer: Off";
	case M_MAPANIM: return cfg->mapAnim ? "Map Animations: On" : "Map Animations: Off";
	case M_MATCH: return cfg->matchDisplay ? "Match Display Rate: On" : "Match Display Rate: Off";
	case M_RESTART: return restartArmed ? "Tap Again to Restart" : menuLabels[item];
	default: return menuLabels[item];
	}
}

/* Menu geometry, in SNES-sized units u (screen pixels per SNES pixel). */
typedef struct {
	int u, cap, titleCap;
	SDL_Rect title, tabs[N_PAGES], lPill, rPill, panel, items[12];
	int noteY;
} MenuGeom;

static MenuGeom menu_geom(int scr) {
	MenuGeom g;
	memset(&g, 0, sizeof(g));
	SDL_Rect r = screens[scr].rect;
	int u = SDL_max(1, SDL_min(r.h / SNES_H, r.w / SNES_W));
	g.u = u;
	g.cap = 11 * SDL_max(2, u - 1); /* item text: 2 pixels a font dot on these screens */
	g.titleCap = tf_cap(u * 11);
	int w = SDL_min(r.w - u * 16, u * 224);
	int x = (r.w - w) / 2;
	int y = u * 6;
	g.title = (SDL_Rect) { x, y, w, g.titleCap };
	y += g.titleCap + u * 8;
	/* page tabs between L and R pills */
	int pillW = u * 14, tabH = g.cap + u * 8, gap = u * 3;
	int tabsW = w - 2 * (pillW + gap);
	int tw = (tabsW - gap * (N_PAGES - 1)) / N_PAGES;
	int pillH = SDL_min(tabH, g.cap + u * 4);
	g.lPill = (SDL_Rect) { x, y + (tabH - pillH) / 2, pillW, pillH };
	g.rPill = (SDL_Rect) { x + w - pillW, g.lPill.y, pillW, pillH };
	for (int t = 0; t < N_PAGES; ++t) g.tabs[t] = (SDL_Rect) { x + pillW + gap + t * (tw + gap), y, tw, tabH };
	y += tabH + u * 10;
	int rowH = SDL_max(g.cap + u * 6, u * 14), rowGap = u * 2;
	int n = SDL_max(nMenuItems, 5);
	int top = g.cap / 2 + u * 6;
	g.panel = (SDL_Rect) { x, y, w, top + u * 6 + n * rowH + (n - 1) * rowGap };
	for (int i = 0; i < nMenuItems; ++i) {
		g.items[i] = (SDL_Rect) { x + u * 8, y + top + i * (rowH + rowGap), w - u * 16, rowH };
	}
	g.noteY = g.panel.y + g.panel.h + u * 10;
	return g;
}

static int tab_at(int scr, int x, int y) {
	MenuGeom g = menu_geom(scr);
	for (int t = 0; t < N_PAGES; ++t) {
		if (in_rect(g.tabs[t], x, y)) return t;
	}
	if (in_rect(g.lPill, x, y)) return curPage > 0 ? curPage - 1 : -1;
	if (in_rect(g.rPill, x, y)) return curPage < N_PAGES - 1 ? curPage + 1 : -1;
	return -1;
}

static int item_at(int scr, int x, int y) {
	MenuGeom g = menu_geom(scr);
	for (int i = 0; i < nMenuItems; ++i) {
		if (in_rect(g.items[i], x, y)) return i;
	}
	return -1;
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

/* While paused: the time and the battery at the top of the other screen. */
static void draw_status(int s) {
	int u = SDL_max(1, screens[s].rect.h / SNES_H);
	int cap = tf_cap(u * 8 > 11 ? u * 8 : 11);
	char buf[16];
	time_t now = time(NULL);
	struct tm tm;
	if (localtime_r(&now, &tm)) {
		snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
		ui_font_text(s, TF_BOLD, u * 6, u * 6 + cap / 2, cap, buf, SM_WHITE, -1);
	}
	int pct = battery_percent();
	if (pct >= 0) {
		snprintf(buf, sizeof(buf), "%d%%", pct);
		ui_font_text(s, TF_BOLD, screens[s].rect.w - u * 6, u * 6 + cap / 2, cap, buf, SM_WHITE, 1);
	}
}

static void draw_pill(int scr, SDL_Rect r, int u, const char* label, bool enabled) {
	/* the pause screen's grey L / R shoulder pills */
	SDL_Color body = enabled ? SM_GREY_LIGHT : SM_GREY_DARK;
	ui_fill(scr, (SDL_Rect) { r.x + u * 2, r.y, r.w - u * 4, r.h }, body);
	ui_fill(scr, (SDL_Rect) { r.x + u, r.y + u, r.w - u * 2, r.h - u * 2 }, body);
	ui_fill(scr, (SDL_Rect) { r.x, r.y + u * 2, r.w, r.h - u * 4 }, body);
	ui_fill(scr, (SDL_Rect) { r.x + u * 3, r.y + u, r.w - u * 6, u }, SM_WHITE);
	int cap = tf_cap(SDL_max(11, r.h - u * 4));
	ui_font_text(scr, TF_BOLD, r.x + r.w / 2, r.y + r.h / 2, cap, label, enabled ? SM_PURPLE : SM_GREY, 0);
}

static void draw_menu(void) {
	int scr = ui_touch_screen();
	SDL_Rect sr = screens[scr].rect;
	MenuGeom g = menu_geom(scr);
	int u = g.u;
	ui_sm_backdrop(scr, (SDL_Rect) { 0, 0, sr.w, sr.h }, u);
	/* the title, between grey bars like the pause screen's SAMUS */
	{
		int tw = ui_font_width(TF_BOLD, g.titleCap, "SETTINGS");
		int cy = g.title.y + g.titleCap / 2;
		int bx0 = g.title.x, bx1 = g.title.x + g.title.w;
		int l1 = g.title.x + (g.title.w - tw) / 2 - u * 8, r0 = g.title.x + (g.title.w + tw) / 2 + u * 8;
		for (int side = 0; side < 2; ++side) {
			int a = side ? r0 : bx0, b = side ? bx1 : l1;
			ui_fill(scr, (SDL_Rect) { a, cy - u * 2, b - a, u }, SM_WHITE);
			ui_fill(scr, (SDL_Rect) { a, cy - u, b - a, u * 2 }, SM_GREY_LIGHT);
			ui_fill(scr, (SDL_Rect) { a, cy + u, b - a, u }, SM_GREY_DARK);
			for (int k = 0; k < 3; ++k) { /* the ||| ends */
				int x = side ? a + u * (1 + k * 2) : b - u * (2 + k * 2);
				ui_fill(scr, (SDL_Rect) { x, cy - u * 3, u, u * 6 }, SM_WHITE);
			}
		}
		ui_font_text(scr, TF_BOLD, g.title.x + g.title.w / 2 + u, cy + u, g.titleCap, "SETTINGS", SM_BLUE, 0);
		ui_font_text(scr, TF_BOLD, g.title.x + g.title.w / 2, cy, g.titleCap, "SETTINGS", SM_WHITE, 0);
	}
	/* page tabs: the current one a yellow button, the others grey */
	int tabCap = g.cap;
	while (tabCap > 11 && ui_font_width(TF_BOLD, tabCap, "Display") > g.tabs[0].w - u * 6) tabCap = tf_cap(tabCap - 11);
	for (int t = 0; t < N_PAGES; ++t) {
		SDL_Rect r = g.tabs[t];
		bool on = t == curPage;
		if (on) {
			ui_sm_button(scr, r, u, false);
		} else {
			ui_fill(scr, r, SM_GREY_DARK);
			ui_fill(scr, (SDL_Rect) { r.x + u, r.y + u, r.w - 2 * u, r.h - 2 * u }, SM_BLACK);
		}
		ui_font_text(scr, TF_BOLD, r.x + r.w / 2, r.y + r.h / 2, tabCap, pageNames[t], on ? SM_TEXT_DARK : SM_GREY, 0);
	}
	draw_pill(scr, g.lPill, u, "L", curPage > 0);
	draw_pill(scr, g.rPill, u, "R", curPage < N_PAGES - 1);
	/* the items, in a panel named after the page */
	ui_sm_frame(scr, g.panel, u, pageNames[curPage], g.cap, SM_BLACK);
	for (int i = 0; i < nMenuItems; ++i) {
		SDL_Rect r = g.items[i];
		bool sel = i == menuSel, en = item_enabled(menuItems[i]);
		if (sel) {
			/* selected: a white and blue frame around it, like the pause screen's cursor box */
			ui_fill(scr, (SDL_Rect) { r.x - u * 2, r.y - u * 2, r.w + u * 4, r.h + u * 4 }, SM_WHITE);
			ui_fill(scr, (SDL_Rect) { r.x - u, r.y - u, r.w + u * 2, r.h + u * 2 }, (SDL_Color) { 0, 0, 230, 255 });
		}
		ui_fill(scr, r, en ? SM_YELLOW : SM_GREY);
		int cy = r.y + r.h / 2;
		ui_sm_bullet(scr, r.x + u * 6, cy, u, en);
		ui_font_text(scr, TF_BOLD, r.x + u * 12, cy, g.cap, item_label(menuItems[i]), SM_TEXT_DARK, -1);
	}
	if (menuNote[0]) ui_font_text(scr, TF_BOLD, sr.w / 2, g.noteY, g.cap, menuNote, SM_YELLOW, 0);
	if (nScreens > 1) {
		int o = ui_other_screen();
		int ou = SDL_max(1, screens[o].rect.h / SNES_H);
		ui_dim(o, 170);
		ui_font_text(o, TF_BOLD, screens[o].rect.w / 2, screens[o].rect.h / 2, tf_cap(ou * 22), "PAUSED", SM_WHITE, 0);
		draw_status(o);
	}
}

static void open_menu(void) {
	set_page(0);
	menuOpen = true;
	menuSel = 0;
	menuPressed = -1;
	menuNote[0] = 0;
	restartArmed = false;
	buttonPressed = false;
}

/* ------------------------------------------------- settings that stick */

static void save_prefs(void) {
	char prefs[1100];
	snprintf(prefs, sizeof(prefs), "%s/prefs.cfg", cfg->stateDir);
	FILE* f = fopen(prefs, "w");
	if (!f) return;
	fprintf(f, "# written by the settings menu; overrides sm-ds.cfg\n");
	fprintf(f, "swap = %d\nscale = %s\nmap_scale = %s\nlcd_grid = %d\nshow_fps = %d\nmap_animations = %d\nmap_interval = %d\nframe_buffer = %d\nmatch_display = %d\n",
	        cfg->swap ? 1 : 0, cfg->scale, cfg->mapScale, cfg->lcdGrid ? 1 : 0, cfg->showFps ? 1 : 0, cfg->mapAnim ? 1 : 0,
	        cfg->mapInterval, cfg->frameBuffer ? 1 : 0, cfg->matchDisplay ? 1 : 0);
	fclose(f);
}

static bool gameTexAll = true;

static void swap_screens(void) {
	gameTexAll = true;
	mapDirty = true;
	if (nScreens < 2) {
		singleShowsMap = !singleShowsMap;
		return;
	}
	cfg->swap = !cfg->swap;
	save_prefs();
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
	if (ok) aheadReady = false;
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
		snprintf(dir, sizeof(dir), "%s/screenshots", cfg->stateDir);
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
		menuOpen = false;
		break;
	case M_SHOT:
		menuOpen = false;
		shotPending = true;
		break;
	case M_SWAP:
		swap_screens();
		snprintf(menuNote, sizeof(menuNote), "Screens swapped");
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
		cfg->lcdGrid = !cfg->lcdGrid;
		save_prefs();
		break;
	case M_SAVE:
		if (state_save(gp.state)) {
			ui_osd("State saved");
			menuOpen = false;
		} else {
			snprintf(menuNote, sizeof(menuNote), "Save failed");
		}
		break;
	case M_LOAD:
		if (!item_enabled(M_LOAD)) {
			snprintf(menuNote, sizeof(menuNote), "No saved state yet");
		} else if (state_load(gp.state)) {
			ui_osd("State loaded");
			menuOpen = false;
		} else {
			snprintf(menuNote, sizeof(menuNote), "Load failed");
		}
		break;
	case M_FPS:
		cfg->showFps = !cfg->showFps;
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
		aheadReady = false;
		haveMap = false;
		mapShownIdx = -1;
		mapDirty = true;
		restartArmed = false;
		menuOpen = false;
		ui_osd("Game restarted");
		break;
	case M_EXIT:
		return true;
	}
	return false;
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
	ui_image_pixels(s, secTex, SNES_W, SNES_H, cfg->mapScale, cfg->lcdGrid ? 1 : 0);
	if (!haveMap) {
		SDL_Rect img = ui_image_rect(s, SNES_W, SNES_H, cfg->mapScale);
		int u = SDL_max(1, img.w / SNES_W);
		ui_font_text(s, TF_BOLD, r.w / 2, r.h / 2, tf_cap(u * 11), "MAP NOT AVAILABLE", SM_WHITE, 0);
	}
	draw_settings_button(s);
}

static void draw_game(void) {
	bool two = nScreens > 1 && cfg->map;
	for (int s = 0; s < nScreens; ++s) {
		bool showGame = nScreens == 1 ? !singleShowsMap : (s == game_screen() || !two);
		if (!showGame) {
			draw_secondary(s);
			continue;
		}
		if (nScreens == 2 && !two && s != game_screen()) continue;
		ui_image_pixels(s, gameTex, SNES_W, SNES_H, cfg->scale, cfg->lcdGrid ? 1 : 0);
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
	fprintf(stderr, "game %s, save %s, HUD patches %d of %d\n", gp.base, gp.sav, patched, sm_patch_count());
	ui_on_background(on_background);
	ui_on_foreground(on_foreground);

	mc = cfg->map ? mapcap_create(romData, romSize) : NULL;
	memset(&mapRes, 0, sizeof(mapRes));
	haveMap = false;
	mapShownIdx = -1;
	frameCount = 0;
	menuOpen = false;
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
	pages[0][pageLen[0]++] = M_RESTART;
	pages[0][pageLen[0]++] = M_SHOT;
	pages[0][pageLen[0]++] = M_EXIT;
	pages[1][pageLen[1]++] = M_SWAP;
	pages[1][pageLen[1]++] = M_SCALE;
	pages[1][pageLen[1]++] = M_LCD;
	pages[1][pageLen[1]++] = M_FPS;
	pages[2][pageLen[2]++] = M_MAPANIM;
	pages[2][pageLen[2]++] = M_MAPRATE;
	pages[2][pageLen[2]++] = M_FRAMEBUF;
	pages[2][pageLen[2]++] = M_MATCH;
	set_page(0);

	if (cfg->loadState[0]) {
		if (!state_load(cfg->loadState)) fprintf(stderr, "could not load %s\n", cfg->loadState);
		cfg->loadState[0] = 0;
	} else if (cfg->autoResume && file_exists(gp.autoState)) {
		if (state_load(gp.autoState)) ui_osd("Resumed");
	}

	SDL_AudioSpec want = { 0 }, have;
	want.freq = 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = getenv("SMDS_AUDIO_SAMPLES") ? atoi(getenv("SMDS_AUDIO_SAMPLES")) : 512;
	SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio) SDL_PauseAudioDevice(audio, 0);
	else fprintf(stderr, "no audio: %s\n", SDL_GetError());
	SDL_AudioStream* audioConv = NULL;
	int convFrom = 0;
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
	bool done = false, wasMenu = false;

	pacing_update();
	const double freq = (double) SDL_GetPerformanceFrequency();
	double next = SDL_GetPerformanceCounter() / freq;
	double fpsT0 = next;
	int fpsFrames = 0;
#ifdef __ANDROID__
	android_perf_init((int64_t) (frameSec * 0.5 * 1e9));
	android_set_pinning(cfg->pinCores);
	android_pin_game_thread();
#endif
	while (!done) {
		ahead_sync();
		if (!ui_events()) break;
		ui_tick();
		ui_read_buttons();

		/* ---- touch: the settings button opens the menu; in the menu, taps press items */
		Pointer p;
		while (ui_next_pointer(&p)) {
			if (!menuOpen) {
				bool onSecond = nScreens == 1 ? singleShowsMap : p.screen == map_screen();
				if (nScreens == 1 && !singleShowsMap) {
					/* one screen showing the game: any tap opens the menu */
					if (p.kind == 1) open_menu(), ignorePointerUp = true;
					continue;
				}
				if (!onSecond) continue;
				bool inside = in_rect(settings_button_rect(p.screen), p.x, p.y);
				if (p.kind == 1) buttonPressed = inside;
				else if (p.kind == 3) buttonPressed = buttonPressed && inside;
				else if (p.kind == 2) {
					if (buttonPressed && inside) {
						open_menu();
						ignorePointerUp = false;
					}
					buttonPressed = false;
				}
				continue;
			}
			if (p.screen != ui_touch_screen()) continue;
			if (p.kind == 1) {
				int t = tab_at(p.screen, p.x, p.y);
				if (t >= 0) {
					if (t != curPage) set_page(t);
					ignorePointerUp = true;
					continue;
				}
			}
			int it = item_at(p.screen, p.x, p.y);
			if (p.kind == 1) {
				ignorePointerUp = false;
				menuPressed = it;
				if (it >= 0) menuSel = it;
			} else if (p.kind == 3 && menuPressed >= 0 && it >= 0) {
				menuSel = it;
			} else if (p.kind == 2) {
				if (!ignorePointerUp && it >= 0 && it == menuPressed && activate(menuItems[it])) done = true;
				menuPressed = -1;
				ignorePointerUp = false;
			}
		}
		if (done) break;

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
			if (btn_pressed(B_Y)) swap_screens(), comboUsed = true;
		}
		if (btn_released(B_HOME) && !(modBtn == B_HOME && comboUsed)) {
			if (menuOpen) menuOpen = false;
			else open_menu();
		}
		if (btn_released(modBtn)) {
			if (comboUsed) suppress_held();
			if (hotkeyBack && !comboUsed && !menuOpen) selectPulse = 4;
			comboUsed = false;
		}
		if (done) break;

		if (menuOpen && !comboOpened) {
			if (btn_pressed(B_L) && curPage > 0) set_page(curPage - 1);
			if (btn_pressed(B_R) && curPage < N_PAGES - 1) set_page(curPage + 1);
			if (btn_pressed(B_DOWN)) menuSel = (menuSel + 1) % nMenuItems;
			if (btn_pressed(B_UP)) menuSel = (menuSel + nMenuItems - 1) % nMenuItems;
			if (btn_pressed(B_B) || btn_pressed(B_START)) menuOpen = false;
			else if (btn_pressed(B_A) && activate(menuItems[menuSel])) break;
		}
		if (menuOpen && !wasMenu && audio) {
			SDL_PauseAudioDevice(audio, 1);
			SDL_ClearQueuedAudio(audio);
		}
		if (wasMenu && !menuOpen) {
			suppress_held();
			if (audio) SDL_PauseAudioDevice(audio, 0);
			next = SDL_GetPerformanceCounter() / freq;
			fpsT0 = next;
			fpsFrames = 0;
			forceMap = true;
		}
		wasMenu = menuOpen;

		if (menuOpen) {
			ui_clear();
			draw_game();
			draw_menu();
			ui_present();
			SDL_Delay(16);
			next = SDL_GetPerformanceCounter() / freq;
			continue;
		}

		if (btn_pressed(B_L2)) swap_screens();

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

		bool ff = btnNow[B_R2];
		int runs = ff ? SDL_max(1, cfg->ffSpeed) : 1;
		int queuedNow = audio ? (int) (SDL_GetQueuedAudioSize(audio) / 4) : 0;
		bool dropAudio = queuedNow > targetQueued * 3;
		for (int i = 0; i < runs; ++i) {
			if (i == 0 && aheadReady) aheadReady = false;
			else emu_frame(&emu, keys);
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
				int rate = (int) lround(emu_audio_rate(&emu) * audioSpeed);
				if (rate != convFrom || !audioConv) {
					if (audioConv) SDL_FreeAudioStream(audioConv);
					audioConv = SDL_NewAudioStream(AUDIO_S16SYS, 2, rate, AUDIO_S16SYS, 2, want.freq);
					convFrom = rate;
				}
				if (!audioConv) continue;
				SDL_AudioStreamPut(audioConv, samples, (int) got * 4);
				static int16_t conv[8192 * 2];
				int bytes;
				while ((bytes = SDL_AudioStreamGet(audioConv, conv, sizeof(conv))) > 0) {
					if (pacedHz > 0 && bytes >= 8) {
						int q = (int) (SDL_GetQueuedAudioSize(audio) / 4);
						if (q > targetQueued * 5 / 4) bytes -= 4;
						else if (q < targetQueued * 3 / 4 && bytes + 4 <= (int) sizeof(conv)) {
							memcpy((char*) conv + bytes, (char*) conv + bytes - 4, 4);
							bytes += 4;
						}
					}
					SDL_QueueAudio(audio, conv, bytes);
				}
			}
		}
		if (sm_strip_unusable(&emu)) sm_blank_strip(emu.fb);
		++frameCount;
		if (frameCount % 120 == 0) emu_save_flush(&emu); /* the game saved at a station: on disk soon after */

		/* ---- the map */
		if (sm_no_map_here(&emu)) {
			if (haveMap && debug) fprintf(stderr, "frame %llu: no map here (game state %02x)\n", (unsigned long long) frameCount, emu_read8(&emu, 0x0998));
			if (haveMap) mapDirty = true;
			haveMap = false;
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
			int interval = cfg->mapInterval << SDL_min(fails, 3);
			bool due = sinceMapRequest >= interval || ((forceMap || roomChanged) && (!fails || sinceMapRequest >= cfg->mapInterval));
			if (due && sm_can_pause(&emu) && !mapcap_busy(mc) && mapcap_request(mc, &emu)) {
				sinceMapRequest = 0;
				lastRoomKey = rk;
				forceMap = false;
			}
			if (mapcap_poll(mc, mapRes.serial, &mapRes)) {
				if (!sm_no_map_here(&emu)) {
					haveMap = true;
					mapAnimStart = frameCount;
					mapShownIdx = 0;
					mapDirty = true;
					if (debug) fprintf(stderr, "frame %llu: map updated (%d frames)\n", (unsigned long long) frameCount, mapRes.nFrames);
				}
			} else if (haveMap && cfg->mapAnim && mapRes.nFrames > 1) {
				int idx = (int) ((frameCount - mapAnimStart) / SDL_max(1, mapRes.frameStep)) % mapRes.nFrames;
				if (idx != mapShownIdx) mapShownIdx = idx, mapDirty = true;
			} else if (haveMap && !cfg->mapAnim && mapShownIdx != 0) {
				mapShownIdx = 0;
				mapDirty = true;
			}
		}

		/* ---- the second screen: the HUD changes every frame it's up */
		bool hud = sm_hud_shown(&emu);
		static bool hadHud;
		if (hud || hadHud || mapDirty) {
			compose_secondary(hud);
			int sv = screens[map_screen()].view;
			for (int i = 0; i < nViews; ++i) {
				if (nScreens == 1 || i == sv) ui_update_texture(secTex[i], secPx, SNES_W * 4);
			}
			mapDirty = false;
		}
		hadHud = hud;

		/* ---- draw */
		const uint32_t* shownFb = emu.fb;
		if (cfg->frameBuffer && !ff) {
			memcpy(aheadFb, emu.fb, sizeof(aheadFb));
			if (ahead_start(keys)) aheadInFlight = true, shownFb = aheadFb;
		}
		for (int i = 0; i < nViews; ++i) {
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
		fpsFrames += runs;
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
	if (audioConv) SDL_FreeAudioStream(audioConv);
	mapcap_destroy(mc);
	mc = NULL;
	emu_close(&emu);
	for (int i = 0; i < MAP_MAX_FRAMES; ++i) free(mapRes.frames[i]), mapRes.frames[i] = NULL;
	for (int i = 0; i < nViews; ++i) {
		SDL_DestroyTexture(gameTex[i]);
		SDL_DestroyTexture(secTex[i]);
		gameTex[i] = secTex[i] = NULL;
	}
	return exitTo;
}
