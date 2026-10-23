# Regression tests

Each test plays a save state in the desktop build with a scripted input, and
checks what comes out against the rules in `docs/RULES.md`, which it names by
ID. It also compares chosen screenshots pixel for pixel with approved ones.

    make test                     # build, then run them all
    tests/run.py G4 S10           # just the cases that check those rules
    tests/run.py ship-jump        # one case
    tests/run.py --list           # what there is
    tests/run.py --bless NAME     # approve NAME's screenshots as they are now

A failed check prints its rule and what it saw. The run's screenshots, its
trace (a line per frame: game state, page, paused...) and, for a screenshot
that changed, `NAME.diff.png` (approved | now | the pixels that differ) are
in `build/test-out/NAME/`.

## What's needed

- The desktop build: `scripts/setup-host.sh`, then `make`.
- A Super Metroid ROM (Japan / USA) at `tests/fixtures/rom.sfc`, or
  `SMDS_TEST_ROM=path`.
- The save states in `tests/fixtures/`, listed with their checksums in
  `tests/fixtures.txt`. They aren't in the repository, because they hold the
  game's own graphics. The approved screenshots (`tests/fixtures/golden/`)
  aren't either. Their checksums are in `tests/golden.txt`, so a run without
  the pictures still catches a change, but only the pictures show what
  changed. A case whose state is missing is skipped.

## When a change is meant to look different

Run the tests. Look at the `diff.png` of each screenshot that changed. If
the new picture is right, approve it with `tests/run.py --bless NAME`, and
commit `tests/golden.txt` with the change. If a rule changed, change
`docs/RULES.md` and the case's checks in the same commit.

## Adding a case

Make a save state just before the moment (Save State on the MENU page, then
copy the `.state` file from `states/`). Put it in `tests/fixtures/`, add it to
`tests/fixtures.txt`, and write the case in `tests/cases.py`: the input
(`hold`, `tap`, `swipe`, `shot` at ticks, one tick a frame), the checks (each
under its rule's ID), and the screenshots to approve. `tests/explore.py
FIXTURE OUTDIR ...` shows the trace and a contact sheet of a run, to find
the ticks worth checking.

## How a run is made the same every time

- `SDL_VIDEODRIVER=offscreen`: no window, and one game frame a tick.
- `SMDS_SYNC_MAPCAP=1`: a map capture finishes before the game goes on, so
  the pages change on the same frame every run.
- `SMDS_TEST_CLOCK="10:24 87"`: the time and the battery on the MENU page.
- `SMDS_TRACE=file`: the trace.
- `SMDS_TEST_ESCAPE=1`: the escape message counts as up from the start, for
  states made after the room where it's written.
