# Per-core measurement tools (Apple Silicon)

Used for `docs/results/e-core-diagnosis-2026-10-19.md`. They expect a working directory
`/tmp/lb` on the machine under test holding `rus`, `pmc` and the engine binaries; copy them there
first. All tests run as single-thread Fermat tests of k·2^(n+2i)−1, one per directory, killed by
PID (never by pattern, so a crunch client is left alone).

| File | What it does | Root |
|---|---|---|
| `rus.c` | per-process instructions, cycles and user/system time, in total and on P cores (`proc_pid_rusage` v6) | no |
| `pmc.c` | system-wide counts per CPU of named kpep events over N seconds (kperf/kperfdata); on the M4, CPUs 0–5 are E cores, 6–9 P | yes |
| `scale.sh SECS "COUNTS" "K:N ..."` | ABBA per-test ms/squaring for two engines at each test count; `KITE_BIN` and `PRST_BIN` pick the two engines (any two wrapper scripts, e.g. one setting `KITE_T4=table`) | no |
| `pe.sh ENGINE K N COUNT` | `rus` P/E split between 25 s and 85 s of COUNT tests | no |
| `pmcrun.sh ENGINE K N` | `pmc` over 40 s of 10 tests (events in `EVENTS`), plus each test's ms/squaring | yes |
| `prof.sh SECS K N "COUNTS"` | `KITE_PROF=1` phase profile per test at each count (`KITE_BIN`) | no |
| `xtrecord.sh ENGINE TEMPLATE OUT` | starts an xctrace recording, then 10 tests, and records each test's load address with vmmap (`OUT.slides`) | yes |
| `xtsym.py TS.xml CC.xml SYMS PROC SLIDES` | joins xctrace time samples to core type and symbolises leaf PCs (`nm -n` output as SYMS) | no |
| `xtcols.py CC.xml PROC` | per-sample deltas of the raw counter columns (0 = cycles, 1 = instructions) | no |
| `../kloop.c` | one kernel in a loop on L1-resident data at the 240K shape; run 10 copies, then read the P/E split | no |

xctrace needs Xcode (`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`), its licence
accepted, and enough free disk: a 50 s CPU Counters recording is about 750 MB, and the exports
another 350 MB.
