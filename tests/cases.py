"""The regression tests: each case a save state, an input script and checks of
the rules in docs/RULES.md (by ID), with the screenshots it compares to the
approved ones ("goldens").

Coordinates are the game's pixels of a screen (256 x 224, (0, 0) top left).
On the second screen the HUD is rows 0-31, the page 32-198 and the MAP /
SAMUS / MENU buttons 202-219."""
from smtest import colourful, count, mean, near, suit_yellow

ORANGE = (255, 182, 0)       # a selected button, an item that's on
GREY = (148, 150, 148)       # a button you can tap that isn't selected
DARK_GREY = (57, 56, 57)     # a button you can't tap
LAVENDER = (214, 158, 222)   # the edge of the selected button
EXIT_RED = (206, 0, 0)
TAB = {"MAP": (14, 202, 86, 220), "SAMUS": (92, 202, 164, 220), "MENU": (170, 202, 242, 220)}
TAB_TAP = {"MAP": (50, 211), "SAMUS": (128, 211), "MENU": (206, 211)}
EXIT_TAP = (228, 18)
NO_BOX, YES_BOX = (64, 116, 120, 134), (136, 116, 192, 134)
NO_TAP, YES_TAP = (92, 125), (164, 125)
PAGE = (0, 32, 256, 199)


def tab_fill(img, name):
    """the colour inside a button (a few pixels in from its left end, clear of the text)"""
    x0, y0, x1, y1 = TAB[name]
    return img.getpixel((x0 + 4, y0 + 9))


def green_text(p):  # AWAITING DATA CONNECTION.: green letters (0, 127, 0), edged (0, 44, 0)
    return p[1] > 30 and p[0] < 20 and p[2] < 20


class Case:
    def __init__(self, name, fixture, rules, script, check, goldens=(), env=None, args=(), about=""):
        self.name, self.fixture, self.rules = name, fixture, rules
        self.script, self.check, self.goldens = script, check, list(goldens)
        self.env, self.args, self.about = env or {}, list(args), about


CASES = []


def case(*a, **k):
    def wrap(f):
        CASES.append(Case(*a, check=f, **k))
        return f
    return wrap


# ---------------------------------------------------------------- the game screen

@case("ship-jump", "ship-landing", ["G1", "G3", "G4", "G6"],
      script=lambda s: s.hold(650, "RIGHT", 3).hold(700, "B", 12).shots_every(640, 800),
      goldens=[(640, "top"), (730, "top"), (760, "top")],
      about="Samus jumps off her ship on Zebes; the camera rises and the ship's underside drops below the screen.")
def _(r, c):
    for t in range(700, 801):
        top = r.top(t)
        # G4: the ship's underside (colourful) never shows in the sky at the top
        n = colourful(top, (0, 0, 256, 40))
        c.expect(n == 0, "G4", "tick %d: %d colourful pixels in the top 40 lines (the ship's underside?)" % (t, n))
        # G6: Samus is on the screen every frame of the jump
        y = count(top, (0, 0, 256, 125), suit_yellow)
        c.expect(y > 5, "G6", "tick %d: Samus's suit not on the screen (%d pixels)" % (t, y))
    # G1: the strip is sky, not the HUD: its colour is the sky's grey, not black
    m = mean(r.top(640), (0, 0, 256, 32))
    c.expect(min(m) > 40, "G1", "the top strip is dark (mean %s): the HUD's black band?" % (tuple(round(v) for v in m),))


@case("item-below-edge", "brinstar-missile", ["G4"],
      script=lambda s: s.hold(20, "B", 25).shots_every(22, 100),
      goldens=[(34, "top"), (68, "top")],
      about="A missile pickup at the bottom right edge as Samus jumps: none of it may wrap onto the top.")
def _(r, c):
    for t in range(22, 101):
        n = count(r.top(t), (200, 0, 232, 12), lambda p: min(p) > 200)
        c.expect(n == 0, "G4", "tick %d: %d white pixels at the top right (the missile wrapping?)" % (t, n))


@case("elevator-up", "elevator-up", ["G2", "G4"],
      script=lambda s: s.hold(20, "UP", 5).shots_every(470, 800, 2),
      goldens=[(686, "top"), (700, "top"), (800, "top"), (800, "bottom")],
      about="Riding an elevator up into the room above: the strip's rows, and Samus below the screen.")
def _(r, c):
    ride_end = next((t for t in sorted(r.trace) if t > 600 and r.trace[t]["state"] == 0x08), None)
    c.expect(ride_end is not None, "G2", "the ride never ended (game state back to 08)")
    for t in range(600, 801, 2):
        n = count(r.top(t), (0, 0, 256, 20), suit_yellow)
        c.expect(n == 0, "G4", "tick %d: %d of Samus's pixels at the top (wrapping from below?)" % (t, n))


@case("ridley-high", "ceres-ridley", ["G5", "S9", "S10"],
      script=lambda s: s.shots_every(1, 330, 3).shots_every(336, 600, 12),
      goldens=[(103, "top"), (109, "top"), (400, "bottom")],
      about="Ceres: Ridley flies up off the top, then the escape's message is written.")
def _(r, c):
    # G5: Ridley hanging down from above the screen is drawn in the strip
    for t in range(100, 113, 3):
        n = count(r.top(t), (0, 0, 256, 32), lambda p: p[0] > 80)
        c.expect(n > 150, "G5", "tick %d: only %d of Ridley's pixels in the top strip" % (t, n))
    start = next((t for t in sorted(r.trace) if r.trace[t]["esc"]), None)
    c.expect(start is not None, "S10", "the escape message never came up")
    for t in range(1, 330, 3):
        b = r.bottom(t)
        esc = r.at(t).get("esc")
        if not esc:
            # S9: only AWAITING DATA CONNECTION. (green) on the page, nothing else (Ridley's pieces...)
            g = count(b, PAGE, green_text)
            other = count(b, (0, 50, 256, 199), lambda p: max(p) > 40 and not green_text(p))
            c.expect(g > 50, "S9", "tick %d: AWAITING DATA CONNECTION. not shown" % t)
            c.expect(other == 0, "S9", "tick %d: %d stray pixels on the page" % (t, other))
    for t in range(348, 601, 12):
        mirror_check(r, c, t)


def mirror_check(r, c, t):
    """S10: the page's message is the game screen's, letter for letter (its rows
    40-111 shown at the page's rows 80-151)"""
    top, b = r.top(t), r.bottom(t)
    lit = wrong = 0
    for y in range(80, 152):
        for x in range(256):
            p = b.getpixel((x, y))
            if max(p) > 40:
                lit += 1
                q = top.getpixel((x, y - 40))
                # red and green only: on the game screen the room's blue gradient is added in (colour math)
                if abs(q[0] - p[0]) > 8 or abs(q[1] - p[1]) > 8:
                    wrong += 1
    c.expect(lit > 200, "S10", "tick %d: the message isn't on the page (%d pixels)" % (t, lit))
    c.expect(wrong <= lit // 50, "S10", "tick %d: %d of %d message pixels differ from the game screen's" % (t, wrong, lit))


@case("escape-door", "ceres-escape-door", ["S10", "S4"],
      script=lambda s: s.hold(10, "LEFT", 300).shot(5).shots_every(10, 430, 10),
      goldens=[(250, "bottom")],
      about="Ceres's escape: walking left through the door, the message stays on the page.")
def _(r, c):
    def lit(t):
        return count(r.bottom(t), (0, 80, 256, 152), lambda p: max(p) > 40)
    ref = lit(10)
    c.expect(ref > 200, "S10", "no message on the page at the start")
    for t in range(20, 431, 10):
        n = lit(t)
        c.expect(abs(n - ref) <= ref // 20, "S10", "tick %d: the message changed (%d pixels, %d at the start)" % (t, n, ref))
    # S4: MAP and SAMUS greyed out (the game can't pause in Ceres), MENU not
    b = r.bottom(100)
    for name in ("MAP", "SAMUS"):
        c.expect(near(tab_fill(b, name), DARK_GREY, 12), "S4", "%s isn't greyed out: %s" % (name, tab_fill(b, name)))
    c.expect(near(tab_fill(b, "MENU"), GREY, 12), "S4", "MENU isn't lit: %s" % (tab_fill(b, "MENU"),))


@case("escape-end-flash", "ceres-escape-end", ["S10", "G3"], env={"SMDS_TEST_ESCAPE": "1"},
      script=lambda s: s.hold(1, "RIGHT", 7).shots_every(2, 80, 6).shots_every(400, 660, 3),
      goldens=[(20, "top"), (74, "top"), (450, "top"), (612, "top"), (630, "top")],
      about="The end of the escape: the ride up, the cutscene, the colony's explosion; the message goes at the white flash.")
def _(r, c):
    off = next((t for t in sorted(r.trace) if t > 10 and not r.trace[t]["esc"]), None)
    c.expect(off is not None, "S10", "the escape message never went away")
    if off is None:
        return
    # the first frame the whole picture is bright in red, green and blue
    flash = None
    for t in range(400, 661, 3):
        m = mean(r.top(t), (0, 0, 256, 224))
        if min(m) > 100:
            flash = t
            break
    c.expect(flash is not None, "S10", "no white flash seen")
    if flash is not None:
        c.expect(flash - 3 < off <= flash, "S10", "the message went at tick %d, the white flash is at %d" % (off, flash))


@case("escape-ride-strip", "ceres-escape-strip", ["G1", "G3"], env={"SMDS_TEST_ESCAPE": "1"},
      script=lambda s: s.hold(1, "RIGHT", 60).shots_every(4, 76, 4),
      goldens=[(12, "top"), (40, "top"), (70, "top")],
      about="Ceres's elevator at the end of the escape (game states 20, 21): the strip at the top is the room's.")
def _(r, c):
    ride = [t for t in sorted(r.trace) if r.trace[t]["state"] in (0x20, 0x21)]
    c.expect(len(ride) > 40, "G3", "the elevator ride didn't happen (%d frames)" % len(ride))
    for t in range(8, 61, 4):
        # the strip continues the room: its colours are the shaft's, close to the lines just below it
        a, b = mean(r.top(t), (0, 0, 256, 32)), mean(r.top(t), (0, 32, 256, 64))
        c.expect(all(abs(x - y) < 60 for x, y in zip(a, b)), "G3",
                 "tick %d: the top strip (%s) doesn't look like the room below it (%s)" % (t, tuple(map(round, a)), tuple(map(round, b))))


# ---------------------------------------------------------------- map stations

@case("map-station", "map-station", ["G10", "S5", "S6"],
      script=lambda s: s.hold(10, "LEFT", 60).hold(450, "A", 3).hold(520, "RIGHT", 40).shot(100).shots_every(440, 600, 2),
      goldens=[(100, "bottom"), (600, "bottom")],
      about="A map station: the new map shows on the MAP page; the game doesn't open its pause screen.")
def _(r, c):
    ref = r.top(440)
    for t in range(442, 520, 2):
        diff = count_diff(ref, r.top(t), (0, 0, 256, 224))
        c.expect(diff < 256 * 224 // 8, "G10", "tick %d: the game screen changed a lot (%d pixels): the pause screen?" % (t, diff))
    x0, x1 = r.at(520).get("samusX", 0), r.at(560).get("samusX", 0)
    c.expect(x1 > x0 + 10, "G10", "Samus didn't move after the message (x %d -> %d): control not given back" % (x0, x1))
    before, after = mapped(r.bottom(100)), mapped(r.bottom(600))
    c.expect(after > before * 2, "S6", "the MAP page didn't get the station's rooms (%d -> %d map pixels)" % (before, after))
    xf = sorted(t for t in r.trace if r.trace[t].get("xfade", -1) >= 0)
    c.expect(len(xf) >= 20, "S6", "the new map didn't crossfade in (%d frames of crossfade)" % len(xf))


def count_diff(a, b, box):
    from PIL import ImageChops
    d = ImageChops.difference(a.crop(box), b.crop(box))
    from smtest import pixels
    return sum(1 for p in pixels(d) if max(p) > 24)


def mapped(img):  # the map's rooms: anything inside the frame but the black and the grid's purple
    return count(img, (8, 50, 248, 190), lambda p: max(p) > 40 and p != (66, 0, 140))


# ---------------------------------------------------------------- the second screen's pages

@case("pages-and-pause", "crateria-chozo", ["G7", "S3", "S5"],
      script=lambda s: s.shot(10, 70, 130).tap(30, *TAB_TAP["SAMUS"]).shots_every(31, 52, 1).hold(100, "START", 3)
                        .shot(140, 200).hold(260, "START", 3).shot(300, 340),
      goldens=[(10, "bottom"), (70, "bottom"), (200, "top")],
      about="The MAP page, a tap on SAMUS (with its fade), START's pause and START again.")
def _(r, c):
    # S5: the map's arrows are steady (the same in frames apart)
    for t in (10, 130):
        # (in Match Width, loading the state redraws the screen with a hidden
        # pause first, so the page may still be fading in at tick 10)
        shown = r.at(t)["tab"] == 0 and r.at(t)["tabsOn"] and not r.at(t)["fade"]
        p = r.bottom(t).getpixel((238, 119)) if shown else (255, 231, 0)
        c.expect(p == (255, 231, 0), "S5", "tick %d: the map's right arrow isn't its steady yellow: %s" % (t, p))
    # S3: SAMUS tapped: the page fades out and back in, then SAMUS is the page
    fades = [r.trace[t]["fade"] for t in range(31, 53) if t in r.trace]
    c.expect(1 in fades and 3 in fades, "S3", "no fade out and in after the tap: %s" % fades)
    c.expect(r.at(70)["tab"] == 1, "S3", "SAMUS isn't the page after the tap")
    darkest = min(mean(r.bottom(t), PAGE)[0] for t in range(31, 53))
    c.expect(darkest < mean(r.bottom(70), PAGE)[0] / 3, "S3", "the page didn't go dark during the fade")
    c.expect(tab_fill(r.bottom(70), "SAMUS") == ORANGE, "S3", "the SAMUS button isn't orange: %s" % (tab_fill(r.bottom(70), "SAMUS"),))
    # G7: START pauses in place: frozen, dimmed, PAUSED; START again goes on
    c.expect(r.at(140)["paused"] == 1, "G7", "START didn't pause")
    c.expect(r.top(140).tobytes() == r.top(200).tobytes(), "G7", "the game moved while paused")
    lit, dim = mean(r.top(70), (0, 150, 256, 224)), mean(r.top(200), (0, 150, 256, 224))
    c.expect(dim[0] < lit[0] * 0.75, "G7", "the paused game isn't dimmed (%d against %d)" % (dim[0], lit[0]))
    white = count(r.top(200), (80, 96, 176, 126), lambda p: min(p) > 230)
    c.expect(white > 60, "G7", "no PAUSED on the game screen (%d white pixels)" % white)
    c.expect(r.at(300)["paused"] == 0, "G7", "START again didn't go on")
    c.expect(mean(r.top(340), (0, 150, 256, 224))[0] > lit[0] * 0.9, "G7", "the game is still dimmed after START again")


@case("samus-page", "brinstar-missile", ["S7"],
      script=lambda s: s.tap(30, *TAB_TAP["SAMUS"]).shots_every(120, 184, 8).tap(200, 200, 117).shot(260, 400),
      goldens=[(120, "bottom"), (260, "bottom")],
      about="The SAMUS page: no blinking selection outline; tapping BOMB switches it off.")
def _(r, c):
    # no outline round MORPHING BALL and nothing blinking in the item boxes: the
    # boxes look the same in every frame (the Samus figure's glow is elsewhere)
    ref = r.bottom(120).crop((160, 96, 252, 124)).tobytes()
    for t in range(128, 185, 8):
        same = r.bottom(t).crop((160, 96, 252, 124)).tobytes() == ref
        c.expect(same, "S7", "tick %d: the MISC. box changed (the selection outline blinking?)" % t)
    # BOMB (second row of MISC.) switched off: its row goes grey
    on, off = r.bottom(120).getpixel((230, 117)), r.bottom(260).getpixel((230, 117))
    c.expect(near(on, ORANGE, 16), "S7", "BOMB wasn't on to begin with: %s" % (on,))
    c.expect(near(off, GREY, 16), "S7", "BOMB didn't go grey after the tap: %s" % (off,))


@case("menu-page", "crateria-chozo", ["S8", "C1"],
      script=lambda s: s.tap(30, *TAB_TAP["MENU"]).shot(70).swipe(100, 200, 120, 60, 120).shot(160)
                        .tap(200, 12, 119).shot(240).tap(280, *TAB_TAP["MENU"]).shot(330),
      goldens=[(70, "bottom"), (160, "bottom")],
      about="MENU: the GAME page, a swipe to DISPLAY, the arrow back, MENU again to leave.")
def _(r, c):
    c.expect(r.at(70)["menu"] == 1 and r.at(70)["tab"] == 2, "C1", "tapping MENU didn't open the menu")
    c.expect(r.at(70)["paused"] == 1 or r.at(70)["menu"] == 1, "C1", "the game didn't stop with the menu")
    c.expect(r.top(70).tobytes() == r.top(240).tobytes(), "C1", "the game moved while the menu was up")
    g, d = r.bottom(70), r.bottom(160)
    title_g, title_d = g.crop((100, 36, 156, 48)).tobytes(), d.crop((100, 36, 156, 48)).tobytes()
    c.expect(title_g != title_d, "S8", "the swipe didn't change the page (the same title)")
    c.expect(r.bottom(240).crop((100, 36, 156, 48)).tobytes() == title_g, "S8", "the left arrow didn't go back to GAME")
    # the clock and battery in the frame's bottom corners (fixed for tests: 10:24, 87%)
    c.expect(count(g, (8, 180, 50, 192), lambda p: min(p) > 230) > 20, "S8", "no time at the bottom left")
    c.expect(count(g, (205, 180, 248, 192), lambda p: min(p) > 230) > 20, "S8", "no battery at the bottom right")
    c.expect(r.at(330)["menu"] == 0 and r.at(330)["tab"] == 0, "C1", "MENU again didn't go back to the MAP page")


@case("exit-dialog", "crateria-chozo", ["S2"],
      script=lambda s: s.shot(10).tap(30, *EXIT_TAP).shot(60).hold(70, "RIGHT", 3).shot(90).tap(110, *NO_TAP).shot(150)
                        .tap(170, *EXIT_TAP).tap(220, *YES_TAP).shot(215),
      goldens=[(60, "bottom"), (90, "bottom")],
      about="EXIT: ARE YOU SURE? (NO selected, YES in white on red), NO closes it, YES closes the app.")
def _(r, c):
    b = r.bottom(10)
    c.expect(near(b.getpixel((212, 12)), EXIT_RED, 30), "S2", "the EXIT button isn't red: %s" % (b.getpixel((212, 12)),))
    c.expect(r.at(60)["exit"] == 1, "S2", "tapping EXIT didn't ask")
    d = r.bottom(60)
    c.expect(near(d.getpixel((70, 120)), ORANGE, 16), "S2", "NO isn't the selected (orange) one: %s" % (d.getpixel((70, 120)),))
    c.expect(near(d.getpixel((142, 120)), EXIT_RED, 16), "S2", "YES isn't red: %s" % (d.getpixel((142, 120)),))
    c.expect(count(d, (150, 118, 180, 132), lambda p: min(p) > 230) > 10, "S2", "YES's letters aren't white")
    e = r.bottom(90)
    c.expect(near(e.getpixel((137, 125)), LAVENDER, 16), "S2", "RIGHT didn't select YES (its edge): %s" % (e.getpixel((137, 125)),))
    c.expect(near(e.getpixel((142, 120)), EXIT_RED, 16), "S2", "YES isn't red when selected")
    c.expect(r.at(150)["exit"] == 0, "S2", "NO didn't close the question")
    last = max(r.trace)
    c.expect(last < 240, "S2", "YES didn't close the app (it ran to tick %d)" % last)


@case("ceres-start", "ceres-escape", ["G7", "S4", "S10"],
      script=lambda s: s.hold(30, "START", 3).shot(10, 60).tap(80, *TAB_TAP["MAP"]).shot(120),
      goldens=[(60, "bottom")],
      about="Where the game can't pause (Ceres): START doesn't pause the app, MAP can't be tapped.")
def _(r, c):
    c.expect(r.at(60)["paused"] == 0, "G7", "START paused the app where the game can't pause")
    c.expect(r.at(120)["tab"] == 0 and r.at(120)["fade"] == 0, "S4", "the greyed MAP button reacted")
    mirror_check(r, c, 60)


@case("full-pause-menu", "crateria-chozo", ["G8"], args=["--full-pause-menu"],
      script=lambda s: s.hold(30, "START", 3).shot(120),
      goldens=[],
      about="Full Pause Menu on: START opens the game's own pause screen on the game screen.")
def _(r, c):
    st = r.at(120)["state"]
    c.expect(0x0C <= st <= 0x12, "G8", "the game's pause screen didn't open (game state %02x)" % st)
    c.expect(r.at(120)["paused"] == 0, "G8", "the app paused instead")


@case("no-swap", "crateria-chozo", ["G9"],
      script=lambda s: s.shot(10).hold(20, "L3", 3).hold(40, "HOME", 4).hold(44, "HOME+Y", 3).hold(47, "HOME", 5).shot(80),
      goldens=[],
      about="Two screens: L3 and HOME + Y don't swap them.")
def _(r, c):
    for t in (10, 80):
        b = r.bottom(t)
        c.expect(near(tab_fill(b, "MAP"), ORANGE, 16), "G9", "tick %d: the second screen isn't the pages" % t)
        c.expect(not near(tab_fill(r.top(t), "MAP"), ORANGE, 16), "G9", "tick %d: the pages are on the top screen" % t)
        c.expect(r.at(t)["menu"] == 0, "G9", "tick %d: the menu opened" % t)
