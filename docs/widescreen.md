# Widescreen (a prototype, on the `widescreen-prototype` branch)

## Trying it

1. Put bsnes-hd's Super Metroid widescreen patch in the `patches` folder
   next to the program: `Super_Metroid_WS.bps`, and its `Super_Metroid_WS.bso`
   (same name). The patch has to be for your ROM: the app checks the ROM's
   checksum against the one the patch is for.
2. Display → Scale until it reads **Match Width**. The option only shows up
   when a matching patch is in the folder.
3. Display → Show FPS shows the emulation's time a frame (EMU ... MS) and
   WIDE while it's on. Compare it with Pixel Perfect.

**Match Width** draws the game as wide as fits the game screen at whole
pixels, up to the patch's 16:9:

| Screen | Scale | Picture | Columns added a side |
| --- | --- | --- | --- |
| RG DS, 640 x 480 | 2x | 320 x 224 | 32 |
| RG DS Plus, 1024 x 768 | 3x | 340 x 224 | 42 |
| 1920 x 1080 | 4x | 398 x 224 (16:9) | 71 |

## How it works

- **The patch** (`src/bps.c`): the app applies the .bps to a copy of the ROM
  at start, then its own patches (`src/sm.c`) to both copies. Switching Scale
  copies one ROM over the other between two frames. From the .bso it takes
  the aspect ratio (W), the faster CPU (O) and "no sprite limit" (l).
- **The picture** (`core/snes9x.patch`, gfx.cpp, clip.cpp, tileimpl.h):
  snes9x draws `256 + 2 × ext` columns. The game's x is the picture's x less
  ext:
  - the background layers scroll ext further left
  - a window (a screen effect) that reaches a screen edge reaches the
    picture's edge
  - sprites at x 256 to 256 + ext (which wrap to the left on a SNES) are
    drawn on the right, as bsnes-hd's "unsafe" sprites do
  - mode 7's maths takes the game's x

  It's off for the hi-res modes, which Super Metroid doesn't use.
- **The map and SAMUS pages** come from the hidden copy, which keeps the
  plain ROM at 256 wide.
- **Our own patches moved** to make room for the widescreen patch: the row
  pick from `$80:F000` to `$80:EF00`, and Samus's front sprite from `$90:FF00`
  to `$90:FE00`. Both places were taken by the patch. New ones go where both
  ROMs are free: the sprite X cut at `$81:F100` and `$A0:F7D4`, the sky row's
  wrap at `$88:EE40`.

## Cost (measured on the desktop, the core alone, Brinstar's shaft)

| | per frame |
| --- | --- |
| plain ROM, 256 | 0.60 ms |
| widescreen ROM, 256 | 0.58 ms |
| widescreen ROM, 340 (RG DS Plus) | 0.66 ms |
| widescreen ROM, 398 (16:9) | 0.72 ms |
| 398, CPU x1.5 or x2 | 0.73 ms |

That's +14% at the RG DS Plus's width and +25% at 16:9; the faster CPU costs
next to nothing here. The device's own numbers are the test that matters.

## What the sweep fixed

- **Seams and a garbage row in layer 3**: snes9x only stepped across one
  tilemap seam a line; a wider picture can cross two. It now wraps.
- **Other rooms' tiles at the edges**: a room narrower than the picture
  (most rooms one screen wide) showed whatever was in the tilemap beyond its
  edge, and sprites that wrapped round showed there too. Each frame, the
  layers and sprites are now cut to the room's own columns (from the room's
  width at `$07A5` and the camera at `$0911`); the backdrop colour fills the
  rest. Outside play (doors, cutscenes, the title) they're cut to the game's
  256 columns. When layer 1 is moved for an effect (Ridley's flyby, the
  Brinstar shaft), the camera is used instead.
- **The top rows (G2) under the patch**: the patch draws rows with its own
  code (`$80:F6B1`, `$80:F6F3`), which now get our top-row fix too.
- **Garbage across the top after an elevator ride up**: the patch's row
  reader started before the start of the level data and read from the ROM.
  It now starts in the bank before (`$80:F7AC` → `$80:EF10`).
- **Stale columns after switching Scale or loading a state**: the app now
  runs a hidden pause and unpause (up to 4 frames, out of sight), which makes
  the game redraw the whole screen.
- **The enemy check** at `$81:EF30` only acts for the game's own callers, so
  our copy of the sprite routine doesn't upset it.

### From the first batch of save states

- **Samus's ship showed its edge on the far side** as she walked away from
  it (G11 in `docs/RULES.md`): a piece's X on the SNES is 9 bits, and the
  ship's far edge, 230 columns off the right, read as just off the left. The
  spritemap routines in bank $81 (and our cull copy in $A0) now move a piece
  whose full X is outside -128 to 383 to -128, off the screen at any width.
  The full X is the spritemap's 9-bit signed X plus the base ($14): the
  stored value's high bits aren't usable (bit 15 is the piece's size).
- **A strip of the wall went missing jumping at the landing site**, also in
  Pixel Perfect: the sky's row stream ($88:B032, $88:B049) wrote one row past
  the end of layer 2's tilemap, onto layer 1's top row. It's wrapped now.
- **The door blinked in and out in a transition to the left or right**: the
  room clip used the new room's width with a camera still in the old room's
  terms, and the scroll shows both rooms anyway. Those transitions now show
  the whole picture (up and down ones keep the clip).
- **Stray tiles at the top after falling through a floor door**, and **a row
  of them at the top of the black screen on an elevator up**: the top row in
  a transition is left to the end (G2), and its place held the old room's
  tiles. Going up it's painted black while the screen scrolls; once the
  scrolling stops the app draws the row itself from the level data
  (`draw_top_row` in `src/game.c`), as the game's row drawer does.
- **Cutscenes** (Ceres's explosion): the extra columns are black bars outside
  play; the clip didn't cover the backdrop's colour effects.

For looking into a glitch: `SMDS_WS_NOCLIP=1` turns the room clip off and
`SMDS_LAYERS_OFF=<hex mask>` hides layers (1, 2, 4, 8 the backgrounds, 10
the sprites).

The regression tests run in Match Width too: `python3 tests/run.py --wide`
(it needs the patch in `tests/fixtures/patches`). Its screenshots are kept
apart from the plain ones (`*.wide`).

## Known problems

- Sprites that wrap within the room's own columns (an enemy more than
  about 70 columns off the left of a wide room) could still show on the
  wrong side. None were found in the sweep.
- Single screen, the frame buffer option, and the end of the game haven't
  been tried in Match Width.
