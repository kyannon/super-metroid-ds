/* The libretro SNES core (snes9x), one copy per slot, loaded with dlopen. */
#include "emu.h"
#include "libretro.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

struct EmuCore {
	void* lib;
	void (*init)(void);
	void (*deinit)(void);
	bool (*load_game)(const struct retro_game_info*);
	void (*unload_game)(void);
	void (*run)(void);
	void (*reset)(void);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	size_t (*serialize_size)(void);
	bool (*serialize)(void*, size_t);
	bool (*unserialize)(const void*, size_t);
	void* (*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
	void (*get_system_av_info)(struct retro_system_av_info*);
	/* our additions (core/smds_ext.cpp) */
	void (*set_layers)(unsigned);
	uint8_t* (*vram)(void);
	uint16_t* (*cgram)(void);
	uint8_t* (*rom)(void);
	size_t (*rom_size)(void);
	void (*set_hud_hdma)(int);
	void (*set_hud_bg3)(int);
	void (*bg)(int, int*);
	void (*cheat_reset)(void);
	void (*cheat_set)(unsigned, bool, const char*);

	uint32_t fb[SNES_W * SNES_H];
	uint32_t keys;
	bool render;
	unsigned rate;
	uint8_t* romCopy;
	size_t romSize;
	uint8_t* savedSram; /* what's on disk */
	/* audio ring (stereo frames) */
	pthread_mutex_t aLock;
	int16_t* ring;
	size_t ringCap, ringHead, ringLen;
};

static char coreDir[1024] = ".";
static EmuCore* slotCore[EMU_SLOTS];
static uint32_t lut565[65536];

void emu_set_core_dir(const char* dir) {
	snprintf(coreDir, sizeof(coreDir), "%s", dir);
}

static void core_log(enum retro_log_level level, const char* fmt, ...) {
	if (level < RETRO_LOG_WARN) return;
	va_list ap;
	va_start(ap, fmt);
#ifdef __ANDROID__
	__android_log_vprint(ANDROID_LOG_WARN, "sm-ds-core", fmt, ap);
#else
	vfprintf(stderr, fmt, ap);
#endif
	va_end(ap);
}

static bool env_common(EmuCore* c, unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return *(const enum retro_pixel_format*) data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**) data = coreDir;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool*) data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		*(int*) data = (c->render ? 1 : 0) | 2;
		return true;
	default:
		return false;
	}
}

static void video_common(EmuCore* c, const void* data, unsigned w, unsigned h, size_t pitch) {
	if (!data) return; /* a repeated frame */
	int step = w >= 512 ? 2 : 1; /* hi-res: every other pixel */
	unsigned ow = w / step, oh = h;
	if (ow > SNES_W) ow = SNES_W;
	int yskip = h > SNES_H ? (int) (h - SNES_H) / 2 : 0;
	if (oh > SNES_H) oh = SNES_H;
	for (unsigned y = 0; y < oh; ++y) {
		const uint16_t* row = (const uint16_t*) ((const uint8_t*) data + (y + yskip) * pitch);
		uint32_t* out = c->fb + y * SNES_W;
		for (unsigned x = 0; x < ow; ++x) out[x] = lut565[row[x * step]];
	}
}

static size_t audio_common(EmuCore* c, const int16_t* d, size_t frames) {
	if (!c->ring) return frames;
	pthread_mutex_lock(&c->aLock);
	for (size_t i = 0; i < frames; ++i) {
		if (c->ringLen == c->ringCap) { /* full: drop the oldest */
			c->ringHead = (c->ringHead + 1) % c->ringCap;
			--c->ringLen;
		}
		size_t at = (c->ringHead + c->ringLen) % c->ringCap;
		c->ring[at * 2] = d[i * 2];
		c->ring[at * 2 + 1] = d[i * 2 + 1];
		++c->ringLen;
	}
	pthread_mutex_unlock(&c->aLock);
	return frames;
}

static int16_t input_common(EmuCore* c, unsigned port, unsigned device, unsigned index, unsigned id) {
	(void) index;
	if (port != 0 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t) c->keys;
	return (int16_t) ((c->keys >> id) & 1);
}

/* libretro callbacks carry no context: one set per slot */
#define SLOT_CALLBACKS(n) \
	static bool env_##n(unsigned cmd, void* data) { return env_common(slotCore[n], cmd, data); } \
	static void video_##n(const void* d, unsigned w, unsigned h, size_t p) { video_common(slotCore[n], d, w, h, p); } \
	static void sample_##n(int16_t l, int16_t r) { int16_t s[2] = { l, r }; audio_common(slotCore[n], s, 1); } \
	static size_t batch_##n(const int16_t* d, size_t f) { return audio_common(slotCore[n], d, f); } \
	static void poll_##n(void) {} \
	static int16_t input_##n(unsigned p, unsigned d, unsigned i, unsigned id) { return input_common(slotCore[n], p, d, i, id); }
SLOT_CALLBACKS(0)
SLOT_CALLBACKS(1)

static const char* slotLib[EMU_SLOTS] = { "sm-core-a.so", "sm-core-b.so" };

static bool load_lib(EmuCore* c, int slot) {
	char path[1200];
	snprintf(path, sizeof(path), "%s/%s", coreDir, slotLib[slot]);
	c->lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!c->lib) c->lib = dlopen(slotLib[slot], RTLD_NOW | RTLD_LOCAL); /* Android: the app's own libraries */
	if (!c->lib) {
		fprintf(stderr, "can't load the SNES core %s: %s\n", path, dlerror());
		return false;
	}
#define SYM(field, name) \
	*(void**) &c->field = dlsym(c->lib, name); \
	if (!c->field) { \
		fprintf(stderr, "SNES core: %s missing\n", name); \
		return false; \
	}
	SYM(init, "retro_init")
	SYM(deinit, "retro_deinit")
	SYM(load_game, "retro_load_game")
	SYM(unload_game, "retro_unload_game")
	SYM(run, "retro_run")
	SYM(reset, "retro_reset")
	SYM(set_environment, "retro_set_environment")
	SYM(set_video_refresh, "retro_set_video_refresh")
	SYM(set_audio_sample, "retro_set_audio_sample")
	SYM(set_audio_sample_batch, "retro_set_audio_sample_batch")
	SYM(set_input_poll, "retro_set_input_poll")
	SYM(set_input_state, "retro_set_input_state")
	SYM(serialize_size, "retro_serialize_size")
	SYM(serialize, "retro_serialize")
	SYM(unserialize, "retro_unserialize")
	SYM(get_memory_data, "retro_get_memory_data")
	SYM(get_memory_size, "retro_get_memory_size")
	SYM(get_system_av_info, "retro_get_system_av_info")
	SYM(set_layers, "smds_set_layers")
	SYM(vram, "smds_vram")
	SYM(cgram, "smds_cgram")
	SYM(rom, "smds_rom")
	SYM(rom_size, "smds_rom_size")
	SYM(set_hud_hdma, "smds_set_hud_hdma")
	SYM(set_hud_bg3, "smds_set_hud_bg3")
	SYM(bg, "smds_bg")
	SYM(cheat_reset, "retro_cheat_reset")
	SYM(cheat_set, "retro_cheat_set")
#undef SYM
	return true;
}

uint8_t* emu_rom_read(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	if (n <= 0 || n > 16 << 20) {
		fclose(f);
		return NULL;
	}
	uint8_t* d = malloc((size_t) n);
	if (!d || fread(d, 1, (size_t) n, f) != (size_t) n) {
		free(d);
		fclose(f);
		return NULL;
	}
	fclose(f);
	size_t off = (n & 0x7FFF) == 512 ? 512 : 0; /* copier header */
	if (off) memmove(d, d + off, (size_t) n - off);
	*size = (size_t) n - off;
	return d;
}

bool emu_rom_is_sm(const uint8_t* rom, size_t size) {
	return size >= 0x8000 && !memcmp(rom + 0x7FC0, "Super Metroid", 13);
}

bool emu_open_mem(Emu* e, int slot, const uint8_t* rom, size_t size, bool audio) {
	memset(e, 0, sizeof(*e));
	if (slot < 0 || slot >= EMU_SLOTS || slotCore[slot]) return false;
	if (!lut565[0xFFFF]) {
		for (uint32_t v = 0; v < 65536; ++v) {
			uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
			lut565[v] = 0xFF000000u | (b << 3 | b >> 2) << 16 | (g << 2 | g >> 4) << 8 | (r << 3 | r >> 2);
		}
	}
	EmuCore* c = calloc(1, sizeof(EmuCore));
	if (!c) return false;
	c->render = true;
	pthread_mutex_init(&c->aLock, NULL);
	if (!load_lib(c, slot)) {
		if (c->lib) dlclose(c->lib);
		free(c);
		return false;
	}
	slotCore[slot] = c;
	if (slot == 0) {
		c->set_environment(env_0), c->set_video_refresh(video_0), c->set_audio_sample(sample_0);
		c->set_audio_sample_batch(batch_0), c->set_input_poll(poll_0), c->set_input_state(input_0);
	} else {
		c->set_environment(env_1), c->set_video_refresh(video_1), c->set_audio_sample(sample_1);
		c->set_audio_sample_batch(batch_1), c->set_input_poll(poll_1), c->set_input_state(input_1);
	}
	c->init();
	/* the core keeps a pointer to the data: our own copy */
	c->romCopy = malloc(size);
	if (!c->romCopy) goto fail;
	memcpy(c->romCopy, rom, size);
	c->romSize = size;
	struct retro_game_info gi = { "game.sfc", c->romCopy, size, NULL };
	if (!c->load_game(&gi)) {
		fprintf(stderr, "the SNES core couldn't load the game\n");
		goto fail;
	}
	struct retro_system_av_info av;
	c->get_system_av_info(&av);
	c->rate = (unsigned) (av.timing.sample_rate + 0.5);
	if (audio) {
		c->ringCap = 32768;
		c->ring = malloc(c->ringCap * 4);
	}
	e->core = c;
	e->slot = slot;
	e->fb = c->fb;
	e->audio = audio;
	return true;
fail:
	c->deinit();
	slotCore[slot] = NULL;
	dlclose(c->lib);
	free(c->romCopy);
	free(c);
	return false;
}

bool emu_open(Emu* e, int slot, const char* romPath, const char* savePath, bool audio) {
	size_t size;
	uint8_t* rom = emu_rom_read(romPath, &size);
	if (!rom) {
		fprintf(stderr, "can't read %s\n", romPath);
		return false;
	}
	bool ok = emu_open_mem(e, slot, rom, size, audio);
	free(rom);
	if (!ok) return false;
	if (savePath) {
		snprintf(e->savePath, sizeof(e->savePath), "%s", savePath);
		EmuCore* c = e->core;
		uint8_t* sram = c->get_memory_data(RETRO_MEMORY_SAVE_RAM);
		size_t n = c->get_memory_size(RETRO_MEMORY_SAVE_RAM);
		if (sram && n) {
			c->savedSram = malloc(n);
			FILE* f = fopen(savePath, "rb");
			if (f) {
				size_t got = fread(sram, 1, n, f);
				fclose(f);
				fprintf(stderr, "battery save: %s (%zu bytes)\n", savePath, got);
			}
			if (c->savedSram) memcpy(c->savedSram, sram, n);
		}
	}
	return true;
}

void emu_save_flush(Emu* e) {
	EmuCore* c = e->core;
	if (!c || !e->savePath[0] || !c->savedSram) return;
	uint8_t* sram = c->get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t n = c->get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram || !n || !memcmp(sram, c->savedSram, n)) return;
	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s.tmp", e->savePath);
	FILE* f = fopen(tmp, "wb");
	if (!f) return;
	bool ok = fwrite(sram, 1, n, f) == n;
	ok = fclose(f) == 0 && ok;
	if (ok && rename(tmp, e->savePath) == 0) {
		memcpy(c->savedSram, sram, n);
		fprintf(stderr, "battery save written\n");
	}
}

void emu_close(Emu* e) {
	EmuCore* c = e->core;
	if (!c) return;
	emu_save_flush(e);
	c->unload_game();
	c->deinit();
	dlclose(c->lib);
	slotCore[e->slot] = NULL;
	free(c->ring);
	free(c->romCopy);
	free(c->savedSram);
	pthread_mutex_destroy(&c->aLock);
	free(c);
	e->core = NULL;
	e->fb = NULL;
}

void emu_frame(Emu* e, uint32_t keys) {
	e->core->keys = keys;
	e->core->run();
}

void emu_reset(Emu* e) {
	e->core->reset();
}

void emu_set_render(Emu* e, bool on) {
	e->core->render = on;
}

void emu_set_hud_hdma(Emu* e, bool on) {
	e->core->set_hud_hdma(on);
}

void emu_bg(Emu* e, int bg, EmuBg* out) {
	int v[7];
	e->core->bg(bg, v);
	out->mapBase = v[0], out->tileBase = v[1], out->hofs = v[2], out->vofs = v[3];
	out->mapSize = v[4], out->big = v[5], out->mode = v[6];
}

void emu_set_hud_bg3(Emu* e, bool on) {
	e->core->set_hud_bg3(on);
}

void emu_cheats_set(Emu* e, const char* const* codes, int n) {
	EmuCore* c = e->core;
	c->cheat_reset();
	unsigned idx = 0;
	for (int i = 0; i < n; ++i) {
		/* one code at a time: the core copies a line into 256 bytes */
		char buf[4096];
		snprintf(buf, sizeof(buf), "%s", codes[i]);
		char* save = NULL;
		for (char* t = strtok_r(buf, "+,; \t", &save); t; t = strtok_r(NULL, "+,; \t", &save)) {
			if (strlen(t) < 64) c->cheat_set(idx++, true, t);
		}
	}
}

void emu_set_layers_off(Emu* e, unsigned mask) {
	e->core->set_layers(mask);
}

uint8_t* emu_rom(Emu* e, size_t* size) {
	if (size) *size = e->core->rom_size();
	return e->core->rom();
}

size_t emu_state_size(Emu* e) {
	return e->core->serialize_size();
}

bool emu_state_save(Emu* e, void* buf) {
	return e->core->serialize(buf, e->core->serialize_size());
}

bool emu_state_load(Emu* e, const void* buf) {
	return e->core->unserialize(buf, e->core->serialize_size());
}

bool emu_state_save_file(Emu* e, const char* path) {
	size_t n = emu_state_size(e);
	void* buf = malloc(n);
	if (!buf || !emu_state_save(e, buf)) {
		free(buf);
		return false;
	}
	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "wb");
	bool ok = f && fwrite(buf, 1, n, f) == n;
	if (f) ok = fclose(f) == 0 && ok;
	free(buf);
	if (ok) ok = rename(tmp, path) == 0;
	return ok;
}

bool emu_state_load_file(Emu* e, const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	bool ok = false;
	void* buf = n > 0 ? malloc((size_t) n) : NULL;
	if (buf && fread(buf, 1, (size_t) n, f) == (size_t) n) ok = e->core->unserialize(buf, (size_t) n);
	fclose(f);
	free(buf);
	return ok;
}

uint8_t* emu_wram(Emu* e) {
	return e->core->get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
}

uint8_t* emu_vram(Emu* e) {
	return e->core->vram();
}

const uint16_t* emu_cgram(Emu* e) {
	return e->core->cgram();
}

size_t emu_audio_read(Emu* e, int16_t* out, size_t maxFrames) {
	EmuCore* c = e->core;
	if (!c->ring) return 0;
	pthread_mutex_lock(&c->aLock);
	size_t n = c->ringLen < maxFrames ? c->ringLen : maxFrames;
	for (size_t i = 0; i < n; ++i) {
		size_t at = (c->ringHead + i) % c->ringCap;
		out[i * 2] = c->ring[at * 2];
		out[i * 2 + 1] = c->ring[at * 2 + 1];
	}
	c->ringHead = (c->ringHead + n) % c->ringCap;
	c->ringLen -= n;
	pthread_mutex_unlock(&c->aLock);
	return n;
}

unsigned emu_audio_rate(Emu* e) {
	return e->core->rate;
}
