# Where kite's lead goes under load (M4, 4 P + 6 E), 2026-10-19

kite 0.1.13 against PRST 14 (gwnum 31.6 under Rosetta). 1, 4, 6 and 10 single-thread Fermat tests at
once, ABBA order, 60–100 s per run. Figures are total squarings per millisecond (sum over tests of
1/ms) as kite ÷ PRST. Raw per-test medians: `scaling-pe-2026-10-19.txt`.

| Case | FFT | 1 test | 4 (P cores) | 6 | 10 (all) | E-core tests add, kite ÷ PRST |
|---|--:|--:|--:|--:|--:|--:|
| 2007·2^960000−1 | 64K | 1.106 | 1.110 | 1.081 | 1.035 | 0.93 |
| 2007·2^1900000−1 | 128K | 1.069 | 1.076 | 1.042 | 0.998 | 0.89 |
| 2009·2^3322280−1 | 240K | 1.080 | 1.079 | 1.046 | 0.989 | 0.86 |
| 6767·2^3322174−1 | 256K | 1.048 | 1.054 | 1.020 | 0.956 | 0.81 |

- On the P cores kite keeps its single-test lead: 1 → 4 tests changes the ratio by under 1%. The
  23% per-test slowdown from 1 to 4 tests is the same for both engines and is there at 64K, where
  four tests fit easily in the 16 MB P-cluster L2, so it is clock, not memory.
- The lead goes on the E cores: the throughput the six E-core tests add (4 → 10) is 7–19% lower
  with kite, and the shortfall grows with FFT size. At 64K, where six tests' data (0.5 MB each)
  fits the 4 MB E-cluster L2, it is 7%; at 256K, 19%.
- So two parts: about 7% that does not depend on size (kite's kernels run relatively worse on the
  E cores), and a size-dependent part that fits a larger working set per test.
- The tests in one run all show the same rate: macOS rotates them across P and E cores, so P and
  E can only be separated from totals.

A suspect for the size-dependent part: the four-step twiddle table `t4` is as large as the number
(1.9 MB at 240K) and is read every squaring, roughly doubling kite's cache footprint. The
scratch-buffer version of generated twiddles lost 2–4% at 10 tests; generating them inside the
t4 kernels (no scratch store and reload) is untried.
