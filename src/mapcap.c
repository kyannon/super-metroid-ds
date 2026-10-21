/* Pause-map capture on a hidden copy of the game (see mapcap.h). */
#include "mapcap.h"
#include "sm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Super Metroid RAM ($7E0000 + offset) */
#define SM_GAME_STATE 0x0998
#define SM_BRIGHTNESS 0x0051 /* INIDISP shadow: 0x0F full, 0x80 forced blank */
#define SM_PAUSE_SCREEN 0x0727 /* 0 map, 1 equipment */
#define SM_ROOM 0x079B
#define SM_AREA 0x079F

/* the pause screen's markers blink with a 24-frame loop */
#define ANIM_FRAMES 12
#define ANIM_STEP 2
/* the equipment screen's glow and arrow: a 64-frame loop */
#define SAMUS_FRAMES 32
#define PAUSE_MAX 150 /* frames from START to the map being up */

struct MapCapture {
	uint8_t* rom;
	size_t romSize;
	Emu shadow;
	bool open;

	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool quit, busy, pending;
	int pendingMode;
	void* state; /* the live game's state to capture from */
	size_t stateCap, stateLen;

	/* the map scrolled with its arrows (taps on the second screen): steps of
	 * 8 pixels, x right, y down, from where the pause screen puts it */
	int scrollX, scrollY;
	bool scrollPending; /* a new scroll to show, from mapUp */
	uint8_t* mapUp;     /* the copy at the moment its map was up */
	size_t mapUpCap;
	bool haveMapUp;
	int mapUpX, mapUpY; /* the scroll it's at */

	MapResult result[2]; /* the latest good one of each mode (owned here) */
	uint32_t serials;
	int failures;
	const char* why;
};

bool sm_can_pause(Emu* e) {
	return emu_read8(e, SM_GAME_STATE) == 0x08;
}

bool sm_pause_available(Emu* e) {
	/* the game's own test before it pauses ($90:EA45) */
	return emu_read16(e, 0x0CEE) == 0 && emu_read16(e, 0x0A78) == 0 && emu_read16(e, 0x0797) == 0 &&
	       emu_read16(e, SM_AREA) != 6 && emu_read16(e, SM_GAME_STATE) == 0x08;
}

bool sm_no_map_here(Emu* e) {
	uint8_t s = emu_read8(e, SM_GAME_STATE);
	return s <= 0x06 || s == 0x1E || s == 0x1F || s >= 0x26;
}

bool sm_hud_shown(Emu* e) {
	uint8_t s = emu_read8(e, SM_GAME_STATE);
	/* $20 / $21: Ceres's elevator ride at the end of the escape (the room
	 * still drawn with its HUD) */
	return (s >= 0x08 && s <= 0x17) || s == 0x19 || s == 0x1B || s == 0x20 || s == 0x21 || s == 0x2A;
}

uint32_t sm_room_key(Emu* e) {
	return (uint32_t) emu_read16(e, SM_ROOM) | (uint32_t) emu_read8(e, SM_AREA) << 16 | 1u << 31;
}

static bool map_up(Emu* e, int mode) {
	return emu_read8(e, SM_GAME_STATE) == 0x0F && emu_read8(e, SM_BRIGHTNESS) == 0x0F &&
	       emu_read8(e, SM_PAUSE_SCREEN) == (mode == MAPCAP_SAMUS ? 1 : 0);
}

static void copy_rows(uint32_t* dst, const uint32_t* fb) {
	memcpy(dst, fb + MAP_Y0 * SNES_W, (size_t) SNES_W * MAP_ROWS * 4);
}

/* Scrolls the map (up) by the steps asked for, a D-pad press a step; stops
 * in a direction where it stops moving (the end of the map). Returns the
 * steps made. */
static int scroll_map(Emu* e, int steps, uint32_t plus, uint32_t minus, uint32_t reg) {
	int made = 0;
	for (int i = 0; i < abs(steps); ++i) {
		uint16_t before = emu_read16(e, reg);
		/* the press shows a few frames later; the next is taken 8 frames on */
		emu_frame(e, steps > 0 ? plus : minus);
		for (int f = 0; f < 9; ++f) emu_frame(e, 0);
		if (emu_read16(e, reg) == before) break;
		made += steps > 0 ? 1 : -1;
	}
	return made;
}

/* One capture from the state in mc->state (or, fromMapUp, from the copy kept
 * at its map: a scroll only). Fills r (frames malloc'd). */
static void capture(MapCapture* mc, const void* state, MapResult* r, bool fromMapUp, int mode) {
	memset(r, 0, sizeof(*r));
	Emu* e = &mc->shadow;
	if (!mc->open) {
		if (!emu_open_mem(e, EMU_SLOT_SHADOW, mc->rom, mc->romSize, false)) {
			r->why = "the map copy of the game didn't start";
			return;
		}
		mc->open = true;
	}
	/* the equipment screen: the pause screen opens on it */
	{
		size_t romSize;
		uint8_t* rom = emu_rom(e, &romSize);
		if (!rom || !sm_set_pause_patch(rom, romSize, mode == MAPCAP_SAMUS)) {
			if (mode == MAPCAP_SAMUS) {
				r->why = "the equipment screen patch didn't go in";
				return;
			}
		}
	}
	if (mode == MAPCAP_SAMUS) fromMapUp = false;
	emu_set_render(e, false);
	if (fromMapUp) {
		if (!emu_state_load(e, mc->mapUp)) {
			r->why = "state didn't load";
			return;
		}
	} else {
		if (!emu_state_load(e, state)) {
			r->why = "state didn't load";
			return;
		}
		if (!sm_can_pause(e)) {
			r->why = "not in normal play";
			return;
		}
		int f = 0;
		for (; f < 4; ++f) emu_frame(e, K_START);
		bool started = false;
		for (; f < PAUSE_MAX && !map_up(e, mode); ++f) {
			emu_frame(e, 0);
			if (emu_read8(e, SM_GAME_STATE) != 0x08) started = true;
			if (f == 20 && !started) break;
		}
		if (!started) {
			r->why = "the game didn't pause";
			return;
		}
		if (!map_up(e, mode)) {
			r->why = mode == MAPCAP_SAMUS ? "equipment screen did not appear" : "map screen did not appear";
			return;
		}
	}
	if (!fromMapUp && mode == MAPCAP_MAP) {
		size_t n = emu_state_size(e);
		if (n > mc->mapUpCap) {
			free(mc->mapUp);
			mc->mapUp = malloc(n);
			mc->mapUpCap = mc->mapUp ? n : 0;
		}
		pthread_mutex_lock(&mc->lock);
		mc->haveMapUp = mc->mapUp && emu_state_save(e, mc->mapUp);
		pthread_mutex_unlock(&mc->lock);
	}
	pthread_mutex_lock(&mc->lock);
	int sx = mode == MAPCAP_MAP ? mc->scrollX : 0, sy = mode == MAPCAP_MAP ? mc->scrollY : 0;
	int bx = fromMapUp ? mc->mapUpX : 0, by = fromMapUp ? mc->mapUpY : 0; /* where the kept copy is */
	if (!fromMapUp && mode == MAPCAP_MAP) mc->mapUpX = mc->mapUpY = 0;
	pthread_mutex_unlock(&mc->lock);
	if (sx != bx || sy != by) {
		/* the pause screen's map scroll is layer 1's ($B1, $B3) */
		int mx = bx + scroll_map(e, sx - bx, K_RIGHT, K_LEFT, 0x00B1);
		int my = by + scroll_map(e, sy - by, K_DOWN, K_UP, 0x00B3);
		if (getenv("SMDS_DEBUG")) fprintf(stderr, "map scroll: asked %d,%d, now %d,%d (from %s)\n", sx, sy, mx, my, fromMapUp ? "the kept map" : "a new pause");
		for (int i = 0; i < 6; ++i) emu_frame(e, 0); /* the last step's glide */
		/* the next scroll starts from here */
		bool kept = mc->mapUp && emu_state_save(e, mc->mapUp);
		pthread_mutex_lock(&mc->lock);
		if (kept) mc->mapUpX = mx, mc->mapUpY = my;
		else mc->haveMapUp = false;
		/* at an end, more taps that way do nothing (unless asked anew meanwhile) */
		if (mc->scrollX == sx) mc->scrollX = mx;
		if (mc->scrollY == sy) mc->scrollY = my;
		pthread_mutex_unlock(&mc->lock);
	}
	emu_set_render(e, true);
	int nAnim = mode == MAPCAP_SAMUS ? SAMUS_FRAMES : ANIM_FRAMES;
	for (int i = 0; i < nAnim; ++i) {
		for (int k = 0; k < (i ? ANIM_STEP : 1); ++k) emu_frame(e, 0);
		r->frames[i] = malloc((size_t) SNES_W * MAP_ROWS * 4);
		if (!r->frames[i]) break;
		copy_rows(r->frames[i], e->fb);
		r->nFrames = i + 1;
	}
	/* a still map (no marker on screen): one frame */
	bool same = true;
	for (int i = 1; i < r->nFrames && same; ++i) same = !memcmp(r->frames[0], r->frames[i], (size_t) SNES_W * MAP_ROWS * 4);
	if (same) {
		for (int i = 1; i < r->nFrames; ++i) free(r->frames[i]), r->frames[i] = NULL;
		r->nFrames = r->nFrames ? 1 : 0;
	}
	r->frameStep = ANIM_STEP;
	r->ok = r->nFrames > 0;
	if (!r->ok) r->why = "out of memory";
}

static void free_frames(MapResult* r) {
	for (int i = 0; i < MAP_MAX_FRAMES; ++i) free(r->frames[i]), r->frames[i] = NULL;
	r->nFrames = 0;
}

static void* worker(void* arg) {
	MapCapture* mc = arg;
	void* local = NULL;
	size_t localCap = 0;
	/* behind the game and its sound: a map a little late is fine, a game
	 * frame late is a crackle */
	setpriority(PRIO_PROCESS, (id_t) syscall(SYS_gettid), 10);
	pthread_mutex_lock(&mc->lock);
	for (;;) {
		while (!mc->pending && !mc->scrollPending && !mc->quit) pthread_cond_wait(&mc->cond, &mc->lock);
		if (mc->quit) break;
		if (!mc->pending) {
			/* a scroll: from the copy kept at its map */
			mc->scrollPending = false;
			bool can = mc->haveMapUp;
			mc->busy = true;
			pthread_mutex_unlock(&mc->lock);
			MapResult r;
			memset(&r, 0, sizeof(r));
			if (can) capture(mc, NULL, &r, true, MAPCAP_MAP);
			pthread_mutex_lock(&mc->lock);
			if (r.ok) {
				free_frames(&mc->result[MAPCAP_MAP]);
				mc->result[MAPCAP_MAP] = r;
				mc->result[MAPCAP_MAP].serial = ++mc->serials;
			} else {
				free_frames(&r);
			}
			mc->busy = false;
			continue;
		}
		int mode = mc->pendingMode;
		if (mode == MAPCAP_MAP) mc->scrollPending = false; /* a whole capture scrolls too */
		if (localCap < mc->stateLen) {
			free(local);
			local = malloc(mc->stateLen);
			localCap = local ? mc->stateLen : 0;
		}
		if (local) memcpy(local, mc->state, mc->stateLen);
		mc->pending = false;
		pthread_mutex_unlock(&mc->lock);
		MapResult r;
		if (local) capture(mc, local, &r, false, mode);
		else memset(&r, 0, sizeof(r)), r.why = "out of memory";
		pthread_mutex_lock(&mc->lock);
		if (r.ok) {
			free_frames(&mc->result[mode]);
			mc->result[mode] = r;
			mc->result[mode].serial = ++mc->serials;
			mc->failures = 0;
		} else {
			free_frames(&r);
			++mc->failures;
			mc->why = r.why;
		}
		mc->busy = false;
	}
	pthread_mutex_unlock(&mc->lock);
	free(local);
	return NULL;
}

MapCapture* mapcap_create(const uint8_t* rom, size_t size) {
	MapCapture* mc = calloc(1, sizeof(*mc));
	if (!mc) return NULL;
	mc->rom = malloc(size);
	if (!mc->rom) {
		free(mc);
		return NULL;
	}
	memcpy(mc->rom, rom, size);
	mc->romSize = size;
	pthread_mutex_init(&mc->lock, NULL);
	pthread_cond_init(&mc->cond, NULL);
	if (pthread_create(&mc->thread, NULL, worker, mc) != 0) {
		free(mc->rom);
		free(mc);
		return NULL;
	}
	return mc;
}

void mapcap_destroy(MapCapture* mc) {
	if (!mc) return;
	pthread_mutex_lock(&mc->lock);
	mc->quit = true;
	pthread_cond_broadcast(&mc->cond);
	pthread_mutex_unlock(&mc->lock);
	pthread_join(mc->thread, NULL);
	if (mc->open) emu_close(&mc->shadow);
	free_frames(&mc->result[0]);
	free_frames(&mc->result[1]);
	free(mc->state);
	free(mc->mapUp);
	free(mc->rom);
	pthread_mutex_destroy(&mc->lock);
	pthread_cond_destroy(&mc->cond);
	free(mc);
}

bool mapcap_request(MapCapture* mc, Emu* live, int mode) {
	pthread_mutex_lock(&mc->lock);
	if (mc->busy) {
		pthread_mutex_unlock(&mc->lock);
		return false;
	}
	size_t n = emu_state_size(live);
	if (n > mc->stateCap) {
		free(mc->state);
		mc->state = malloc(n);
		mc->stateCap = mc->state ? n : 0;
	}
	bool ok = mc->state && emu_state_save(live, mc->state);
	if (ok) {
		mc->stateLen = n;
		mc->busy = mc->pending = true;
		mc->pendingMode = mode;
		pthread_cond_broadcast(&mc->cond);
	}
	pthread_mutex_unlock(&mc->lock);
	return ok;
}

bool mapcap_busy(MapCapture* mc) {
	pthread_mutex_lock(&mc->lock);
	bool b = mc->busy;
	pthread_mutex_unlock(&mc->lock);
	return b;
}

bool mapcap_poll(MapCapture* mc, int mode, uint32_t lastSerial, MapResult* out) {
	pthread_mutex_lock(&mc->lock);
	const MapResult* res = &mc->result[mode];
	bool fresh = res->ok && res->serial != lastSerial;
	if (fresh) {
		for (int i = 0; i < MAP_MAX_FRAMES; ++i) {
			if (i < res->nFrames) {
				if (!out->frames[i]) out->frames[i] = malloc((size_t) SNES_W * MAP_ROWS * 4);
				if (out->frames[i]) memcpy(out->frames[i], res->frames[i], (size_t) SNES_W * MAP_ROWS * 4);
			}
		}
		out->ok = true;
		out->nFrames = res->nFrames;
		out->frameStep = res->frameStep;
		out->serial = res->serial;
	}
	pthread_mutex_unlock(&mc->lock);
	return fresh;
}

int mapcap_failures(MapCapture* mc, const char** why) {
	pthread_mutex_lock(&mc->lock);
	int n = mc->failures;
	if (why) *why = mc->why;
	pthread_mutex_unlock(&mc->lock);
	return n;
}

MapResult mapcap_capture_sync(MapCapture* mc, Emu* live) {
	MapResult r;
	size_t n = emu_state_size(live);
	void* st = malloc(n);
	if (st && emu_state_save(live, st)) capture(mc, st, &r, false, MAPCAP_MAP);
	else memset(&r, 0, sizeof(r)), r.why = "state";
	free(st);
	return r;
}

void mapcap_scroll(MapCapture* mc, int dx, int dy) {
	pthread_mutex_lock(&mc->lock);
	mc->scrollX += dx;
	mc->scrollY += dy;
	mc->scrollPending = true;
	pthread_cond_broadcast(&mc->cond);
	pthread_mutex_unlock(&mc->lock);
}

void mapcap_reset_scroll(MapCapture* mc) {
	pthread_mutex_lock(&mc->lock);
	mc->scrollX = mc->scrollY = 0;
	mc->haveMapUp = false;
	mc->scrollPending = false;
	pthread_mutex_unlock(&mc->lock);
}
