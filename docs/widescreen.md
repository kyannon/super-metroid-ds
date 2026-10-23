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
  to `$90:FE00`. Both places were taken by the patch.

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

## Known problems

- **After switching mid-room**, a room exactly one screen wide shows other
  tiles at its edges until the next door. The patch blanks what's outside a
  room as the room loads.
- **After an elevator ride up**, a strip of the room above shows across the
  top of the screen. The patch replaces the scrolling routine (`$80:A3DF`)
  that our fix for the top rows (G2 in `docs/RULES.md`) hooks into.
- **The landing site's background** shows a seam near the right edge. This
  may be how the patch draws layer 2; it needs comparing with bsnes-hd.
- **Enemies far off-screen**: the patch adds a check at the end of the enemy
  sprite routine (`$81:8B1C`) to hide some enemies when they're far off the
  screen. Our copy of that routine (G4's fix) doesn't have the check yet.
- **Not tried yet:** mode 7 (the Ceres intro), the end of the game, single
  screen, the frame buffer option, and the regression tests in Match Width.
