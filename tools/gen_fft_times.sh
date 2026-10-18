#!/bin/sh
# Rebuild src/fft_times_default.h from the tuning files: tune/fft_times_m4pro.txt below 98304,
# tune/fft_times_m4.txt from 98304 (`kite tune LMIN LMAX --out FILE` makes them).
cd "$(dirname "$0")/.." || exit 1
{ awk '$1 < 98304' tune/fft_times_m4pro.txt; awk '$1 >= 98304' tune/fft_times_m4.txt; } |
    sort -n -k1,1 | python3 -c '
import sys
rows = [l.split() for l in sys.stdin if l.strip()]
out = ["// Default FFT squaring times, used when there is no tuning file (see load_times in ctx.c):",
       "// one core of an M4 Pro below 98304 (tune/fft_times_m4pro.txt) and of an M4 from 98304",
       "// (tune/fft_times_m4.txt), from `kite tune`. Lines: length, us, stage order.",
       "// Regenerate: tools/gen_fft_times.sh.",
       "static const struct { uint32_t L; float us; uint8_t ord; } kite_default_times[] = {"]
line = "   "
for L, us, o in rows:
    item = " {%s, %s, %s}," % (L, us, o)
    if len(line) + len(item) > 99:
        out.append(line)
        line = "   "
    line += item
out += [line, "};"]
print("\n".join(out))
' > src/fft_times_default.h
