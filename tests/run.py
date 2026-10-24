#!/usr/bin/env python3
"""Runs the regression tests (tests/cases.py) against the rules in
docs/RULES.md.

    tests/run.py                 all the cases
    tests/run.py NAME...         some (by name, or by rule ID: G4, S10...)
    tests/run.py --bless [NAME]  approve the screenshots as they are now
    tests/run.py --list          the cases and the rules they check

Needs the desktop build (make), a Super Metroid ROM (tests/fixtures/rom.sfc,
or SMDS_TEST_ROM) and the save states listed in tests/fixtures.txt in
tests/fixtures/. A case whose save state is missing is skipped. The
screenshots of failed checks and their differences from the approved ones
go to build/test-out/NAME/.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if "--wide" in sys.argv:
    os.environ["SMDS_TEST_WIDE"] = "1"  # before smtest reads it
import smtest  # noqa: E402
from cases import CASES  # noqa: E402
from PIL import Image, ImageChops  # noqa: E402

OUT = os.path.join(smtest.ROOT, "build", "test-out" + ("-wide" if smtest.WIDE else ""))


class Checker:
    def __init__(self):
        self.failures = []
        self.passed = 0

    def expect(self, ok, rule, msg):
        if ok:
            self.passed += 1
        else:
            self.failures.append((rule, msg))


def golden_name(case, tick, which):
    return "%s.%d.%s%s" % (case.name, tick, which, ".wide" if smtest.WIDE else "")


def compare_goldens(case, run, approved, bless, outdir):
    fails, new = [], {}
    for tick, which in case.goldens:
        name = golden_name(case, tick, which)
        img = run.top_full(tick) if which == "top" else run.bottom(tick)  # (in Match Width, all its columns)
        h = smtest.digest(img)
        path = os.path.join(smtest.GOLDEN_DIR, name + ".png")
        if bless:
            os.makedirs(smtest.GOLDEN_DIR, exist_ok=True)
            img.save(path)
            new[name] = h
            continue
        if name not in approved:
            fails.append(("golden", "%s: no approved screenshot yet (run with --bless once it looks right)" % name))
            continue
        if approved[name] == h:
            continue
        img.save(os.path.join(outdir, name + ".now.png"))
        msg = "%s: differs from the approved screenshot" % name
        if os.path.exists(path):
            old = Image.open(path).convert("RGB")
            d = ImageChops.difference(old, img)
            box = d.getbbox()
            n = sum(1 for p in smtest.pixels(d) if max(p) > 0)
            msg += " (%d pixels, in x %d-%d y %d-%d)" % (n, box[0], box[2] - 1, box[1], box[3] - 1) if box else ""
            old.save(os.path.join(outdir, name + ".approved.png"))
            # the difference, brightened, beside the two
            side = Image.new("RGB", (256 * 3 + 8, 224), (255, 0, 255))
            side.paste(old, (0, 0))
            side.paste(img, (260, 0))
            side.paste(d.point(lambda v: 255 if v else 0), (520, 0))
            side.save(os.path.join(outdir, name + ".diff.png"))
        fails.append(("golden", msg))
    return fails, new


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*")
    ap.add_argument("--bless", action="store_true", help="approve the screenshots as they are now")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--no-build", action="store_true", help="don't run make first")
    ap.add_argument("--wide", action="store_true", help="in Match Width (the widescreen patch from tests/fixtures/patches; screenshots approved separately)")
    a = ap.parse_args()

    cases = [c for c in CASES if not a.names or c.name in a.names or set(c.rules) & set(a.names)]
    if a.list:
        for c in CASES:
            print("%-18s %-22s %s\n%18s %s" % (c.name, c.fixture, " ".join(c.rules), "", c.about))
        return 0
    if not cases:
        print("no such case")
        return 2
    if not smtest.rom_path():
        print("no ROM: put a Super Metroid ROM at tests/fixtures/rom.sfc or set SMDS_TEST_ROM")
        return 2
    if not a.no_build:
        r = subprocess.run(["make", "-s", "-C", smtest.ROOT], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode:
            print(r.stdout.decode(errors="replace"))
            print("the build failed")
            return 2

    known = {}
    for line in open(os.path.join(smtest.ROOT, "tests", "fixtures.txt")):
        w = line.split()
        if len(w) >= 2 and not line.startswith("#"):
            known[w[1]] = w[0]
    approved = smtest.load_golden_list()
    blessed = dict(approved)
    total_fail = skipped = 0
    rules_failed = set()
    t0 = time.time()
    for c in cases:
        if not os.path.exists(os.path.join(smtest.FIXTURES, c.fixture + ".state")):
            print("SKIP %-18s (no tests/fixtures/%s.state)" % (c.name, c.fixture))
            skipped += 1
            continue
        with open(os.path.join(smtest.FIXTURES, c.fixture + ".state"), "rb") as f:
            h = hashlib.sha256(f.read()).hexdigest()
        if known.get(c.fixture) != h:
            print("WARN %-18s tests/fixtures/%s.state isn't the one in tests/fixtures.txt" % (c.name, c.fixture))
        outdir = os.path.join(OUT, c.name)
        shutil.rmtree(outdir, ignore_errors=True)
        os.makedirs(outdir)
        s = smtest.Script()
        c.script(s)
        for tick, _ in c.goldens:
            s.shot(tick)
        t1 = time.time()
        run = smtest.Run(c.fixture, s, outdir, env=c.env, args=c.args)
        chk = Checker()
        try:
            c.check(run, chk)
        except AssertionError as e:
            chk.failures.append(("run", str(e)))
        try:
            gf, new = compare_goldens(c, run, approved, a.bless, outdir)
        except AssertionError as e:
            gf, new = [("run", str(e))], {}
        if a.bless:
            blessed.update(new)
        fails = chk.failures + gf
        took = time.time() - t1
        if fails:
            total_fail += 1
            print("FAIL %-18s %s  (%.0f s)" % (c.name, " ".join(c.rules), took))
            for rule, msg in fails[:12]:
                print("       [%s] %s" % (rule, msg))
                rules_failed.add(rule)
            if len(fails) > 12:
                print("       ... and %d more" % (len(fails) - 12))
            print("       screenshots and trace: %s" % os.path.relpath(outdir, smtest.ROOT))
        else:
            print("ok   %-18s %s  (%d checks%s, %.0f s)" % (c.name, " ".join(c.rules), chk.passed,
                                                           ", %d screenshots" % len(c.goldens) if c.goldens else "", took))
            shutil.rmtree(os.path.join(outdir, "s"), ignore_errors=True)  # the shots of a passing case aren't needed
    if a.bless:
        smtest.save_golden_list(blessed)
        print("approved the screenshots (tests/golden.txt)")
    print("\n%d passed, %d failed, %d skipped in %.0f s" % (len(cases) - total_fail - skipped, total_fail, skipped, time.time() - t0))
    if rules_failed:
        print("rules broken: " + " ".join(sorted(rules_failed)))
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
