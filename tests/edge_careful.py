#!/usr/bin/env python3
"""Fermat tests at the edge of the round-off limit: squarings over 0.40 are redone carefully
at the same FFT length, and too many in one round move the test to a longer FFT without going
back to the verified state. Both must give the residue of a run at a comfortable length.

2007*2^432901-1 sits at kite_fit_bpw for its length (kite chooses 28800; the fit allows n up to
~432,920), where a full test sees about a dozen squarings over 0.40. The residue is from
`kite prp 2007 432901 --fft 32768`.

Usage: tests/edge_careful.py KITE    (build/kite)
"""
import re
import subprocess
import sys

K, N, RES64 = 2007, 432901, "EEF2EAA30690DC6A"


def run(kite, *extra):
    out = subprocess.run([kite, "prp", str(K), str(N), "--expect", RES64, *extra],
                         capture_output=True, text=True).stdout
    line = next((l for l in out.splitlines() if "RES64" in l), "")
    if "MATCH" not in out:
        sys.exit(f"{extra or 'default'}: wrong residue: {line}")
    num = lambda key: int(re.search(key + r" (\d+)", line).group(1))
    return num("careful_ops"), num("fft_retries"), line


def main():
    kite = sys.argv[1]
    careful, retries, line = run(kite)
    if careful < 1:
        sys.exit(f"expected careful redos at the edge: {line}")
    print(f"edge length: {careful} careful redos, {retries} FFT retries, residue matches")
    careful, retries, line = run(kite, "--careful-limit", "2")
    if retries < 1:
        sys.exit(f"expected a move to a longer FFT with --careful-limit 2: {line}")
    print(f"careful limit 2: moved up {retries} time(s) after {careful} careful redos, residue matches")


if __name__ == "__main__":
    main()
