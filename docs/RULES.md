# How Super Metroid DS shows the game: the rules

This is the list of everything the app does to the picture on the two screens.
It's here so we agree on what the app *should* do, and so a regression can be
named by rule. Each rule has an ID (G for the game screen, S for the second
screen, C for controls). The **Tested by** line names the regression tests
(`tests/cases.py`) that check it. "Not tested yet" means nothing checks it
automatically, so a change there needs a look by eye.

When something looks wrong, the most useful report is: the rule ID, what you
expected, what you saw, and a save state from just before it.

## Background, in a few lines

- Super Metroid draws its HUD (energy, missiles, the mini-map) in a 32-line
  bar across the top of the picture. The app takes the bar off the game
  screen and redraws the HUD on the second screen.
- The game never had to draw anything good behind that bar, because the bar
  hid it. Most of the rules below are about making those 32 lines (the
  **strip**) look like the rest of the room.
- The map and the equipment screens come from a **hidden copy** of the game:
  the app hands it a snapshot of your game, the copy pauses, and the app
  copies its pause screen onto the second screen. Your game is never paused
  for this.
- "In play" means the game's normal play state: walking around, with a HUD.
  Doors, cutscenes, the title screen and the game's own pause screen aren't
  "in play".

---

## The game screen (top)

### G1. No HUD bar on the game screen
In play, the top 32 lines show the room, like everything below them. The
energy and ammo bar doesn't show there. Outside play (the title, the intro,
cutscenes) the picture is exactly what the game draws.

*How:* the game switches the screen to its HUD settings at line 0 and back at
line 31. The app makes it skip the switch, so the room's settings run from
the top.
The room fading in as play starts (arriving at Ceres, a game loaded) counts
as play: no garbage across the top before the HUD is drawn.
**Tested by:** ship-jump (the strip is sky, not a black band),
escape-ride-strip, ceres-arrival.

### G2. The strip's background is the room's own
When the screen scrolls up, the room's top row of tiles is drawn, not left
over from wherever the screen was before. After an upward door transition
(an elevator going up), the whole screen is the new room: no row of the old
room's tiles along the bottom or the top.

In a door transition the top row is never the old room's either: on an
elevator going up, nothing of the room below shows across the black screen,
and falling through a door in the floor, the new room's top row is its own
from the first frame it shows.

The landing site's sky (layer 2) never writes over the room: jumping under
the overhang, the wall above Samus stays whole. (The game's sky stream wrote
one row past the end of the sky's tilemap, onto the room's top row. With the
HUD the room redrew that row before anyone could see it; now it's wrapped.)

*How:* the game drew the row under the top one (the top one was behind the
HUD). It now draws the top row, except during a door transition, where it
keeps its old order. There the top row is painted black while an upward
transition scrolls (the screen is black anyway), the app draws it itself
once the scrolling stops, and the game redraws it as the transition ends.
**Tested by:** elevator-up (screenshots after the ride), brinstar-elevator-up2,
brinstar-fall-door, landing-jump-wall.

### G3. Line effects carry on into the strip
Effects the game sets line by line (the sky's parallax, rain, Ceres's
different background mode) carry on into the strip as they are on line 32,
so the strip matches the room below it. This applies in play, at doors, and on
Ceres's elevator at the end of the escape. In cutscenes (the colony
exploding, the intro) the game's own top lines are left alone, so flashes
and fades look as the game made them.

**Tested by:** escape-ride-strip, escape-end-flash (screenshots of the
explosion), ship-jump (the rain).

### G4. Nothing below the bottom edge peeks in at the top
A sprite hanging off the bottom of the screen never shows at the top of the
screen. On the real SNES its lower rows wrap round to the top lines, which
the HUD used to hide. This covers:
- enemies and their parts (the gunship's underside as Samus jumps off it)
- item pickups and the like (a missile at the bottom edge as Samus jumps)
- Samus herself, her arm cannon, her speed echoes, and the sprite drawn over
  her when she faces the screen (riding an elevator up into a room)
- drops, smoke and other effects

*How:* the game's sprite routines are patched to hide a piece whose position
is below line 224, before its position wraps.
**Tested by:** ship-jump, item-below-edge, elevator-up.

### G5. Things just above the top edge still show
A sprite hanging down from just above the screen (up to 32 lines above it)
shows the part that's on screen, as on a SNES without the HUD. For example,
Ridley rising off the top of the screen in Ceres stays whole as he goes.
**Tested by:** ridley-high.

### G6. Samus never vanishes
G4's hiding never catches Samus while she's on screen, in any pose (spin
jumps included).
**Tested by:** ship-jump (every frame of a spin jump).

### G7. START pauses the game in place
With two screens, START in play stops the game where it is. The game screen
dims and shows PAUSED (white, with a dark grey edge, in the game's
lettering). START again goes on. The second screen's pages still work while
paused. Where the game itself can't pause (doors, Ceres, cutscenes, an
item's message), START does nothing special and goes to the game as usual.
**Tested by:** pages-and-pause, ceres-start.

### G8. Full Pause Menu (off by default)
With Settings → Game → Full Pause Menu on, START opens the game's own pause
screen on the game screen, as on a SNES.
**Tested by:** full-pause-menu.

### G9. The screens never swap
The game is always on the top screen, and the HUD and pages are always on the
bottom (touch) screen. L3 and HOME + Y do nothing with two screens. With one
screen, they switch it between the game and the pages.
**Tested by:** no-swap (two screens only).

### G10. Map stations don't stop the game
At a map station, after MAP DATA ACCESS COMPLETED., the game's pause screen
doesn't come up. The hidden pause runs out of sight, Samus is free to move
at once, and the MAP page shows the new map (S6).
**Tested by:** map-station.

### G11. Match Width: nothing from far off the side
With Scale on Match Width (`docs/widescreen.md`), a sprite far off one side
never shows on the other side's extra columns. On a SNES a sprite's X is 9
bits, so one far off the right reads as just off the left, which is off the
screen at 256 wide but not in the extra columns: Samus's ship, as she walks
away from it.

*How:* the game's sprite routines move a piece whose full X is far off the
screen (outside -128 to 383) to where no width up to 16:9 shows it.
**Tested by:** landing-ship-left.

### G12. Match Width: the extra columns show the room, and only the room
In play, the extra columns show the room and nothing past its edges. In a
door transition to the left or right, the whole picture shows the scroll
across both rooms (the door slides across without blinking out). Outside play
(the title, cutscenes, the game's own pause screen) the extra columns are
black bars, with the game's 256 columns centred between them (mode 7 too:
the title, Ceres).
**Tested by:** crateria-door-left, title-screen, ceres-arrival,
escape-end-flash (the explosion's screenshots), and every case's screenshots
in Match Width
(`tests/run.py --wide`).

---

## The second screen (bottom, the touch screen)

The second screen has the HUD bar in rows 0-31, the page in rows 32-198, and
the three buttons MAP / SAMUS / MENU in rows 202-219.

### S1. The live HUD
The HUD bar (energy, reserve tanks, missiles, super missiles, power bombs,
grapple, X-ray) is drawn from the game's own HUD as you play. Tap an item
icon to select it, and tap again for none.
**Tested by:** the screenshots of every bottom-screen case (the bar is in
them). The taps aren't tested yet.

### S2. EXIT
A red EXIT button with white letters sits where the HUD's mini-map was.
Tapping it asks ARE YOU SURE?, with NO selected (orange) and YES in white on
red, like the EXIT button. The D-pad moves the selection (the selected
button gets a light purple edge). NO closes the question, and YES closes the
app.
**Tested by:** exit-dialog.

### S3. The page buttons
MAP, SAMUS and MENU sit along the bottom. The one shown is orange with a
light purple edge, and the others are grey. Tapping another fades the page to
black (8 frames), waits for its picture if needed, and fades it back in (8
frames).
**Tested by:** pages-and-pause.

### S4. Greyed out when the game can't pause
Wherever the game couldn't pause (doors, Ceres, cutscenes, an item's
message), MAP and SAMUS are dark grey and can't be tapped, and the page is
dimmed. MENU always stays usable.
**Tested by:** escape-door, ceres-start.

### S5. The MAP page
The MAP page shows the pause screen's map of the area you're in, with its
name. It's refreshed every 1.5 seconds (`map_interval`, 90 frames) and when you
enter a room. Its scroll arrows are a steady yellow (they don't blink, as they
do in the game). Tap near an edge to scroll that way. It goes back to Samus
in the next room.
**Tested by:** pages-and-pause (steady arrows), map-station. The scrolling
isn't tested yet.

### S6. A new map crossfades in
When the map gains rooms (a map station) or changes (another area), the new
map fades in over the old one over 24 frames, rather than cutting.
**Tested by:** map-station.

### S7. The SAMUS page
The SAMUS page shows the equipment screen. It has no blinking selection
outline (nothing on the second screen moves it), and nothing in its item boxes
blinks. The Samus figure keeps its glow. Tap an item to switch it on or off,
and the game takes it at once (suit colours, the beam, the HUD's AUTO sign).
**Tested by:** samus-page (BOMB switched off).

### S8. The MENU page
The settings, in the pause screen's frame, with the page's name (GAME or
DISPLAY) in its top edge. Swipe, or tap the arrow at the side, to change
page. The time and the battery are in the frame's bottom corners.
**Tested by:** menu-page (the tests use a fixed time, 10:24, and battery,
87%).

### S9. AWAITING DATA CONNECTION.
With no map or equipment screen to show (the intro, the title, the file
screen, Ceres, cutscenes), the MAP and SAMUS pages say AWAITING DATA
CONNECTION. in the opening's green lettering with a dark edge, in the small
font. There's nothing else on the page.
**Tested by:** ridley-high (before the escape starts).

### S10. Ceres's escape message
During Ceres's escape, the MAP and SAMUS pages show the game's EMERGENCY /
SELF DESTRUCT SEQUENCE ACTIVATED EVACUATE COLONY IMMEDIATELY message instead,
letter for letter as on the game screen and written out as it is there.
- It stays up through doors, the elevator ride and the black screen.
- It goes back to AWAITING DATA CONNECTION. at the white flash of the
  colony's explosion, not before.
- Nothing else is taken from the game screen (Ridley flying off, the room's
  tiles).

**Tested by:** ridley-high (letter for letter), escape-door (kept through the
door), escape-end-flash (gone at the flash), ceres-start.

The same message at the end of the game (Zebes's escape) isn't done yet.

---

## Controls

### C1. MENU works like HOME
Tapping MENU (or pressing HOME) pauses the game and shows the MENU page, with
the D-pad on the menu. Tapping MENU again (or B, START, HOME) goes back to the
page you were on and to playing.
**Tested by:** menu-page.

---

## Known gaps

- **Map station dimming:** while the hidden pause runs (G10), MAP and SAMUS go
  grey and the page dims for about 12 frames, because the game counts as
  paused for that moment (S4). It's a short flicker, not a rule.
- **Zebes's escape message** (S10, at the end of the game) is waiting for a
  save state.
- **Not tested yet:** S1's taps, S5's scrolling, rewinding, fast-forward,
  cheats, save and load states.

## How the tests work

`tests/run.py` (or `make test`) plays each case. It loads a save state into the
desktop build, plays a scripted input (buttons, taps, swipes), takes
screenshots and checks the rules above. It also compares chosen screenshots
pixel for pixel with ones approved earlier. A run is the same every time:
one game frame a tick, the map captures waiting for each other, and a fixed
clock. See `tests/README.md`.
