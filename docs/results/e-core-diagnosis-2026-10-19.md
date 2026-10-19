# Why kite loses its lead on the E cores (M4, 240K, 10 tests), 2026-10-19

kite 0.1.14 against PRST 14 on eagle (4 P + 6 E), 10 single-thread tests of 2009·2^3322280−1.
Tools in `tools/pmc/`: `rus` (per-process instructions/cycles/time on P cores and in total,
`proc_pid_rusage` v6, no root), `pmc` (system-wide per-CPU kpc counters through kperf, root),
`xtsym.py` (xctrace CPU Counters samples joined to core type and symbolised).

**Per core-second** (`rus`): kite does 4.6% more squarings per P-core second than PRST and 10%
fewer per E-core second. Tests spend about 59% of their time on E cores.

**Phase profile** (`KITE_PROF=1`, 4 → 10 tests): the normaliser and split grow least (1.4×); the
pass-2 row copies and the first stages of each pass grow most (1.7–1.9×). The same at 64K, where
six tests' data fits the E-cluster L2. Per-function samples by core type (`xtsym.py`) agree:
`dif_r4_rows` 1.28, `dit_r4_rows_tan` 1.20, `dit_r8_tan` 1.20, `dit_r8_t4g_b` 1.16 times their P
share on E cores; `norm_av` 0.86, `split_sq` 0.81.

**Not memory traffic** (`pmc`, per squaring): kite has fewer L1 load misses (240k against 393k),
store misses (50k / 126k), write-backs (75k / 99k) and L1 TLB misses (7k / 23k) than PRST. Cycles
stalled on an old L1 miss: E cores 9.1% (kite) against 8.3% (PRST).

**Not the vector pipes**: PRST sustains 2.52 vector-arithmetic instructions per cycle on an E
core, kite 1.86 (P cores: 4.84 / 4.12). kite issues 21% fewer vector-arithmetic instructions per
squaring (11.2M against 14.1M) and 38% fewer vector loads, at a lower rate; total cycles per
squaring are level (3.92M / 3.98M).

**Not radix-8 register pressure in the simple form**: factoring without radix 8 (radix-4/2
stages, fewer live registers) is 5% slower on one test and 6% slower at 10.

So kite's code issues fewer instructions per cycle than gwnum's, most on E cores, and the cause is
inside the kernels that start each pass (dependency chains, store-to-load ordering, or the
layout-b loads), not misses or pipe count. Next: per-kernel IPC on E cores, and the bottleneck
category (delivery / processing / discarded) for those kernels.

## Per kernel, in isolation (`tools/kloop.c`, 10 copies, L1-resident data)

| Kernel | IPC P | IPC E | E/P |
|---|--:|--:|--:|
| dit_r8_tan | 8.22 | 3.49 | 0.42 |
| dit_r4_rows_tan | 8.94 | 3.86 | 0.43 |
| dit_r8_tan_b | 8.76 | 3.93 | 0.45 |
| dif_r4_rows | 7.56 | 3.75 | 0.50 |
| dit_r8_t4g_b | 6.51 | 3.31 | 0.51 |
| dif_r8 | 6.12 | 3.18 | 0.52 |
| dif_r8_b | 6.51 | 3.61 | 0.55 |
| dif_r8_t4g_b | 6.57 | 3.65 | 0.56 |
| split_sq | 4.81 | 3.12 | 0.65 |

E cores stay near 3–4 instructions per cycle in every kernel; P cores reach 8–9 on the tangent
kernels, where 21% of instructions are vector `mov`s (eliminated at rename on P cores).

**Register-offset addressing** in the layout-b stage kernels (`ldr q, [x12, off]`, the imaginary
half from a second base 64 bytes on) removes 15 of 17 address adds per loop in pass 1
(`dit_r8_tan_b` 148 → 133 instructions). Measured against 0.1.14, ABBA, 60 s: 1 test −0.3 to
+0.1%; 10 tests +0.4% (240K, 256K), +0.6% (400K). So the E cores are not bound by instruction
count: the integer adds ran on otherwise idle integer pipes. Kept: correct and slightly faster.
