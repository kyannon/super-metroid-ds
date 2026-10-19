/* Cheats from RetroArch .cht files (see cheats.h); parsing and the saved
 * choices as in GBA DS. */
#include "cheats.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static void scan(Cheats* c, const char* dir, int depth) {
	DIR* d = opendir(dir);
	if (!d) return;
	struct dirent* de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(full, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 2) scan(c, full, depth + 1);
			continue;
		}
		const char* ext = strrchr(de->d_name, '.');
		if (ext && !strcasecmp(ext, ".cht") && c->nFiles < CHEAT_MAX_FILES) {
			snprintf(c->files[c->nFiles++], sizeof(c->files[0]), "%s", full);
		}
	}
	closedir(d);
}

static int cmp_paths(const void* a, const void* b) {
	return strcasecmp((const char*) a, (const char*) b);
}

static void unquote(const char* v, char* out, size_t size) {
	while (isspace((unsigned char) *v)) ++v;
	size_t o = 0;
	if (*v == '"') {
		++v;
		while (*v && *v != '"' && o + 1 < size) {
			if (*v == '\\' && v[1]) ++v;
			out[o++] = *v++;
		}
	} else {
		while (*v && *v != '\n' && *v != '\r' && o + 1 < size) out[o++] = *v++;
		while (o && isspace((unsigned char) out[o - 1])) --o;
	}
	out[o] = 0;
}

/* cheatN_desc / cheatN_code / cheatN_enable lines; cheats without a code
 * are left out */
static void parse_file(Cheats* c, int fileIdx) {
	FILE* f = fopen(c->files[fileIdx], "r");
	if (!f) return;
	enum { MAXC = 512 };
	struct Tmp {
		char desc[128];
		char* code;
		bool on;
	}* tmp = calloc(MAXC, sizeof(*tmp));
	if (!tmp) {
		fclose(f);
		return;
	}
	int maxIdx = -1;
	char line[4096];
	while (fgets(line, sizeof(line), f)) {
		char* p = line;
		while (isspace((unsigned char) *p)) ++p;
		if (strncmp(p, "cheat", 5) || !isdigit((unsigned char) p[5])) continue;
		char* end;
		long i = strtol(p + 5, &end, 10);
		if (i < 0 || i >= MAXC || *end != '_') continue;
		char* eq = strchr(end, '=');
		if (!eq) continue;
		char key[32];
		size_t kl = (size_t) (eq - end - 1);
		while (kl && isspace((unsigned char) end[1 + kl - 1])) --kl;
		if (kl >= sizeof(key)) continue;
		memcpy(key, end + 1, kl);
		key[kl] = 0;
		char val[4096];
		unquote(eq + 1, val, sizeof(val));
		if (!strcmp(key, "desc")) snprintf(tmp[i].desc, sizeof(tmp[i].desc), "%s", val);
		else if (!strcmp(key, "code")) free(tmp[i].code), tmp[i].code = strdup(val);
		else if (!strcmp(key, "enable")) tmp[i].on = !strcasecmp(val, "true") || !strcmp(val, "1");
		else continue;
		if (i > maxIdx) maxIdx = (int) i;
	}
	fclose(f);
	for (int i = 0; i <= maxIdx; ++i) {
		if (!tmp[i].code || !tmp[i].code[0]) {
			free(tmp[i].code);
			continue;
		}
		Cheat* grown = realloc(c->items, sizeof(*c->items) * (c->n + 1));
		if (!grown) {
			free(tmp[i].code);
			continue;
		}
		c->items = grown;
		Cheat* ch = &c->items[c->n++];
		memset(ch, 0, sizeof(*ch));
		snprintf(ch->desc, sizeof(ch->desc), "%s", tmp[i].desc[0] ? tmp[i].desc : "(no name)");
		ch->code = tmp[i].code;
		ch->on = tmp[i].on;
		ch->file = fileIdx;
	}
	free(tmp);
}

/* <state_dir>/<game>.cheats: a header line, then "<cht file name>\t<desc>"
 * for each cheat that is on. */
static const char* file_name(const char* path) {
	const char* b = strrchr(path, '/');
	return b ? b + 1 : path;
}

void cheats_save(const Cheats* c) {
	if (!c->prefs[0] || !c->n) return;
	FILE* f = fopen(c->prefs, "w");
	if (!f) return;
	fprintf(f, "# Super Metroid DS cheats that are on: cht file<TAB>description\n");
	for (int i = 0; i < c->n; ++i) {
		if (c->items[i].on) fprintf(f, "%s\t%s\n", file_name(c->files[c->items[i].file]), c->items[i].desc);
	}
	fclose(f);
}

static void load_prefs(Cheats* c) {
	FILE* f = fopen(c->prefs, "r");
	if (!f) return;
	for (int i = 0; i < c->n; ++i) c->items[i].on = false;
	char line[1400];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#') continue;
		line[strcspn(line, "\r\n")] = 0;
		char* tab = strchr(line, '\t');
		if (!tab) continue;
		*tab = 0;
		for (int i = 0; i < c->n; ++i) {
			if (!strcmp(c->items[i].desc, tab + 1) && !strcmp(file_name(c->files[c->items[i].file]), line)) {
				c->items[i].on = true;
			}
		}
	}
	fclose(f);
}

void cheats_load(Cheats* c, const char* dir, const char* prefsPath) {
	memset(c, 0, sizeof(*c));
	snprintf(c->prefs, sizeof(c->prefs), "%s", prefsPath ? prefsPath : "");
	scan(c, dir, 0);
	qsort(c->files, c->nFiles, sizeof(c->files[0]), cmp_paths);
	for (int i = 0; i < c->nFiles; ++i) parse_file(c, i);
	load_prefs(c);
	fprintf(stderr, "cheats: %d in %d file(s) in %s\n", c->n, c->nFiles, dir);
}

void cheats_apply(const Cheats* c, Emu* e) {
	const char** codes = c->n ? malloc(sizeof(*codes) * c->n) : NULL;
	int n = 0;
	for (int i = 0; i < c->n && codes; ++i) {
		if (c->items[i].on) codes[n++] = c->items[i].code;
	}
	emu_cheats_set(e, codes, n);
	if (n) fprintf(stderr, "cheats: %d on\n", n);
	free(codes);
}

int cheats_on(const Cheats* c) {
	int n = 0;
	for (int i = 0; i < c->n; ++i) n += c->items[i].on;
	return n;
}

void cheats_free(Cheats* c) {
	for (int i = 0; i < c->n; ++i) free(c->items[i].code);
	free(c->items);
	c->items = NULL;
	c->n = 0;
}
