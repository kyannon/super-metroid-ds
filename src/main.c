/* Super Metroid DS: Super Metroid on dual-screen handhelds, with the HUD
 * and the pause map on the second screen.
 *
 *   game.c     play session, settings menu, auto save/resume
 *   emu.c      the SNES core (snes9x, libretro), loaded at run time
 *   mapcap.c   pause-map capture on a hidden second copy of the game
 *   sm.c       the HUD patch, the HUD drawn from video RAM
 *   ui.c       windows, screens, touch/buttons, drawing
 */
#include "android.h"
#include "config.h"
#include "emu.h"
#include "game.h"
#include "paths.h"
#include "ui.h"

#include <dirent.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void on_signal(int sig) {
	(void) sig;
	uiQuit = 1;
}

static bool rom_name(const char* n) {
	const char* dot = strrchr(n, '.');
	return dot && (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc"));
}

static bool is_sm(const char* path) {
	size_t n;
	uint8_t* d = emu_rom_read(path, &n);
	bool yes = d && emu_rom_is_sm(d, n);
	free(d);
	return yes;
}

/* The first Super Metroid ROM in dir (one level of subfolders too). */
static bool find_rom_in(const char* dir, int depth, char* out, size_t size) {
	DIR* d = opendir(dir);
	if (!d) return false;
	struct dirent* de;
	bool found = false;
	while (!found && (de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char path[1400];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (rom_name(de->d_name)) {
			if (is_sm(path)) snprintf(out, size, "%s", path), found = true;
		} else if (depth > 0 && de->d_type == DT_DIR) {
			found = find_rom_in(path, depth - 1, out, size);
		}
	}
	closedir(d);
	return found;
}

static bool find_rom(const Config* cfg, const char* exeDir, char* out, size_t size) {
	char dirs[2200];
	snprintf(dirs, sizeof(dirs), "%s:%s/roms:%s", exeDir, exeDir, cfg->romDir);
	for (char *save = NULL, *p = strtok_r(dirs, ":", &save); p; p = strtok_r(NULL, ":", &save)) {
		if (find_rom_in(p, 1, out, size)) return true;
	}
	return false;
}

/* No ROM: say where to put one, until a button or a tap. */
static void no_rom_screen(const char* exeDir) {
	char where[1200];
	snprintf(where, sizeof(where), "Put your Super Metroid ROM (.sfc) in %s", exeDir);
	while (!uiQuit && ui_events()) {
		ui_tick();
		ui_read_buttons();
		Pointer p;
		bool tapped = false;
		while (ui_next_pointer(&p)) tapped |= p.kind == 2;
		if (tapped || btn_pressed(B_A) || btn_pressed(B_B) || btn_pressed(B_START) || btn_pressed(B_HOME)) break;
		ui_clear();
		for (int s = 0; s < nScreens; ++s) {
			SDL_Rect r = screens[s].rect;
			int u = SDL_max(1, r.h / 224);
			ui_sm_backdrop(s, (SDL_Rect) { 0, 0, r.w, r.h }, u);
			ui_font_text(s, TF_BOLD, r.w / 2, r.h / 2 - u * 12, tf_cap(u * 11), "ROM NOT FOUND", SM_YELLOW, 0);
			ui_font_wrap(s, TF_BODY, u * 12, r.h / 2 + u * 4, r.w - u * 24, tf_cap(SDL_max(11, u * 6)), where, SM_WHITE);
		}
		ui_present();
		SDL_Delay(16);
	}
}

int main(int argc, char** argv) {
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--version")) {
			printf("Super Metroid DS %s\n", SMDS_VERSION);
			return 0;
		}
	}
#ifdef __ANDROID__
	android_init_log();
#endif
	fprintf(stderr, "Super Metroid DS %s\n", SMDS_VERSION);
	Config cfg;
	config_defaults(&cfg);
#ifdef __ANDROID__
	android_config_defaults(&cfg);
#endif

	char exeDir[1024] = ".";
	char exe[1024];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0) {
		exe[n] = 0;
		snprintf(exeDir, sizeof(exeDir), "%s", dirname(exe));
	}
#ifdef __ANDROID__
	if (android_home()) snprintf(exeDir, sizeof(exeDir), "%s", android_home());
#endif

	/* defaults < sm-ds.cfg < states/prefs.cfg (the settings menu) < command line */
	char path[1100];
	if (getenv("SMDS_CONFIG")) {
		config_load_file(&cfg, getenv("SMDS_CONFIG"));
	} else {
		snprintf(path, sizeof(path), "%s/sm-ds.cfg", exeDir);
		config_load_file(&cfg, path);
		if (getenv("HOME")) {
			snprintf(path, sizeof(path), "%s/.config/sm-ds/sm-ds.cfg", getenv("HOME"));
			config_load_file(&cfg, path);
		}
	}
	if (!config_parse_args(&cfg, argc, argv)) {
		config_usage(argv[0]);
		return 1;
	}
	if (!cfg.stateDir[0]) snprintf(cfg.stateDir, sizeof(cfg.stateDir), "%s/states", exeDir);
	if (!cfg.screenshotDir[0]) snprintf(cfg.screenshotDir, sizeof(cfg.screenshotDir), "%s/screenshots", exeDir);
	if (!cfg.cheatDir[0]) snprintf(cfg.cheatDir, sizeof(cfg.cheatDir), "%s/cheats", exeDir);
	mkdirs(cfg.cheatDir); /* there to put .cht files in */
	mkdirs(cfg.stateDir);
	snprintf(path, sizeof(path), "%s/prefs.cfg", cfg.stateDir);
	config_load_file(&cfg, path);
	config_parse_args(&cfg, argc, argv);
	emu_set_core_dir(cfg.coreDir[0] ? cfg.coreDir : exeDir);

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGHUP, on_signal);

	if (!ui_init(&cfg, true)) return 1;
	if (cfg.inputScript[0]) ui_load_script(cfg.inputScript);

	char rom[1400];
	if (cfg.rom[0]) snprintf(rom, sizeof(rom), "%s", cfg.rom);
	else if (!find_rom(&cfg, exeDir, rom, sizeof(rom))) rom[0] = 0;
	if (rom[0]) {
		fprintf(stderr, "ROM: %s\n", rom);
		game_run(&cfg, rom);
	} else {
		fprintf(stderr, "no Super Metroid ROM found (next to the program, in roms/, or in %s)\n", cfg.romDir);
		no_rom_screen(exeDir);
	}
	ui_shutdown();
	return 0;
}
