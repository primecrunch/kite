#!/usr/bin/env python3
"""Sum check: with it on every squaring (KITE_SUM_EVERY=1), a fault injected into the state
between two squarings is caught at the next one (the sum of its outputs does not match the
square of the sum of its inputs) and redone, so the test ends with the right residue, a sum
error counted and no Gerbicz error. At the default (1 in 64) the same fault is left to Gerbicz.

Usage: tests/sum_check.py KITE    (build/kite)
"""
import os
import re
import subprocess
import sys

K, N, RES64, FAULT = 9357, 200001, "BDD48C45E17CDD83", 5000


def run(kite, every):
    env = dict(os.environ, KITE_SUM_EVERY=str(every))
    out = subprocess.run([kite, "prp", str(K), str(N), "--fault", str(FAULT), "--expect", RES64],
                         capture_output=True, text=True, env=env).stdout
    line = next((l for l in out.splitlines() if "RES64" in l), "")
    if "MATCH" not in out:
        sys.exit(f"every {every}: wrong residue: {line}")
    return {key: int(re.search(key + r" (\d+)", line).group(1)) for key in ("sum_errors", "gerbicz_errors")}


def main():
    kite = sys.argv[1]
    r = run(kite, 1)
    if r["sum_errors"] < 1 or r["gerbicz_errors"] != 0:
        sys.exit(f"check on every squaring: expected a sum error and no Gerbicz error: {r}")
    print(f"every squaring: fault caught by the sum check ({r}), residue matches")
    r = run(kite, 64)
    if r["sum_errors"] + r["gerbicz_errors"] < 1:
        sys.exit(f"default: expected the fault to be caught: {r}")
    print(f"default: fault caught ({r}), residue matches")


if __name__ == "__main__":
    main()
