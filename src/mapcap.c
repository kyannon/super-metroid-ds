/* Pause-map capture on a hidden copy of the game (see mapcap.h). */
#include "mapcap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Super Metroid RAM ($7E0000 + offset) */
#define SM_GAME_STATE 0x0998
#define SM_BRIGHTNESS 0x0051 /* INIDISP shadow: 0x0F full, 0x80 forced blank */
#define SM_PAUSE_SCREEN 0x0727 /* 0 map, 1 equipment */
#define SM_ROOM 0x079B
#define SM_AREA 0x079F

/* the pause screen's markers blink with a 24-frame loop */
#define ANIM_FRAMES 12
#define ANIM_STEP 2
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
	void* state; /* the live game's state to capture from */
	size_t stateCap, stateLen;

	MapResult result; /* the latest good one (owned here) */
	int failures;
	const char* why;
};

bool sm_can_pause(Emu* e) {
	return emu_read8(e, SM_GAME_STATE) == 0x08;
}

bool sm_no_map_here(Emu* e) {
	uint8_t s = emu_read8(e, SM_GAME_STATE);
	return s <= 0x06 || s == 0x1E || s == 0x1F || s >= 0x26;
}

bool sm_hud_shown(Emu* e) {
	uint8_t s = emu_read8(e, SM_GAME_STATE);
	return (s >= 0x08 && s <= 0x17) || s == 0x19 || s == 0x1B || s == 0x2A;
}

uint32_t sm_room_key(Emu* e) {
	return (uint32_t) emu_read16(e, SM_ROOM) | (uint32_t) emu_read8(e, SM_AREA) << 16 | 1u << 31;
}

static bool map_up(Emu* e) {
	return emu_read8(e, SM_GAME_STATE) == 0x0F && emu_read8(e, SM_BRIGHTNESS) == 0x0F &&
	       emu_read8(e, SM_PAUSE_SCREEN) == 0;
}

static void copy_rows(uint32_t* dst, const uint32_t* fb) {
	memcpy(dst, fb + MAP_Y0 * SNES_W, (size_t) SNES_W * MAP_ROWS * 4);
}

/* One capture from the state in mc->state. Fills r (frames malloc'd). */
static void capture(MapCapture* mc, const void* state, MapResult* r) {
	memset(r, 0, sizeof(*r));
	Emu* e = &mc->shadow;
	if (!mc->open) {
		if (!emu_open_mem(e, EMU_SLOT_SHADOW, mc->rom, mc->romSize, false)) {
			r->why = "the map copy of the game didn't start";
			return;
		}
		mc->open = true;
	}
	if (!emu_state_load(e, state)) {
		r->why = "state didn't load";
		return;
	}
	if (!sm_can_pause(e)) {
		r->why = "not in normal play";
		return;
	}
	emu_set_render(e, false);
	int f = 0;
	for (; f < 4; ++f) emu_frame(e, K_START);
	bool started = false;
	for (; f < PAUSE_MAX && !map_up(e); ++f) {
		emu_frame(e, 0);
		if (emu_read8(e, SM_GAME_STATE) != 0x08) started = true;
		if (f == 20 && !started) break;
	}
	if (!started) {
		r->why = "the game didn't pause";
		return;
	}
	if (!map_up(e)) {
		r->why = "map screen did not appear";
		return;
	}
	emu_set_render(e, true);
	for (int i = 0; i < ANIM_FRAMES; ++i) {
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
	pthread_mutex_lock(&mc->lock);
	for (;;) {
		while (!mc->pending && !mc->quit) pthread_cond_wait(&mc->cond, &mc->lock);
		if (mc->quit) break;
		if (localCap < mc->stateLen) {
			free(local);
			local = malloc(mc->stateLen);
			localCap = local ? mc->stateLen : 0;
		}
		if (local) memcpy(local, mc->state, mc->stateLen);
		mc->pending = false;
		pthread_mutex_unlock(&mc->lock);
		MapResult r;
		if (local) capture(mc, local, &r);
		else memset(&r, 0, sizeof(r)), r.why = "out of memory";
		pthread_mutex_lock(&mc->lock);
		if (r.ok) {
			uint32_t serial = mc->result.serial + 1;
			free_frames(&mc->result);
			mc->result = r;
			mc->result.serial = serial;
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
	free_frames(&mc->result);
	free(mc->state);
	free(mc->rom);
	pthread_mutex_destroy(&mc->lock);
	pthread_cond_destroy(&mc->cond);
	free(mc);
}

bool mapcap_request(MapCapture* mc, Emu* live) {
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

bool mapcap_poll(MapCapture* mc, uint32_t lastSerial, MapResult* out) {
	pthread_mutex_lock(&mc->lock);
	bool fresh = mc->result.ok && mc->result.serial != lastSerial;
	if (fresh) {
		for (int i = 0; i < MAP_MAX_FRAMES; ++i) {
			if (i < mc->result.nFrames) {
				if (!out->frames[i]) out->frames[i] = malloc((size_t) SNES_W * MAP_ROWS * 4);
				if (out->frames[i]) memcpy(out->frames[i], mc->result.frames[i], (size_t) SNES_W * MAP_ROWS * 4);
			}
		}
		out->ok = true;
		out->nFrames = mc->result.nFrames;
		out->frameStep = mc->result.frameStep;
		out->serial = mc->result.serial;
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
	if (st && emu_state_save(live, st)) capture(mc, st, &r);
	else memset(&r, 0, sizeof(r)), r.why = "state";
	free(st);
	return r;
}
