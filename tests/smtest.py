"""Super Metroid DS regression tests: running the app on a save state with a
scripted input, and looking at what came out.

A test (tests/cases.py) is a save state (tests/fixtures/NAME.state), an input
script (buttons, taps, swipes, screenshots at given ticks) and checks: rules
from docs/RULES.md, each by its ID, plus "golden" screenshots compared pixel
for pixel with ones approved earlier.

The app runs without a window (SDL's offscreen driver), at one game frame a
tick, with the map captures waiting for each other (SMDS_SYNC_MAPCAP) and a
fixed clock (SMDS_TEST_CLOCK), so a run is the same every time. It writes a
line a tick of what it's doing (SMDS_TRACE): the game state, the page shown,
whether it's paused...

Screens: the window is 640 x 960, the game screen on top and the second
screen below, each a 256 x 224 picture at twice its size. Checks look at them
at the game's own size, in its pixels: top(t) and bottom(t) are 256 x 224
images, (0, 0) the top-left corner.
"""
import hashlib
import os
import shutil
import subprocess
import tempfile

from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURES = os.environ.get("SMDS_TEST_FIXTURES", os.path.join(ROOT, "tests", "fixtures"))
GOLDEN_DIR = os.path.join(FIXTURES, "golden")
GOLDEN_LIST = os.path.join(ROOT, "tests", "golden.txt")
APP = os.environ.get("SMDS_TEST_APP") or os.path.join(ROOT, "build", "host", "sm-ds")  # (another build: to see a test fail without a fix)
WIN_W, WIN_H = 640, 960
WIDE = os.environ.get("SMDS_TEST_WIDE") == "1"  # tests/run.py --wide
WIDE_EXT = 32 if WIDE else 0
SCREEN_X, SCREEN_Y, SCALE = 64, 16, 2  # where a 256 x 224 picture sits in each 640 x 480 half


def rom_path():
    p = os.environ.get("SMDS_TEST_ROM") or os.path.join(FIXTURES, "rom.sfc")
    return p if os.path.exists(p) else None


class Script:
    """Input at absolute ticks (the app's loop count, 1 a frame). Buttons are
    the app's names: A B X Y START SELECT HOME L R L2 R2 UP DOWN LEFT RIGHT L3."""

    def __init__(self):
        self.lines = []
        self.shots = set()
        self.end = 0

    def hold(self, tick, buttons, frames):
        self.lines.append((tick, buttons))
        self.lines.append((tick + frames, "-"))
        self.end = max(self.end, tick + frames)
        return self

    def tap(self, tick, x, y):
        """a tap on the second screen at (x, y) in its 256 x 224 pixels"""
        fx, fy = to_window(x, y)
        self.lines.append((tick, "TAP:%.4f,%.4f" % (fx, fy)))
        self.end = max(self.end, tick + 3)
        return self

    def swipe(self, tick, x0, y0, x1, y1):
        a, b = to_window(x0, y0)
        c, d = to_window(x1, y1)
        self.lines.append((tick, "SWIPE:%.4f,%.4f,%.4f,%.4f" % (a, b, c, d)))
        self.end = max(self.end, tick + 8)
        return self

    def shot(self, *ticks):
        for t in ticks:
            self.shots.add(t)
            self.end = max(self.end, t)
        return self

    def shots_every(self, first, last, step=1):
        return self.shot(*range(first, last + 1, step))

    def text(self, shotdir):
        lines = list(self.lines)
        for t in sorted(self.shots):
            lines.append((t, "SHOT:%s/%d.png" % (shotdir, t)))
        lines.append((self.end + 2, "QUIT"))
        lines.sort(key=lambda l: l[0])  # stable: same-tick lines keep their order
        return "".join("%d %s\n" % l for l in lines)


def to_window(x, y):
    """second-screen pixel -> window fraction (as the app's TAP: takes it)"""
    return ((SCREEN_X + SCALE * x + 1) / WIN_W, (WIN_H / 2 + SCREEN_Y + SCALE * y + 1) / WIN_H)


class Run:
    """One run of the app: its screenshots and its trace."""

    def __init__(self, fixture, script, workdir, env=None, args=()):
        self.fixture = fixture
        self.dir = workdir
        shotdir = os.path.join(workdir, "s")
        os.makedirs(shotdir, exist_ok=True)
        statedir = os.path.join(workdir, "st")
        os.makedirs(statedir, exist_ok=True)
        sp = os.path.join(workdir, "input.txt")
        with open(sp, "w") as f:
            f.write(script.text(shotdir))
        tp = os.path.join(workdir, "trace.txt")
        e = dict(os.environ)
        e.update({"SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy", "SMDS_WINDOW": "%dx%d" % (WIN_W, WIN_H),
                  "SMDS_SYNC_MAPCAP": "1", "SMDS_TRACE": tp, "SMDS_TEST_CLOCK": "10:24 87"})
        e.update(env or {})
        cmd = [APP, "--load-state=" + os.path.join(FIXTURES, fixture + ".state"), "--input-script=" + sp,
               "--state-dir=" + statedir, "--no-auto-resume", "--scale=integer", "--map-scale=integer"]
        if WIDE:
            # Match Width (docs/widescreen.md): the widescreen patch from tests/fixtures/patches;
            # in the 640-wide window 32 columns a side, so top() is still the middle 256
            cmd += ["--scale=wide", "--patch-dir=" + os.path.join(FIXTURES, "patches")]
        cmd += list(args) + [rom_path()]
        r = subprocess.run(cmd, env=e, cwd=workdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900)
        self.log = r.stdout.decode(errors="replace")
        self.trace = {}
        if os.path.exists(tp):
            for line in open(tp):
                d = {}
                for kv in line.split():
                    k, v = kv.split("=", 1)
                    d[k] = int(v, 16) if k in ("state", "room", "bright") else int(v)
                self.trace[d["tick"]] = d
        self._shots = {}
        self.shotdir = shotdir
        self.wanted = sorted(script.shots)

    def shot(self, t):
        if t not in self._shots:
            p = os.path.join(self.shotdir, "%d.png" % t)
            if not os.path.exists(p):
                raise AssertionError("no screenshot at tick %d (the app stopped early?)" % t)
            self._shots[t] = Image.open(p).convert("RGB")
        return self._shots[t]

    def top(self, t):
        return screen(self.shot(t), 0)

    def top_full(self, t):
        """the whole game screen: 256 + 2 * WIDE_EXT columns in Match Width"""
        w = self.shot(t)
        return w.crop((0, SCREEN_Y, WIN_W, SCREEN_Y + 224 * SCALE)).resize((WIN_W // SCALE, 224), Image.NEAREST) if WIDE else self.top(t)

    def bottom(self, t):
        return screen(self.shot(t), 1)

    def at(self, t):
        """the trace line of tick t (or the last one before it)"""
        ks = [k for k in self.trace if k <= t]
        return self.trace[max(ks)] if ks else {}


def screen(win, which):
    y0 = which * WIN_H // 2 + SCREEN_Y
    return win.crop((SCREEN_X, y0, SCREEN_X + 256 * SCALE, y0 + 224 * SCALE)).resize((256, 224), Image.NEAREST)


# ---------------------------------------------------------------- pixel help

def pixels(img):
    f = getattr(img, "get_flattened_data", None)
    return f() if f else img.getdata()


def count(img, box, pred):
    """pixels in box (x0, y0, x1, y1, end exclusive) for which pred((r, g, b))"""
    return sum(1 for p in pixels(img.crop(box)) if pred(p))


def saturated(p, amount=60):
    return max(p) - min(p) > amount


def colourful(img, box, amount=60):
    return count(img, box, lambda p: saturated(p, amount))


def suit_yellow(p):  # Samus's power suit
    return p[0] > 180 and p[1] > 120 and p[2] < 60


def near(p, rgb, tol=24):
    return all(abs(a - b) <= tol for a, b in zip(p, rgb))


def mean(img, box):
    px = list(pixels(img.crop(box)))
    return tuple(sum(c[i] for c in px) / len(px) for i in range(3))


def digest(img):
    return hashlib.sha256(img.tobytes()).hexdigest()


# ---------------------------------------------------------------- goldens

def load_golden_list():
    d = {}
    if os.path.exists(GOLDEN_LIST):
        for line in open(GOLDEN_LIST):
            w = line.split()
            if len(w) == 2 and not line.startswith("#"):
                d[w[0]] = w[1]
    return d


def save_golden_list(d):
    with open(GOLDEN_LIST, "w") as f:
        f.write("# approved screenshots: NAME sha256 of the 256x224 RGB pixels (tests/run.py --bless writes it)\n")
        for k in sorted(d):
            f.write("%s %s\n" % (k, d[k]))
