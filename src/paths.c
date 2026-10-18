#include "paths.h"

#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

bool file_exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

void mkdirs(const char* path) {
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char* p = tmp + 1; *p; ++p) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

bool copy_file(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	if (!in) return false;
	FILE* out = fopen(to, "wb");
	if (!out) {
		fclose(in);
		return false;
	}
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	return true;
}

void game_paths(const Config* cfg, const char* rom, GamePaths* o) {
	char a[1024], b[1024];
	snprintf(a, sizeof(a), "%s", rom);
	snprintf(b, sizeof(b), "%s", rom);
	const char* romDir = dirname(a);
	snprintf(o->base, sizeof(o->base), "%s", basename(b));
	char* dot = strrchr(o->base, '.');
	if (dot) *dot = 0;

	/* the battery save is a .srm next to the ROM (or in save_dir), as RetroArch keeps it */
	const char* saveDir = cfg->saveDir[0] ? cfg->saveDir : romDir;
	mkdirs(saveDir);
	mkdirs(cfg->stateDir);
	snprintf(o->sav, sizeof(o->sav), "%s/%s.srm", saveDir, o->base);
	snprintf(o->state, sizeof(o->state), "%s/%s.state", cfg->stateDir, o->base);
	snprintf(o->autoState, sizeof(o->autoState), "%s/%s.auto.state", cfg->stateDir, o->base);
	/* saves kept elsewhere (save_dir, e.g. BananaOS's /data/saves): a .srm
	 * next to the ROM is carried over the first time */
	if (cfg->saveDir[0] && !file_exists(o->sav)) {
		char beside[1400];
		snprintf(beside, sizeof(beside), "%s/%s.srm", romDir, o->base);
		if (file_exists(beside) && copy_file(beside, o->sav)) fprintf(stderr, "battery save carried over: %s -> %s\n", beside, o->sav);
	}
}
