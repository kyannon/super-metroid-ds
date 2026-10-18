#ifndef SMDS_GAME_H
#define SMDS_GAME_H

#include "config.h"

enum GameExit {
	GAME_QUIT_APP,
};

/* Plays the game until the player closes it. */
enum GameExit game_run(Config* cfg, const char* rom);

#endif
