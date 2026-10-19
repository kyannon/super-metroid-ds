# Changelog

## v0.1

The first release of Super Metroid DS: Super Metroid on the RG DS / RG DS Plus, with the game on one screen and its HUD and pause map on the other. For Linux only for now (ROCKNIX and BananaOS); Android will follow.

**Install:** unzip `super-metroid-ds-v0.1-linux-aarch64.zip` into the ports folder (`/storage/roms/ports` on ROCKNIX, `roms/ports` on a BananaOS card), put your own Super Metroid ROM (Japan / USA, `.sfc` or `.smc`) in `super-metroid-ds/` or your SNES folder, and start **Super Metroid** from Ports. The ROM isn't included.

**What's in it**
- No HUD bar on the game screen: the room is drawn where it was, Ceres and the overworld included.
- The second screen: the live HUD, and the pause map of where you are, refreshed as you play; tap its arrows to scroll it, and tap an item icon in the HUD to select it (SELECT still works).
- The pause screen goes straight to the equipment screen (the map lives on the second screen).
- Settings (tap TAP FOR SETTINGS or press HOME), drawn at the game's resolution: save / load states, rewind (hold L2), cheats from RetroArch `.cht` files in `cheats/`, screenshots, swap screens (L3), scale (Pixel Perfect / Full Width / Zoom / Stretch), shaders (Pixel Grid, Scanlines), FPS, frame buffer, matching the display's refresh rate.
- Fast-forward (hold R2), resume on launch, RetroArch-compatible battery saves.

Super Metroid DS's own code is under the MIT license. It runs the snes9x core, which is under snes9x's non-commercial license (see `licenses/` in the zip). `super-metroid-ds-v0.1-source.tar.gz` is the source of this build.
