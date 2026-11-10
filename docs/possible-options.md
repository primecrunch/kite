# Possible options, 2026-11-10

Three approaches not in `speed-improvement-priorities.md` (tried and dropped) or
`docs/next-investigations.md` (closed), each with a first measurement that bounds it before any
real work, under the rules in `docs/large-fft-plan.md`: production sizes, every core busy, a step
dropped only when its bound is too small to detect.

## Where the room is

- On one P core at 240K kite runs at about 95% of its FP-issue floor: 11.2M vector instructions
  per squaring (`docs/results/pmc-mix-2026-10-19.txt`) over 4 pipes at 4.4 GHz is 0.64 ms, against
  0.665 ms measured alone (`kite time`, 2009·2^3322280−1). Single-core gains need fewer
  instructions per squaring, and the kernel list for that is empty.
- The E-core gap (10% per E-core second against PRST) has been chased to the noise:
  investigations 7, 8 and 9 closed.
- So the remaining room is in behaviour under load and in fit to the fleet, which is what the
  three below address.

## The fleet (production, 2026-11-10)

Latest `client_revision` per client, darwin/arm64, seen in the last 14 days: 10 machines, 113
cores, all reporting `kite version 0.1.16`.

| cpu_model | cores | machines |
|---|--:|--:|
| Apple M2 | 8 | 3 |
| Apple M6 (as reported) | 12 | 2 |
| Apple M4 | 10 | 1 |
| Apple M4 Pro | 12 | 1 |
| Apple M4 Pro | 14 | 1 |
| Apple M4 Max | 14 | 1 |
| Apple M5 Pro | 15 | 1 |

Six of the ten machines (63 of 113 cores) are chips kite has never been measured on. Every
policy switch was tuned on eagle (M4) and wolf (M4 Pro).

## 1. Self-tune the policy switches on the member's own chip, under the member's own load

### Facts

- Hard-coded from two M4 machines: the normaliser table off above FFT 96K (`ntab_policy`),
  four-step twiddles generated from 192K (`t4_gen_policy`, H ≥ 98304), stage orders from the M4
  loaded tune (`tune/fft_orders_m4_load.txt`), shape R × C from `choose_shape` plus the tuned
  rows column.
- Each switch was measured to decide 2–24% under load: ntab up to 24% at 256K
  (`docs/results/ntab-sweep-2026-10-17.md`), t4 table against gen 2–6% above 192K
  (`docs/results/t4-generated-2026-10-19.md`), order 2–4% (journal, 2026-10-18 20:10), shape 5.6%
  alone (same entry).
- The crossovers are set by L2 per cluster and cores per cluster, which differ by chip. An M2
  base has 4 P + 4 E; the M5 Pro and the two "M6" machines are unmeasured.
- The t4 path, the ntab path and the plan order all give the same integers (the pattern
  normaliser is bit for bit the table's digits; the other two differ in rounding only), so a
  switch mid-test needs no new correctness argument beyond the Gerbicz check.

### Approach

At Gerbicz block boundaries in `kite_prp`, run A/B/B/A over two blocks each for one switch at a
time (t4 table or gen, ntab table or patterns, plan order), compare squaring time, keep the
winner, re-check every few hundred blocks since load changes as other tests start and finish.
The t4 and ntab tables are allocated lazily on first use. Shape changes need a relayout and are
out of scope.

### Bound (no engine code)

A `kite-bench` variant that runs 10 tests at 256K with `KITE_T4=table|gen` and `KITE_NTAB=on|off`
flipped, ABBA, on one of the M2 volunteers' machines (forum thread 82). If any winner differs
from the M4 default, build it. Expected: nothing on M4, 2–6% on a chip where a policy flips,
more if ntab flips.

### Exit

The in-process tuner picks the same policies the external sweep does on eagle and on one non-M4
machine, with no loss at 10 tests on eagle (its defaults are already right there).

## 2. Share the length-only tables between the processes on a machine

### Facts

- The full four-step table is the FP-cheapest path: `dit_r8_t4_b` 88 FP ops per radix-8
  butterfly against 120 for `dit_r8_t4g_b` (`tools/count_ops.py`), 1.5% faster alone at every
  size (journal, 2026-10-19 17:30).
- It loses under load above 192K (2.2% at 240K, 3.8% at 256K, 5.6% at 400K, 10 tests on eagle)
  because each of the 10 processes holds its own copy, 2 MB at 256K, and the E-cluster's 4 MB L2
  overflows.
- The table depends only on L (w_H^(c·k_r) over the shape and the bit-reversal), not on k or n.
  So do the stage twiddle tables (`twmem`), the tangent tables and the t4 hi/lo tables.
- A file-backed `MAP_SHARED` read-only mapping is the same physical pages in every process, and
  L2 is physically tagged: six E-cluster tests hold 2 MB instead of 12 MB.

### Approach

A per-length cache file (`~/.kite/cache/t4-<L>.bin` or `$KITE_CACHE`), written once under a
lock with an atomic rename, mapped read-only in place of the `kite_alloc` for `t4`. Then the
same for `twmem` and the tangent tables. Startup gets faster as a side effect (tables computed
once per length per machine).

### Bound

About 100 lines replacing one allocation. 10 tests on eagle at 240K, 256K and 400K, shared table
against today's generated path, ABBA. The existing numbers bracket the result: the private table
loses 2.2 / 3.8 / 5.6% under load and wins 1.5% alone. If the shared table lands at the
single-test figure, +1.5% everywhere plus whatever the E-cluster L2 gives back; if the footprint
was never the reason, 0.

### Caveat

Only tests at the same length share. A machine's 10 tests usually span two or three lengths, so
the production gain is a fraction of the bench figure; the bench should mix two lengths.

## 3. Speed-weighted work split across threads, so one test can span P and E cores

### Facts

- Pass 1 gives thread t the fixed group range `[gstart[t], gstart[t+1])` and pass 2 the fixed
  row-pair range `[npairs·t/n, npairs·(t+1)/n)`, with a barrier after each pass
  (`kite_pool_run`). On a P plus E pair the P thread finishes and spins at the barrier every pass;
  an E core does well under half a P core's squarings (`docs/results/scaling-pe-2026-10-19.md`).
- Measured but never explained: 5×2 at 256K on the M4 gets 5.9 tests per hour against 7.7 for
  10×1 (journal, 2026-10-18 11:30); 892 against 1314 at 370K on wolf (README). How much of that
  is barrier wait and how much cross-cluster traffic is unknown.
- At 400K four tests' data plus tables (3.2 MB + ~0.6 MB each) nearly fill a 16 MB P-cluster L2.
  As n passes 6M (FFT ≥ 512K, 4 MB of data per test) fewer, wider tests become the way to keep
  the footprint down, and the client's per-FFT thread calibration (`client/internal/benchmark/
  tuner.go`) needs an engine that scales on hybrid cores to choose from. Record sizes already run
  across P and E (69·2^24612729−1 at 10 threads).

### Approach

Pass 2: an atomic chunk counter over row pairs (4–8 pairs a chunk), so a thread takes what it
can. Pass 1: ranges stay contiguous (the carry chain runs group to group within a range), but
their boundaries move each squaring from the previous squaring's measured per-thread time; or
pass 1 is cut into more ranges than threads, at one extra fix-up group per range.

### Bound

Instrument the join wait: the time thread 0 spends spinning in `kite_pool_run` after `pass2_job`
and `pass1_a`, as a share of the squaring, in a 5×2 run on eagle at 256K. That share is the
ceiling. If it is small, the loss is cross-cluster traffic and the idea is dropped.

### Exit

5×2 within the calibration noise of 10×1 at 256K on eagle, and a 14-thread record-size run on
wolf faster than the 10-thread P-only one.

## Ruled out while looking

- FCMLA: the layout keeps re and im in separate registers with lanes as rows, so twiddles are
  already scalar-broadcast FMAs; the complex-multiply instructions have nothing to fuse.
- A concurrent SME worker (an 11th test fed to the SME unit): one unit per cluster, and
  streaming-mode vector arithmetic runs at about 15 GFLOPS, so the normaliser cannot live there.
- The k penalty (0.5·log2 k bits per word): inherent to the IBDWT (Percival), see the literature
  check, journal 2026-10-12 21:25.

## Still open from earlier lists

- The GPU at production sizes on desktops (next-investigations 5): the largest listed but
  unbuilt item, +13% at 32K on eagle.
- Software prefetch of the next pass-1 group has only been measured on P cores at FFT 49K (2%
  slower). On E cores at production sizes the 9% of cycles stalled on an old L1 miss
  (`docs/results/e-core-diagnosis-2026-10-19.md`) bounds a re-run.
