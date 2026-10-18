# Super Metroid DS

Super Metroid on dual-screen handhelds (the Anbernic RG DS / RG DS Plus), in
the style of [GBA DS](https://github.com/kyannon/gba-ds): the game on one
screen, and on the other its HUD and its own pause map, kept up to date while
you play.

- **The game screen has no HUD bar.** The energy / ammo bar is taken off the
  top of the picture and the room shows through where it was.
- **The second screen** has the HUD bar at the top (energy, reserve tanks,
  missiles, super missiles, power bombs, grapple, X-ray, live as you play),
  and below it the pause screen's map of the area you're in, without the
  button prompts. The map is refreshed every 1.5 s and whenever you enter a
  room, from a hidden second copy of the game: your game is never paused.
- Before there's a map to show (the intro, the title and file screens, Ceres
  station, the cutscenes), the second screen says **MAP NOT AVAILABLE**.
- Where the HUD's mini-map was, a **TAP FOR SETTINGS** button opens the
  settings menu (HOME, or SELECT+START with `menu_combo`, open it too).
- Menus and messages are drawn in the Super Metroid font, in the colours and
  shapes of the game's pause screen.

## Install (ROCKNIX)

1. Unzip `super-metroid-ds-v<version>-linux-aarch64.zip` into `/storage/roms/ports/`.
2. Put your Super Metroid ROM (`.sfc` / `.smc`, the Japan / USA release) in
   `/storage/roms/ports/super-metroid-ds/` (or in `/storage/roms/snes/`).
   A RetroArch battery save (`.srm` with the same name as the ROM) next to it
   is used as it is.
3. Start **Super Metroid** from Ports.

The ROM isn't included: you need your own copy.

## Controls

The SNES layout: A right, B bottom, X top, Y left face button; L / R the
shoulders. L2 swaps the screens, R2 fast-forwards. HOME opens the settings
menu; HOME + START quits, HOME + R / L save / load a state, HOME + X shows the
FPS, HOME + Y swaps the screens.

## Settings

`sm-ds.cfg` next to the program (commented); choices made in the menu are kept in
`states/prefs.cfg`. The menu has three pages (L / R or tap the tabs):

- **Game**: Resume, Save State, Load State, Restart Game, Take Screenshot, Close Super Metroid
- **Display**: Swap Screens, Scale (Pixel Perfect / Full Width / Zoom / Stretch), Pixel Grid, Show FPS
- **System**: Map Animations, Map Refresh, Frame Buffer, Match Display Rate

Leaving saves a resume state (`auto_resume`), loaded the next time.

## Building

- Desktop (development, tests): `scripts/setup-host.sh` (fetches snes9x and
  SDL2 into `deps/`), then `make` and `make tools`.
- ROCKNIX (aarch64): `scripts/build-rocknix.sh`, which fetches an aarch64
  toolchain and produces the zip in `dist/`.

Testing without a screen:

    SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy SMDS_WINDOW=1024x1536 \
      build/host/sm-ds --state-dir=/tmp/st --load-state=X.st --input-script=SCRIPT game.sfc

Script lines are `TICK BUTTONS` (A, B+UP, - for none), `TICK SHOT:file.bmp`,
`TICK TAP:x,y` (fractions of the window), `TICK QUIT`; `tools/script.py`
writes them from relative steps (`wait N`, `hold BTNS N`, `shot FILE`,
`tap X,Y`, `quit`). `SMDS_DEBUG=1` logs map captures. `build/host/probe`
drives the core alone, frame by frame, and saves states the app can load.

## How it works

- **The SNES core** is snes9x's libretro core (`core/`, plus
  `core/smds_ext.cpp` for video RAM, the palette and the loaded ROM), built
  twice as `sm-core-a.so` (the game) and `sm-core-b.so` (the hidden copy) so
  the two copies don't share globals; `src/emu.c` loads them with `dlopen`.
- **The HUD patch** (`src/sm.c`): the game switches the screen to its HUD's
  settings with an interrupt at line 0 and back at line 31. Each "start of
  HUD" handler in the interrupt table is turned into a jump to its "end of
  HUD" partner (main gameplay, door transitions, Draygon's room), so the
  room's settings hold from the top. The game still draws the HUD into video
  RAM, and `sm_draw_hud` draws it from there (BG3's tiles and palette) for
  the second screen. Ceres doesn't draw the room under the strip, so it's left
  black there.
- **The map** (`src/mapcap.c`): the hidden copy gets a snapshot of the game,
  START is pressed, and once the pause screen's map is up and fully faded in
  (game state $0F, brightness $0F), rows 32-198 (the area name and the map's
  frame) are copied, 12 frames 2 apart for the blinking markers.

## Licences

Super Metroid DS is GPL-3.0 (LICENSE), based on GBA DS. The SNES core is
[snes9x](https://github.com/snes9xgit/snes9x), whose own licence allows free
non-commercial use; it is a separate library loaded at run time, and ships with
its licence. The Super Metroid font (`assets/fonts`) was supplied for this
project.
