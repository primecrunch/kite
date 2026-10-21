# Next investigations at production sizes

After 0.1.9 (2026-10-18), with every core busy at n = 2.3M–5.5M, kite runs 0.85–0.92× PRST's
tests per hour on an M4 (4P + 6E) and 0.91–1.00× on an M4 Pro (10P + 4E)
(`docs/results/load-bench-2026-10-18-*-0.1.9.md`). With one test alone it is 4–10% behind
from FFT 160K up. Three areas account for that gap. Each starts with a measurement that bounds
what it could gain, under the rules in `docs/large-fft-plan.md`: production sizes, every core
busy, a step dropped below 3%.

| Area | Where it costs | Size of the prize | Kind of work |
|---|---|---|---|
| 1. FFT round-off | kite needs a longer FFT than PRST at some sizes | ~8% at those sizes (432K against 400K) | research |
| 2. Normaliser without the table | every size above FFT 96K | up to 11% alone; unknown under load | kernel work |
| 3. Four-step twiddle table | every size, mostly under load | unknown: bound first | kernel work |

Suggested order: the bound for 3 first (a day or less), then 2, then 1.

## 1. FFT round-off: shorter FFTs at the same safety

### Facts

PRST (gwnum 31.6) runs 4813·2^5500155−1 at 400K (13.43 bits per word); kite needs 432K, 8%
more work, because its max error at 400K is 0.375 within 2000 squarings. Measured max errors
(`kite time --fft L`, one core):

| Number | L | bits/word | `kite_safe_bpw` | max error | squarings |
|---|--:|--:|--:|--:|--:|
| 2007·2^425985−1 | 28672 | 14.86 | 14.90 | 0.3125 | 20000 |
| 2007·2^425985−1 | 30720 | 13.87 | 14.87 | 0.0781 | 20000 |
| 2007·2^425985−1 | 32768 | 13.00 | 14.85 | 0.0195 | 20000 |
| 6073·2^2290815−1 | 172800 | 13.26 | 13.31 | 0.2812 | 5000 |
| 6073·2^2290815−1 | 184320 | 12.43 | 13.29 | 0.0898 | 5000 |
| 2009·2^3322280−1 | 245760 | 13.52 | 13.95 | 0.1562 | 3000 |
| 2009·2^3322280−1 | 262144 | 12.67 | 13.92 | 0.0391 | 3000 |
| 4813·2^5500155−1 | 409600 | 13.43 | 13.10 | 0.3750 | 2000 |
| 4813·2^5500155−1 | 442368 | 12.43 | 13.05 | 0.1055 | 2000 |

- Each bit per word roughly quadruples the max error, as it should for products of digits twice
  as large.
- Every max error is a short dyadic fraction (3/8, 5/16, 9/32, 5/32, 5/64, 5/128). So the
  rounded value v = D·osc is large enough that its own ulp is 1/32 to 1/128: the error is set
  by how close the convolution outputs come to 2^53, not by noise building up through the
  stages. That points at the output magnitude (digit size, the k factor, the weights' range),
  not at twiddle accuracy.
- The tangent-form DIT twiddles are not the cause: `KITE_NO_TAN=1` gives the same 0.375 at
  400K, and 0.1406 against 0.1562 at 240K.
- `kite_safe_bpw` was fitted to "max error ≤ 0.25 over a few hundred squarings", less 0.2 bits.
  gwnum's FFT limits come from its own error targets and checks, so the two are not calibrated
  the same way. Part of the gap may be calibration, part real.

### Questions

1. **What does gwnum's max round-off look like** at the same L and bits per word? PRST's `-d`
   output and gwnum's round-off checking should report it. If gwnum also sits near 0.35–0.4 at
   400K, the difference is calibration and what to tolerate (with Gerbicz checks catching the
   rare bad block), not accuracy.
2. **Where is kite's largest error made?** Instrument the normaliser to log the digit index,
   row and v of each new max: one weight class (big digits, e = 1, the top digits around the
   fold) would point at the scaling. Repeat at k = 2007 and k = 6767 to separate the
   k penalty (`0.5·log2 k` in the model) from the length term.
3. **Is the output magnitude higher than it has to be?** Candidates: the digit balance (are
   digits in [−base/2, base/2) everywhere, including the fold and the carry absorbed at
   digit 15?), the scale factor `k/(4H)` folded into IBs, and the weights' range (2^±1/2
   steps).

### Ways to gain, once the source is known

- Recalibrate `kite_safe_bpw` against full tests with Gerbicz retries counted: if a full test at
  0.3–0.35 never retries, the model is too cautious.
- Reduce the output magnitude: tighter balanced digits, or splitting k (k = k1·k2 with
  k1 in the weights) if the k penalty dominates.
- Keep the 0.25 bits-per-word room in `kite_choose_fft_len` until full tests show it can shrink.

### Exit

4813·2^5500155−1 at 400K with max error ≤ 0.3 over a full test and no `fft_retries`, its
residue matching `tests/large_fft_cases.csv`; or a measured reason kite cannot get there with
53-bit doubles.

## 2. A cheaper normaliser without the table

### Facts

- Above FFT 96K the normaliser uses the compressed weights (`A`, `IA`, `mask`: 3 bytes per
  element) instead of the 48-byte-per-element table, because with every core busy the table
  costs up to 24% at 256K in cache misses (`docs/results/ntab-sweep-2026-10-17.md`).
- One test alone is then 11% slower at 240K–256K (0.727 → 0.811 ms per squaring), 5–11% across
  160K–432K against PRST.
- Per digit, the table path does 7 vector ops plus 3 loads; the compressed path 18 ops. A
  two-chain version (`docs/experiments/norm-two-chain-compressed.patch`) changed nothing on one
  core, so the compressed path is limited by its op count, not by the carry chain's latency.

### Ideas

- **Fewer ops per digit.** Four of the 18 extract the e and big flags (`shl` + `and`, twice). A
  per-row expansion of the flags into lane masks, once per row pair, or a `tbl` lookup on a byte
  per digit, could cut that to one or two. `osc` and `w` are both `A`- or `IA`-times-a-per-digit
  constant, adjusted by a power of two; precomputing `IA·IBs[i]` and `A·B[i]` once per row
  pair for 16 digits is 32 multiplies per 32 digits, the same as now, so the saving has to come
  from the flag handling and the base/ib adds.
- **A smaller table.** Store only `osc` and `bw` (16 bytes per element, a third of today's),
  derive `ib` from the big flag. At 256K that is 2 MB, the size of the data. Measure it under
  load at 120K–256K against both today's paths: it may win where the full table loses.
- **Choose by measurement, not size.** If neither variant wins everywhere, `ntab_policy` could
  pick per FFT length from a loaded calibration, like `kite tune` does for lengths.

### Bound

`KITE_NTAB=on` alone gives the ceiling for one test (the table path's FP work with no cache
cost). Under load the ceiling is unknown. Measure a wrong build whose compressed kernel skips
the flag extraction (constant te and tb): its gain at 10 and 14 tests bounds the first idea.

### Exit

The compressed path within 3% of the table path on one core at 240K, with no loss at 10 and
14 tests, or a measured reason it cannot get there.

## 3. The four-step twiddle table

### Facts

- `t4` holds w_H^(c·k_r) for every element, 16 bytes each: the size of the data (2 MB at 256K,
  3.4 MB at 432K). Each pass-1 group reads its slice once and reuses it between the inverse and
  forward stages.
- Building each group's twiddles from two small tables (w^((8g+j)k_r) = hi[g][p] · lo[p][j])
  into a per-thread scratch (`docs/experiments/t4-generated-twiddles.patch`) cost 6% on one
  core and 2–4% at 10 tests on the M4 (1.55 → 1.61 ms at 256K, 2.87 → 2.91 at 432K). The
  max error rose 0.039 → 0.047 from the two rounded factors.

### Ideas

- **Generate inside the t4 kernels.** `dif_t4` / `dit_t4` load the twiddle at the element's
  address plus `delta`. Loading hi (one per row) and lo (eight per group, in registers) and
  multiplying there removes the scratch store and reload (16 KB a group) and keeps one complex
  multiply. Whether that is cheaper than the patch's C loop is the question.
- **Half a table from symmetry.** Rows p and their partner pp (k_r and R − k_r) have
  t4[pp][c] = w_C^c · conj(t4[p][c]): one table of half the rows plus a C-sized table, at one
  complex multiply for half the elements.
- **Move the twiddles to pass 2.** Apply w^(c·k_r) to each row in pass 2, where a row's twiddles
  are powers of a single w^(k_r): a recurrence with an exact reload every 8 or 16 elements, no
  table. gwnum's layout does something of this kind; it changes where the multiply happens, so
  it needs both passes reworked.

### Bound

A wrong-results build that points every group at the same 16 KB twiddle slice (no traffic, the
same FP work), at 10 and 14 tests on both machines at 240K–432K. If it gains under 3%, drop the
area: the table's traffic is not what costs.

### Exit

The bound measured, and if it clears the bar, a version within 3% of it with the max error at
or below today's.

## Correctness for all three

Every change is checked as 0.1.9 was:
- the ctest suite (22 tests, including the long known-residue test);
- at least one complete production-size test against `tests/large_fft_cases.csv`;
- max error compared with the table above at the same L.

Round-off work in particular needs complete tests, not 2000-squaring samples, before any
length changes.

## Status, 2026-10-18 (0.1.10)

Area 2 was solved differently from the ideas above: pattern records (`gen_norm_pat`) give the
table path's digits from 26 KB, and kite with every core busy is now 0.96–1.06× PRST on the M4
and 0.99–1.13× on the M4 Pro at n ≤ 4.8M (`journal.md`, 2026-10-18). Area 1 turned out to be a
policy question, not accuracy: gwnum's limits sit on kite's fitted line without the 0.2 margin,
worth ~2% net (journal, 11:30). Area 3 measured twice as a net loss. A smaller pass-1 group for
the E-cores' 64 KB L1 did nothing (journal, 12:35).

## 4. Engine choice per FFT size in the primecrunch client

kite and PRST now trade wins by size: kite 1.13× at 160K on the M4 Pro, PRST 1.12× at 432K on the
M4 (where it runs 400K and kite 432K). The client already calibrates the tests × threads split per
FFT (`client/internal/benchmark/tuner.go`). Calibrating both engines per FFT the same way and
running the faster one gives every Mac the better engine at every size, with no further engine
work, and moves each size over to kite as kite improves.

- Needs: kite 0.1.10 released and embedded; the client's hints file keyed by engine as well as
  FFT; both engines' checkpoints stay readable only by their own engine, so a test keeps the
  engine it started on.
- Exit: on eagle and wolf, the client picks kite at 160K–288K and PRST at 432K, and its tests
  per hour match the faster engine's within the calibration's noise.
- A primecrunch change: needs Andy's go-ahead.
- Status 2026-10-19: lower priority. With 0.1.14+ kite is level or ahead at every production
  size measured (240K, 256K, 400K), so there is no size range left for PRST to win. Revisit only if
  production data shows one.

## 5. The GPU on desktop Macs

`gpu/gpu_prp.m` runs complete Gerbicz-checked tests at one length (32768): 173 tests/hour alone
on the M4 Pro. On that laptop CPU + GPU lost (2026-10-12): within a minute macOS capped the
P-cores at 1800 MHz and ~5 W to give the GPU its power, and 13 CPU tests + GPU sustained ~244
tests/hour against ~372 for 14 CPU tests. A desktop (eagle, a Mac mini) has a larger power
budget, so the GPU may add throughput on top of every CPU core.

- First measurement (no new code): on eagle, 10 CPU tests alone against 9 CPU tests + the GPU
  prototype at 32K, 25 minutes each with `powermetrics`, as the 2026-10-12 laptop runs.
- Only if CPU + GPU wins there: lengths at production size (160K–432K), word size per n, and the
  driver, before kite-prst could use it.
- Exit: a measured sustained gain on a desktop, or a measured reason the GPU cannot add to the
  CPU on Apple Silicon at these sizes.
- Status 2026-10-19: the first measurement is done. On eagle, 9 CPU tests + the GPU at 32K gave
  +13% over 10 CPU tests. The next step is the production-size work above.

## 6. kite as the default engine on Apple Silicon

Today only machines started with `PC_ENGINE=kite` run kite; every other member's Mac runs PRST
under Rosetta. Over whole production tests, kite 0.1.13 beat PRST by 16% (240K) and 10% (256K) on
wolf (M4 Pro, 4 of 14 cores E) and by 2–6% (240K) / level (256K) on eagle (M4, 6 of 10 E); 0.1.14
and 0.1.15 add 2–6% under load on top.

- Before deciding: a day of 0.1.15 on both machines, then the production comparison by
  `compute_seconds ÷ n` per FFT class (dba agent), excluding tests restarted across engines.
- Cost: checkpoints are not shared between engines, so switching restarts the tests a machine has
  in progress, at most one test per core, once.
- A primecrunch change: needs Andy's go-ahead. Highest expected gain of anything on this list.

## 7. Vector scheduling for the E cores

Measured on eagle (`docs/results/e-core-diagnosis-2026-10-19.md`): kite does 4.6% more squarings
per P-core second than PRST and 10% fewer per E-core second. Not memory traffic, misses, the
normaliser, radix-8 register pressure or instruction count. Isolated kernels reach 8–9 IPC on P
cores and 3–4 on E cores; kite issues 1.86 vector instructions per cycle on an E core against
PRST's 2.52.

- Idea: interleave two lane-groups per loop iteration in the stage kernels, so an E core has two
  independent butterfly chains to overlap; measure E-core IPC per kernel with `tools/kloop.c`
  first, then 10 tests ABBA.
- Bound: the ~10% per E-core second, about 5% overall on eagle, less on wolf.
- Exit: a kernel variant that raises E-core IPC in `kloop` and shows at 10 tests, or a measured
  reason the E cores cannot do better on this code.
- Status 2026-10-20: E cores have three FP pipes, only two of which multiply; `mov` is free;
  stores go at 16 B/cycle. Mul-form butterflies lose on both core types; unrolling does nothing;
  storing outputs early gains 4–6.5% per call on E in `kloop` but 0.0% (240K) and 0.16% (384K)
  over 10 whole tests on eagle (journal, 2026-10-20 22:20). No further idea in this area clears
  the 10-test noise (~1%); parked.
- 2026-10-21: the multiply-pipe floor is not the limit either. The plain DIT kernels
  (`KITE_NO_TAN=1`) have a lower E-core issue floor than the tangent ones but measured level on
  E cores only and 1.7–1.8% slower at 10 tests (journal, 2026-10-21 21:05).

## 8. Four-pair pattern normaliser

The pattern normaliser handles one or two row pairs per loop (P = 1, 2). Four pairs per loop
gives more independent carry chains. Expected about 1.5%, well understood; the bit-identical check
(same residues and max error) applies.
- Status 2026-10-21: built and measured, dropped. Bit-identical; the normalise phase is 3.8%
  faster alone (0.7% of a squaring) and level at 10 tests on eagle (240K −0.2%, 256K −0.3%,
  384K +0.3%; journal, 2026-10-21 20:22). Under load something other than the carry chains
  limits it (memory traffic is the likely candidate, not measured).

## 9. Pass 2 without the row copies

Pass 2 copies each row pair into a layout-a scratch, transforms it there and copies it back
(`dif_r4_rows`, `dit_r4_rows_tan`, about 11–13% of the time at 240K). Working in place in layout b
would remove the copies. A few percent at most and uncertain, now that L1 traffic is ruled out as
the E-core limit; larger work (new kernel layouts and a reworked pass 2).
- Status 2026-10-21: upper bound 4.5% at 10 tests on eagle (240K), 5.1% alone, from a build
  that skips the main array in pass 2 altogether. The row copies are already fused into the first
  and last stages, so an in-place pass 2 keeps those loads and stores; the removable share of the
  4.5% is unmeasured (journal, 2026-10-21 21:40).

## Proposals, 2026-10-19 (0.1.15), in order

1. kite as the default on Apple Silicon (6), after a day of 0.1.15 and a production check.
   Largest gain; a primecrunch decision.
2. The GPU at production sizes on desktop Macs (5). +13% measured at 32K on eagle; substantial work.
3. Vector scheduling for the E cores (7). Up to ~5% on eagle; uncertain.
4. Pass 2 in place (9), a few percent, uncertain. (Four-pair normaliser (8): measured level
   under load, dropped 2026-10-21.)
5. Engine choice per FFT size (4), only if production data shows a size where PRST wins.
