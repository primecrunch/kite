# ahnum journal

Times are local (2026-10-11). Entries before 12:30 were reconstructed from file timestamps and
process logs after CLAUDE.md asked for this journal, so they are approximate to a few minutes.

## 2026-10-11 10:30 — Plan

Goal: a library for squaring modulo Riesel numbers k·2^n−1, as fast as possible on Apple
Silicon, for Fermat PRP testing. The workload is k < 10000 from n ≈ 400K upward (throughput),
plus record attempts above 69·2^24612729−1.

Decisions from the planning questions:
- CPU first. NEON baseline, with an SME path on M4 where it measures faster.
- The hot code is assembly. C, C++ or Rust were ruled out because the AMD64 state of the art
  (gwnum) is hand-written assembly. Here a Python generator emits AArch64 assembly, and thin C
  handles setup, threads and the API.
- Gerbicz plus round-off checking. Verified residues come from the primecrunch API.
- Throughput matters most. The tests × threads split has to be measured per FFT size.

## 10:48 — Pinning the residue convention

There are several plausible "Fermat residue" definitions: 3^(N−1), 3^(N+1), the half
exponents, and so on. A GMP oracle printing all of them took 38 minutes for one 370K-bit
number. The project's own first FFT finished sooner, and it showed that primecrunch's
`F5FDA01218F20439` for 9357·2^369717−1 is the low 64 bits of 3^(N−1) mod N.

## 10:55 — Scalar reference IBDWT

The generalised IBDWT (Crandall–Fagin, extended to k·2^n±c by Percival 2003) with k folded
into the place values, P_0 = 1 and P_j = k·2^{s_j}. It matched GMP on the first run,
including FFT lengths that are not powers of two.

## 11:05 — The cost of k, and fixing it

Measured round-off showed that this layout loses about 0.8·log2(k) bits per FFT word. For
k ≈ 9357 that is about 10.5 bits per word instead of about 21, roughly doubling the FFT length.
The cause is weights of size k^{1−j/L}: their spread of k inflates the convolution's error.

gwnum's source comments describe a different approach, which I re-derived: fold k into the
irrational base instead (T = n + log2 k, s_j = ⌈jT/L⌉), so the weights stay in [1, 2). The
wrapped terms then come out as integers divided by k. Multiplying every output by k makes
them integers again, at a cost of log2(k) bits of the 53, which is about 0.5·log2 k bits per
word. Squaring then yields k·V², so numbers are stored as x/k ≡ x·2^n (mod N), a
Montgomery-like form with R = 2^n. Bits that land above 2^n are "folded": H·2^n with
H = qk + r becomes q + r·2^n.

Result: k = 9357 went from about 10.5 to about 14.3 bits per word, a FFT about 35% shorter.
A fold bug for k = 1 (no top region, so the code overwrote the last digit) showed up in the
GMP comparison straight away.

## 11:20 — Two-pass structure, prototyped in C

A complex FFT of length H = L/2 on packed pairs, four-step H = R×C:
- **Pass 1:** column FFTs on groups of 8 columns, fused with normalisation.
- **Pass 2:** row FFTs on row pairs (p, p′) with k_r′ = −k_r, so the real-split partners
  (k, H−k) are in the same pair.

Carries cross group boundaries. The first group is held back as integer digits until the last
group's carries, and the wrapped top, are known. It matched GMP on the first run. It was
written as plain C so that the assembly could be checked against an independent path.

## 11:25 — Assembly generator

`gen/gen_kernels.py` emits DIF/DIT stage kernels for radix 2, 3, 4, 5, 7 and 8. Each comes in
three variants: stage twiddles, no twiddles, and a four-step twiddle table at the same offsets
as the data. Complex numbers occupy two NEON registers (re lanes, im lanes), and twiddles use
by-element FMA. All 36 variants matched a scalar reference on the first test.

Pass 2 transposes a row pair so that row p is lane 0 and row p′ is lane 1. Every row-FFT
butterfly then uses the same twiddles in both lanes, and the real-split partner of (p, q) is
lane 1 at C−1−q, so pairing costs one `ext` lane swap.

Timings for 9357·2^369717−1, single core:

| Step | µs per squaring |
|---|---|
| Plain C two-pass | 4537 |
| Assembly FFT stages | 142 |
| + assembly normaliser | 98 |
| + assembly split and transposes | 87 |
| + table-driven, 4-row interleaved normaliser | 69 (L = 28672) |

The normaliser was limited by latency, not throughput. The carry chain went
fadd → fmul → frintn per digit, about 9 cycles. Two changes fixed it: rewriting the chain as
c′ = rint(fma(c, 2^−b, x·2^−b)), which is still exact because everything is a power-of-two
scale, and interleaving two independent row pairs.

## 11:40 — Gerbicz, checkpoints, fault injection

d_t = ∏ x_{jB}, checked as x_0·d_t^(2^B) = d_{t+1} every M blocks, with rollback. The first
fault-injection test changed a frequency-domain value. That showed up as round-off error 0.5
and went through the "larger FFT" path instead of Gerbicz. Injecting +1 into an integer digit
(a valid encoding of a wrong number) exercises the Gerbicz path: it was detected and rolled
back, and the residue came out correct.

## 11:50 — Threading

A fork-join pool. Pass 2 splits row pairs across threads; pass 1 splits column groups, and
each thread holds back its first group. Scaling stopped at 4 threads at first. The workers
spun for a fixed count of `yield`s, ran out and fell asleep between passes, so every pass paid
the wake-up latency. Spinning for 5 ms by the ARM counter fixed it.

Results:
- 69·2^24612729−1: 10.3 ms per squaring on 1 thread, 1.28 ms on 8 threads. 9 threads were
  slower than 8.
- At 256K the FFT stays in L2. 5 threads gave a 4.1× speedup and 10 threads collapsed. With
  every P-core spinning, one preempted thread stalls the whole barrier, and a second cluster
  adds cross-cluster L2 traffic.

## 12:00 — FFT length selection

Fitted from measured round-off over L = 4K … 1M and k = 1 … 9999:
bpw_max ≈ 21.44 − 0.305·(log2 L − 12) − 0.525·log2 k. Round-off is quantised (0.25, 0.3125,
…) because the outputs are near 2^50, where one ulp is 0.25.

The speed of a given length depends heavily on its odd factors. At 28K, 24576 = 2^13·3 runs at
61 µs and 28224 = 2^6·3²·7² at 88 µs, while 32768 is barely slower than 28224 despite 16% more
data. `ahbench tune` measures every length, and selection takes the fastest safe one.

## 12:10 — Kernel ceiling

Per-kernel microbenchmarks show every kernel at about 4 FP instructions per cycle. NEON FMLA
peaks at 35.9 GFLOPS, which implies 4 pipes at 4.49 GHz. Further gains have to come from fewer
instructions, not better scheduling.

## 12:15 — SME measurements

| Operation | Throughput |
|---|---|
| Streaming-SVE FMLA | 15 GFLOPS (39 per cluster) |
| FP64 FMOPA | 492 GFLOPS from one core; 935 with both clusters' units |
| Streaming load/store | about 440 GB/s from L1 to 8 MB |

Streaming-SVE vector arithmetic is slower than NEON. An FFT stage written as 8×8 complex
matrix products, with the twiddles folded into per-j matrices, uses about 5× the flops of a
radix-8 butterfly. It still projects to about 2.2× faster per stage on the one core driving
SME. The unit is shared per cluster.

## 12:20 — Edge cases

- `ahn_prp` hung on 8191·2^25000−1, because N is divisible by 3, so dividing by base² mod N
  has no solution. It now uses CRT: 0 mod the base-prime part of N, and y/base² mod the rest.
- A wrong square at n ≤ 20 with k = 9357 came from a carry that rippled up into the top region
  and was treated as sitting at 2^{s_L}.
- The automatic backend fell back to the scalar reference for tiny n but freed state that had
  never been allocated.

## 12:35 — primecrunch residues

The API is served from `https://api.primecrunch.com`. The OpenAPI `servers` entry,
`https://primecrunch.com`, returns the web app's HTML for `/v2/...`.

GET `/v2/k/{k}/n/{n}` is public and lists jobs with a `residual`. Only `fermat` jobs are used,
because LLR residues are a different quantity. No endpoint lists candidates, so
`tools/primecrunch_fetch.py` takes (k, n) pairs or does a rate-limited scan; sieved-out n
return 404.

From n = 369500…369699, 13 residues for k = 9357 were fetched, and ahbench matched all 13
(10 tests in parallel). The primecrunch record for 9357·2^369717−1 shows the original result
came from a darwin/amd64 client in 30 s. ahnum runs it natively in 26 s on one P-core with
Gerbicz checking.

Benchmark caveat: this machine also runs a distributed compute project, so the throughput
numbers so far come from a loaded system.

## 12:40 — Quiet-machine baselines

The distributed compute project was paused, giving these baselines:

| Test | Configuration | Time per squaring | Throughput / time per test |
|---|---|---|---|
| 9357·2^369717−1 | 1 core | 69 µs | 26 s per test |
| 9357·2^369717−1 | 10 tests × 1 thread | 86 µs each | 1125 tests/hour |
| 9357·2^369717−1 | 14 tests × 1 thread (P- and E-cores) | 110 µs each | 1244 tests/hour |
| 69·2^24612729−1 | 1 thread | 6.5 ms | 45 hours |
| 69·2^24612729−1 | 10 threads | 0.77 ms | 5.3 hours |

The loaded figures earlier (10.3 ms on 1 thread, 1.28 ms on 8 threads) were about 40% worse.

At n ≈ 370K, splitting one test across threads always loses: 5 tests × 2 threads gives 892
tests/hour and 1 test × 10 threads gives 510. One test per core is the right setting there.

## 12:50 — Layout change for SME

Numbers moved from 2-column lane-groups to 8-column blocks of 128 bytes, [re c0..c7][im c0..c7].
For the NEON kernels this only changes load/store offsets. The generator now emits stage kernels
for both layouts: blocks for pass 1, pairs for the pass-2 scratch. Squaring speed did not change
(70 µs at 28672). Now 72 stage-kernel variants are tested.

## 13:00 — SME radix-8 stage kernel

`ahn_sme_r8` replaces each radix-8 batch (8 points × 8 lanes) with Y = M_j X, where M_j is a
complex 8×8 matrix with the twiddles folded in. The DIT direction uses
M = conj(F8)·diag(conj w), so one kernel and two sets of tables cover both directions.

Probe results that shaped the kernel:

| Operation | Cost |
|---|---|
| FMOPA FP64 | latency 4.6 cycles, throughput 1 per 1.15 cycles (needs 4 tiles in flight) |
| smstart + smstop | about 9 ns |
| `st1d` of a ZA tile slice | 7.7 cycles |
| MOVA of 4 rows to Z registers, then normal stores | about 1.2 cycles per 64-byte vector |
| `ld1d` | 0.3–0.7 cycles per vector |

Along the way:
- `smstart` and `smstop` zero every FP register, including the callee-saved d8–d15. A timing
  probe that did not save them produced nonsense clock readings.
- Benchmarking stores into the same 2 KB repeatedly made them look 2× slower than streaming
  stores do.
- I first counted the FMOPA cost wrong: a pair of items needs 64, not 32. With that corrected,
  the kernel is FMOPA-bound at 0.85 cycles/point against 1.3 for NEON. The floor for this
  formulation is 0.58.

## 13:20 — SME in the squaring loop: slower

Pass-1 radix-8 column stages were wired to SME, with the four-step twiddle applied by a small
NEON kernel. The results were correct and slower:

| FFT length | NEON | SME |
|---|---|---|
| 24576 | 64 µs | 83 µs |
| 28672 | 70 µs | 100 µs |
| 262144 | 720 µs | 903 µs |

A hand-off microbenchmark explains it. Alternating an SME stage and a NEON pass over the same
8 KB costs 577 ns, against 180 ns for the two run separately. The SME unit sits at L2, so each
change of owner moves the group's cache lines out of the core's L1 and back, at about 28 cycles
per line. Normalisation has to run on NEON, because streaming-SVE arithmetic is about 5× slower,
so each group changes hands at least twice per squaring.

Throughput cannot gain either: an SME test is slower than a NEON test on the same core, and the
unit is shared by 5 cores.

Decision: keep `--backend sme` as a documented, tested opt-in. NEON stays the default.

The full 9357·2^369717−1 run:
- NEON: 26.7 s
- SME: 35.6 s

Both give RES64 F5FDA01218F20439.

## 13:10 — Benchmark: 8243·2^872226−1 (known prime)

On the quiet machine, NEON, FFT 71680 (12.17 bits/word):

| Setup | Time per squaring | Result |
|---|---|---|
| 1 test, 1 core | 198 µs | 172.6 s; RES64 0000000000000001 (probable prime), max round-off 0.11, no Gerbicz errors |
| 10 tests × 1 thread | 254 µs each | 162 tests/hour |
| 14 tests × 1 thread | 325 µs each | 178 tests/hour |
| 5 tests × 2 threads | — | 149 tests/hour |
| 1 test × 10 threads | 41 µs | 101 tests/hour, but one test takes about 36 s |

The number is a known prime, so it was added to `tests/known_residues.csv` as a PRP check.

## 13:20 — Reference benchmark: 9001·2^662695−1

primecrunch records this known prime's Fermat job at 195 s on wolf (this M4 Pro, darwin/arm64
client); its LLR proof took 407 s. ahnum on one quiet P-core: 91.5 s (138 µs/iter, FFT 51200,
12.94 bits/word, max round-off 0.31, no Gerbicz errors), 2.13× faster than the reference. It
is now the reference benchmark (`tests/benchmarks.csv`, `tools/bench.sh`).

The 195 s comes from the distributed client, which may have been sharing the machine with other
work at the time, so it is an upper bound rather than a quiet single-core figure.

## 13:35 — One test per core on 9001·2^662695−1

Full Gerbicz-checked PRP runs as separate processes on the quiet machine (all residues matched):

| Concurrent tests | Time per test | Throughput |
|---|---|---|
| 1 | 91.6 s | 39 tests/hour |
| 10 (P-cores) | 121 s | 298 tests/hour |
| 14 (P- and E-cores) | 156 s | 321 tests/hour |

The squaring-rate samples (`ahbench throughput`) suggested 342 and 385 tests/hour. The full
runs come out about 15% lower, which is consistent with Gerbicz overhead, context setup and
sustained all-core clocks.

Each test is 32% slower when all P-cores are busy (91.6 s to 121 s), from shared L2 and power.
The E-cores add 8%.

Against the reference client's 195 s per test: if it ran one test per P-core, it would manage
about 185 tests/hour, so ahnum is about 1.6–1.7× on throughput. That assumes the 195 s was
measured under a comparable load, which is not known.

## 14:40 — prst on the same workload, and a weight-precision bug

The client's prst (x86_64 gwnum, translated by Rosetta 2), run exactly as the client does it
(`prst "9001*2^662695-1" -d -fermat a 3`), on the quiet machine:

| Concurrent tests | prst per test | prst throughput | ahnum per test | ahnum throughput |
|---|---|---|---|---|
| 1 | 90.2 s | — | 91.6 s | — |
| 10 | 115.9 s | 307.6/h | 121 s | 297.5/h |
| 14 | 152.6 s | 329.4/h | 156 s | 321/h |

So the 195 s in the primecrunch record was measured under load. Even translated, gwnum matched
ahnum on one core and was about 3% ahead on throughput.

The difference was FFT length: gwnum ran at 48K (49152), ahnum at 51200. At 49152 ahnum's max
round-off reached 0.5 (outright failure), and so did the scalar reference backend. The problem
was therefore in the arithmetic, not the NEON code.

A position histogram of round-off above 0.2 was the clue: there were 10× more large errors in
the top tenth of the words than in the bottom tenth. FFT round-off has no reason to depend on
position; the weights do. The exponent of w_j was computed as (s_j·L − n·j)/L − j·log2(k)/L.
Both terms grow to about log2 k ≈ 13 at the top while their difference stays in [0, 1), so double
precision lost about 4 bits to cancellation. Weights near the top were off by a few ulp, and
weight errors enter the convolution like extra round-off.

The fix:
- Compute log2 k as a 64-bit binary fraction by repeated squaring in fixed point.
- Define T′ = n + log2k_fixed.
- Derive both the digit boundaries s_j and the weight exponents from T′ as exact 128-bit integer
  ratios.

z^L = 2^T′ then differs from k·2^n by a relative 2^-64, which is below double precision.

Results:
- Max round-off at 49152 fell from 0.5 to 0.19 over 300 squarings and 0.22 over 20,000.
- The refitted k penalty is 0.50·log2 k instead of 0.525.
- Large k gains about 0.4 bits per word: k = 9999 at L = 65536 now allows 13.57 instead of 13.11.
- The selection margin dropped from 0.45 to 0.2. Full runs at the new lengths had max round-off
  0.22 or lower.

Single-core full tests after the fix:

| Number | FFT before → after | Time before → after |
|---|---|---|
| 9001·2^662695−1 | 51200 → 49152 | 91.6 s → 85.7 s |
| 8243·2^872226−1 | 71680 → 65536 | 172.6 s → 155.3 s |

## 15:55 — prst against ahnum, alternating with cool-downs

After about 40 minutes of all-core runs the machine slowed: both programs lost 15–25% per test.
Fair comparison therefore needed alternating runs with 3-minute cool-downs. 10 concurrent tests
of 9001·2^662695−1:

| Program | Time per test | Throughput |
|---|---|---|
| prst | 125.2 s, 126.4 s | 283 tests/hour |
| ahnum (table normaliser) | 114.9 s, 121.4 s | 295–313 tests/hour |
| ahnum (compressed weights) | 119.7 s, 123.1 s | 293–298 tests/hour |

Two earlier ahnum runs at about 144 s per test did not recur. In those runs every test was
equally slow, which fits a machine-wide state (heat or background work) rather than ahnum's
memory footprint; the L2 hypothesis was ruled out. The table normaliser stays the default.

Net result: ahnum is about 4–10% ahead of prst on throughput and 5% ahead on a single test
(85.7 s against 90.2 s). prst is x86_64 gwnum translated by Rosetta 2. gwnum's FFTs are hand-written AMD64
assembly, so no native arm64 build is possible and this is the best gwnum available on Apple
Silicon.

## 16:10 — Speed-up plan, items 1 and 2a

**Item 1: why a test slows by about 20–30% when 10 run at once.** Measured on 9001·2^662695−1:

| Condition | ahnum µs/squaring | L1 kernel ns/butterfly |
|---|---|---|
| Alone | 128.1 | 4.95 |
| 9 FP-only spinners (no memory traffic) | 136.2 (+6%) | 5.50 (+11%) |
| 9 other ahnum tests | 153.7 (+20%) | — |

The L1-resident kernel shows the all-core clock drop, about 11%. Contention for shared cache
and memory adds about another 10%. Full 2-minute runs lose more because sustained clocks fall
further with heat.

Shrinking the footprint does not help. With 10 tests, the compressed-weight normaliser (no
1.5 MB table) gives 334 tests/hour against 354 for the table. The likely limit is L2 bandwidth:
each test streams about 3.9 MB per squaring, about 30 GB/s per core and 150 GB/s per 5-core
cluster. Cutting that traffic would cost FP instructions, and the kernels are FP-bound. Little
here is recoverable in code.

**Item 2a: fuse the row-pair transpose into the row FFT.**
- Upper bound first: skipping the transposes entirely saved 7%; skipping the split saved 9%.
- `ahn_dif_r4_rows` and `ahn_dit_r4_rows` are the first and last row-FFT stages (radix 4,
  s = C/4). They read or write the two rows directly and do two adjacent butterflies at once,
  so one 16-byte load covers both columns, and `trn1`/`trn2` happen in registers.
- The row plan now always starts with radix 4, which costs the same per bit as radix 8.

Results:

| FFT length | Before | After |
|---|---|---|
| 28672 | 74.3 µs | 67.4 µs |
| 49152 | 132.7 µs | 120.3 µs |

That is −9.3%, more than the 7% bound for the transposes alone. 9001·2^662695−1 went from
85.7 s to 80.3 s. GMP tests, SME tests and the residue suite all pass.

## 16:30 — Speed-up plan, items 2b, 3, 4 and 5: measured and set aside

- **2b, fusing the real split into the row FFT.** Skipping the split entirely saves 9%, but that
  is its arithmetic: the split kernel has about 34 FP instructions to 9 loads and stores per
  step, so it is FP-bound. Fusion would remove only the memory pass, estimated under 2%, for a
  much more complicated mirrored-butterfly kernel. Not done. The transposes were different
  because they were nearly all memory work.
- **3, finer FFT-length granularity.** Over 4000 random (k < 10000, n of 400K–2M), the chosen L
  averages 2.66% above the continuous minimum safe length (worst 14%). Part of that is the
  tuning table deliberately picking a faster, longer length. This is below the 3% threshold,
  and relaxing the shape rules would touch the normaliser and the transposes. Not done.
- **4, safety margin.** Max round-off over 20,000 squarings at L = 32768 and 98304, k = 3, 255
  and 9999:

  | Margin | Max round-off |
  |---|---|
  | 0.2 (current) | 0.25–0.28 |
  | 0.1 | 0.30–0.31 |
  | 0.0 | 0.31–0.375 |

  Full tests run 30–100× more squarings, which adds a few hundredths. A margin of 0.1 would come
  close to the 0.40 retry threshold for about 0.75% shorter FFTs. Kept at 0.2.
- **5, driver overhead.** The full test of 9001·2^662695−1 averages 121.2 µs per squaring
  against 120.3 µs bare. Gerbicz, the multiplications and setup together cost about 0.75%.
  Nothing worth doing.

Of the five planned items, only the transpose fusion (−9%) paid off. Per core, the NEON
kernels run at the 4-pipe limit and the remaining passes are FP-bound. Under full load, about
half the slowdown is the all-core clock drop and the rest is shared L2 bandwidth.

## 16:35 — Throughput after the transpose fusion

The same routine as before: 10 concurrent tests of 9001·2^662695−1, alternating prst and ahnum,
with a 3-minute cool-down before each run.

| Program | Run 1 | Run 2 |
|---|---|---|
| prst | 117.2 s per test (305.0/h) | 123.5 s (290.3/h) |
| ahnum | 105.0 s per test (339.6/h) | 107.3 s (336.4/h) |

ahnum gives 336–340 tests/hour against prst's 290–305, about 10–16% more throughput. It is
also 11% faster on a single test (80.3 s against 90.2 s). The two ahnum runs agree within 1%;
prst's differ by 5%.

## 17:15 — Second speed-up round: where the cycles go

The last round assumed the kernels run at the 4-pipe limit everywhere. They do in isolation, but
counting the generated instructions (`tools/count_ops.py`) against the measured time says the
whole squaring at 49152 ran at about 75% of that. The distributed compute project was not
running. I misread a load average of ~3 as outside load, but it was my own builds and
benchmarks. From about 17:00 my residue-suite run kept one other core busy, so the later
measurements (the order sweep, the ~4.2 GHz clock estimate, the tangent A/B) shared the cluster
with it. A/B pairs saw the same conditions; the absolute numbers will be repeated with nothing else running.

**Phase profiler.** `AHN_PROF=1` accumulates `cntvct` ticks per phase on thread 0 and prints the
split at exit. Its total agrees with the untimed figure to within ~2%. Measured ceiling under this
load: `frintn`, `fmul`, `fsub` and `trn1` each run at ~16.5 per ns (4 per cycle at ~4.2 GHz).
Against that, at 49152 (R = 96, C = 256):

| Phase | Share of ceiling |
|---|---|
| p1 forward stages | 95% |
| p2 fwd stages, split | 92% |
| p2 inv stages | 87% |
| p1 inverse stages | 80% |
| p2 rows in | 77% |
| normaliser | 80% (see below) |

**Normaliser: latency, not throughput.** With 2 carry chains in flight, each digit's link is
fmla → frintn, about 6.6 cycles, against 16 ops of work, which predicts 2.46 ops/cycle. Outside
error-check squarings it measured 2.45. Rewriting the table record as {osc, 1/base, base·w}
lets each digit be worked in place in one register:

    D = rint(D·osc)·ib;  D += cin·ib;  cin = rint(D);  D = (D − cin)·bw

Every step before the last multiply is a power-of-two scaling of an integer below 2^53, so it is
exact, and (D − cin)·(base·w) rounds the same as the old x·w. The output is bit-identical: maxerr
and residues match on every test number. It is 7 ops instead of 8 (9 instead of 10 with error
tracking), the table is 25% smaller (1.15 MB instead of 1.5 MB at 49152), and with only D and the
carry live per chain, 4 row pairs (8 rows) fit in the 32 registers. In L1 the new kernel runs at
16.1 ops/ns, the full ceiling. 49152 went from 124.6 to 118.7 µs (−4.7%) in back-to-back runs. In
place it still reaches only ~86%; making the table L1-resident (a deliberately wrong build)
recovered just 1 µs, and the last-rows C path costs 0.12 µs, so the remainder is not
explained yet.

Mode b (group 0's first visit) now stores digit/base, which mode c consumes directly, so neither
needs `base` itself.

**Set aside:**
- *Software prefetch of the next pass-1 group:* 2% slower. The hardware prefetcher already
  follows the 4224-byte stride, and the extra prefetches only add L1 pressure.
- *Moving the special row pair (k_r = 0, R/2) out of scalar C:* it costs 0.62 µs (0.5%), against
  ~0.29 µs for a vector pair, so there is ~0.3 µs to gain.

**Radix order matters.** `factorize` puts radix 8 first and the odd radix last, so at 49152 the
column s = 1 stage (the one that loads the group from L2 and applies the four-step twiddle)
was radix 3, which has little compute per byte. Reversing it to [3, 4, 8] saved 3.4 µs (−2.7%).
A sweep of ascending versus default order for the column and row plans:

| L | default | col asc | row asc | both |
|---|---|---|---|---|
| 24576 | 60.8 | 60.8 | 59.8 | 59.4 |
| 40960 | 100.4 | 100.4 | 102.6 | 103.7 |
| 65536 | 170.3 | 163.2 | 169.2 | 163.5 |
| 98304 | 275.5 | 264.5 | 266.4 | 260.6 |
| 131072 | 376.9 | 373.2 | 361.4 | 346.5 |

No order wins everywhere, so the order is now tuned per length. `ahbench tune` times all four
orders interleaved and writes "L us order", the context reads the third column (old two-column
files still parse), `ahn_opts.plan_order` overrides it, and `ahbench --order` exposes that.
`test_gmp` cycles through all four orders, and the residue suite passes in each.

**Twiddle bound for item 2.** Deleting every stage-twiddle multiply (wrong results) gives the upper
bound: 67.6 → 60.0 µs at 28672, 120.3 → 103.7 at 49152, 320.9 → 275.3 at 114688, so 11–14%.
Vector `mov` is eliminated at rename (16 mov+fmla pairs take the time of 16 fmla), so the
tangent form w = c·(1 + iτ) costs 2 FMAs, with c folded into the butterfly's following FMAs.
For DIT stages, where twiddles sit on the inputs, every scale folds: radix 8 drops from 84 to
66 ops, radix 4 from 28 to 22, radix 3 from 22 to 18. τ can be large near cos θ = 0 without hurting
accuracy, because the error stays ~ε·|x|. Only an exact zero needs care (use c = 2^-60). DIF stages
are harder: the trick does not transpose (radix 2 stays at 8 ops). Gaining there means moving
each stage's output twiddles into the next stage's inputs and carrying the leftover scales
down to the four-step table.

## 17:30 — Tangent-form DIT twiddles

New kernels `ahn_dit_r{2,3,4,8}_tan[_b]` and `ahn_dit_r4_rows_tan` take a per-j table of tangents
and scale ratios built by `ahn_tan_table` (src/neon.c). Every DIT stage with s > 1 uses them,
in both pass-1 inverse column stages and pass-2 inverse row stages. Radices 5 and 7 keep the
complex multiply, because folding the scales into their cosine/sine constants needs more scalar
registers than they have free. The DIF (forward) side is unchanged.

Scale bookkeeping in the radix-8 DIT, input scales c_t after x_t (1 + iτ_t):
- Layer 1: (x0, x4) folds c4 → scale 1. (x1, x5), (x2, x6), (x3, x7) fold c5/c1, c6/c2, c7/c3
  → scales c1, c2, c3.
- W8 multiplies lose their √½ multiply. For W8³ the factor is −c·(1 − i), a uniform real
  scale once the sign moves into it.
- Layer 2 folds c2 and c3/c1, or −c3/c1 on the W8³ side. Layer 3 folds c1 or c1·√½.

Every output comes out at unit scale with no extra multiply. Per j that is 7 tangents plus 9
scale scalars, 8 q-registers against the old 7, and the radix-8 constant register is no longer
needed. Op counts per butterfly: radix 8 84 → 66, radix 4 28 → 22, radix 3 22 → 18, radix 2 8 → 6,
fused rows radix 4 56 → 44.

Accuracy: against the scalar reference the new kernels match the old ones (1.8e-15 for radix 8),
including s = 8 and 16, where cos θ = 0 exactly and the table stores c = 2^-60. Max round-off
at the selection boundary over 20,000 squarings is equal or lower everywhere:

| L | k | tangent | old |
|---|---|---|---|
| 32768 | 3 | 0.2500 | 0.2500 |
| 32768 | 255 | 0.2812 | 0.3125 |
| 32768 | 9999 | 0.2500 | 0.2812 |
| 98304 | 3 | 0.2500 | 0.2500 |
| 98304 | 255 | 0.2500 | 0.2539 |
| 98304 | 9999 | 0.2500 | 0.2812 |

Speed in back-to-back runs, with my residue-suite job busy on another core (µs per squaring, `AHN_NO_TAN=1` for the old path):

| Number | FFT | old | tangent |
|---|---|---|---|
| 9357·2^369717−1 | 28672 | 67.3 | 65.5 (−2.7%) |
| 3·2^400000−1 | 24576 | 46.7 | 45.4 (−2.7%) |
| 9001·2^662695−1 | 49152 | 119.5 | 115.9 (−3.0%) |
| 9999·2^1500000−1 | 114688 | 320.5 | 308.5 (−3.7%) |

That is about half of the DIT half of the 11–14% bound, as expected for saving 2 of every 4
twiddle ops.

**Item 4 dropped.** Folding the split's four doublings into FMAs saves 1–2 of its 30 FP ops
(~0.3%), since the factor 2 lands on terms that need a negation to fold. The scalar special pair
is ~0.3 µs. Neither clears the 3% bar.

## 17:50 — Quiet-machine results for this round

Nothing else was running (the distributed compute project was off, and my own residue job was
stopped first). µs per squaring, best of 3, interleaved:

| Number | Original | + normaliser | + tangent DIT | + tuned order |
|---|---|---|---|---|
| 3·2^400000−1 | 47.5 | 45.9 | 44.6 | — |
| 9357·2^369717−1 | 68.6 | 66.4 | 64.6 | 64.3 (now picks 26880) |
| 9001·2^662695−1 | 121.3 | 117.9 | 115.0 | 113.0 (−6.8%) |
| 8243·2^872226−1 | 168.0 | 164.2 | 156.7 | 153.5 (−8.6%) |
| 9999·2^1500000−1 | 324.3 | 315.9 | 304.7 | 298.0 (−8.1%) |

**Re-tune.** `ahbench tune 8192 196608` with all four stage orders took 104 s. The default order is
fastest at only 24 of 169 lengths. Column ascending won 30, row ascending 41, and both ascending
74. The new table is 6–16% faster than the old one length for length (49152: 128.7 → 113.1;
98304: 290.6 → 245.3). It is installed as `~/.ahnum/fft_times.txt` and
`tune/fft_times_m4pro.txt`; the old file is kept in the session scratchpad.

**Full tests.** 9001·2^662695−1 single-core: 74.6 s and 74.9 s, against 80.3 s (−7%). Max round-off
was 0.25, with no Gerbicz errors or retries. 10 concurrent tests, after a 3-minute cool-down:
96.8–97.3 s each, 371 tests/hour, against 336–340 last round (+9–10%). Against the earlier prst
runs (290–305/h) that is ~22–28% more throughput, though prst was not rerun today. The
throughput command logged only the times, so those ten residues were not individually checked.
The single runs matched.

By step: normaliser −2.3 to −3.4%, tangent DIT −2.5 to −4.6%, tuned order −0.5 to −2%. The
loaded-machine order sweep had suggested more at 49152 (−2.7%). I have not measured whether the
tangent kernels changed which order wins there.

Residue suite (28 known residues, n up to 872226) on the final build: orders 2 and 3 pass 28/28.
Order 1 passed 28/28 earlier on the pre-tangent build, and order 0 passes via ctest. test_gmp
cycles through all four orders against GMP.

## 20:37 — Throughput on 9357·2^369717−1 with the new build

`ahbench throughput` squaring-rate samples, 20 s per configuration, quiet machine after a rest:
10 × 1 gives 1314 tests/hour (74.1 µs per squaring per test), 14 × 1 (P + E) gives 1405
(97.1 µs). The first build measured 1125 and 1244. A second round after only a 60 s rest came out
2–6% lower (1283 and 1315), which matches the heat effect seen before. Two earlier attempts this
evening were discarded: one ran right after hours of prst work by the distributed client, and one
overlapped a video call (camera, conferencing and video-encoder processes busy). They gave
inconsistent results, including 14 × 1 slower than 10 × 1.

## 20:47 — Safety margin 0.1: dropped again

With the tangent DIT kernels in, I re-ran the margin sweep at 12 points (L = 32768, 49152, 98304,
196608 × k = 3, 255, 9999), 20,000 squarings each. Max round-off:
- Margin 0.2: 0.25–0.28.
- Margin 0.1: 0.28–0.375, with 0.344 at L = 32768 k = 3 and 0.375 at L = 49152 k = 255.
- Margin 0.0: 0.31–0.375.

Last round's six-point sweep showed 0.30–0.31 at margin 0.1. The wider sweep finds worse points,
and full tests run 30–100× more squarings, so margin 0.1 would sit at the 0.40 retry threshold.
A retry restarts at a larger FFT and costs far more than the ~0.5–1% saving. `BPW_MARGIN` stays
at 0.2. The tangent kernels lowered round-off at margin 0.2 but did not move the safe margin.

## 20:51 — Normaliser fusion: no bound

Fusion would only help if the joins between the inverse stages, the normaliser and the
forward stages cost something: the group's store and reload, or lost overlap between kernels.
I timed pass 1 three ways with a temporary switch (µs per squaring, best of 3, quiet machine):

| Number | all (A) | normaliser skipped (B) | column stages skipped (C) | B + C − A |
|---|---|---|---|---|
| 9001·2^662695−1 | 111.1 | 91.6 | 72.0 | 52.5 |
| 9999·2^1500000−1 | 294.1 | 249.7 | 197.7 | 153.3 |

If the joins are free, B + C − A equals the time outside pass 1. At 49152 that is pass 2, ~53 µs
by the profiler, so the joins cost nothing measurable. The normaliser costs ~19.5 µs whether it
runs after the stages (data in L1) or alone (data from L2). Its ~16% shortfall against the
isolated kernel rate is inside the kernel itself. Dropped. (Skipping the normaliser lets the
values overflow to NaN and inf; Apple's FP units showed no slowdown for that, since B came out
as consistent as A.)

## 20:57 — Forward (DIF) tangent twiddles: bound 5.3–6.4%, shelved

Bound build: every DIF output-twiddle multiply removed (wrong results), DIT tangent kernels kept.
µs per squaring, best of 3, quiet machine:

| Number | full | no forward twiddles |
|---|---|---|
| 9357·2^369717−1 | 62.8 | 58.8 (−6.4%) |
| 9001·2^662695−1 | 110.7 | 103.6 (−6.4%) |
| 8243·2^872226−1 | 150.2 | 141.5 (−5.8%) |
| 9999·2^1500000−1 | 294.4 | 278.7 (−5.3%) |

On the inverse side, the twiddles sat on the butterfly inputs and saved half their ops, which
recovered 45–65% of the bound. On the forward side they have to move to the next stage's inputs:
- That makes p twiddles per butterfly instead of p − 1, so radix 8 saves 12 of 28 twiddle ops
  and radix 4 saves 4 of 12.
- Pass 2's last forward stage leaves a per-element scale that has to be multiplied out before
  the split, about one extra op per point.

Expected net ~1.5–3%, below the 3% bar, for the largest change still on the list. Shelved. The
remaining rows-in item has a ceiling of ~2 µs (~1.7%) at 49152, so no candidate on the list
clears the bar any more.

## 21:07 — New candidates: table traffic under load, and what is left

The throughput workload loses 18% per test at 10 concurrent tests (133.5 µs per squaring against
113 alone). Each squaring streams ~3 MB from L2, about half of it the four-step table (393 KB) and
the normaliser table (1.15 MB). The bound: both tables made L1-resident (every group reuses group
0's slice; wrong results, same instructions). One core: 111.3 → 107.7 µs (~3%). Throughput,
alternating with 90 s cool-downs:

| Run | 10 tests normal | 10 tests hot | 14 tests normal | 14 tests hot |
|---|---|---|---|---|
| 1 | 407.0/h | 409.6/h | 422.1/h | 422.2/h |
| 2 | 397.3/h | 407.5/h | 401.9/h | 416.7/h |

Round 2's normal run fell 2–5% from round 1, which fits the machine warming up, so the bound under
load is 0–2%. Table traffic is not what slows tests down at full load, and rebuilding tables on
the fly would cost FP work. Dropped.

Two more ideas reasoned through and not built:
- **The k penalty.** It comes from the wrapped terms carrying k⁻¹ mod N, and gwnum pays it too.
- **New radices for finer FFT lengths.** The chosen length already averages only 2.66% above the
  minimum.

What is left with real size is off the CPU: an integer NTT on the idle GPU running extra tests
alongside the CPU ones. It is now on the priorities list as a research item; its first step is
a prototype kernel timed at 49152.

## 21:17 — Forward (DIF) tangent twiddles: built, measured, dropped

Design: forward stages stop applying their output twiddles. Each next stage applies them on
its inputs in tangent form, divided by input 0's factor F_0, so it still uses p − 1 tangents per
butterfly. Its outputs carry F_0 as a pending complex factor, folded into the next stage's
per-butterfly table.
- Pass 1: the last stage's pending factors go into a forward copy of the four-step table.
- Pass 2: the pending factor depends only on the column. The last stage (`tana`) gives every
  input its own tangent and leaves a real per-column scale, which the split folds into its e/d
  sums (`ahn_split_sq_s`, +2 ops per iteration).

Kernels: `ahn_dif_r{2,3,4,8}_tanf[_b]`, `_tana`, `_tant4_b`, `ahn_dif_r4_rows_notw`. GMP tests
passed on the first build.

µs per squaring, quiet machine, new path against `AHN_NO_TANF=1`:

| Number | FFT | old | new |
|---|---|---|---|
| 9357·2^369717−1 | 26880 | 63.5 | 63.4 |
| 9001·2^662695−1 | 49152 | 112.4 | 110.7 (−1.5%) |
| 8243·2^872226−1 | 65536 | 151.7 | 152.0 |
| 9999·2^1500000−1 | 114688 | 295.8 | 297.9 (+0.7%) |

Why it fell short of the 1.5–3% estimate (and far short of the 5.3–6.4% bound):
- **The last stages gain ops.** The old last forward stages had no twiddles: radix 8 at 56 ops,
  or 88 with the four-step multiply. They now take over the previous stage's twiddles: 68 for
  `tana`, 98 for `tant4`. The bound build never paid that, and my estimate missed it.
- **Rows in does not care about FP.** Its kernel went from 56 to 32 FP ops, and its time stayed
  at 19.5–19.8 µs (at 114688); it is bound by loading the two rows from L2.
- **The second four-step table costs ~1%.** Before, the forward pass reused the inverse pass's
  table slice while it was still in L1. Pointing the forward pass at the original table (wrong
  results) recovered ~3.2 µs at 114688, but even then pass 1 forward was no faster than the old
  path.

Removing that table would mean also redesigning the inverse so the pending factors fold into the
normaliser weights, worth at most ~1% more. Dropped. The patch is kept in
`docs/experiments/dif-tangent-forward.patch` (applies to 6b24cf7).

## 21:27 — GPU NTT prototype

`gpu/ntt.metal` plus `gpu/ntt_proto.m` (Objective-C host; shaders compiled at run time, since the
offline `metal` compiler is not installed). A batched four-step NTT squaring mod p = 15·2^27 + 1,
N = R × C, in three dispatches that mirror the CPU passes:
1. Column DIF in threadgroup memory, then the four-step twiddle.
2. Row DIF, pointwise square, row DIT.
3. Inverse twiddle and scale, then column DIT.

The arithmetic is Montgomery with 2^32 from 32-bit `mul`/`mulhi`. The kernels are naive radix 2
with a barrier after each stage. Each size checks one squaring against a direct cyclic square at
64 positions. All pass.

| N | numbers in flight | µs per squaring mod one prime |
|---|---|---|
| 32768 | 16 / 64 / 256 | 6.55 / 5.63 / 5.41 |
| 65536 | 32 / 128 | 12.06 / 11.77 |
| 131072 | 64 | 38.84 (a 3.3× jump, not explained yet) |

Cost model for 9001·2^662695−1: the arithmetic is exact, so there is no k penalty and no safety
margin. 45-bit words give 14,727 words, zero-padded to 32768, and the convolution needs ~104 bits,
so 4 primes. That is ~22 µs of NTT per squaring. Combining the primes, carries, reduction mod
k·2^n−1 and splitting back into words are not built.

Alongside the CPU (14 tests of 9001·2^662695−1, 90 s cool-downs between runs):

| | CPU tests/hour | GPU µs per prime-squaring |
|---|---|---|
| alone | 452.2 | 5.57 (99% busy) |
| together | 352.7 (−22%) | 6.21 (92% busy) |

At 6.21 µs, and guessing 10–20 µs for the unbuilt parts, the GPU would add ~120–150 tests/hour, so
together ≈ 490 against 452 alone, about +8% (+2% to +26% depending on those parts). The CPU loses
~100 tests/hour when the GPU runs, probably from the shared power budget, possibly also memory
bandwidth. The GPU's feeder thread competes with 14 CPU tests for 14 cores. Next: separate the
power and bandwidth causes, try 13 CPU tests plus the feeder, and optimise the NTT kernel to
see if a cheaper GPU workload costs the CPU less.

## 21:47 — GPU alongside the CPU: the cost is power, not the feeder thread

I reran with the shader and binaries frozen in a separate directory. The first attempt read
`gpu/ntt.metal` at start-up while I was editing it, so its GPU half never compiled and its
results were discarded. 9001·2^662695−1, `ahbench throughput` 30 s, 90 s cool-downs, GPU at
N = 32768 with 256 numbers in flight:

| Run | CPU tests/hour | GPU µs per prime-squaring (busy) |
|---|---|---|
| 14 tests alone | 441.7 | — |
| 14 tests + GPU at 50% | 409.2 (−7.4%) | 14.56 wall (46%) |
| 13 tests alone | 422.6 | — |
| 13 tests + GPU full | 330.4 (−21.8%) | 5.86 (97%) |
| 14 tests + GPU full | 329.6 (−25.4%) | 6.19 (93%) |

With 13 CPU tests the GPU runs at almost its solo speed (5.86 against 5.57), so with 14 the
feeder thread was being starved. The CPU loss stays at ~22%, though, and scales with how busy
the GPU is: −7.4% at half load, −22 to −25% at full. That fits the shared power budget.

Combined, using the cost model (4 primes at N = 32768 plus a guessed ~15 µs per squaring for the
unbuilt prime-combining, carries and reduction):
- 13 tests + full GPU: 330 + ~141 ≈ 471 tests/hour.
- 14 tests + half GPU: 409 + ~62 ≈ 471.
- 14 tests + full GPU: 330 + ~137 ≈ 467.

All about +6–7% over the best CPU-only figure (442). The CPU's loss tracks the GPU's busy time,
not its work done, so a faster GPU kernel gives more GPU tests for the same CPU cost. GPU kernel
efficiency is therefore the lever. If the GPU did the same work in half the time, the same run
would come out near +25%.

**Fused radix-2² stages: no help.** Fusing pairs of radix-2 stages into one step (one barrier
and one threadgroup round trip per pair, `AHN_GPU_FUSE=1`) is correct but slower at C = 256
(32768: 5.69 → 6.60 µs; 256 × 256: 11.86 → 15.50 µs). It only helps at C = 512 (18.2 → 13.4),
where the plain kernel was poorly placed. Fusion keeps every modular multiply, so this points
to the integer multiplies as the limit: ~2.4M `mul`/`mulhi` per prime-squaring. Next to try:
Shoup's precomputed-quotient multiply for twiddles (3 multiplies instead of Montgomery's 4).

## 21:50 — GPU kernel: two more hypotheses ruled out

The fusion result suggested the integer multiplies were the limit. Shoup's precomputed-quotient
multiply for all twiddles (tables of (w, ⌊w·2^32/p⌋) pairs, 3 multiplies instead of Montgomery's
4) made it **twice as slow**: 5.53 → 10.32 µs at 32768 and 11.76 → 22.06 at 65536. Combined with
fusion it came back to 5.88. So the multiplies are not the limit, and the fusion reasoning was
wrong. Doubling the twiddle bytes doubled the time, which looked like twiddle loads from device
memory. Copying each stage's twiddles into threadgroup memory at kernel start changed little
(5.53 → 5.31 at 32768, 11.76 → 12.32 at 65536), and Shoup stayed 2× slower with on-chip
twiddles. Both changes are reverted (the kernel stays as committed in e9db715).

What limits the kernel is not known. Timings alone have not found it: three hypotheses (barriers,
multiplies, twiddle loads) are ruled out. GPU performance counters (Xcode's Metal System Trace)
would show it directly, but Xcode is not installed on this machine.

## 22:06 — GPU counters: the NTT kernel is latency-bound

Xcode is now installed. The default Metal System Trace records only one GPU counter (RT unit),
and the counter set cannot be chosen from the command line. A template saved from Instruments
with the Metal GPU Counters track set to "Performance Limiters" (`~/metal-limiters.tracetemplate`)
records 80 counters with `xctrace record --template`. Averaged over the 9017 samples where
compute kernels ran (prototype at N = 32768, 256 numbers in flight):

| Counter | Value |
|---|---|
| Instruction throughput limiter / utilization | 75% / 21% |
| Integer and complex limiter / utilization | 54% / 39% |
| Integer and conditional limiter / utilization | 51% / 43% |
| ALU utilization | 31% |
| Kernel occupancy | 88% |
| Compute shader launch limiter / utilization | 96% / 2% |
| Threadgroup memory read / write bandwidth | 709 / 709 GiB/s |
| GPU (DRAM) bandwidth | 53 GB/s |

No execution unit is saturated and DRAM is far from its limit. Instructions are queued but
mostly stalled, and the cores are full, so new threadgroups cannot launch. Each thread does one
butterfly (a few integer ops) between threadgroup-memory round trips and barriers, 8–15 times per
transform, and 88% occupancy is not enough to hide that latency. This explains the earlier
results:
- Shoup multiplies cut ALU work, which was not the limit, and added memory traffic.
- Fused radix-2² steps still did little work per barrier.

The usual GPU FFT layout should fix it: each thread holds 8–16 elements in registers, runs a
radix-8/16 sub-transform there, and exchanges through threadgroup memory 2–3 times per
transform instead of 8–15.

## 22:09 — Register-based GPU kernels: −12%, now limited by register spills

`gpu/ntt_reg.metal` (`AHN_GPU_REG=1`): each length L splits as A × B. A thread holds B elements
in registers and runs a length-B DFT there (unrolled radix-2 network). After the inner twiddle
w_L^(a·kb) and one exchange through threadgroup memory, it runs the length-A DFTs on the other
index. Two barriers per row square instead of one per radix-2 stage. Rows are read and written in
natural order. The column pass stores rows in natural frequency order, so the four-step table is
w_N^(k1·n2). Correct on the first run (same direct-square check).

| N | radix-2 kernels | register kernels |
|---|---|---|
| 32768 (128 × 256: 8×16 columns, 16×16 rows) | 5.55 µs | 4.87 µs (−12%) |
| 65536 (256 × 256) | 11.78 µs | 10.20 µs (−13%) |

Counters for the register kernels against radix-2:

| Counter | radix-2 | register |
|---|---|---|
| Threadgroup memory read / write | 709 / 709 GiB/s | 94 / 94 GiB/s |
| Kernel occupancy | 88% | 31% |
| Occupancy manager target | 100% | 47% |
| L1 eviction rate | 25% | 61% |
| Register traffic through L1 | ~0 | 72 GiB/s |
| Integer and complex limiter / utilization | 54% / 39% | 86% / 45% |

The exchange traffic went as planned, but 16 elements per thread, plus the radix-16 network's
temporaries, overflow the register file. Values spill to L1, and the occupancy manager cuts
occupancy to a third. Forcing full unrolling on every register-array loop changed nothing
(4.88 µs), so the arrays were already in registers; the pressure is genuine. Next: 8 elements per
thread with a three-step split (C = 256 as 8 × 8 × 4), one more exchange, but small enough to stay
in registers at full occupancy.

## 22:13 — GPU: Stockham radix-8 loses, Shoup in the register kernels wins (−21%)

**Stockham, 8 elements per thread.** Radix-8 passes (the last one radix 4 or 2), natural order in
and out, two ping-pong threadgroup buffers, one barrier per pass. Correct, but slower than the
16-per-thread register kernels: 7.56 µs at 32768 at first. The per-pass index arithmetic divided by
a runtime sub-length, and the GPU has no hardware integer divide; with shifts and masks instead it
reached 6.86, still behind 4.87. Counters: occupancy 40% with the occupancy target at 100% and low
evictions. That cap comes from threadgroup memory (two buffers per threadgroup), not registers.
The integer "complex" pipe showed an 82% limiter against 47% utilization, the same pattern as the
register kernels (86% / 45%). Removed.

**Shoup in the register kernels.** The complex pipe handles the costly integer ops, probably
including `mulhi`. Montgomery uses two `mulhi` per multiply, Shoup one (plus a second table word).
In the radix-2 kernel Shoup was 2× slower, because that kernel was bound by threadgroup round
trips and the bigger table cost more than the saved `mulhi`. In the register kernels the complex
pipe is the limit:

| N | radix-2 | register | register + Shoup |
|---|---|---|---|
| 32768 | 5.55 µs | 4.87 µs | 3.83 µs |
| 65536 | 11.78 µs | 10.18 µs | 7.93 µs |

So −31% against where the GPU work started. The same optimisation went from 2× slower to 21%
faster once the limit moved.

Cost model update for 9001·2^662695−1 (4 primes at 32768, plus a guessed ~15 µs per squaring for
the unbuilt parts): ~30 µs per squaring on an idle GPU. Alongside 13 CPU tests at the measured
−22% CPU cost (if the GPU's slowdown under CPU load stays ~10%), that is ~330 CPU + ~170 GPU ≈
500 tests/hour, about +13% over the CPU alone (442), up from ~+7%.

## 22:20 — Full GPU squaring mod k·2^n−1: correct, 30.6 µs per squaring

`gpu/gpu_sq.m` plus `gpu/sq.metal` (appended to `ntt_reg.metal`). A complete squaring step on
the GPU for B numbers at once, with no CPU work between squarings:
1. `to_res`: 45-bit words → residues mod four NTT primes (15·2^27+1, 27·2^26+1, 7·2^26+1,
   45·2^24+1), zero-padded to 32768.
2. Four NTT squarings: register kernels with Shoup twiddles, three dispatches each.
3. `crt_split`: Garner CRT to a 128-bit coefficient (< 2^104), split into three 45-bit parts.
4. `carry`: product words.
5. `redc` and `fold`.

**The reduction mod N = k·2^n−1.** Numbers are kept in Montgomery form with R = 2^n. Since
N ≡ −1 (mod 2^n), Montgomery reduction of a product P = A + B·2^n (A < 2^n) is just
P·2^−n ≡ **B + k·A** (mod N): a word-parallel multiply by the small k plus an add, no division.
The result can reach 2^(n+29) and the input to the next square must stay bounded, so a
one-thread fold takes T = bits above n, writes T = q·k + r, and replaces T·2^n by
q + r·2^n (since k·2^n ≡ 1). That keeps values under 2^(n+14). The squaring step is therefore
x → x²·2^−n ≡ x²·k (mod N), which the check reproduces on the CPU with ahnum
(`ahn_square`, then `ahn_mul_small(k)`).

**Results.** Correct on the first run at 3·2^20000−1, and at 9001·2^662695−1 after 20 squarings
(14,728 words, NP = 32768, no carry overflows). Throughput: 30.6 µs per squaring with 32 or 128
numbers in flight; sustained 31.4 µs, so ~20 s of GPU time per test and 173 tests/hour from
the GPU alone. The earlier estimate (4 × 3.83 µs NTT + ~15 µs other) was close: the CRT, carries
and reduction cost about as much as the four NTTs.

Not handled yet: a carry that ripples past one word (counted, never seen in testing), Gerbicz
checks, and checkpoints.

## 22:26 — CPU + GPU together: +22% throughput

9001·2^662695−1, frozen binaries, `ahbench throughput` 30 s, full GPU squaring (`gpu_sq`, 64 in
flight) running alongside, 90 s cool-downs:

| Run | CPU tests/hour | GPU tests/hour | Total |
|---|---|---|---|
| 14 CPU tests alone | 455.7 | — | 455.7 |
| 13 CPU tests + GPU | 382.1 | 174 (31.3 µs, 99% busy) | 556 (+22%) |
| 14 CPU tests + GPU | 374.2 | 158 (34.3 µs, 92% busy) | 532 (+17%) |

Leaving one core for the GPU's feeder thread is better: with 14 CPU tests the feeder is
starved and the GPU drops to 92% busy. This went better than the NTT-only prototype predicted,
which cut the CPU by ~22% at full GPU load. With the full squaring kernel the CPU loses ~10%
(382 against ~423 for 13 tests alone, measured earlier this evening), and the GPU loses nothing.
The full squaring spends half its time in the CRT, carry and reduction kernels, and those
presumably draw less power than the NTT. This is a measured throughput rate, not full tests:
the GPU path still lacks Gerbicz checks, checkpoints and handling for a carry rippling past one
word.

## 22:40 — GPU PRP driver: carry fix-up, Gerbicz, checkpoints, known residues match

**Carry ripple.** The one-word carry leaves a word over 45 bits only when a carry ripples
further, which never happened in testing but is possible. That would be a deterministic error:
Gerbicz would catch it, but rolling back would reproduce it every time. Now any overflow sets a
per-number flag, and a one-thread-per-number `fixup_carry` pass normalises that number
sequentially (it costs one flag read when nothing overflowed). It runs after the product
words and after the reduction. A test mode (`AHN_GPU_FORCE_RIPPLE=1`) skips the fast one-word
carry and flags every number, so the sequential path does all the carrying. It matches the CPU
at 3·2^20000−1 and at 9001·2^662695−1. Normal speed is unchanged (31.5 µs per squaring).

**Shared host code.** `gpu/gpu_ctx.h` holds the setup (pipelines, per-prime tables, buffers),
`gctx_sqr` and `gctx_mul` (a general Montgomery product: both operands through the column pass,
a new two-operand row kernel `row_mul_r`, then the usual CRT, carries and reduction), and the
host-side conversions. `gpu_sq.m` (benchmark) and `gpu_prp.m` (driver) use it.

**Driver** (`gpu/gpu_prp.m`). It mirrors `ahn_prp`:
- x_0 = 3^k is computed on the CPU and converted to Montgomery form (x·2^n ≡ q + r·2^n for
  x = q·k + r).
- Gerbicz block L = √n with checks every L/4 blocks; on failure it rolls back to the last
  verified (i, x, d), and the < L tail is run twice and compared.
- The result is converted back from Montgomery form (x = X·k, since 2^−n ≡ k) and finished
  with the CPU's `ahn_residue_nm1`, now exported from prp.c.

There is no round-off check, because the integer NTT is exact; Gerbicz covers hardware faults
and bugs. Checkpoints are written atomically and resume automatically. All B slots run the same
number: the point is to measure throughput and check end to end.

| Test | Result |
|---|---|
| 1203·2^90001−1, 4 slots | RES64 4BE9F1953E350085, matches, 6.9 s |
| same, fault injected at 50,100 | caught at the next check (67,500), rolled back to 45,000, matches |
| same, killed mid-run then resumed | resumed at 45,000, matches |
| 9357·2^369717−1, 16 slots | F5FDA01218F20439, matches, 11.0 s per test, 327 tests/hour |

At n = 369717 the 45-bit word count (8217) lands just past half of 16384, so the padded length
doubles to 32768. 46-bit words would fit 16384 (8038 words, a coefficient bound of 2^105 against
the primes' 2^119.9). The word size should be chosen per n.

## 23:20 — 14-test full-run throughput for the speed history

The README speed history tracked only 10-test full runs (371/h at `3631fc5`). The 456/h figure
from earlier tonight is a 30 s `ahbench throughput` squaring-rate sample with 14 tests. It is not
comparable: rate samples read ~10% above complete runs. So I measured the same way as the 10-test
column, after a 3-minute cool-down: 14 concurrent `ahbench prp 9001 662695` runs, each checked
against the expected residue. All 14 were probable prime with RES64 0000000000000001, 0 Gerbicz
errors and max round-off 0.25, at 123.2–124.0 s per test (mean 123.6 s), so
**14 × 3600 / 123.6 = 408 tests/hour** (+10% over 10 tests). The load log peaked at 18.7, a few
threads above the 14 tests (`duetexpertd` held a core), worth perhaps 2–4%.

The README table gained a 14-test column. The first build (321/h, 156 s per test) and prst
(329.4/h) come from the 13:35 and 14:40 entries; the intermediate versions were not measured with
14 tests.

Two attempts before that never started. In zsh a glob that matches nothing is an error, so
`rm -f dir/f14_*.log` aborted the command before the cool-down. Then a backgrounded
`cd && S=... && sleep && (logger) &` chain took the `cd` and the variable into its subshell,
away from the foreground part that launched the tests. A script file fixed both.

Earlier in the evening the CPU + GPU end-to-end run hit heavy outside contention: a 15-minute
load average of ~32 against ~14 threads of our own. All 64 GPU tests of 9001·2^662695−1 were
correct (probable prime, RES64 1, Gerbicz errors 0) at 23.3 s per test, 154 tests/hour, but the
CPU side read 91 tests/hour, so the combined figure needs a quiet rerun.

## 2026-10-12 08:15 — Guarding benchmarks against outside processes

The 408 tests/hour run lost perhaps 2–4% to `duetexpertd`, the macOS daemon behind Siri
Suggestions and proactive suggestions. It rebuilds its prediction models in bursts when the
machine looks idle, and a box running only command-line tests looks idle to it. It runs as a
per-user LaunchAgent and had used 65 minutes of CPU time since it last started. Rather than
chase that one daemon (Spotlight indexing, photo analysis, Time Machine and software update do
the same), `tools/quiet.sh` checks for any outside process:

- `check [SETTLE] [TIMEOUT]` waits until nothing outside has been busy for SETTLE seconds
  (default 180, which doubles as the cool-down), or exits 1 listing the busy processes.
- `log FILE` runs in the background during a measurement and records the load average and every
  outside process at 5%+ CPU every 5 s; `summary FILE` gives the peak load and each process's peak.

The first thresholds (5% per process, 25% in total) never passed with the display on:
WindowServer sits at 6–17% and a Chrome renderer at ~10%. A process at 10% takes a tenth of a
core, about 0.7% of 14-test throughput; `duetexpertd` took a whole core. So `check` now waits on
any process above 20% or a total above 50%, and `log` still records everything from 5%, so the
journal can say what ran alongside each measurement.

## 2026-10-12 09:40 — Quiet-machine round: 14 CPU, CPU + GPU, GPU alone

Distributed compute stopped, `tools/quiet.sh check` before each phase (3 quiet minutes), the
load logged throughout, 9001·2^662695−1 at `999a580`, every residue checked.

- **14 CPU tests:** all correct, 118.7–119.4 s (mean 119.1 s), **423 tests/hour**. The 408/h run
  with `duetexpertd` holding a core was 3.7% low. Outside processes peaked at 27% of a core.
- **GPU alone, 64 slots:** all correct, 0 Gerbicz errors, 1330.5 s, **173 tests/hour**, the same
  as the squaring-rate sample. `gpu_prp` itself used ~0% CPU (it waits in `waitUntilCompleted`)
  and the load average stayed under 1.6.
- **13 CPU + GPU:** the GPU batch was correct at 1407 s (164 tests/hour, 5% below alone). All 39
  completed CPU tests were correct, but they slowed down from round to round: mean 159.5 s, then
  249 s, then 522 s. The load average rose from 13 to 54.7 while outside processes logged only
  7–17% of a core, `ahbench` runs one thread and `gpu_prp` one, memory was 87% free and no
  thermal warning was recorded. The display turned off at 08:54, about when round 3 began and
  the load passed 40. Last night's combined run (load ~32, CPU side 91 tests/hour) looks like the
  same effect rather than outside load. Round 1 alone gives 13 × 3600 / 159.5 + 164 = 457
  tests/hour (+8% over 423), well below the +22% from 30 s samples, and even that holds only if
  the slowdown can be fixed.

Two things went wrong with the method. The Mac's system sleep timer is 1 minute, and with the
compute client stopped nothing held it awake: it slept from 08:33 to 08:41 during a pre-flight
wait. Benchmark processes do not prevent idle sleep, so runs now need `caffeinate -i`. And the
logger only recorded outside processes, so it could not show how much CPU our own processes got
while the load climbed; it now logs their total as well.

## 2026-10-12 10:35 — CPU + GPU rerun with the display kept on

Same run as 09:40's middle phase under `caffeinate -i -d`, plus a 30 s sampler of runnable
threads per process and each `ahbench`'s %CPU. The display was not the cause: rounds averaged
157 s, 221 s and 494 s again (GPU 1415.6 s, 163 tests/hour, all 64 + 39 residues correct).

The sampler shows what happened. For the first ~4 minutes the 13 `ahbench` processes got 1226–
1298% CPU (each ≥92%). From 10:14 the total fell to 900%, then held at 520–630% for the rest
of the run, with single tests down to 32–45%. Outside processes used 5–15% and `gpu_prp` ~0%.
So about 6 of the 14 cores were doing our work and nothing else was using the other 8, while
the load average (runnable threads) sat at ~20: the tests were runnable but not being run.
No thermal or performance warning was recorded by `pmset -g therm`.

The machine is a 16" MacBook Pro (Mac16,7, M4 Pro, 10 P + 4 E cores) on AC. The slowdown starts
a few minutes into sustained load and does not recover, which looks like thermal or power
management taking cores away. Every CPU-only measurement in the speed history lasts ~2 minutes
per test with all tests in step, short enough to finish before it starts. Next: a CPU-only run
of the same length to see whether the GPU is needed to trigger it, with `powermetrics` reading
frequency, residency and thermal pressure directly.

## 2026-10-12 11:30 — 25 minutes of CPU-only load, with powermetrics

14 `ahbench prp 9001 662695` slots kept busy for 1500 s, no GPU, `sudo powermetrics -i 10000
--samplers cpu_power,gpu_power,thermal` alongside. All 154 completed tests were correct.

No cores were lost: the 14 tests held 1300–1376% CPU and the load stayed at ~15.5. But they got
slower each round: 116.7, 124.0, 126.8, 128.6, 130.4, 130.6, 131.5, 131.7, 132.6, 136.3,
135.4 s. powermetrics shows why. Both P-clusters ran at 2948–3016 MHz with ~38.5 W of CPU power
in the first minutes, then fell steadily to ~2650 MHz and ~31 W (E-cluster fixed at 2592 MHz).
Thermal pressure went Nominal → Moderate after ~12 minutes → Heavy after ~17. The 10% clock
drop matches the 13–16% slowdown. `pmset -g therm` reported none of this; the pressure level is
readable without root as `notifyutil -g com.apple.system.thermalpressurelevel` (0 nominal,
1 moderate, 2 heavy).

So the README's CPU numbers are 2-minute figures from a cool machine. Sustained 14-test
throughput on this MacBook Pro is 14 × 3600 / ~135.5 = ~372 tests/hour, not 423. And the
collapse seen in both CPU + GPU runs (whole cores idle while tests wait) did not happen
without the GPU. The likely explanation, not yet measured, is that GPU power on top of ~31 W of
CPU pushes thermal pressure past Heavy, where macOS stops scheduling work on some cores rather
than only lowering clocks. If so, running both on this laptop is a net loss after ~6 minutes.

`tools/quiet.sh check` now waits for thermal pressure 0 and `log` records the level, since a
warm start lowers the clocks by up to 10%.

## 2026-10-12 12:05 — CPU + GPU with powermetrics: macOS gives the power to the GPU

Same 13 CPU + 64-slot GPU run, with `powermetrics` (10 s samples). All residues correct
(64 GPU, 39 CPU). GPU 1446.3 s, 159 tests/hour. CPU rounds: 181 s, 506 s, 588 s.

| time | P-cluster MHz | CPU W | GPU W | pressure | `ahbench` total CPU |
|---|---|---|---|---|---|
| 11:36:20 (start) | 3026 / 3138 | 40.6 | 21.9 | Nominal | — |
| 11:37:20 | 1692 / 1812 | 14.8 | 22.0 | Nominal | 1193% |
| 11:39:21 | 1800 / 1800 | 11.9 | 19.3 | Heavy | 719% |
| 11:41–12:00 | ~1798 / ~1796 | 4.1–4.9 | 14–17 | mostly Heavy | 490–570% |

Within a minute of the GPU starting, and while thermal pressure was still Nominal, the P-cluster
clocks were capped at 1800 MHz (from ~3000) and CPU power cut from 40.6 W to 14.8 W. The GPU
kept 22 W. Once pressure reached Heavy, the cap stayed and the P-cores were also kept idle much
of the time: in a sample at 11:50 the four E-cores were 100% active at 2592 MHz, but the ten
P-cores were active 81, 42, 27, 35, 17, 80, 36, 26, 25 and 11% (3.8 cores' worth) on 4.3 W
while the GPU drew 15.5 W. The runnable `ahbench` threads waited for P-cores that were held
idle, which is the load of ~20 with little CPU in use.

So this is macOS's power policy on this MacBook Pro, not outside load. With the GPU busy the
CPU is limited to a few watts, and the whole package runs at ~20 W, well below the ~31 W the
CPU alone sustains. Sustained throughput: CPU side 13 × 3600 / ~550 s ≈ 85 tests/hour + GPU 159
≈ **244 tests/hour**, against ~372 for the CPU alone and 173 for the GPU alone. Running both is
a 35% loss here. The +22% from 30 s samples measured the first minute, before the cap.

## 2026-10-12 13:25 — Decision: CPU only for now

The GPU path goes on hold. On a laptop macOS gives the power budget to the GPU and the CPU
loses more than the GPU adds, so it would only pay on desktops, and that makes it lower
priority than CPU work that helps every machine. It may be revisited on the Mac mini. The
README now gives complete-test figures for the GPU rows, labels the speed history as
one-round (~2 minute) figures, and adds the sustained 14-test number (~372 tests/hour); the
priorities file moves the GPU item to an "On hold" section with these measurements.

## 2026-10-12 19:40 — Next CPU candidates

With the GPU on hold, the candidate list had one item left, below the bar (pass-2 rows in,
~1.7%). Today's runs change what to rank against: the workload is 14 tests sustained on a
power-limited laptop (P-cores ~2650 MHz, ~31 W), but every phase share and FP-limit figure in
the file comes from one quiet core at ~4.2 GHz. The new list in
`speed-improvement-priorities.md` starts with a profile under sustained load (step 0), then:
test count and placement under the power cap (measurement only), per-test cache footprint for
the 4 MB E-cluster L2, the pass-1 inverse stages (80% of the FP limit against 95% forward), the
normaliser's carry-chain latency, and radix-16 stages to cut FP work in pass 1. None has a
measured upper bound yet; each gets one before any code is written.

## 2026-10-12 21:00 — Profile under load, and 14 vs 12 tests sustained

Quiet single test, `AHN_PROF=1`: 75.5 s, 112.95 µs per squaring, the same split as before.
Then 14 and 12 tests kept busy for 25 minutes each (quiet check and thermal pressure 0 before
each), slot 1 profiled on every test. All 154 + 144 completed tests correct.

Profile of slot 1, µs per squaring:

| Phase | Quiet | 14 tests, round 1 | round 6 | round 11 | round 11 / quiet |
|---|---|---|---|---|---|
| p1 inverse stages | 20.44 | 33.99 | 37.48 | 39.15 | 1.92 |
| p1 normalise | 17.02 | 28.94 | 32.08 | 33.41 | 1.96 |
| p1 forward stages | 19.13 | 29.83 | 32.87 | 34.22 | 1.79 |
| p2 rows in | 8.39 | 13.80 | 15.23 | 15.92 | 1.90 |
| p2 fwd stages | 13.63 | 21.60 | 23.97 | 25.01 | 1.83 |
| p2 split | 13.01 | 19.53 | 21.64 | 22.54 | 1.73 |
| p2 inv stages | 12.15 | 19.96 | 22.21 | 23.13 | 1.90 |
| p2 rows out | 7.35 | 12.11 | 13.36 | 13.93 | 1.90 |
| total | 112.95 | 182.79 | 202.20 | 210.80 | 1.87 |

Every phase slows by about the same factor. The FP-bound ones slow least, as they would with a
lower clock; the normaliser (latency-bound), the p1 inverse stages and the pass-2 row loads
slow most, so they gain a little share. No phase becomes a new bottleneck, and memory traffic
costs only a little more under load, which fits the 0–2% table bound from 2026-10-11.

Test count: 14 tests settle at 366–372 tests/hour (137 s each by round 11), 12 tests at 344–347
(124.5 s each). The faster tests do not make up for two fewer, so the E-cores are worth running
even under the power cap. The 12-test run started 4 minutes after the 14-test one, warmer, but
the settled rounds compare like with like. The 10-test run was dropped: the trend was clear,
and the scheduler moves tests between clusters, so 10 tests would not have meant P-cores only.

The priorities file now carries the under-load profile, drops items 0 and 1, and keeps the
cache footprint, p1 inverse stages, normaliser and radix-16 items in that order.
On reflection the cache-footprint item goes to the bottom of the list: the under-load profile
is the evidence against it. Next is the pass-1 inverse stages, the largest phase (18.6% under
load) and at ~80% of the FP limit against 95% for the forward stages of the same pass.

## 2026-10-12 21:03 — Pass-1 inverse stages: the gap is the group load

The inverse stages are the first to touch each pass-1 group after pass 2, so their first stage
loads the group's 12 KB (96 rows × 128 B, 4224-byte stride) from L2; the forward stages run on
data the normaliser has just left in L1. A temporary switch read each group into L1 before
its inverse stages (results stay correct; the read was charged to the "phase b" bucket).
One core, `ahbench time 9001 662695 --iters 20000`, three alternating pairs, quiet machine:

| | p1 inverse | p1 forward | read | total |
|---|---|---|---|---|
| normal | 20.42–20.53 µs | 18.93–19.03 | — | 114.0–114.7 |
| read first | 18.41–18.45 | 18.96–19.00 | 2.87–2.89 | 114.9–115.3 |

On hot data the inverse stages are as fast as the forward ones, so there is nothing to fix in
their code. The cold load costs them 2.1 µs; doing it separately costs 2.9 µs, so the stages
already hide part of it. The ceiling is 2.1 µs (1.8%) even if the load were free, below the 3%
bar. Dropped; the patch is in `docs/experiments/p1-touch-before-inverse.patch`. The "80% of the
FP limit" figure was this load, not slow kernels. The normaliser is next.

## 2026-10-12 21:06 — Normaliser: at the FP limit on hot data

The priorities file said the normaliser was still tied to its carry-chain latency. Counting
`ahn_norm_ao`'s loop (`tools/count_ops.py`): 576 vector ops per 128 digits (448 FP + 128
`trn`), 704 with the round-off check. A microbenchmark (`docs/experiments/normbench.c`) runs the
kernel on one group with its data and table in L1:

| Kernel | ns per 128 digits | ops/ns | µs per squaring |
|---|---|---|---|
| `ao` | 34.0–34.3 | 16.9 | 13.1 |
| `aoe` | 40.8–42.1 | 17.3 | 15.7 |

Both run at the measured FP ceiling (~16.5 ops/ns), so eight chains in flight already hide the
fmla → frintn latency. With one squaring in four checking round-off the floor is ~13.7 µs; in
place it takes 17.0–18.4. The gap is the 1.15 MB normaliser table streamed from L2 (the group's
data is still in L1 from the inverse stages), which the 2026-10-11 L1-resident-tables run
already bounded at 0–2% under load. Dropped. The `trn` shuffles are 22% of its ops, but they
follow from the layout: the pass-1 FFT keeps one complex value per register, and the
normaliser needs two rows' digits per register for two chains.

That leaves one candidate on the list with a ceiling near 3%: radix-16 stages to cut FP work.
Two of the three items ranked above it today turned out to be memory loads already measured,
not slow code.

## 2026-10-12 21:12 — Radix-16: counted, no saving

Op counts of the generated kernels (`tools/count_ops.py`), vector ops per butterfly: radix-8
no-twiddle 56, tangent 66; radix-4 no-twiddle 16, tangent 22. One complex value per register,
so the counts track real flops. A 16-point DFT needs 168 real flops (144 adds, 24 multiplies),
2.63 per point per bit, against 2.33 for 8 points. Radix-16 cuts the outer twiddles from 0.29
to 0.23 per point per bit, about 0.08 ops at ~1.4 per tangent twiddle, which does not cover the
butterfly's extra 13%. For the 256-point pass-2 rows, today's 8 × 8 × 4 costs ≈ 20.75 ops per
point and 16 × 16 ≈ 21.1. The sweep it would remove runs over rows in L1. Dropped.

The earlier note in the priorities file, that 16 complex values would not fit NEON's registers,
was wrong: the kernels hold one complex value per q register, so 16 fit with room to spare. It
was not the reason to drop it.

The list is now down to items with measured or estimated ceilings of 0–2% each: per-test
cache footprint (tables), pass-2 rows in (~1.7%), and the pass-1 group load (1.8%, but a
separate read costs more than it saves). The kernels run at 80–95% of the FP limit and the
rest is L2 traffic that is cheap under load. The next gains need either a lower bar with
several 1–2% items stacked, or a change at the algorithm level.

## 2026-10-12 21:25 — Literature check: the k penalty, proofs, error bounds

With the kernel list empty, a search for techniques at the algorithm level.

**The k penalty is inherent to the IBDWT for k·2^n±1.** Percival (Math. Comp. 72 (2003),
387–395, "Rapid multiplication modulo the sum and difference of highly composite numbers") is
the source of the method ahnum uses. Its worst-case bound for k·2^m ± 1 has
|ax| < sqrt((k·2^m)^(1/N) · 3(k² − 1)/(2 log k² log 4)): the weighted digits grow with k, so
the error grows with k and the usable word size shrinks by about ½·log2 k bits, which is what
ahnum's fitted 0.525·log2 k shows. The paper's own example: an FFT of 2^18 suffices up to m = 6M
for k = 3 but only 3M for k = 557. Nothing found removes it within the IBDWT. The known
alternative avoids the k-weighting by computing the full product with a Mersenne-style
transform (no k term, ~20.4 bits per word here) and reducing with x·2^n ≡ x/k, i.e.
x² = H·2^n + L ⇒ x²·2^−n ≡ H + k·L. That needs about 2(n + log2 k) bits of transform against
n bits at 20.4 − 0.525·log2 k, so it wins only above log2 k ≈ 19–20. That matches what LLR-family
software does: an alternative LLR implementation on mersenneforum uses zero-padded FFTs for k of
22–45 bits and generic reduction beyond. At k = 9001 (13 bits) the IBDWT stays the faster choice.

**Proofs instead of double-checks.** Li and Gallot (arXiv:2209.15623, "An Efficient Modular
Exponentiation Proof Scheme") generalise the Gerbicz–Pietrzak proof from power-of-two exponents
to any modular exponentiation, so it covers a Fermat PRP of k·2^n − 1 (exponent N − 1). They
report negligible prover overhead and that at PrimeGrid it doubled the efficiency of the
distributed searches, by replacing full double-checks with certificate checks. PRST implements
proof generation and certificate verification; LLR2 is archived in its favour. This does not
speed up a squaring. For a search that double-checks its results, a proof-producing ahnum would
roughly double useful output per machine; for one that already uses PRST's certificates, ahnum
needs to produce certificates PRST accepts before it can take that work.

**Error bounds.** Percival's proven bounds are worst-case; his measurements put random-input
errors 1.6–8.7 bits below the "unlucky" inputs as N grows from 2^4 to 2^18. GIMPS and gwnum, like
ahnum, choose word sizes from measured errors on real inputs and rely on Gerbicz checks and
retries to catch the rare bad case. The searches found nothing newer that would justify more
bits per word.

Sources: Percival 2003 (ams.org/mcom/2003-72-241/S0025-5718-02-01419-9, PDF at
daemonology.net/papers/fft.pdf); Li & Gallot, arXiv:2209.15623; github.com/patnashev/llr2
(archived, replaced by PRST); mersenneforum "Alternative LLR tester implementation"
(showthread.php?t=25343).

## 2026-10-12 21:29 — Decision: a PRST-compatible CLI with proofs

Goal: ahnum as a drop-in for prst for Fermat tests of k·2^n − 1 with proofs. The slice of
PRST needed: the Fermat test (`EXPR -d -fermat a 3 [-t T]`), `-proof save 512` / `build 512`
/ `cert FILE`, `-time progress 5 coarse`, `-fft info`, `-v`, and SIGTERM checkpointing and
resume in the working directory; LLR (`EXPR -d`) stays with PRST. For k·2^n − 1 with small k PRST takes its
"smooth" path, so the proof is plain Pietrzak (points along the n squarings after a^k), not
Gerbicz–Li. The algorithm and file formats are small: `src/proof.cpp` (~770 lines) and
`framework/file.cpp`, with fingerprint N mod 3417905339, magic 0x9f2b3cd4, 32-bit little-endian
giants and `.md5` sidecar files. Interop is tested against the PRST 14.0.1888 x86 binary
under Rosetta; the source read is 14.1.1903.

Decisions: the CLI reports its own version, `ahnum a.b.c`, rather than imitating PRST's
banner; LLR is handed to PRST for now.

## 2026-10-12 21:43 — ahprst: PRST-compatible proofs, byte for byte

`ahprst` (tools/ahprst.c) implements a slice of PRST's command line (docs/prst-compat.md)
and hands everything else, LLR included, to PRST. The
proof code (src/prstproof.c, src/md5.c) ports PRST 14.1's src/proof.cpp for the smooth path
and framework/file.cpp's format.

How it was checked. PRST 14.0.1888 (x86) ran under Rosetta
to produce reference output: `-proof save 512 keep` leaves all 513 points, so ahnum could
compute the 9 products and the certificate from PRST's own points and compare files. All
matched byte for byte on the first run, including the .md5 sidecars. Then `ahn_prp` gained
a point hook: points are captured as the squaring loop passes them, held until the Gerbicz
check covering them passes, and dropped on a rollback, so a SIGTERM never leaves an
unverified point on disk. With it ahprst's own 513 points for 1203·2^90001 − 1 were
identical to PRST's, and so were its products and certificate (RES64 EC81DC6893506908).
Across the two programs: PRST verifies ahprst's certificate, PRST builds a certificate from
ahprst's points, and ahprst verifies PRST's certificate, all giving the same certificate
RES64. 1203·2^400001 − 1, interrupted with SIGTERM at 25% and resumed, gave a certificate
identical to PRST's (C17686F5F6B4C2CE) and RES64 ACB155E96C137D61, PRST's value.

Details that took reading the source rather than guessing:
- File names use N mod 3417905339, but point and product headers carry
  MD5(fp ‖ "3.n")'s first word, and the certificate header carries the plain fp.
- The challenge is only the first 64 bits of MD5(fp ‖ Y ‖ product): `hash_giants` writes 16
  bytes but then reinitialises the giant to 2 words. It is made odd and pushed past prime
  factors below 1000.
- PRST does not trial-divide by default; its "divisible by" result comes from the Fermat
  test when a divides N.

`tests/prst_compat.sh` (ctest `prst_compat`) holds MD5s of PRST's files for
1203·2^90001 − 1 and checks ahprst against them in 2 s. Still missing then:
PRST's roots-of-unity check during build (a safety check that changes no output), added
before the next entry.

## 2026-10-12 22:00 — ahprst at n = 2,000,003: identical certificates

9001·2^2000003 − 1, with `-proof save 512` then `-proof build 512`, 4 threads each. Both programs give RES64
6B2FB550F09B6CBD and certificate RES64 3B9230016377CE4A; the two .cert files are identical;
PRST verifies ahprst's certificate and ahprst verifies PRST's. ahprst's build ran the new
roots-of-unity check.

Save step, start to finish: ahprst 244.7 s, PRST 364.8 s (1.49×). Not a benchmark: the
machine was not checked for quiet, and PRST runs x86 SSE2 code under Rosetta at 4 threads.
PRST's own "time per op" during a proof save read 0.42 ms against ahprst's 0.12, but PRST's
figure during `-proof save` is not comparable with its plain test, so only wall time
compares. The figure that matters is 14 single-thread proof tests side by side, still to
measure.

## 2026-10-12 22:10 — Deployment target macOS 11

The binaries were marked `minos 27.0`: with no deployment target set, CMake uses the build
machine's macOS, and the loader refuses the binary on anything older. CMakeLists.txt now sets
`CMAKE_OSX_DEPLOYMENT_TARGET` to 11.0, the first macOS for Apple Silicon. A clean build gives
`minos 11.0` for ahprst and ahbench with no availability warnings, ahprst links only
libSystem, and all 11 tests pass. The only linker warnings come from Homebrew's GMP (built
for macOS 15), used by the test tools gmp_ref and test_gmp, which are not shipped.

On other M chips: C is compiled with `-mcpu=apple-m1` and the NEON kernels use ARMv8 FP64
SIMD only. The SME backend is opt-in and checks `hw.optional.arm.FEAT_SME_F64F64` before use.
Without `~/.ahnum/fft_times.txt` (written by `ahbench tune`) ahnum uses its default FFT length
choice and stage order. The macOS 11 build, copied (not rebuilt) to a base M4 Mac mini
(macOS 27, busy with other work), passed `tests/prst_compat.sh` and all 12 known residues
below n = 100,000 with no tuning file. Not yet run on an M1–M3 or on a macOS older than 27.
No M1–M3 is available locally (the other Macs are Intel, where ahprst cannot run and PRST
stays), so `.github/workflows/ci.yml` builds and runs `ctest -LE long` on GitHub's Apple
Silicon runners: macos-14 and macos-15, both M1. That covers an older chip and older macOS
for correctness; the runners are virtualised, so no timings.

## 2026-10-12 22:15 — CI on M1: one test assumed SME

First CI run: on both M1 runners 10 of 11 tests passed, including prst_compat, the known
residues and the GMP checks of the NEON backend; `asm_stages` died with an illegal
instruction. It ran the SME radix-8 kernel unconditionally, and an M1 has no SME. ahnum's
own SME backend already checks `hw.optional.arm.FEAT_SME_F64F64`; the test now does the same
and skips that part. Second run: all 11 tests pass on macOS 14.8.9 (AppleClang 15) and
macOS 15.7.9 (AppleClang 17), both "Apple M1 (Virtual)". The library and ahprst needed no
change for M1 or the older macOS and compilers.

## 2026-10-12 22:30 — Renamed: kite and kite-prst

ahnum is now kite: the library `libkite.a` (header `include/kite.h`), the benchmark command
`kite` (was ahbench) and the PRST-compatible command `kite-prst` (was ahprst), with banner
`kite version 0.1.0`. The name comes from the red kite, a raptor that lives largely on
carrion. The name is free on Homebrew and under primecrunch on GitHub;
several unrelated projects use "Kite", none in prime searching. `kite-prst` keeps a separate
name so it is clear it speaks PRST's command line without being PRST.

Changed with it: `$AHPRST_PRST` → `$KITE_PRST`, checkpoint files `kite_FP.A.ckpt` (tag
`KITECKP1`, so old checkpoints do not resume), tuning file `~/.kite/fft_times.txt`, and the
whole C namespace: 140 `ahn_` names (971 uses in 29 files, including the generated assembly
symbols) became `kite_`, and the 28 `AHN_` names (backend enum, result codes, header guards,
the `AHN_PROF` / `AHN_FFT_TIMES` / `AHN_NO_TAN` style switches) became `KITE_`. The library
exports 152 `kite_` symbols and none with the old prefix; the 11 tests pass, and the GPU
prototypes build against `libkite.a` and give the right residue. This journal's earlier
entries and the patches in `docs/experiments/` keep the old names as written.
The CMake library target is `kitelib` with output name `kite`, because target names must be
unique and the command is `kite`.

## 2026-10-12 22:38 — LLR in kite-prst, matching PRST's Morrison test

`kite-prst EXPR -d` now runs the LLR test itself instead of handing it to PRST.

What PRST computes. Its default test for k·2^n − 1 is the "Morrison (LLR) test"
(`src/morrison.cpp`) with Q = −1, not Riesel's classic u_0 = V_k(P, 1) start. P is the
smallest P ≥ 1 with Jacobi(P² + 4 | N) ≠ 1, and gcd(4P(P² + 4), N) > 1 is reported as a factor
before the test is announced; that is why PRST called 1203·2^90001 − 1 "divisible by 5" (P = 1,
P² + 4 = 5). N is prime iff V_{k·2^(n−1)}(P, −1) ≡ 0, and a composite prints "is not a probable
prime. Have you run Fermat test first? RES64: X" with X the low 64 bits of that V. A Python
prototype reproduced PRST's P and RES64 on 9357·2^20000 − 1 and 9357·2^25000 − 1 before any C
was written.

How kite computes it (`src/llr.c`). V_k by a Lucas ladder on k's bits, then V_2k = V_k² + 2
(k is odd, so Q^k = −1), then n − 2 steps of V² − 2. The constant ±2 needed a new library
operation: `kite_add` / `kite_sub` add two numbers element-wise. That is valid because every
backend stores weighted digits put through linear FFT stages, so the arrays are linear in the
value; the sum's digits are unbalanced until the next square renormalises them. GMP checks on
the ref, c2p and NEON backends (and NEON with 4 threads) pass, including a 30-step V² − 2
chain.

Error handling differs from PRST. Gerbicz checking does not apply to a V² − 2 chain; PRST runs a
Gerbicz–Li check on the Lucas sequence, which is why its "complexity" is about 2n. kite checks
round-off on every step and redoes a block from the last verified state with a longer FFT if
it is too high, and confirms a composite result with a second full run at a longer FFT
length (a third if they disagree). The asymmetry is the point: an arithmetic error turns a
prime's 0 into some non-zero value far more often than it lands exactly on 0, and LLR is
normally run only after a Fermat PRP, so the extra run is almost never paid. The checkpoint
keeps the earlier runs' RES64s, so a confirmation interrupted by SIGTERM resumes inside the
confirmation (tested: interrupted at 62% of run 2, resumed, matched without a third run).

Checked against PRST 14.0.1888: primes 3·2^26459 − 1 and 3·2^41628 − 1; composites
9357·2^20000 − 1 (DC52061161A0DE1C), 9357·2^25000 − 1 (54A27E03D75F2447), 4095·2^70000 − 1
(6399FFF11BF53EC0) and 33·2^99999 − 1 (3FAF838186E8A836), same P each time; 1203·2^90001 − 1
divisible by 5. Five of these are now ctest cases (`llr_*`).

The known prime 9001·2^662695 − 1 (P = 1, FFT 48K) came out prime in 177.2 s on one thread
with the machine under heavy outside load (load average ~46). The primecrunch record for this
number gives its LLR proof as 407 s on the same Mac, also measured under unknown load, so the
two are not a benchmark; a quiet comparison is still to do.

## 2026-10-12 23:05 — kite-prst against PRST, one thread

9001·2^662695 − 1, `-d -fermat a 3 -t 1`, alternating runs on the M4 Pro with the distributed
compute client stopped, each after 60 quiet seconds and thermal pressure 0:

| Run | Program | Time |
|---|---|---|
| 1 | kite-prst | 74.5 s |
| 2 | PRST 14.0.1888 (x86, Rosetta) | 93.5 s |
| 3 | PRST | 93.4 s |
| 4 | kite-prst | 75.4 s |

All four report a probable prime. kite-prst is 1.25× faster on one thread (74.5–75.4 s against
93.4–93.5 s). Run 4 overlapped for about 70 s with a duplicate kite-prst started by mistake
(on another core, same cluster), so it may read about 1% slow. The pre-flight check before it
was held up by WindowServer at 36–43% of a core while the screen was in use; the 20% limit was
set for 14-test runs and is stricter than a one-core test needs.

## 2026-10-16 19:19 — LLR: ready for primecrunch, and a benchmark volunteers can run

How the primecrunch client uses LLR. The server hands out an `llr` job only for a candidate
that the same client has already found to be a Fermat PRP (`ReserveLlrWork.sql`), so nearly
every LLR input is prime. The client runs `prst EXPR -d [-t T]`, reads stdout, and takes a
composite's RES64 as the job's result value; 0 means prime. primecrunch searches only odd k
below 10,000.

A bug in the composite confirmation. kite accepts a 0 (prime) after one run and confirms a
non-zero result with a second run at a longer FFT length, and a third if they disagree. The
verdict was taken from run 0. So a prime whose run 0 went wrong, with runs 1 and 2 agreeing on 0,
would have printed "is not a probable prime ... RES64: 0000000000000000". The verdict and RES64
now come from the agreeing runs. The Lucas ladder for V_k also had no round-off check: the
check after it was reset before the first block. It now has one, and a failed ladder is never
written to the checkpoint.

Fault tests (`tests/test_llr.c`, ctest `llr_faults`, 4 s). New hooks in `kite_llr_opts`
corrupt the value at a chosen doubling in chosen runs, or fail a round-off check once. Covered:
a prime outvoting a bad run 0 (fails on the old code), a composite with run 0 or run 1 wrong,
three different answers (error -3), a round-off redo in the doublings and in the ladder, with
and without a checkpoint, a real too-short FFT (7 redos, right answer), an impossible
round-off limit (gives up with -2), and a stop and resume inside run 0 and inside the
confirmation.

Against PRST 14.0.1888 at production sizes. Eleven composites with P = 1, 3, 5, 7, 8, 12 and 13,
k from 1003 to 987,654,321 and n from 454,049 to 1,999,999: identical RES64s. Seven primes
primecrunch has found (2911·2^333069 − 1, 4299·2^522773 − 1, 4935·2^401257 − 1,
4935·2^401310 − 1, 6369·2^398211 − 1, 8025·2^347103 − 1, 8333·2^339626 − 1): all prime in both.
9357·2^1500007 − 1: both report the factor 5. kite also proved 1011·2^4702339 − 1 prime in
3054 s with 4 threads at FFT length 324K. The same binary gave the same results on a Mac mini
(base M4, 4P+6E, macOS 27), where the non-GMP ctest set also passes. The one miss is k =
3,221,225,471: kite takes k < 2^30 only, for Fermat as well.

On the loaded M4 Pro, kite took about 0.4× PRST's time on the primes (32.9 s against 75.1 s,
71.8 s against 178.9 s) and about the same on composites, which kite runs twice. Not
benchmarks; the machine was at load ~20.

Why PRST's LLR costs 2n. PRST's `morrison.cpp` computes the Lucas U and V sequences together
(`LucasUVMulFast`) when its default `CheckStrong` is on. That makes the test an exponentiation
a Gerbicz–Li check can verify, with 16 check points; carrying U is what doubles the work. With
`CheckStrong` off it runs V alone (`LucasVMulFast`), which is what kite does. The earlier entry
put the 2n down to the Gerbicz–Li check itself; the check costs little, the restructuring
costs 2×. For primecrunch's input (Fermat PRPs) one unchecked run per prime is n against
PRST's ~2n, and a composite costs 2n in both. Gerbicz–Li would win only where errors are
common, since it rolls back a block where kite reruns the whole test. Two runs can agree on a
wrong answer if a fault is systematic, such as a bug in code both runs share; the stronger
safeguard is on the server: an LLR composite after a Fermat PRP is almost certainly a fault
and could be sent to another machine.

Progress now only goes forward, as PRST's does: the first run reports 0–99% and any
confirmation run holds at 99% until the result line. The client would otherwise have shown a
test going from 100% back to 0%. The "confirming" note moved to stderr.

`tools/kite-bench.sh`: a benchmark for volunteers. It waits for 60 quiet seconds (no other
process above 20% of a core, all of them under 50%, thermal pressure nominal). If the machine
stays busy it shows what is busy, grouped by name, with a hint for the primecrunch client,
Spotlight, backups and browsers, and asks the person to quieten it or type go. It then runs
PRST and kite alternately, Fermat (`9357*2^650001-1`, RES64 2AB1256594388021, checked with
both programs) and LLR (`4299*2^522773-1`, prime), one test alone and then one per core, and
writes a Markdown report with both times, the speed ratio and the residues checked against
the known answers. The report gives only load average and other processes' CPU %, no process
names. Testing it on the hot M4 Pro found three hangs or gaps: the cool-down waited for thermal
pressure 0 forever, a bare `wait` would have waited on the background sampler, and the busy
list could be empty at the prompt.

Release workflow (`.github/workflows/release.yml`): a tag vX.Y.Z builds and tests on an M1
runner and publishes the tarball, a copy of `kite-bench.sh` stamped with the version and the
tarball's SHA-256, and SHA256SUMS. Volunteers run `curl -fsSLO .../kite-bench.sh` and `sh
kite-bench.sh`; curl downloads carry no quarantine flag, so the unsigned binary runs without
Gatekeeper prompts, and the script refuses a tarball whose checksum does not match. Tested
locally with a file:// URL, including a tarball with one byte appended.

The script also downloads PRST 14.0 from its GitHub release (`prst-14.0-mac64.7z`, pinned by
SHA-256) unless given a path. macOS's own `tar` unpacks 7-Zip, and the binary inside is
byte-identical to the prst the primecrunch client ships, so every report compares against
the same PRST whether or not the volunteer runs the client. The archive stores prst without
its execute bit, which first showed up as a misleading "needs Rosetta" error; the script now
sets the bit and tests for Rosetta with `arch -x86_64 /usr/bin/true` before blaming it.

## 2026-10-16 20:00 — First full kite-bench run: M4 Pro, quiet

kite 0.1.0 from the GitHub release against PRST 14.0.1888 under Rosetta, run with
`caffeinate -i sh kite-bench.sh`, the distributed compute client stopped. 19 minutes. Other
processes used 28% of one core on average, with one burst to 266%.

| Test | Mode | PRST | kite | kite vs PRST |
|---|---|---|---|---|
| Fermat 9357·2^650001 − 1 | 1 test | 91.3 s | 73.1 s | 1.25× |
| LLR 4299·2^522773 − 1 | 1 test | 131.6 s | 47.9 s | 2.75× |
| Fermat | 14 tests | 145.1 s, 345/h | 122.5 s, 410/h | 1.19× on throughput |
| LLR | 14 tests | 212.6 s, 237/h | 76.4 s, 655/h | 2.76× on throughput |

Every residue matched the known answer in both programs. The one-test Fermat ratio is the
same 1.25× measured by hand on 12 October (9001·2^662695 − 1), so the script agrees with the
manual method. Under full load kite's per-test Fermat time grows 1.68× against PRST's 1.59×,
so its lead drops from 1.25× to 1.19×: the memory and power limits in priority 1 of the speed
file cost kite more than PRST. LLR keeps its lead under load: one Mac running kite does the LLR
work of nearly three running PRST, from half the work per prime (V alone against U and V)
times the faster squaring.

The report said "quiet machine: peak load average 59.7". The load average counts the
benchmark's own 14 tests, so it says nothing about outside noise; the report now gives the
load average from before the runs and the outside-CPU figures from during them. The time
estimate is now 20–40 minutes rather than 30. While the user's run waited in its quiet
check, the status line stayed at "0s of 60s" with no reason given; the next version names
the busiest process while it waits. My own test copy of the script, started at the same
time, briefly ran PRST alongside it before I stopped it; that could only have delayed the
quiet check, not the measured runs.

## 2026-10-16 20:29 — kite-bench 0.1.1 rerun: the figures repeat

Same M4 Pro, quiet (load 1.06 before the runs, other processes 41% of one core on average
during them). Fermat one test 90.9 s / 73.0 s (1.25×), LLR one test 132.6 s / 48.4 s (2.74×),
Fermat 14 tests 341/h / 413/h, LLR 14 tests 234/h / 655/h. Every figure is within about 1% of
the first run, so one run per machine is enough for the README table. The second report
replaced the first (same chip and date in the file name); a repeat run now gets -2, -3, ....

## 2026-10-16 20:37 — Volunteers asked to run kite-bench (forum thread 82)

kite 0.1.2 is out with the report-naming fix. The OP of primecrunch forum thread 82 ("Apple
Silicon Replacement for prst and gwnum") now has the two-line instructions, what the script
downloads and checks, what the report contains, the M4 Pro results table and why LLR is
faster. Machines wanted: macOS older than the current release, and M1, M2, M5 and M6 (one
member has offered an M2, an M5 and an M6). The edit went through the forum's own
`PUT /v2/forum/threads` from a logged-in browser tab, because the editor is rich text and
would have re-formatted the Markdown table and code block. The forum's renderer starts a new
numbered list after a code block, even an indented one, so the commands sit below the
three steps instead of inside step 2.

## 2026-10-16 20:39 — kite 0.1.3: the report has no heading

The primecrunch forum does not render Markdown headings, so the report's first line
(`### kite vs PRST — ...`) would have shown its hashes. It is now a bold line. Tables, bold
and inline code render there, so nothing else in the report changes.

## 2026-10-17 09:42 — The compat doc said the roots-of-unity check was missing; it is not

Asked whether kite-prst and PRST verify each other's certificates, I answered from
`docs/prst-compat.md`, which listed PRST's roots-of-unity check as not implemented. Reading
`src/prstproof.c` showed `roots_of_unity_check` there since the first PRST commit (b41bedc),
called from `pp_build_cert` before the certificate is built. The doc line was wrong from the
day it was written.

What the check does: a Fermat test gives R = a^(N−1) mod N, which is 1 for a prime. The
proof is Pietrzak's VDF construction, whose weak spot in (Z/NZ)* is elements of small order
(roots of unity), which can be multiplied into values without the verifier's
random-exponent checks seeing them. PRST defends this twice: the challenge exponents are
odd and free of primes below 1000, and before building it confirms R is not itself a
small-order root of unity. Since R's order divides N−1, it raises R to the small-prime part
of N−1 (2, then each odd prime below 2^12 dividing N−1 to its largest power below 2^31) and
fails if that gives 1. kite-prst does the same but does not print PRST's
"Factorizing N-1" line. No test reaches a failing case; the match comes from reading
`proof.cpp:82`.

The doc now describes the check and marks the Factorizing line as PRST only. Lesson: a
"not implemented" list needs checking against the code as well as the prose that sits
beside it.

## 2026-10-17 10:01 — Damaged proof files: kite-prst and PRST fail the same way

The interop tests only ever used good files. `tests/prst_faults.py` (ctest `prst_faults`,
5 s) makes a good proof set for 1203·2^90001 − 1, damages copies, and checks `-proof build`
and `-proof cert` against what PRST 14.0.1888 (the primecrunch client's copy) did with the
same files. Given a prst path instead of kite-prst it runs the whole pipeline through PRST,
and it passes there too, so the expectations describe PRST and not just kite.

Rejected with exit 1 by both: a product with a stale `.md5`, with a changed header
fingerprint, or missing (`is missing or corrupt.`); point 0 not equal to a^k
(`invalid a^k.`); R = a^(N−1) set to −1 (`roots of unity check failed.`); a certificate
with a stale `.md5` (`Invalid certificate file.`). Both leave the same files behind.

Accepted by both, with the same certificate RES64: a product, point 512 or certificate
damaged with its `.md5` rewritten, R = 1 (a faked probable prime), and a certificate with
no `.md5` at all. The `.md5` sidecar is optional and only guards against accidental damage.
Neither program can tell a damaged file from a good one by itself; the scheme relies on the
server comparing the builder's certificate RES64 with a verifier's.

One attempt failed: I tried to make R an element of order 139 (139 divides N − 1) as
g^((N−1)/139). For a composite N that has order 139 only if g^(N−1) = 1, which is exactly
what fails, and building one needs the factors of N. So the test covers the check through
R = −1 only. Python's own pow on a 90,000-bit modulus was also too slow for this (killed
after 9 minutes); GMP took 34 s.

Differences the test does not check, now in docs/prst-compat.md: PRST prefixes build
errors with the expression and appends every error to `result.txt`, and kite-prst does
neither. PRST's build also reprints the test line, `a^k is correct`, `restarting at
100.0%` and the test's RES64 before the certificate. The doc had listed `a^k is correct`
and `restarting at 100.0%` as output kite-prst gives; it does not.

## 2026-10-17 10:05 — CI: actions/checkout v4 to v7

GitHub warned that `actions/checkout@v4` targets Node.js 20, which runners now force onto
Node.js 24. Checkout v5, v6 and v7 all declare `node24`; both workflows now use v7. Its one
behaviour change blocks checking out fork PRs under `pull_request_target` and
`workflow_run`, which neither workflow uses. checkout is the only action either uses.

## 2026-10-17 10:26 — Ignore .cache/

`.cache/` holds clangd's index, built locally by the editor's language server. Added to
`.gitignore` so it stops showing as untracked.

## 2026-10-17 13:02 — 0.1.4: Fermat tests resume, and stop within a second of SIGTERM

Seen on wolf, the primecrunch client running kite-prst at n around 2.7M: after "Waiting for prst
processes to save and stop..." kite took 10 s to nearly 5 minutes to exit, against under a second
for PRST, so the client's 30 s wait ran out and it killed the tests.

Two bugs, both in every release so far.

- **The stop request was read only between Gerbicz rounds.** A round is B·M squarings
  (L2 = 814·203 at n = 662,695, about 2 minutes of work at n = 2.7M on a loaded M4 Pro). It is now
  also read after each block of B squarings; a stop part way through a round drops the unverified
  squarings and saves the last verified state, as the existing between-rounds stop does. Measured at
  9001·2^662695−1: 15.6 s to exit before, 0.01 s after.
- **No Fermat checkpoint was ever read back.** ckpt_write still spelt the magic as the
  pre-rename project name, "AHNCKPT1", while both readers checked "KITECKP1"; the rename commit
  (35c9f2a) changed the string literals and missed the character list. Every interrupted test
  started again from zero, which at n = 3.3M is over an hour per test. Writes now use "KITECKP1",
  and reads accept both, since the layout is the same, so a test interrupted under 0.1.3 resumes:
  checked by stopping 9001·2^662695−1 under the 0.1.3 binary and finishing it under 0.1.4
  ("restarting at 74.8%", PRP as expected). LLR checkpoints ("KITELLR1") were consistent and were
  never affected.

New test, `tests/prst_resume.py` (ctest `prst_resume`, quick suite): stops 9357·2^200001−1 at 40%
with progress every half second, requires exit within 1 s, then requires the rerun to report a
non-zero restart and the uninterrupted RES64. It fails on 0.1.3 at the resume check.

Nothing exercised resume before. prst_compat and prst_faults run tests to completion, and the
Gerbicz fault test recovers inside one process.

## 2026-10-17 14:32 — 0.1.5: a stop saves the block it reached, not the start of the round

Seen on wolf straight after moving the primecrunch client to 0.1.4: all 14 tests stopped cleanly
on SIGTERM and wrote "KITECKP1" checkpoints, but every header held iteration 0 and the rerun
started from zero again. Example: 2007·2^3321961−1 stopped at 4.2% and saved iter = 0.

0.1.4 saved only the last Gerbicz-verified state, and a round is B·M squarings. At n = 3,321,961
that is 1,822 · 455 = 829,010, a quarter of the test and about 25 minutes per test on a loaded
M4 Pro. A stop before 25% saved nothing; after that it lost on average half a round. The 600 s
periodic checkpoint has the same limit, since it too can only write verified state.

The Gerbicz check d_m = x0 · d_{m−1}^(2^B) holds at every block boundary, not only at the end of a
round, as long as t holds d from before the latest update. t is now copied at every block (one
copy per B squarings), and a stop verifies up to the block it reached, saves that, and exits. The
check costs B squarings: 1.74 s at n = 3,321,961 on an idle M4 Pro (stopped at 3.7%, restarted at
3.7%), against the client's 30 s wait before it kills.

`tests/prst_resume.py` now stops at 15%, inside the first round (which ends near 25% at
n = 200,001), and requires the rerun to restart within 1% of where it stopped. 0.1.4 fails it (no
restart, as iteration 0 is not reported); 0.1.5 stops in 0.03 s and restarts at 16.8% with the
uninterrupted RES64. Quick suite 19/19.

Not changed: the periodic checkpoint still waits for a round boundary, so a kill -9 or power loss
can still lose up to 25 minutes per test at n = 3.3M. Ending a round early when the 600 s timer
fires would fix that for B squarings per checkpoint (about 0.5% at that n).

## 2026-10-17 15:01 — 0.1.6: an unverified checkpoint every minute, so a kill loses at most a minute

0.1.5 made a SIGTERM stop keep its work, but a SIGKILL, crash or power cut still fell back to the
last verified file, written only at round ends: a quarter of the test apart, 25 minutes per test
at n = 3.3M.

Ending rounds early on a timer would bound that, but each check costs B squarings (3.3 s at
n = 3.3M), so a 600 s timer costs about 0.5%. Instead kite now also writes the current (x, d, i)
at a block boundary every minute without checking it: one 830 KB file write, no squarings. It is
safe to resume from because d is the product of every block's x since the start, so the first
check after the resume covers the squarings before the save as well. A bad unverified file is
caught there, deleted, and the test goes back to the verified state. Same file format, separate
file (`kite_<fp>.<a>.u.ckpt`); a resume uses it only when it is ahead of the verified one.

Proof points are reported when they are verified, so a resume from an unverified state would have
had no record of the points since the last check. Points reached so far are now reported with
each unverified save. A failed check redoes them and reports them again, overwriting the files.

kite-prst takes PRST's `-time write <sec>` for the interval (default 60, where PRST's write
interval is its checkpoint interval). The kite CLI has `--uckpt FILE --uckpt-secs S`.

New test, `tests/prst_kill.py` (ctest `prst_kill`): SIGKILLs 9357·2^200001−1 inside the first
round with saves every 0.2 s, and requires the rerun to restart where it was killed (5.1%) with
the uninterrupted RES64 and raw certificate RES64; then injects a fault at iteration 5,000, kills
after a later unverified save, and requires the rerun to fail its first check and still finish
with the right residue. 0.1.5 fails it: it writes no unverified file. Quick suite 20/20.

At n = 3,321,961 on wolf alongside 14 client tests: kill -9 at 2.0%, unverified file at
iteration 63,770, restarted at 1.9%.

Cost: one file write per minute per test, 14 tests × 830 KB = about 12 MB a minute, 17 GB a day
on a fully loaded M4 Pro. Not changed: LLR checkpoints, which are not on this path.

## 2026-10-17 15:14 — 0.1.7: proof points are written before the unverified checkpoint

0.1.6 wrote the unverified checkpoint and then the proof points reached since the last save. A
kill between the two left a checkpoint at iteration 13,410 with points only up to index 26 on
disk; the resume skipped the points before 13,410, so 27-34 were never written and
`pp_save_products` failed with "prst_715692104.proof.32 is missing or corrupt", exit 1, after a
correct RES64. Caught by `prst_kill` in the clean rebuild after tagging v0.1.6, about one run in
three: its poll kills as soon as a new unverified file appears, inside that window.

The points are now written first. A kill while they are written leaves the older checkpoint, and
the points after it are redone and written again. `prst_kill` 12/12 after the change (all 12
passing by chance on 0.1.6 would be about 1%). v0.1.6 should not be used.

## 2026-10-17 16:09 — 0.1.8: a stop while compressing the proof keeps the result and the products

Seen on eagle: the primecrunch client stopped while 8 tests at n ≈ 3.32M had printed their
result and were compressing 512 proof points. kite read no stop request during compression, the
client killed them after its 30 s wait, and the results were recorded with no certificate.

Compression is 9 levels of challenge powers over the points, about 63k multiplications whatever
n is (4.3 s at n = 200,001, 0.068 ms each), so about 2 minutes at n = 3.3M on a loaded M4. The
last level is about half of it.

- `pp_save_products` reads the stop request at every level and at every point within a level,
  and returns `PP_ABORTED` leaving the points and the finished products. On a rerun it uses the
  products already on disk: Y and the challenges follow from point `count` and the products, one
  power each, and the tree is rebuilt at the start of each level, so nothing else needs saving. A
  half-written product fails its md5 and is redone.
- kite-prst writes `kite_<fp>.<a>.done` (k, n, a, RES64, PRP flag, test time) when a test with
  `-proof save` finishes, before removing its checkpoints. A rerun that finds it prints the
  result again without repeating the test, finishes the compression, and removes it. The result
  is not appended to result.txt a second time.

New test, `tests/prst_proof_stop.py` (ctest `prst_proof_stop`): SIGTERM as soon as the result is
printed, and again once 4 products are on disk; each must stop within 1 s (0.01 s), and the
rerun must not show progress, must print the result, and must give the uninterrupted raw
certificate RES64. 0.1.7 fails it (it ignores the stop and finishes). Quick suite 21/21.

## 2026-10-17 19:31 — At n = 3.3M kite loses to PRST on a busy M4: the tables don't fit

primecrunch's Fermat work has moved to n ≈ 3.3M (FFT 256K), and on eagle (M4, 4P + 6E) kite 0.1.8
ran 5.2 tests per hour where PRST had done about 8. ms per squaring, 2007·2^3322001−1 and
neighbours, `-t 1`, 60 s per configuration:

| Configuration | kite 0.1.8 (256K) | kite, `KITE_NO_NTAB=1` | PRST 14.0 (240K) |
|---|--:|--:|--:|
| 1 test, P-core | 0.708 | 0.787 | 0.706 |
| 4 tests, P-cores | 0.946 | 0.948 | 0.872 |
| 10 tests, all cores | 2.03 | 1.56 | 1.30 |

Alone, kite and PRST are level, so the gap is not FP work. From 4 tests to 10, kite gains 0.7
squarings per ms and PRST 3.1: per E-core kite is about 4× slower. At H = 128K each test streams
about 10.4 MB per squaring: 2 MB of data, 2 MB of four-step twiddles (`t4`) and 6 MB of normaliser
table (`ntab`, on for H ≤ 2^17), against an E-cluster L2 of 4 MB for 6 cores. Turning `ntab` off
alone takes 23% off at 10 tests. The 2026-10-11 tables bound and the 2026-10-12 load profile
were at FFT 49152, where the same arrays total about 3 MB, so they ranked the footprint last
correctly for that size. The plan for production sizes is in `docs/large-fft-plan.md`.

`taskpolicy -b` does not isolate E-cores usefully: macOS also lowers their clock (kite 5.8 ms per
squaring alone), and PRST printed no progress within 60 s under it.

## 2026-10-17 21:10 — Load benchmark baseline: M4 and M4 Pro, FFT 28K–432K

`tools/load-bench.sh --ladder`, 0.1.8 against PRST 14.0.1888, both machines quiet, 60 s per run.
Full tables in `docs/results/load-bench-2026-10-17-m4.md` (eagle, 4P + 6E) and
`-m4pro.md` (wolf, 10P + 4E). kite against PRST in tests per hour, all cores busy:

| FFT (kite) | M4, 10 tests | M4 Pro, 14 tests | `ntab` |
|---|--:|--:|---|
| 28K | 1.14× | 1.33× | on |
| 48K | 1.54× | 1.91× | on |
| 64K | 0.94× | 1.10× | on |
| 96K / 89600 | 0.87× | 1.06× | on |
| 128K / 120K | 0.78× | 1.03× | on |
| 160K | 0.78× | 0.93× | on |
| 192K | 0.71× | 0.87× | on |
| 256K (two cases) | 0.65–0.66× | 0.81–0.84× | on |
| 288K | 0.90× | 0.96× | off |
| 384K | 0.88× | 0.96× | off |
| 432K (PRST 400K) | 0.81× | 0.93× | off |

- With the table on, the M4 falls behind from 64K, where 6 tests' footprint (~2.5 MB each)
  already exceeds the E-cluster's 4 MB L2, and the M4 Pro from 160K. Above the H ≤ 2^17 cut-off
  (288K+) the gap closes to 4–12%: the table is most of the loss, as `KITE_NO_NTAB` showed.
- On the M4 Pro the 4 E-cores add nothing to kite at 160K–256K (10 tests 9.5/h, 14 tests 9.6/h
  at 256K), against +10–15% for PRST.
- Alone, kite is 0–12% ahead up to 256K and 5–10% behind above it (compressed normaliser,
  step 3; at 432K also PRST's shorter 400K length, step 4).
- The 28K and 48K PRST rows read slow: 48K gives 188 tests/h on 10 P-cores against 283–307 from
  complete tests in the README. Either PRST's "time per op" counts differently there or it
  picks a slower FFT type for this k; check with complete tests before using those ratios.

## 2026-10-17 22:30 — 0.1.9: the normaliser table only up to FFT 96K, and a refitted length choice

Three candidates from `docs/large-fft-plan.md`, each measured with every core busy before
keeping it.

**Normaliser table cut-off (kept).** `KITE_NTAB=on|off` sweep at FFT 28K–256K, both machines,
one test per P-core and one per core (`docs/results/ntab-sweep-2026-10-17.md`). All cores, ms
per squaring, off against on: the table wins up to 64K (M4 −5% to −7%, M4 Pro −7% to −10% for
off), 90K is level within 3%, and off wins from 120K: −6% / −4% at 120K, −15% / −10% at 160K,
−22% / −14% at 240K, −24% / −16% at 256K (M4 / M4 Pro). The cut-off goes from H ≤ 2^17
(FFT 256K, set from one test alone) to H ≤ 49152 (FFT 96K), the same on both machines. One
test alone is ~11% slower without the table at 240K; primecrunch runs a test on every core.

**Generated four-step twiddles (dropped).** Building each group's twiddles in a per-thread
scratch from two small tables (w^{(8g+j)k_r} = hi[g][p] · lo[p][j]) removes the 2 MB table at
256K. One core: +6% (727 → 770 µs per squaring at 256K), and the max error rises 0.039 → 0.047
(two rounded factors). 10 tests on the M4: 1.55 → 1.61 ms with the normaliser table off, 2.87 →
2.91 at 432K. The extra complex multiply per element costs more than the traffic it saves even
on E-cores. Patch: `docs/experiments/t4-generated-twiddles.patch`.

**Two-chain compressed normaliser (dropped).** A version of the no-table normaliser in the
table path's scaled form (carry chain fmla + frintn instead of fadd + fmul + frintn), two row
pairs per iteration. Bit-identical output (same max error, same residues). One core: no gain
(still 12% behind the table), so that path is limited by its 18 vector ops per digit, not by the
chain. 10 tests on the M4: 1.522–1.532 against 1.533–1.571 ms, under the 3% bar. Patch:
`docs/experiments/norm-two-chain-compressed.patch`.

**FFT length choice (kept).** Client machines have no `~/.kite/fft_times.txt`, so the length
comes from `cost_model`, which charged 3.5%, 5.9% and 8.2% per factor of 3, 5 and 7. Fitted to
`tune/fft_times_m4pro.txt` they cost about 2%, 4% and 5%; every length there with both 3 and 7
that ran 2.4–4× slow is divisible by 21, so those are excluded. With the refit, 2009·2^3322280−1
moves from 256K to 240K (one core 710 → 654 µs, max error 0.039 → 0.156) and 6073·2^2290815−1
first to 172800 (slower, 512 µs against 503, max error 0.28: at the edge of the safe-bpw model).
So a heuristic choice now also needs 0.25 bits per word below the safe limit when any length in
the range has it; that gives 184320 (498 µs, max error 0.09). 400K for 4813·2^5500155−1, PRST's
length, is unsafe for kite: max error 0.375 in 2000 squarings. PRST's FFT carries less round-off
at the same bits per word; closing that is an accuracy project, not a tuning one.

Version 0.1.9.

## 2026-10-18 08:30 — 0.1.9 against PRST at production sizes, and round-off room everywhere

`tools/load-bench.sh --ladder` again, 0.1.9 against PRST 14.0.1888, 60 s per run
(`docs/results/load-bench-2026-10-18-m4-0.1.9.md`, `-m4pro-0.1.9.md`). wolf went into
Maintenance Sleep every 15–20 minutes under `caffeinate -i` overnight, so the runs use
`caffeinate -dims`; eagle had `dasd` on ~1.1 cores throughout, so its absolute figures may read
a few percent slow, while each row's kite/PRST pair ran under the same load. Tests per hour with
every core busy:

| Case | kite FFT | M4 0.1.8 → 0.1.9 | vs PRST | M4 Pro 0.1.8 → 0.1.9 | vs PRST |
|---|---|--:|--:|--:|--:|
| 1021·2^2300003−1 | 160K | 14.4 → 16.8 (+17%) | 0.78× → 0.92× | 25.7 → 29.7 (+16%) | 0.93× → 1.00× |
| 6073·2^2290815−1 | 192K → 180K | 11.3 → 14.2 (+26%) | 0.71× → 0.91× | 20.5 → 23.7 (+16%) | 0.87× → 0.97× |
| 2009·2^3322280−1 | 256K → 240K | 5.4 → 7.5 (+39%) | 0.65× → 0.90× | 9.6 → 12.1 (+26%) | 0.81× → 0.95× |
| 6767·2^3322174−1 | 256K | 5.4 → 6.9 (+28%) | 0.66× → 0.85× | 9.5 → 11.3 (+19%) | 0.84× → 0.91× |
| 288K–432K | unchanged | same within noise | 0.81–0.90× | same | 0.88–0.95× |
| ladder 120K/128K | 120K | 25.8 → 29.9 (+16%) | 0.78× → 0.90× | 48.3 → 50.1 | 1.03× → 0.99× |

With only the P-cores busy kite is level or ahead on the M4 Pro (1.01–1.11×) and 2–8% behind on
the M4. Alone it is 4–10% behind from 160K up: the price of the compressed normaliser, which
primecrunch's one-test-per-core load more than repays. It is not yet ahead of PRST at n ≥ 2.3M
with every core busy; PRST stays the engine for those machines.

The eagle ladder exposed the round-off room: with no tune file, 2007·2^425985−1 moved from
28672 to 30720 and lost 12% (655 → 579 tests per hour). 28672 is 0.04 bits per word inside
`kite_safe_bpw`, and its max error reaches 0.31 in 20000 squarings (30720: 0.08), so a full
test would likely cross 0.4 and lose time to Gerbicz retries. The room now applies with
measured times as well (wolf's `~/.kite/fft_times.txt` had chosen 28672 too; it now takes 32768,
65.9 µs and max error 0.02 against 60.8 µs).

What is left at production sizes, in order of size:
- the four-step twiddle table (2 MB at 256K): generating it cost more than it saved (above);
  an in-kernel version that skips the scratch store and reload is the remaining variant;
- the compressed normaliser's 18 ops per digit against the table's 7 (alone: 5–11%);
- FFT accuracy: PRST runs 400K where kite needs 432K (max error 0.375 at 400K in 2000
  squarings), 8% more work at the largest size.

Blog material: the same table that made kite 1.25× faster than PRST at FFT 48K made it 1.5×
slower at 256K, and only with every core busy. A benchmark of one test alone could not see it.

## 2026-10-18 09:20 — Three areas left at production sizes

`docs/next-investigations.md` writes up what is left between 0.1.9 and PRST with every core
busy: FFT round-off (kite needs 432K where PRST runs 400K), the compressed normaliser's op count
(5–11% alone), and the four-step twiddle table. Each has a bound to measure before any code.

One new measurement for the round-off section: every max error seen is a short dyadic fraction
(0.375 = 3/8, 0.3125 = 5/16, 0.15625 = 5/32, ...), so the value being rounded has an ulp of 1/32
to 1/128. The error is set by how close the convolution outputs come to 2^53, not by noise
through the stages. The tangent-form twiddles are not the cause: `KITE_NO_TAN=1` gives the same
0.375 at 400K for 4813·2^5500155−1. The first question for that area is what gwnum's own max
round-off is at the same length and bits per word, which separates a calibration difference
from an accuracy one.

## 2026-10-18 10:05 — Pattern records: the table normaliser's speed in 26 KB

Bounds first, at production sizes with every core busy (`kite throughput`, two rounds):

- **Four-step twiddle traffic.** A wrong build pointing every pass-1 group at group 0's twiddle
  slice (same FP work, ~16 KB of twiddles instead of 2–3.4 MB) gains 3.6–6.2% at 10 tests on
  the M4 (240K–432K) and 0–10.6% at 14 on the M4 Pro (round-to-round spread ~10% there).
  Generating twiddles costs ~2 vector ops per element against ~60 for the rest of a squaring
  (the 6% measured earlier), so this area nets 0–4% at best.
- **Phase profile, M4, 256K, 1 → 10 tests** (µs per squaring): p1 inverse 133 → 302 (2.27×),
  normalise 181 → 297 (1.64×), p1 forward 124 → 231, p2 rows in 42 → 112 (2.69×), p2 forward
  110 → 205, split 71 → 117 (1.64×), p2 inverse 94 → 196, rows out 37 → 78; total 796 → 1549.
  The FP-bound phases slow 1.64× (clocks and E-cores); the excess elsewhere, ~240 µs, is memory.
- **Software prefetch** of the next pass-1 group's rows and twiddles: no change at 10 tests
  (1.59–1.62 against 1.59–1.60 ms), 2% slower alone. Dropped.

The profile also showed that alone, at 256K, the compressed normaliser takes 190 µs against
the table's 101: most of kite's 10% single-test gap to PRST.

The table's record for a row pair, {IA·IBs[i]·2^e, 2^−(b+big), A·B[i]·2^−e·base} per digit and
lane, depends on the rows only through A, IA and the two rows' masks. Divided by IA and A, it
depends only on the masks, and there are at most 34 distinct (mask[r], mask[r+1]) pairs at
FFT 32K–432K (8 at 64K, 34 at 160K and up): the digit sizes follow the Sturmian pattern of
ceil(j·T'/L), so a window of 16 has few distinct shapes. So: 34 records of 768 bytes (26 KB),
a 4-byte record index per row pair, and a kernel (`gen_norm_pat`) that multiplies osc by IA and
bw by A. Those are power-of-two scalings of the table's products, so the digits are bit for bit
the table path's (same residues, same max error).

256K, one core: table 102 µs, patterns 113, compressed 190; a squaring 0.725, 0.727 and
0.800 ms. On the M4 at 10 tests (6767·2^3322174−1): 1.526/1.549 ms (0.1.9) → 1.429/1.425
(−7%). At 45K–90K, patterns against the table on the M4: 1–2% slower alone, 4–9% faster at 10
tests (0.235 → 0.225, 0.317 → 0.300, 0.514 → 0.470 ms), so the pattern path can replace the
table at every size.

## 2026-10-18 11:30 — Built-in FFT times; round-off is a policy question; 10×1 stays best

**Built-in FFT times.** Client machines have no `~/.kite/fft_times.txt`, so every length ran stage
order 0 and lengths came from the heuristic. One core, 256K–432K: order 3 (all radices ascending)
is 2–4% faster than 0 (6767·2^3322174−1: 719 → 689 µs); under load on the M4, ABBA, 10 tests:
−1.9% at 256K, −1.7% at 384K. `kite tune 98304 655360` on the M4 (174 lengths, order 3 best at
86 of them) plus the M4 Pro's table below 98304 is now compiled in (`src/fft_times_default.h`,
`tools/gen_fft_times.sh`); with measurements in range only measured lengths are candidates (the
tune output skips lengths the NEON backend cannot shape). M4, 10 tests, ABBA against the
heuristic: −1.7% (6767, 256K, order), −5.7% (6073·2^2290815−1 at 179200 instead of 184320),
−2.1% (2007·2^638977−1 at 48K instead of 45K).

M4 with every core busy, 0.1.10 against PRST (`docs/results/load-bench-2026-10-18-m4-0.1.10.md`):
1.02× (160K), 1.06× (175K against 192K), 0.99× (240K), 0.96× (256K), 1.01× (288K), 0.97× (384K),
0.89× (432K against 400K). 0.1.9 was 0.81–0.92×.

**Round-off: gwnum is not more accurate, it runs closer to the edge.** The reference backend's
max error equals the NEON backend's at the same length and bits per word (32K: 0.25 both at
14.9 bpw; 64K: 0.375 both), so the NEON pipeline adds nothing; the k factor in `oscale` is the
method's (Percival), as in gwnum. gwnum's limits from `prst -fft info` (largest n at a length):
4813 at 400K 13.44 bpw (kite safe 13.10), 6767 at 256K ≥ 13.35 (13.05), 2007 at 240K 14.23
(13.95), 6767 at 240K 13.36 (13.08), 2007 at 28K 15.11 (14.90). That is kite's fitted
"max error 0.25 in a few hundred squarings" line, without kite's 0.2 margin: gwnum checks
round-off near its limits and recovers from the rare bad iteration. kite throws away the round
(up to 25% of the test) on an error over 0.40 and continues at a longer FFT, so it needs the
margin. Over 306 cases (9 values of k, n = 2.3M–5.6M) the squaring time with no 0.25 room would
be 0.979 of today's, with gwnum's limits 0.964. Running at the edge would need block-level retry
(redo one block at a longer length and come back) and a round-off check on every squaring
instead of every 4th (~1–2%), so the net is ~2%: not done.

**Splits.** M4, 256K: 10×1 7.7 tests/hour, 5×2 5.9. One thread per test stays best.

M4 Pro, every core busy, ABBA (`load-bench.sh --abba`: kite, PRST, PRST, kite, so the ~30%
clock drift under sustained load favours neither; `docs/results/load-bench-2026-10-18-m4pro-0.1.10-abba.md`):
1.13× (160K), 1.10× (175K against 192K), 1.01× (240K), 0.99× (256K), 1.03× (288K), 1.00× (384K),
0.94× (432K against 400K). On both machines kite is now level with or ahead of PRST at n ≤ 4.8M
with every core busy, and behind only where PRST runs a shorter length.

## 2026-10-18 12:35 — Pass-1 group size and the E-core L1: no effect

The sizes where kite trailed PRST most on the M4 with every core busy (256K 0.96×, 384K 0.97×,
432K 0.89×) are the ones whose pass-1 group is 32 KB of data plus 32 KB of twiddles (R = 256),
exactly an M4 E-core's 64 KB L1D; 240K and 288K (R = 192, 48 KB) were 0.99× and 1.01×. A cap on
the group size in `choose_shape` (measurement switch, not kept), ABBA, 10 tests:
- 16 KB groups: 256K 0.997 (128 × 1024), 384K 1.052, 432K 1.051 (worse: wider pass-2 rows);
- 24 KB groups: 384K 1.010, 432K 1.009.
The L2 hides the L1 overflow; the shape stays as it is.

## 2026-10-18 13:30 — CPU + GPU on a desktop M4: +13%

The 2026-10-12 laptop runs lost 35% with the GPU busy (macOS capped the P-cores at 1800 MHz and
the package at ~20 W). Repeated on eagle, an M4 desktop (4P + 6E, 10-core GPU), 9001·2^662695−1,
`kite throughput` for 1500 s, `gpu_prp` with 32 slots (the prototype's one length, 32768),
`powermetrics` every 10 s:

| Run | P MHz | CPU W | GPU W | Pressure | Tests/hour |
|---|--:|--:|--:|---|--:|
| 10 CPU tests | 3460 | 24.7 | 0.1 | Nominal | 223.8 |
| 9 CPU tests + GPU | 2912–2930 | 18.0–18.2 | 8.7–9.1 | Nominal | 183.2 + 70 = 253.2 |

The GPU batch was correct (all 32 slots RES64 0000000000000001, no Gerbicz errors, 1645 s,
51.4 s per test). E-cores stayed at 2808 MHz. The desktop holds ~27 W for the package against
24.7 W for the CPU alone, and gives the GPU ~9 W of it at a 16% P-core clock cost, steady for
the whole run rather than collapsing as on the laptop. Net +13% at this size. It says nothing
yet about production sizes: the prototype has one length, 16 times smaller than 256K.

10 CPU tests + GPU on the same desktop: P-cores 2700–2733 MHz, CPU 16.0–16.2 W, GPU 9.6–9.7 W,
Nominal. CPU 178.7 tests/hour (below 9 tests' 183.2: the tenth test only takes clock from the
others), GPU 67 (32 slots correct, 1716.5 s), total 245.7. Best is 9 CPU tests + GPU: 253.2
against 223.8 for the CPU alone. `gpu_prp` uses ~no CPU, but the power it takes comes out of the
P-cores' clocks either way.

## 2026-10-18 14:25 — Careful redo at the same FFT length, as gwnum does

How PRST and gwnum run at the edge, from `~/code/prst/framework` and gwnum 31.07's `gwnum.c`
(read for the method, not copied: gwnum's licence is the GIMPS one):
- PRST checks round-off on every operation only when the FFT is within 1% of its limit
  (`gwnear_fft_limit(gwdata, 1)` in `framework/task.cpp`); other tests run unchecked.
- `ReliableGWArithmetic::mul` (`framework/arithmetic/arithmetic.cpp`): an operation over 0.4 is
  repeated once (a hardware fault would not repeat), then redone with `gwmul3_carefully`; only
  if that is over too does the task restart with a larger FFT. An in-place square cannot be
  redone, so the task restarts from its last state with that operation marked for the careful
  path.
- `gwmul3_carefully` squares x as (x + r)(x − r) + r² for a fixed random r (r² cached): the
  same value from different data, so the round-off is a new draw.

kite now does the same in `kite_prp`:
- `kite_opts.edge`: lengths at `kite_fit_bpw` (no 0.2 margin, no 0.25 room); `kite_prp` sets
  it for an automatic length and `kite-prst` announces the same length. LLR, proof products,
  proof build and certificates keep the conservative choice (they have no recovery).
- Round-off checked on every squaring when n/L is past `kite_safe_bpw`, else every 4th.
  Checking every squaring costs 1.9% at 28K (66.9 against 65.6 µs per squaring).
- A squaring over 0.40 in a block is replayed from the last snapshot (every 32 squarings, so a
  replay is at most 32) and done as (x + r)(x − r) + r², with x ± r normalised by an exact
  digit carry (`kite_mul_small(x, 1)`) first; the block's d·x and the Gerbicz check's t·x0 as
  (t + r)·x − r·x. The tail (< B squarings, run twice) is checked the same way, since two
  identical computations would repeat a round-off error. Only a careful operation over the
  limit goes back to the verified state with a longer FFT.

Tests: forced lengths far too short for 9357·2^200001−1 (13824 → 12288 against 14336 chosen)
all end with the right residue via careful ops and FFT retries. At the edge, 2007·2^432901−1
(kite chose 28800; fit limit 432,920) ran with 12 careful ops, all accepted, no FFT retry,
residue equal to a 32K run (EEF2EAA30690DC6A); 2007·2^425985−1 at 28K: 0 careful ops,
residue equal to 32K. Lengths: 2007·2^425985−1 32K → 28K (gwnum: 28K), 6073·2^2290815−1
175K → 168K (gwnum: 192K); 240K, 256K and 432K unchanged (4813·2^5500155−1 needs 13.43 bits
per word at 400K, the fit allows 13.30, gwnum 13.44).

Production size: 6073·2^2290815−1 with `kite-prst` defaults (168K, was 175K; PRST 192K) gave the
known residue 38C439B2944CD35A in 1134 s; forced to 165,888 (0.29 bits per word past the fit) a
careful redo was over too, so it moved to 172,800 and gave the same residue (1246 s, 2 careful
ops, 1 FFT retry). M4, 10 tests, ABBA: 168K 0.923 ms per squaring against 175K 0.949 (17.0
against 16.6 tests/hour, +2.8% with every squaring checked); PRST 15.7, so 1.08×.

A guard: more than 32 careful redos in one round move the test up a length at the block
boundary, x, d and t carried over unverified (the next check covers them, as for an unverified
checkpoint). `--careful-limit 2` on 2007·2^432901−1 moved it from 28800 to 30720 after 5 careful
redos, residue correct. New ctest `edge_careful` runs both; suite 23/23.

## 2026-10-18 19:15 — 0.1.12: lengths past the fit, where careful redos stay cheap

With careful redos the right limit is where they stop being cheap, not "max error ≤ 0.25".
Complete tests at bpw = fit + δ with `--fft L`, careful redos counted, every residue compared
with a run at a comfortable length (`docs/results/roundoff-delta-sweep-2026-10-18.md`, 42
tests, all correct):

| L | k | last δ with no careful failure | first δ where a careful redo fails |
|---|---|--:|--:|
| 28K | 2007 | +0.05 | +0.10 |
| 48K | 6767 | +0.15 | +0.20 |
| 64K | 2007 / 6767 | +0.20 | +0.25 |
| 96K | 6119 | +0.15 | +0.20 |
| 128K | 4813 | +0.15 | +0.20 |

Near the limit the redos come evenly, not in a burst (64K +0.20: 33 between iterations 6198
and 164404, ~200 per million squarings, ~0.4% of the time), so the flat 32-per-round guard moved
tests up at a fraction of a percent. The guard is now cost-based: B·M/1100 + 8 redos per round
(~2% of the round at ~22 squarings each); a failed careful redo still moves up at once. Same
64K case: 162 careful redos, then one failed and it moved up; residue correct (5FA40A8A6F27F45A).

The deciding case, 4813·2^5500155−1 at 400K (+0.13): RES64 D6321DD51FA5FB38 (matches), no FFT
retry, 349 careful redos (0.14% of the test), 1179.5 µs per squaring on one M4 Pro core
against PRST's 1333 and kite's 1453 at 432K.

`kite_edge_bpw` = fit + δ(L), δ = 0.05 below 40K, 0.10 to 160K, 0.15 from 160K, a notch inside
the failures above. New lengths: 4813·2^5500155−1 432K → 400K (PRST: 400K), 999·2^4000000−1
288K → 270K (PRST: 288K; complete test on the M4 at 0.82–0.85 ms per squaring against PRST's
0.86 alone).

The suite caught one bug before the commit: a search-and-replace turned the new `allowed()`
helper's conservative branch into a call to itself, so every conservative length choice (proof
products, LLR, `kite time`) looped forever while the Fermat test's edge choice worked. The two
proof tests hung at 100% CPU until killed (SIGKILL: the loop never reads the stop flag). Fixed
before anything was committed; the suite now runs with `--timeout 1200`.
999·2^4000000−1 at 270K on the M4 with `kite-prst` defaults: RES64 BBAD057C725281B3 (matches), 3267 s. Version 0.1.12.

## 2026-10-18 20:10 — Three more of gwnum's techniques

From `gwnum.h` (31.07) and PRST's use of it.

**1. Careful first 30 squarings.** "At the start of a PRP exponentiation, the FFT data is
decidedly not random, leading to much larger than expected roundoff errors. In my own PRP code,
I call gwsquare_carefully for the first 30 iterations" (gwnum.h, `gwset_carefully_count`); PRST
does it (`src/exp.cpp`). kite starts from x_0 = 3^k, a few thousand bits, so its first squarings
are that case. `square_at` in `prp.c` squares the first 30 as (x+r)(x−r)+r², ~180 squarings'
time out of millions. Known residues unchanged; 2007·2^432901−1 now runs at 28672 with 15
careful redos.

**2. FFT implementation chosen with every core busy.** gwnum benchmarks implementations per
length and assumes "the user of your program will keep all cores fully occupied"
(`gwset_bench_cores`). kite's shape (R × C) came from a formula and its stage order from
single-core times. New `kite_opts.rows` / `--rows R` and a fourth tuning column. M4, 256K: alone
512 × 256 is 5.6% faster than the default 256 × 512 (0.691 against 0.732 ms per squaring), but
with 10 tests the default wins everywhere (6767·2^3322174−1: 1.39–1.41 against 1.40–1.42 ms; 240K
1.34 against 1.41–1.42; 400K 2.63 against 2.81; 1024 rows 18–69% slower), so shapes stay as they
are. Stage order under load, 10 tests, 0 1 2 3 3 2 1 0: at 240K order 0 1338.9, 1 1342.1, 2 1358.5,
3 1354.2 µs (the table's order 1 is 0.2% off); at 400K 0 2702.7, 1 2700.1, 2 2666.5, 3 2661.1
(the table's order 1 is 1.5% off). `kite tune --load N` now times the orders with N tests running;
an M4 run over 144K–460K gives the orders for the built-in table there, the single-core times
staying for the length choice (loaded times are in other units).

**3. Sum check.** gwnum checks that the sum of a multiplication's outputs matches the product of
its inputs' sums (`gw_test_illegal_sumout`), a hardware-error check per operation. kite's NEON
inverse transform is unnormalised by 2L, so Σ outputs = 2L·(Σ inputs)² (measured ratio
524288.0000000001 at 256K, 65536 at 32K; mismatch on correct squarings 1e-16 to 2e-16 of
Σ|outputs|; the check fails above 1e-9). Sums are taken per pass-1 group while it is in L1, with
NEON intrinsics (plain C does not vectorise: 20% on every squaring; intrinsics 5%). It runs on
every 64th squaring (~0.2%, within noise) and a mismatch raises the round-off reading past the
limit, so `kite_prp` redoes the squaring carefully, as PRST repeats a suspect operation. With the
check on every squaring an injected fault is caught at the next squaring (sum_errors 1,
gerbicz_errors 0, residue correct); at 1 in 64 Gerbicz catches it as before. `sum_errors` is
reported per test, which a client could use to flag unreliable hardware. New ctest `sum_check`.

Loaded order tune on the M4 (`kite tune 147456 471040 --load 10 --secs 2`, 108 lengths,
`tune/fft_orders_m4_load.txt`): fastest order 3 at 53, 2 at 21, 1 at 19, 0 at 15; it differs from
the single-core choice at 26 lengths. `tools/gen_fft_times.sh` now takes orders from it where
present and keeps the single-core times for the length choice. 400K moves to order 3 (−1.5%
under load, measured above). Known residues after the change: 9357·2^200001−1 and
6073·2^2290815−1 (168K, 1080 s on the M4, no careful redos, no sum errors).

## 2026-10-19 14:20 — P against E cores

Production results first: on wolf kite 0.1.13 is 16% (240K) and 10% (256K) faster than PRST over
whole tests (64 and 28 kite tests against 7 and 3 PRST); on eagle 2–6% faster at 240K and level at
256K (16 PRST tests from today). A scaling run on eagle (1/4/6/10 tests, 64K–256K,
`docs/results/scaling-pe-2026-10-19.md`) shows where the single-test lead goes: none of it on the
four P cores, all of it on the six E cores, whose added throughput is 7% (64K) to 19% (256K) lower
with kite. Rosetta's cost shows on the P cores; on the E cores the shared 4 MB L2 decides, and
kite's per-test footprint is larger (the full-size t4 twiddle table). Blog material: emulation
overhead is a CPU cost, and on a cache-starved core it stops being the thing that decides speed.

## 2026-10-19 17:30 — four-step twiddles built in the kernels

The full t4 twiddle table (16 bytes per element, read every squaring) roughly doubled a test's
cache footprint. New `t4g` kernels build row r's twiddle as hi[r] · lo[r][lane-group] with
by-element FMAs: lo is 20 KB at 240K and shared by every group, hi is 1/8 of the old table. With
every M4 core busy: −1.8% at 64K, crossover at 192K, +2.2% at 240K, +3.8% at 256K, +5.6% at 400K;
−1.5% on a single test at every size. Default: generated from FFT 192K. Against PRST at 10 tests,
240K goes 0.99× → 1.01× and 256K 0.96× → 0.985×. The earlier scratch-buffer version lost 2–4% for
the store and reload. Full 2009·2^3322280−1 matches its certificate residue (max error 0.164).
It is a small part of the E-core shortfall; the size-independent ~7% is untouched.
`docs/results/t4-generated-2026-10-19.md`.

## 2026-10-19 21:00 — E-core diagnosis: not misses, not pipes

Measured with the M4's own counters (`tools/pmc/`, `docs/results/e-core-diagnosis-2026-10-19.md`).
kite does 21% less vector arithmetic and 38% fewer vector loads per squaring than PRST, has fewer
L1 misses of every kind, and still needs as many cycles: its instructions issue at 1.86 per cycle
on an E core against PRST's 2.52, so the M4's E cores are not limited to the M1's two pipes.
Removing radix 8 (fewer live registers) costs 5–6%. The kernels that lose most on E cores are the
ones that start each pass. Blog material: an emulated engine doing 26% more work at higher IPC,
level with a native one doing less; instruction counts and cache misses both pointed the wrong way.

## 2026-10-19 22:30 — kite 0.1.15: register-offset addressing

Isolated kernels (`tools/kloop.c`) top out near 3–4 IPC on M4 E cores against 8–9 on P cores.
Removing the per-element address adds in pass 1 (15 of 17 per loop) gained only 0.4–0.6% at
10 tests: E cores are not instruction-count bound, and the vector `mov`s FMA butterflies need have
no cheaper form. Suite 24/24 (run on eagle). The E-core gap (~10% per E-core second) stays; closing
it would mean rescheduling the FFT kernels' vector code, not a table or addressing change.

## 2026-10-20 22:20 — E-core pipes, mul-form butterflies and early stores (investigation 7)

What an M4 / M4 Pro E core can issue, from `tools/pipeprobe.c` (register-only loops, quiet eagle,
the same on wolf): three FP pipes; fadd/fsub on all three, fmul and FMA on only two. FMA latency
4 cycles on E, about 3 on P. Vector `mov` costs nothing on either core type (12 FMAs + 6 movs run as
fast as the 12 FMAs alone), so the 21% `mov`s in the tangent kernels are not the E-core problem.
Stores: the E core writes 16 bytes a cycle, `str q` or `stp q` alike; P writes 32. Loads run
at two or more a cycle on both and do not compete with stores.

The tangent DIT kernels are almost all FMA (`dit_r8_tan_b`: 62 multiplies, 4 adds per butterfly), so
the two multiply pipes bound an E core at 31 cycles per radix-8 butterfly; it measures 38.
Mul-form butterflies (fmul + fadd/fsub in place of copy + two FMAs, moving work to the third pipe)
were slower on both core types: E +3–4%, P +9–23%. The longer chain (fmul then fadd, 7 cycles,
against one FMA, 4) costs more than the freed pipe gives. Unrolling the 4-iteration lane-group
loop: nothing.

Removing the stores (a debug build) took 12% off the kernel on E and 1.7% on P. The kernels
emitted all 16 stores at the end of each butterfly; storing each output as soon as it is made
(bit-identical results) gave, per call on L1-resident data: E −4 to −6.5% for the radix-8 tw and
tan kernels, P −1 to +1%. Over whole tests it did not show: 10 tests on eagle, ABBA, 90 s,
240K 1.3008 → 1.3004 ms/sq, 384K 2.0715 → 2.0682. The isolated kernel gain does not survive real
data sizes and full load. Suite 24/24 on the early-store build (eagle).

Blog material: a small loop-per-instruction-mix probe answers "is this a pipe limit?" in
seconds, and here it overturned the obvious explanation twice (movs, then multiply pipes).
Isolated-kernel wins still have to be confirmed at full load: a 5% per-call gain measured
nothing over whole tests.

## 2026-10-21 20:22 — Four-pair pattern normaliser (investigation 8): dropped

The pattern normaliser ran one or two row pairs per call (kernels `u`, `v`); the table path
already had four. A four-pair pattern kernel (`w`) needs all 32 vector registers: 16 digits, 4
carries, and each pair's A and IA (8), which the table path does not carry. It fits by rounding
into the osc register (dead after its multiply), taking the round-off error in the digit
register, and using each pair's digit-3 register as the fourth transpose temporary. The `u`/`v`
kernels came out byte-identical.

Correct: residues match the reference backend at 49K, 256K and a 400K-class length; maxerr
unchanged (0.1094 at 256K for two pairs, four pairs and HEAD); suite 24/24 on wolf and eagle.

Speed: one test alone on eagle, 256K, the normalise phase went from 113.2 to 108.9 µs per
squaring (−3.8%), the whole squaring −0.7% (normalise is 16% of it). 10 tests on eagle, ABBA,
90 s per run, ms per squaring against HEAD: 240K 1.2898 → 1.2870 (−0.2%), 256K 1.3464 → 1.3427
(−0.3%), 384K 2.0808 → 2.0873 (+0.3%). Inside the ~1% noise, so dropped under the 3% rule; the
code was not committed.

The 1.5% estimate assumed the normaliser stays limited by carry-chain latency under load, as
it is alone. The level result suggests it is limited by something else with every core busy,
most likely the row loads and stores competing for the shared L2 and memory; not measured.

Blog material: the same lesson as the early-store change the day before. A gain measured on one
core (here 3.8% of one phase) says little about a machine with every core busy, and the
phase's share of the whole (16%) caps the gain before anything else.

## 2026-10-21 21:05 — E cores: the multiply-pipe floor does not bind; tangent kernels stay

Hypothesis from the 2026-10-19 counters: kite issues 11.2M vector instructions per squaring at
1.86 per cycle on an E core (~6.0M cycles), and nearly all of the tangent DIT kernels' FP
instructions are multiplies or FMAs (`dit_r8_tan` 62 of 66, `dit_r4_rows_tan` 44 of 44), which
an E core issues on only two of its three FP pipes. Counted from the generated asm, the issue
floor per kernel body (E: max(mul/2, all/3); P: all/4) favours the plain forms on E and the
tangent forms on P: `dit_r8` 28.0 against `dit_r8_tan` 31.0 cycles on E, 21.0 against 16.5 on P;
`dit_r4_rows` 18.7 against `dit_r4_rows_tan` 22.0 on E, 14.0 against 11.0 on P. If E cores sat on
that floor, the plain kernels (`KITE_NO_TAN=1`) would gain there.

Measured on eagle, kite HEAD, ABBA, 90 s, ms per squaring, tangent → plain:
- 1 test: 240K 0.6650 → 0.6850 (+3.0%), 256K 0.6920 → 0.7120 (+2.9%)
- 10 tests: 240K 1.2873 → 1.3102 (+1.8%), 256K 1.3520 → 1.3755 (+1.7%)
- 6 tests on the E cores only (`taskpolicy -b`, 240K): 5.400 → 5.389 and 5.466; level within
  the noise (one tangent run read 7.03, a background-QoS clock change, left out)

So plain gains nothing on E cores and loses ~3% on P cores; the tangent kernels stay. kite on an E
core is not limited by its two multiply pipes in whole tests, which agrees with the mul-form
result (2026-10-20): moving work off the multiply pipes did not help either. Together with
misses, instruction count, register pressure and stores, the remaining suspects are latency
under the E core's smaller out-of-order window (the plain forms have longer dependent chains,
which would cancel their lower floor) and the memory system under load. Not settled.

Blog material: a pipe-count model built from real counters gave a clean, testable prediction
(plain kernels win on E cores) and one environment variable falsified it in half an hour. Cheap
falsification before writing a core-type dispatcher.
