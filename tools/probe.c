/* Probe: drives the SNES core frame by frame from a script (development).
 *   build/host/probe build/host/sm-core-a.so ROM SRM OUTDIR SCRIPT   (SCRIPT - : stdin)
 * States it saves (save NAME -> OUTDIR/NAME.st) load in the app with --load-state.
 * patch OFFSET HEXBYTES writes into the loaded ROM (e.g. the HUD patch, src/sm.c).
 * Script: wait N | hold BTN+BTN N | shot NAME | save NAME | load NAME |
 *         peek ADDR LEN (WRAM offset, hex) | poke ADDR VAL | layers MASK (disabled bits) | dumpvram NAME |
 *         patch OFFSET HEX | print TEXT */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/libretro.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../src/stb_image_write.h"

static void* h;
#define SYM(n) static typeof(n)* p_##n;
SYM(retro_init) SYM(retro_load_game) SYM(retro_run) SYM(retro_set_environment) SYM(retro_set_video_refresh)
SYM(retro_set_audio_sample) SYM(retro_set_audio_sample_batch) SYM(retro_set_input_poll) SYM(retro_set_input_state)
SYM(retro_serialize_size) SYM(retro_serialize) SYM(retro_unserialize) SYM(retro_get_memory_data) SYM(retro_get_memory_size)
SYM(retro_get_system_av_info)

static uint32_t fb[512 * 480];
static int fbw, fbh;
static uint16_t keys;
static const char* outdir;
static unsigned long long frame;

static void logcb(enum retro_log_level l, const char* fmt, ...) {
	if (l < RETRO_LOG_WARN) return;
	va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
static bool env(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*) data)->log = logcb; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(enum retro_pixel_format*) data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**) data = "/tmp"; return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*) data = true; return true;
	default: return false;
	}
}
static void video(const void* d, unsigned w, unsigned hh, size_t pitch) {
	if (!d) return;
	fbw = w, fbh = hh;
	for (unsigned y = 0; y < hh; ++y) {
		const uint16_t* row = (const uint16_t*) ((const uint8_t*) d + y * pitch);
		for (unsigned x = 0; x < w; ++x) {
			uint16_t c = row[x];
			uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
			fb[y * w + x] = 0xFF000000 | ((b << 3 | b >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (r << 3 | r >> 2);
		}
	}
}
static void asample(int16_t l, int16_t r) {}
static size_t abatch(const int16_t* d, size_t n) { return n; }
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
	if (port || dev != RETRO_DEVICE_JOYPAD) return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return keys;
	return (keys >> id) & 1;
}
static uint16_t parse_btn(const char* s) {
	static const char* names[] = { "B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "X", "L", "R" };
	uint16_t k = 0;
	char buf[128]; snprintf(buf, sizeof buf, "%s", s);
	for (char* t = strtok(buf, "+"); t; t = strtok(NULL, "+"))
		for (int i = 0; i < 12; ++i) if (!strcmp(t, names[i])) k |= 1 << i;
	return k;
}
static void shot(const char* name) {
	char p[512]; snprintf(p, sizeof p, "%s/%s.png", outdir, name);
	stbi_write_png(p, fbw, fbh, 4, fb, fbw * 4);
}
int main(int argc, char** argv) {
	if (argc < 6) return 1;
	outdir = argv[4];
	h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define LOAD(n) p_##n = dlsym(h, #n);
	LOAD(retro_init) LOAD(retro_load_game) LOAD(retro_run) LOAD(retro_set_environment) LOAD(retro_set_video_refresh)
	LOAD(retro_set_audio_sample) LOAD(retro_set_audio_sample_batch) LOAD(retro_set_input_poll) LOAD(retro_set_input_state)
	LOAD(retro_serialize_size) LOAD(retro_serialize) LOAD(retro_unserialize) LOAD(retro_get_memory_data) LOAD(retro_get_memory_size)
	LOAD(retro_get_system_av_info)
	void (*set_layers)(unsigned) = dlsym(h, "smds_set_layers");
	uint8_t* (*vram)(void) = dlsym(h, "smds_vram");
	uint16_t* (*cgram)(void) = dlsym(h, "smds_cgram");
	p_retro_set_environment(env);
	p_retro_set_video_refresh(video);
	p_retro_set_audio_sample(asample);
	p_retro_set_audio_sample_batch(abatch);
	p_retro_set_input_poll(poll);
	p_retro_set_input_state(input);
	p_retro_init();
	FILE* f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
	void* rom = malloc(n); fread(rom, 1, n, f); fclose(f);
	struct retro_game_info gi = { argv[2], rom, (size_t) n, NULL };
	if (!p_retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
	f = fopen(argv[3], "rb");
	if (f) { fread(p_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), 1, p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM), f); fclose(f); }
	uint8_t* wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
	FILE* sc = !strcmp(argv[5], "-") ? stdin : fopen(argv[5], "r");
	char line[512];
	while (fgets(line, sizeof line, sc)) {
		char cmd[32], a[256] = "", b[256] = "";
		if (sscanf(line, "%31s %255s %255s", cmd, a, b) < 1 || cmd[0] == '#') continue;
		if (!strcmp(cmd, "wait") || !strcmp(cmd, "hold")) {
			int cnt = atoi(!strcmp(cmd, "wait") ? a : b);
			keys = !strcmp(cmd, "hold") ? parse_btn(a) : 0;
			for (int i = 0; i < cnt; ++i) p_retro_run(), ++frame;
			keys = 0;
		} else if (!strcmp(cmd, "shot")) shot(a);
		else if (!strcmp(cmd, "save") || !strcmp(cmd, "load")) {
			char p[512]; snprintf(p, sizeof p, "%s/%s.st", outdir, a);
			size_t sz = p_retro_serialize_size();
			void* buf = malloc(sz);
			if (cmd[0] == 's') { p_retro_serialize(buf, sz); f = fopen(p, "wb"); fwrite(buf, 1, sz, f); fclose(f); }
			else { f = fopen(p, "rb"); if (!f) { fprintf(stderr, "no %s\n", p); continue; } fread(buf, 1, sz, f); fclose(f); p_retro_unserialize(buf, sz); }
			free(buf);
		} else if (!strcmp(cmd, "peek")) {
			unsigned ad = strtoul(a, NULL, 16), len = atoi(b);
			printf("frame %llu %04x:", frame, ad);
			for (unsigned i = 0; i < len; ++i) printf(" %02x", wram[(ad + i) & 0x1FFFF]);
			printf("\n");
		} else if (!strcmp(cmd, "poke")) {
			wram[strtoul(a, NULL, 16) & 0x1FFFF] = strtoul(b, NULL, 16);
		} else if (!strcmp(cmd, "layers")) {
			set_layers(strtoul(a, NULL, 16));
		} else if (!strcmp(cmd, "dumpvram")) {
			char p[512]; snprintf(p, sizeof p, "%s/%s.vram", outdir, a);
			f = fopen(p, "wb"); fwrite(vram(), 1, 65536, f); fwrite(cgram(), 2, 256, f); fwrite(wram, 1, 0x20000, f); fclose(f);
		} else if (!strcmp(cmd, "patch")) {
			uint8_t* (*romp)(void) = dlsym(h, "smds_rom");
			unsigned off = strtoul(a, NULL, 16);
			for (char* q = b; q[0] && q[1]; q += 2) { unsigned v; sscanf(q, "%2x", &v); romp()[off++] = v; }
		} else if (!strcmp(cmd, "print")) {
			printf("frame %llu: %s\n", frame, a);
		}
		fflush(stdout);
	}
	return 0;
}
