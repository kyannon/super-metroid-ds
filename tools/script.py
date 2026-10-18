#!/usr/bin/env python3
"""Turns a relative input script into the app's absolute one.

    tools/script.py IN OUT
IN lines: 'wait N', 'hold BTNS N', 'shot FILE', 'tap X,Y', 'quit'."""
import sys
t = 1
out = []
for line in open(sys.argv[1]):
    w = line.split()
    if not w or w[0].startswith('#'):
        continue
    if w[0] == 'wait':
        t += int(w[1])
    elif w[0] == 'hold':
        out.append(f"{t} {w[1]}")
        t += int(w[2])
        out.append(f"{t} -")
    elif w[0] == 'shot':
        out.append(f"{t} SHOT:{w[1]}")
        t += 1
    elif w[0] == 'tap':
        out.append(f"{t} TAP:{w[1]}")
        t += 4
    elif w[0] == 'quit':
        out.append(f"{t} QUIT")
open(sys.argv[2], 'w').write("\n".join(out) + "\n")
