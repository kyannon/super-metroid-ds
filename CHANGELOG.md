# Changelog

## Unreleased

- HOME + Y does nothing with two screens (it used to open the MENU page when HOME was let go).
- **RetroAchievements (a preview):** Game → RetroAchievements opens a log-in form with an on-screen keyboard (tap, or the D-pad), and achievement pop-ups slide down at the top of the game screen in the pause screen's style; SAMPLE POP-UP shows one. Nothing connects yet: see `docs/retroachievements.md` for what the real thing needs.
- Regression tests (`make test`) for the rules in `docs/RULES.md`, a plain-language list of how the app shows the game on both screens.

## v0.3

The settings menu as a third page of the second screen, an EXIT button, map stations without the pause, and fixes along the screen's top and bottom edges.

**Install:** as before: unzip `super-metroid-ds-v0.3-linux-aarch64.zip` into the ports folder (`/storage/roms/ports` on ROCKNIX, `roms/ports` on a BananaOS card) over v0.2, with your own Super Metroid ROM in `super-metroid-ds/` or your SNES folder. Saves, states and settings carry over.

- The screens no longer swap: the game is always on top and the HUD and pages on the bottom (touch) screen. L3 and HOME + Y don't swap them any more (with a single screen they still switch it between the game and the HUD), and the `swap` setting is gone.
- The settings menu is in the equipment screen's orange. Save State and Load State share a row (D-pad left / right between them), and Frame Buffer is gone from the menu (`frame_buffer` is still in `sm-ds.cfg`).
- The settings menu is a third page of the second screen: **MENU**, after MAP and SAMUS, in the pause screen's frame with the page's name (GAME / DISPLAY) in its top edge. Swipe left / right, or tap the map-style arrow at its side, for the other page; the L / R tabs are gone. HOME still brings it up (the game paused, the D-pad on the menu).
- The SETTINGS MENU button is now **EXIT** (white on red), with an **ARE YOU SURE?** box first (its YES in the same white on red).
- With nothing to show, the MAP and SAMUS pages say AWAITING DATA CONNECTION. in the prologue's outlined green, in the small font; during Ceres station's escape they show the game's EMERGENCY / SELF DESTRUCT SEQUENCE message instead, written out with it, until the colony explodes.
- Tapping MENU works like HOME: it pauses the game, and tapping it again goes back.
- Map stations: after MAP DATA ACCESS COMPLETED., the game no longer opens the pause screen; the second screen switches to MAP and shows the new map while you play on (with Full Pause Menu on, the game's own pause comes back).
- A new map on the MAP page (a map station's rooms, another room) crossfades in over the old one; the map's scroll arrows (and the MENU page's) no longer blink.
- Elevators going up: no more row of another room's tiles across the bottom of the screen after the transition, and Samus's feet no longer show at the top while she's still below it, nor a stray pixel or two of her at the top as the room above comes into view.
- Samus no longer vanishes for a few frames in the middle of a jump.
- Sprites hanging just below the bottom of the screen (an item pickup as Samus jumps, say) no longer peek in at the top.
- The SAMUS page no longer shows the game's blinking selection outline (around MORPHING BALL, say): nothing on the second screen moves it.
- Close Super Metroid is gone from the Game page (EXIT does it), and the time and battery moved from the paused game screen to the MENU page's bottom corners.

## v0.2

The whole pause screen on the second screen, and equipment switched by touch.

**Install:** as before: unzip `super-metroid-ds-v0.2-linux-aarch64.zip` into the ports folder (`/storage/roms/ports` on ROCKNIX, `roms/ports` on a BananaOS card) over v0.1, with your own Super Metroid ROM in `super-metroid-ds/` or your SNES folder. Saves, states and settings carry over.

- The second screen has the whole pause screen: **MAP** and **SAMUS** buttons at the bottom switch between the map and the equipment screen, with a fade to black and back.
- On the SAMUS page, tap a suit, item, boot or beam to switch it on or off, or MODE for the reserve tanks' AUTO / MANUAL; it takes effect in the running game at once.
- START pauses the game in place (dimmed) instead of opening the pause screen; the second screen's pages still work while paused. With one screen, START opens the game's own pause screen as before.
- While the game couldn't be paused (doors, Ceres, cutscenes), the MAP and SAMUS buttons are greyed out and the page is dimmed.
- Settings → Game → **Full Pause Menu** (off by default) brings back the game's own pause screen on START.
- PAUSED is smaller, with a dark grey edge like the opening text crawl; the settings button reads SETTINGS MENU, in the game's small font, on the equipment screen's orange.
- Swap Screens and Match Display Rate are gone from the settings menu: swap with L3 (or HOME + Y), and `match_display` is still in `sm-ds.cfg`.

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
