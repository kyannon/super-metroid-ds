#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(Config* c) {
	memset(c, 0, sizeof(*c));
	strcpy(c->layout, "auto");
	strcpy(c->scale, "integer");
	strcpy(c->mapScale, "integer");
	strcpy(c->hotkey, "guide");
	strcpy(c->menuCombo, "none");
	strcpy(c->romDir, "/storage/roms/snes:/storage/roms/ports/super-metroid");
	strcpy(c->touchScreen, "second");
	strcpy(c->touchMap, "window");
	c->map = true;
	c->mapAnim = true;
	c->mapInterval = 90;
	c->sway = true;
	c->audioLatency = 64;
	c->ffSpeed = 3;
	c->rewind = true;
	c->rewindMb = 64;
	c->rewindStep = 2;
	c->autoResume = true;
	c->homeButton = -1;
	c->lcdStrength = 35;
	c->scanStrength = 45;
}

static bool parse_bool(const char* v) {
	return !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on");
}

static bool set_option(Config* c, const char* key, const char* val) {
#define STR(name, field) \
	if (!strcmp(key, name)) { \
		snprintf(c->field, sizeof(c->field), "%s", val); \
		return true; \
	}
#define BOOL(name, field) \
	if (!strcmp(key, name)) { \
		c->field = parse_bool(val); \
		return true; \
	}
#define INT(name, field) \
	if (!strcmp(key, name)) { \
		c->field = atoi(val); \
		return true; \
	}
	STR("layout", layout)
	BOOL("swap", swap)
	STR("scale", scale)
	STR("map_scale", mapScale)
	BOOL("smooth", smooth)
	if (!strcmp(key, "lcd_grid")) { /* the older setting */
		c->shader = parse_bool(val) ? 1 : 0;
		return true;
	}
	if (!strcmp(key, "shader")) {
		c->shader = !strcmp(val, "grid") || !strcmp(val, "pixel_grid") ? 1 : !strcmp(val, "scanlines") ? 2 : 0;
		return true;
	}
	INT("scanline_strength", scanStrength)
	INT("lcd_strength", lcdStrength)
	BOOL("vsync", vsync)
	BOOL("pin_cores", pinCores)
	BOOL("frame_buffer", frameBuffer)
	BOOL("map", map)
	BOOL("map_animations", mapAnim)
	BOOL("match_display", matchDisplay)
	INT("map_interval", mapInterval)
	BOOL("ab_swap", abSwap)
	STR("hotkey", hotkey)
	STR("menu_combo", menuCombo)
	INT("home_button", homeButton)
	BOOL("show_fps", showFps)
	BOOL("sway", sway)
	INT("audio_latency", audioLatency)
	INT("ff_speed", ffSpeed)
	BOOL("rewind", rewind)
	INT("rewind_mb", rewindMb)
	INT("rewind_step", rewindStep)
	STR("save_dir", saveDir)
	STR("state_dir", stateDir)
	STR("rom_dir", romDir)
	STR("rom", rom)
	STR("core_dir", coreDir)
	STR("screenshot_dir", screenshotDir)
	STR("cheat_dir", cheatDir)
	STR("touch_screen", touchScreen)
	STR("touch_map", touchMap)
	BOOL("auto_resume", autoResume)
	STR("load_state", loadState)
	INT("quit_after", quitAfter)
	STR("input_script", inputScript)
	return false;
#undef STR
#undef BOOL
#undef INT
}

void config_load_file(Config* c, const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		char* p = line;
		while (isspace((unsigned char) *p)) ++p;
		if (!*p || *p == '#' || *p == ';') continue;
		char* eq = strchr(p, '=');
		if (!eq) continue;
		*eq = 0;
		char* key = p;
		char* val = eq + 1;
		char* end = eq - 1;
		while (end >= key && isspace((unsigned char) *end)) *end-- = 0;
		while (isspace((unsigned char) *val)) ++val;
		end = val + strlen(val) - 1;
		while (end >= val && isspace((unsigned char) *end)) *end-- = 0;
		if (!set_option(c, key, val)) {
			fprintf(stderr, "%s: unknown option '%s'\n", path, key);
		}
	}
	fclose(f);
}

bool config_parse_args(Config* c, int argc, char** argv) {
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			return false;
		}
		if (!strncmp(a, "--", 2)) {
			char key[64];
			const char* eq = strchr(a, '=');
			const char* val = "1";
			size_t len = eq ? (size_t) (eq - a - 2) : strlen(a + 2);
			if (len >= sizeof(key)) len = sizeof(key) - 1;
			memcpy(key, a + 2, len);
			key[len] = 0;
			for (char* k = key; *k; ++k) {
				if (*k == '-') *k = '_';
			}
			if (eq) {
				val = eq + 1;
			} else if (!strncmp(key, "no_", 3)) {
				memmove(key, key + 3, strlen(key + 3) + 1);
				val = "0";
			}
			if (!set_option(c, key, val)) {
				fprintf(stderr, "unknown option %s\n", a);
				return false;
			}
		} else {
			snprintf(c->rom, sizeof(c->rom), "%s", a);
		}
	}
	return true;
}

void config_usage(const char* argv0) {
	fprintf(stderr,
	        "Super Metroid DS " SMDS_VERSION "\n"
	        "usage: %s [options] [super-metroid.sfc]\n"
	        "\n"
	        "Without a ROM, the first Super Metroid ROM found in --rom-dir (or next to\n"
	        "the program) is played.\n"
	        "\n"
	        "Options (also accepted as key=value lines in sm-ds.cfg):\n"
	        "  --rom-dir=DIR[:DIR]    where to look for the ROM\n"
	        "  --screenshot-dir=DIR   where Take Screenshot saves (<app>/screenshots)\n"
	        "  --layout=auto|span-h|span-v|dual|single   screen arrangement (auto)\n"
	        "  --swap                 the second screen first (top)\n"
	        "  --touch-screen=first|second   which screen has the touch panel (second)\n"
	        "  --touch-map=window|screen     touch coordinates span the window or one screen\n"
	        "  --scale=integer|fit|zoom|stretch      game scaling (integer)\n"
	        "  --map-scale=integer|fit|zoom|stretch  second screen scaling (integer)\n"
	        "  --lcd-grid             pixel grid (--lcd-strength=0..100)\n"
	        "  --smooth               plain linear filtering\n"
	        "  --vsync                sync presentation to the display\n"
	        "  --no-map               no second screen\n"
	        "  --map-interval=N       frames between map refreshes (90)\n"
	        "  --ab-swap              SNES A on the bottom face button\n"
	        "  --hotkey=guide|back    menu / hotkey button (guide = HOME)\n"
	        "  --menu-combo=select+start|none   buttons that open the menu together (none)\n"
	        "  --home-button=N        raw joystick button that acts as HOME\n"
	        "  --no-auto-resume       don't save on exit / resume on launch\n"
	        "  --show-fps\n"
	        "  --no-sway              don't ask sway to span both outputs\n"
	        "  --audio-latency=MS     (64)\n"
	        "  --ff-speed=N           fast-forward multiplier (3)\n"
	        "  --no-rewind            L2 doesn't rewind\n"
	        "  --rewind-mb=N          memory for rewinding (64)\n"
	        "  --save-dir=DIR         battery saves (next to the ROM)\n"
	        "  --state-dir=DIR        save states and settings (<app>/states)\n"
	        "  --core-dir=DIR         the SNES core libraries (next to the program)\n"
	        "\n"
	        "Testing: --load-state=FILE --quit-after=TICKS --input-script=FILE\n"
	        "  script lines: 'TICK TOKEN' with TOKEN a button combo held until the next\n"
	        "  line (A+RIGHT, -), HOME, TAP:x,y (0..1 window coords), SHOT:file.bmp, QUIT\n",
	        argv0);
}
