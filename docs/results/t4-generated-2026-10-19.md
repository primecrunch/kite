# Four-step twiddles built in the kernels (M4, 4 P + 6 E), 2026-10-19

The full four-step twiddle table `t4` is as large as the number (16 bytes per element) and is read
every squaring. The `t4g` kernels (`gen/gen_kernels.py`, layout b) build row r's twiddle for a
lane-group as hi[r] · lo[r][lane-group] instead: lo is one 128-byte row per row of the plan
(20 KB at 240K) shared by every group, hi one complex per row and group (1/8 of the full table).
That is one complex multiply per element and direction, by-element FMAs on data already in
registers. The earlier scratch-buffer version (`docs/experiments/t4-generated-twiddles.patch`)
built the same values in C into a strided scratch and lost 2–4%.

Same binary, `KITE_T4=table` against `KITE_T4=gen`, ABBA, 60 s per run, ms per squaring per test
(`t4-generated-2026-10-19.txt`); "gen speed" > 1 means the generated twiddles are faster.

| FFT | case | 1 test | 4 tests | 10 tests |
|---|---|--:|--:|--:|
| 64K | 2007·2^960000−1 | | | 0.982 |
| 128K | 2007·2^1900000−1 | | | 0.986 |
| 160K | 1021·2^2300003−1 | | | 0.992 |
| 168K | 6073·2^2290815−1 | | | 0.998 |
| 192K | 2009·2^2650000−1 | | | 1.003 |
| 210K | 2009·2^2980000−1 | | | 1.010 |
| 240K | 2009·2^3322280−1 | 0.985 | 0.990 | 1.022 |
| 256K | 6767·2^3322174−1 | 0.986 | 0.995 | 1.038 |
| 400K | 4813·2^5500155−1 | 0.985 | 1.025 | 1.056 |

- The extra multiply costs 1.5% on one test at every size; the saved traffic pays from 192K up with
  every core busy, and the gain grows with size, as the E-cores' 4 MB L2 runs out.
- Default: generated from FFT 192K (H ≥ 98304), table below; the SME path keeps the table.
- Against PRST at 10 tests (`scaling-pe-2026-10-19.md`): 240K 0.99× → 1.01×, 256K 0.96× → 0.985×.
- It accounts for a small part of the E-core shortfall (14–19% at 240K–256K); the 7% that shows
  even at 64K, where everything fits, is untouched.
- Max error at 9357·2^200001−1 (14K, table by default): 0.250 table, 0.219 generated.
