# RetroAchievements: where it stands, and what it would take

## In this build (a preview)

- **Game → RetroAchievements** on the MENU page. It shows OFF, or ON once
  you're logged in.
- **Logged out**, it opens a form in the pause screen's style:
  - a USERNAME field and a PASSWORD field (the password shows as dots)
  - a keyboard of 40 keys, plus CAPS, SYM (symbols), SPACE and DEL
  - CANCEL and LOG IN
- **The form's controls:** tap the keys. Or use the D-pad to move and A to
  press. B deletes a letter (and goes back when the field is empty). SELECT is
  CAPS, Y switches fields, and START is LOG IN.
- **Logging in** doesn't connect to anything yet. It keeps the user name in
  `states/retroachievements.cfg` (never the password) and shows a "Welcome"
  pop-up.
- **Logged in**, the screen shows the name, a note that this is a preview,
  SAMPLE POP-UP (an unlock as it will look), LOG OUT and BACK.
- **The pop-ups** appear at the top of the game screen (`docs/RULES.md`, G11).
  They're a box like the pause screen's, titled in its top edge (ACHIEVEMENT
  UNLOCKED, or RETROACHIEVEMENTS for a notice), and slide down from above.
  Inside are:
  - the badge (a trophy or a star until the real badge images come)
  - the achievement's name in white
  - its description in the prologue's green, in the small lettering
  - its points in the equipment screen's orange

  Each one stays up five seconds, one at a time. They're drawn in the game's
  own pixels, so they scale and take the shader like the game.

The code is split so the real client can drop in:
- `src/ra.c` / `ra.h` hold the account, the login and the pop-up queue (the
  seam).
- `src/game.c` holds the screen and the drawing.

## What a real connection needs

1. **rcheevos** (MIT), RetroAchievements' own library, and its `rc_client`.
   It does the login (password in, a token back, and only the token is kept),
   finds the game's set from the ROM's hash, and watches the game's memory
   every frame. For the SNES that's the 128 KB of work RAM and the cartridge's
   save RAM, which `emu.c` can already hand over. It also tells us what to
   show: unlocks, the game's set when it starts, leaderboards and their
   trackers, and challenge indicators.
2. **HTTP**: `rc_client` asks the app to make its web requests. On the device
   that would be libcurl, opened at run time so the build still starts
   without it. ROCKNIX and BananaOS have it.
3. **Badges**: 64 x 64 PNGs from RetroAchievements' site, cached in
   `states/`, scaled to the box's 32 x 32.
4. **Hardcore mode.** RetroAchievements' rules for an emulator say that in
   hardcore:
   - save states can't be loaded
   - rewind, slow motion and frame advance are off
   - cheats are off
   - a resumed session drops to casual
   - switching to hardcore means a reset

   Here that means HOME + L, Load State, Rewind (L2), Cheats and the resume
   state on launch would all be blocked while hardcore is on. Fast-forward
   isn't on their list.
5. **A user agent** for the requests: unique, with a numeric version, e.g.
   `SuperMetroidDS/0.4 (Linux) snes9x/<its version>`.

## Before it can be switched on for real

RetroAchievements lists the emulators it accepts unlocks from in hardcore,
and has rules for getting on that list. Two of them matter here:

- **Six months.** An emulator has to have been publicly available for six
  months, counted from its first public release.
- **Money.** Their list of automatic rejections includes "shipping
  non-commercial cores while monetized without permission". The builds ship
  snes9x, whose licence is non-commercial. With a Ko-fi link in the README,
  RetroAchievements could count the project as monetized. Asking the snes9x
  authors for permission, or taking the donation link off the builds, would
  settle that before applying.

I haven't checked how RetroAchievements treats a client that isn't on its
list in casual mode; it isn't covered by the hardcore compliance page. That's
a question for their team when we're ready to apply.

Source: [Hardcore compliance requirements](https://docs.retroachievements.org/general/hardcore-compliance-requirements.html)
(RetroAchievements' documentation).
