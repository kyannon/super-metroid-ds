/* RetroAchievements: the account, and what the app shows of it.
 *
 * This is the seam the real thing goes into (rcheevos' rc_client, see
 * docs/retroachievements.md). For now there's no connection: logging in
 * keeps the user name (never the password) and says it's a preview, and
 * the pop-ups the game screen will show can be tried out with a sample. */
#ifndef SMDS_RA_H
#define SMDS_RA_H

#include <stdbool.h>
#include <stddef.h>

#define RA_NAME_MAX 32 /* RetroAchievements user names are at most 20 characters */
#define RA_PASS_MAX 64

/* Loads the account kept in DIR/retroachievements.cfg. */
void ra_init(const char* stateDir);

bool ra_logged_in(void);
const char* ra_user(void);

/* Logging in: the result comes from ra_login_result. Until the client is
 * in, it "succeeds" as a preview (no connection) and keeps only the name. */
void ra_login(const char* user, const char* password);
/* 0: still waiting; 1: logged in; -1: failed (why in msg) */
int ra_login_result(char* msg, size_t n);
void ra_logout(void);
/* true while the build has no connection to RetroAchievements yet */
bool ra_preview(void);

/* What the game screen shows at the top: an achievement unlocked, or a
 * notice (logged in, the game's set...). */
typedef enum { RA_POP_UNLOCK, RA_POP_NOTICE } RaPopKind;
typedef struct {
	RaPopKind kind;
	char title[64];   /* the achievement's name, or the notice */
	char detail[96];  /* its description */
	int points;       /* 0 for a notice */
} RaPop;
void ra_push(const RaPop* p);
/* the pop-up to show now, if any, and how far along it is (frames since it came up) */
const RaPop* ra_current(int* age);
/* a frame of the game screen: pop-ups move along */
void ra_tick(void);

#endif
