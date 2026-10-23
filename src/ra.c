#include "ra.h"

#include <stdio.h>
#include <string.h>

static char cfgPath[1100];
static char user[RA_NAME_MAX + 1];
static bool loggedIn;
static int pending; /* a login asked for: its result, given on the next call */

#define POP_QUEUE 8
static RaPop queue[POP_QUEUE];
static int qHead, qLen, age;
/* frames a pop-up is up: sliding in, staying, sliding out (game.c draws it) */
#define POP_FRAMES (12 + 300 + 12)

static void save(void) {
	FILE* f = fopen(cfgPath, "w");
	if (!f) return;
	fprintf(f, "# RetroAchievements: the account last logged in (no password is kept)\n");
	if (loggedIn) fprintf(f, "user = %s\n", user);
	fclose(f);
}

void ra_init(const char* stateDir) {
	snprintf(cfgPath, sizeof(cfgPath), "%s/retroachievements.cfg", stateDir);
	FILE* f = fopen(cfgPath, "r");
	if (!f) return;
	char line[200];
	while (fgets(line, sizeof(line), f)) {
		char name[RA_NAME_MAX + 1];
		if (sscanf(line, " user = %32s", name) == 1) {
			snprintf(user, sizeof(user), "%s", name);
			loggedIn = true;
		}
	}
	fclose(f);
}

bool ra_logged_in(void) {
	return loggedIn;
}

const char* ra_user(void) {
	return user;
}

bool ra_preview(void) {
	return true;
}

void ra_login(const char* name, const char* password) {
	(void) password; /* the real client sends it once for a token; nothing keeps it */
	if (!name[0]) {
		pending = -1;
		return;
	}
	snprintf(user, sizeof(user), "%s", name);
	pending = 1;
}

int ra_login_result(char* msg, size_t n) {
	int r = pending;
	pending = 0;
	if (r == 1) {
		loggedIn = true;
		save();
		snprintf(msg, n, "PREVIEW: NOT CONNECTED YET");
	} else if (r == -1) {
		snprintf(msg, n, "ENTER A USER NAME");
	}
	return r;
}

void ra_logout(void) {
	loggedIn = false;
	user[0] = 0;
	save();
}

void ra_push(const RaPop* p) {
	if (qLen == POP_QUEUE) return;
	queue[(qHead + qLen++) % POP_QUEUE] = *p;
}

const RaPop* ra_current(int* a) {
	if (!qLen) return NULL;
	if (a) *a = age;
	return &queue[qHead];
}

void ra_tick(void) {
	if (!qLen) return;
	if (++age >= POP_FRAMES) {
		age = 0;
		qHead = (qHead + 1) % POP_QUEUE;
		--qLen;
	}
}
