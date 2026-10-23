#!/usr/bin/env python3
"""Looking around a fixture, to write a test for it.

    tests/explore.py FIXTURE OUTDIR [STEP...]

Steps: 'hold TICK BUTTONS FRAMES', 'tap TICK X Y', 'swipe TICK X0 Y0 X1 Y1',
'shots FIRST LAST STEP', 'sheet FIRST LAST STEP' (a contact sheet of both
screens, OUTDIR/sheet.png). Prints the trace's changes (game state, page...)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smtest import Run, Script  # noqa: E402
from PIL import Image  # noqa: E402

fixture, out = sys.argv[1], sys.argv[2]
s = Script()
sheet = None
a = sys.argv[3:]
i = 0
while i < len(a):
    w = a[i]
    if w == "hold":
        s.hold(int(a[i + 1]), a[i + 2], int(a[i + 3])); i += 4
    elif w == "tap":
        s.tap(int(a[i + 1]), int(a[i + 2]), int(a[i + 3])); i += 4
    elif w == "swipe":
        s.swipe(int(a[i + 1]), *map(int, a[i + 2:i + 6])); i += 6
    elif w in ("shots", "sheet"):
        f, l, st = int(a[i + 1]), int(a[i + 2]), int(a[i + 3]); i += 4
        s.shots_every(f, l, st)
        if w == "sheet":
            sheet = list(range(f, l + 1, st))
    else:
        raise SystemExit("step? " + w)
os.makedirs(out, exist_ok=True)
r = Run(fixture, s, out)
keys = ["state", "room", "msg", "tab", "next", "fade", "tabsOn", "paused", "menu", "exit", "haveMap", "haveSam", "esc"]
last = None
for t in sorted(r.trace):
    v = tuple(r.trace[t][k] for k in keys)
    if v != last:
        print(t, " ".join("%s=%s" % (k, (("%02x" % x) if k in ("state",) else ("%04x" % x) if k == "room" else x)) for k, x in zip(keys, v)))
        last = v
if sheet:
    cols = 6
    rows = (len(sheet) + cols - 1) // cols
    im = Image.new("RGB", (cols * 260, rows * 460), (40, 0, 40))
    for k, t in enumerate(sheet):
        x, y = (k % cols) * 260, (k // cols) * 460
        try:
            im.paste(r.top(t), (x, y))
            im.paste(r.bottom(t), (x, y + 228))
        except AssertionError:
            pass
    im.save(os.path.join(out, "sheet.png"))
print(r.log[-400:] if "error" in r.log.lower() else "")
