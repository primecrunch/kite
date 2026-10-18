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
       "// (tune/fft_times_m4.txt), from `kite tune`. Entries: length, us, stage order, matrix rows (0 = default).",
       "// Regenerate: tools/gen_fft_times.sh.",
       "static const struct { uint32_t L; float us; uint8_t ord; uint32_t rows; } kite_default_times[] = {"]
line = "   "
for r in rows:
    L, us, o = r[:3]
    rr = r[3] if len(r) > 3 else "0"
    item = " {%s, %s, %s, %s}," % (L, us, o, rr)
    if len(line) + len(item) > 99:
        out.append(line)
        line = "   "
    line += item
out += [line, "};"]
print("\n".join(out))
' > src/fft_times_default.h
