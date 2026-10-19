// kloop KERNEL SECS: run one kernel in a loop on L1-resident data at the 240K shape (R = 160,
// C = 768) and print its calls with this process's instructions and cycles, in total and on P
// cores (proc_pid_rusage v6). Run several copies at once to load every core; the P/E split of
// the counters then gives the kernel's instructions per cycle on each core type.
#include <libproc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "kernels.h"

static void *amalloc(size_t n) {
    void *p = NULL;
    if (posix_memalign(&p, 128, n)) exit(1);
    for (size_t i = 0; i < n / 8; i++) ((double *)p)[i] = 1e-3 * (double)(i % 97) - 0.04;
    return p;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

enum { R = 160, C = 768 };

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: kloop KERNEL SECS\n");
        return 2;
    }
    const char *k = argv[1];
    double secs = atof(argv[2]);
    // pass 1: one 8-column group, R rows of 128 bytes (layout b, contiguous rows)
    double *grp = amalloc((size_t)R * 128), *hi = amalloc((size_t)R * 16), *lo = amalloc((size_t)R * 128);
    double *tw = amalloc((size_t)C * 64), *twt = amalloc((size_t)C * 64);
    // pass 2: two rows of C complex (layout b row slices) and the pair scratch (layout a)
    double *rp = amalloc((size_t)C * 16), *rpp = amalloc((size_t)C * 16), *scr = amalloc((size_t)C * 32);
    double *rs = amalloc((size_t)R * 16), *cs = amalloc((size_t)C * 16);
    uint64_t calls = 0;
    double t0 = now(), t;
    do {
        for (int i = 0; i < 64; i++) {
            if (!strcmp(k, "dit_r8_t4g_b")) kite_dit_r8_t4g_b(grp, 128, 4, R / 8, 8 * 128, hi, lo);
            else if (!strcmp(k, "dif_r8_t4g_b")) kite_dif_r8_t4g_b(grp, 128, 4, R / 8, 8 * 128, hi, lo);
            else if (!strcmp(k, "dif_r8_b")) kite_dif_r8_b(grp, 128, 4, 2, 64 * 128, 8, tw);
            else if (!strcmp(k, "dit_r8_tan_b")) kite_dit_r8_tan_b(grp, 128, 4, 2, 64 * 128, 8, twt);
            else if (!strcmp(k, "dif_r4_rows")) kite_dif_r4_rows(rp, rpp, scr, C, tw);
            else if (!strcmp(k, "dit_r4_rows_tan")) kite_dit_r4_rows_tan(rp, rpp, scr, C, twt);
            else if (!strcmp(k, "split_sq")) kite_split_sq(scr, C, rs, cs);
            else if (!strcmp(k, "dif_r8")) kite_dif_r8(scr, 32, 1, C / 64, 64 * 32, 8, tw);
            else if (!strcmp(k, "dit_r8_tan")) kite_dit_r8_tan(scr, 32, 1, C / 64, 64 * 32, 8, twt);
            else {
                fprintf(stderr, "unknown kernel %s\n", k);
                return 2;
            }
        }
        calls += 64;
        t = now();
    } while (t - t0 < secs);
    struct rusage_info_v6 r;
    proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r);
    printf("%s calls %llu secs %.3f inst %llu pinst %llu cyc %llu pcyc %llu\n", k,
           (unsigned long long)calls, t - t0, (unsigned long long)r.ri_instructions,
           (unsigned long long)r.ri_pinstructions, (unsigned long long)r.ri_cycles,
           (unsigned long long)r.ri_pcycles);
    return 0;
}
