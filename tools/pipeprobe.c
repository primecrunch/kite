// pipeprobe VARIANT ITERS: run one short instruction mix in a loop and print cycles per iteration
// on P and on E cores (proc_pid_rusage v6). Run under `taskpolicy -b` to keep it on the E cores.
// Answers whether a vector `mov` costs an FP pipe slot or latency on each core type.
#include <libproc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define FMA12                                                                                   \
    "fmla v0.2d, v16.2d, v17.2d\n fmla v1.2d, v16.2d, v17.2d\n fmla v2.2d, v16.2d, v17.2d\n"    \
    "fmla v3.2d, v16.2d, v17.2d\n fmla v4.2d, v16.2d, v17.2d\n fmla v5.2d, v16.2d, v17.2d\n"    \
    "fmla v6.2d, v16.2d, v17.2d\n fmla v7.2d, v16.2d, v17.2d\n fmla v8.2d, v16.2d, v17.2d\n"    \
    "fmla v9.2d, v16.2d, v17.2d\n fmla v10.2d, v16.2d, v17.2d\n fmla v11.2d, v16.2d, v17.2d\n"
#define MOV6                                                                                    \
    "mov v20.16b, v16.16b\n mov v21.16b, v17.16b\n mov v22.16b, v18.16b\n"                      \
    "mov v23.16b, v16.16b\n mov v24.16b, v17.16b\n mov v25.16b, v18.16b\n"
#define ADD6                                                                                    \
    "fadd v20.2d, v16.2d, v17.2d\n fadd v21.2d, v16.2d, v17.2d\n fadd v22.2d, v16.2d, v17.2d\n" \
    "fadd v23.2d, v16.2d, v17.2d\n fadd v24.2d, v16.2d, v17.2d\n fadd v25.2d, v16.2d, v17.2d\n"
// the tangent butterfly as kite emits it (mov, fmls, fmla) and without the copy (fmul, fadd, fsub),
// six independent instances per iteration, each reading loop-invariant inputs
#define BFLY_MOV(t, a, b)                                                                       \
    "mov v" #t ".16b, v" #a ".16b\n fmls v" #t ".2d, v" #b ".2d, v31.d[0]\n"                    \
    "fmla v" #a ".2d, v" #b ".2d, v31.d[0]\n"
#define BFLY_MUL(t, a, b)                                                                       \
    "fmul v" #t ".2d, v" #b ".2d, v31.d[0]\n fsub v" #t ".2d, v" #a ".2d, v" #t ".2d\n"         \
    "fadd v" #a ".2d, v" #a ".2d, v" #t ".2d\n"

#define OP12(op, src)                                                                           \
    op " v0.2d, v16.2d, " src "\n " op " v1.2d, v16.2d, " src "\n " op " v2.2d, v16.2d, " src "\n"   \
    op " v3.2d, v16.2d, " src "\n " op " v4.2d, v16.2d, " src "\n " op " v5.2d, v16.2d, " src "\n"   \
    op " v6.2d, v16.2d, " src "\n " op " v7.2d, v16.2d, " src "\n " op " v8.2d, v16.2d, " src "\n"   \
    op " v9.2d, v16.2d, " src "\n " op " v10.2d, v16.2d, " src "\n " op " v11.2d, v16.2d, " src "\n"
#define ADD4                                                                                    \
    "fadd v20.2d, v16.2d, v17.2d\n fadd v21.2d, v16.2d, v17.2d\n"                               \
    "fadd v22.2d, v16.2d, v17.2d\n fadd v23.2d, v16.2d, v17.2d\n"

#define LOOP(body)                                                                              \
    __asm__ volatile("1:\n" body "subs %0, %0, #1\n b.ne 1b\n"                                  \
                     : "+r"(n)                                                                  \
                     :                                                                          \
                     : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", \
                       "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21",    \
                       "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",    \
                       "cc")

static void counters(uint64_t *cyc, uint64_t *pcyc, uint64_t *ins, uint64_t *pins) {
    struct rusage_info_v6 r;
    proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r);
    *cyc = r.ri_cycles, *pcyc = r.ri_pcycles, *ins = r.ri_instructions, *pins = r.ri_pinstructions;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: pipeprobe fma12|fma12_mov6|fma12_add6|mov12|movchain|fmachain|"
                        "bfly_mov|bfly_mul ITERS\n");
        return 2;
    }
    const char *k = argv[1];
    uint64_t iters = strtoull(argv[2], NULL, 10), n = iters;
    static double buf[64];
    uint64_t c0, p0, i0, q0, c1, p1, i1, q1;
    counters(&c0, &p0, &i0, &q0);
    if (!strcmp(k, "fma12")) LOOP(FMA12);
    else if (!strcmp(k, "fma12_mov6")) LOOP(FMA12 MOV6);
    else if (!strcmp(k, "fma12_add6")) LOOP(FMA12 ADD6);
    else if (!strcmp(k, "mov12")) LOOP(MOV6 MOV6);
    else if (!strcmp(k, "add12")) LOOP(OP12("fadd", "v17.2d"));
    else if (!strcmp(k, "mul12")) LOOP(OP12("fmul", "v17.2d"));
    else if (!strcmp(k, "fmael12")) LOOP(OP12("fmla", "v17.d[0]"));
    else if (!strcmp(k, "fma12_add4")) LOOP(FMA12 ADD4);
    else if (!strcmp(k, "fma12_add12"))
        LOOP(FMA12 ADD6 "fadd v26.2d, v16.2d, v17.2d\n fadd v27.2d, v16.2d, v17.2d\n"
             "fadd v28.2d, v16.2d, v17.2d\n fadd v29.2d, v16.2d, v17.2d\n"
             "fadd v30.2d, v16.2d, v17.2d\n fadd v31.2d, v16.2d, v17.2d\n");
    else if (!strcmp(k, "add12_st4") || !strcmp(k, "add12_stp2") || !strcmp(k, "fma12_st4") ||
             !strcmp(k, "add12_st8") || !strcmp(k, "add12_ld4") || !strcmp(k, "add12_stp4") ||
             !strcmp(k, "add12_ld8") || !strcmp(k, "add12_ld8_st8")) {
        // stores of loop-invariant registers to a small buffer: does the store data take an FP pipe?
        if (!strcmp(k, "add12_st4"))
            __asm__ volatile("1:\n" OP12("fadd", "v17.2d") "str q20, [%1]\n str q21, [%1, #16]\n"
                             "str q22, [%1, #32]\n str q23, [%1, #48]\n subs %0, %0, #1\n b.ne 1b\n"
                             : "+r"(n) : "r"(buf) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
                               "v9", "v10", "v11", "cc", "memory");
        else if (!strcmp(k, "add12_st8"))
            __asm__ volatile("1:\n" OP12("fadd", "v17.2d") "str q20, [%1]\n str q21, [%1, #16]\n"
                             "str q22, [%1, #32]\n str q23, [%1, #48]\n str q20, [%1, #64]\n"
                             "str q21, [%1, #80]\n str q22, [%1, #96]\n str q23, [%1, #112]\n"
                             "subs %0, %0, #1\n b.ne 1b\n"
                             : "+r"(n) : "r"(buf) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
                               "v9", "v10", "v11", "cc", "memory");
        else if (!strcmp(k, "add12_stp4"))
            __asm__ volatile("1:\n" OP12("fadd", "v17.2d") "stp q20, q21, [%1]\n stp q22, q23, [%1, #32]\n"
                             "stp q20, q21, [%1, #64]\n stp q22, q23, [%1, #96]\n"
                             "subs %0, %0, #1\n b.ne 1b\n"
                             : "+r"(n) : "r"(buf) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
                               "v9", "v10", "v11", "cc", "memory");
        else if (!strcmp(k, "add12_ld8"))
            __asm__ volatile("1:\n" OP12("fadd", "v17.2d") "ldr q20, [%1]\n ldr q21, [%1, #16]\n"
                             "ldr q22, [%1, #32]\n ldr q23, [%1, #48]\n ldr q24, [%1, #64]\n"
                             "ldr q25, [%1, #80]\n ldr q26, [%1, #96]\n ldr q27, [%1, #112]\n"
                             "subs %0, %0, #1\n b.ne 1b\n"
                             : "+r"(n) : "r"(buf) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
                               "v9", "v10", "v11", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27",
                               "cc", "memory");
        else if (!strcmp(k, "add12_ld8_st8"))
            __asm__ volatile("1:\n" OP12("fadd", "v17.2d") "ldr q20, [%1]\n ldr q21, [%1, #16]\n"
                             "ldr q22, [%1, #32]\n ldr q23, [%1, #48]\n ldr q24, [%1, #64]\n"
                             "ldr q25, [%1, #80]\n ldr q26, [%1, #96]\n ldr q27, [%1, #112]\n"
                             "str q28, [%1, #128]\n str q29, [%1, #144]\n str q30, [%1, #160]\n"
                             "str q31, [%1, #176]\n str q28, [%1, #192]\n str q29, [%1, #208]\n"
                             "str q30, [%1, #224]\n str q31, [%1, #240]\n"
                             "subs %0, %0, #1\n b.ne 1b\n"
                             : "+r"(n) : "r"(buf) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
                               "v9", "v10", "v11", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27",
                               "cc", "memory");
        else if (!strcmp(k, "add12_stp2"))
            __asm__ volatile("1:\n" OP12("fadd", "v17.2d") "stp q20, q21, [%1]\n stp q22, q23, [%1, #32]\n"
                             "subs %0, %0, #1\n b.ne 1b\n"
                             : "+r"(n) : "r"(buf) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
                               "v9", "v10", "v11", "cc", "memory");
        else if (!strcmp(k, "add12_ld4"))
            __asm__ volatile("1:\n" OP12("fadd", "v17.2d") "ldr q20, [%1]\n ldr q21, [%1, #16]\n"
                             "ldr q22, [%1, #32]\n ldr q23, [%1, #48]\n subs %0, %0, #1\n b.ne 1b\n"
                             : "+r"(n) : "r"(buf) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
                               "v9", "v10", "v11", "v20", "v21", "v22", "v23", "cc", "memory");
        else
            __asm__ volatile("1:\n" FMA12 "str q20, [%1]\n str q21, [%1, #16]\n"
                             "str q22, [%1, #32]\n str q23, [%1, #48]\n subs %0, %0, #1\n b.ne 1b\n"
                             : "+r"(n) : "r"(buf) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
                               "v9", "v10", "v11", "cc", "memory");
    }
    else if (!strcmp(k, "fma12_ld4"))
        __asm__ volatile("1:\n" FMA12 "ldr q20, [%1]\n ldr q21, [%1, #16]\n ldr q22, [%1, #32]\n"
                         "ldr q23, [%1, #48]\n subs %0, %0, #1\n b.ne 1b\n"
                         : "+r"(n) : "r"(buf) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
                           "v9", "v10", "v11", "v20", "v21", "v22", "v23", "cc", "memory");
    else if (!strcmp(k, "movchain"))  // 12 dependent copies per iteration
        LOOP("mov v1.16b, v0.16b\n mov v2.16b, v1.16b\n mov v3.16b, v2.16b\n mov v4.16b, v3.16b\n"
             "mov v5.16b, v4.16b\n mov v6.16b, v5.16b\n mov v7.16b, v6.16b\n mov v8.16b, v7.16b\n"
             "mov v9.16b, v8.16b\n mov v10.16b, v9.16b\n mov v11.16b, v10.16b\n mov v0.16b, v11.16b\n");
    else if (!strcmp(k, "fmachain"))  // 12 dependent FMAs per iteration
        LOOP("fmla v0.2d, v16.2d, v17.2d\n fmla v0.2d, v16.2d, v17.2d\n fmla v0.2d, v16.2d, v17.2d\n"
             "fmla v0.2d, v16.2d, v17.2d\n fmla v0.2d, v16.2d, v17.2d\n fmla v0.2d, v16.2d, v17.2d\n"
             "fmla v0.2d, v16.2d, v17.2d\n fmla v0.2d, v16.2d, v17.2d\n fmla v0.2d, v16.2d, v17.2d\n"
             "fmla v0.2d, v16.2d, v17.2d\n fmla v0.2d, v16.2d, v17.2d\n fmla v0.2d, v16.2d, v17.2d\n");
    else if (!strcmp(k, "bfly_mov"))
        LOOP(BFLY_MOV(12, 0, 1) BFLY_MOV(13, 2, 3) BFLY_MOV(14, 4, 5)
             BFLY_MOV(15, 6, 7) BFLY_MOV(16, 8, 9) BFLY_MOV(17, 10, 11));
    else if (!strcmp(k, "bfly_mul"))
        LOOP(BFLY_MUL(12, 0, 1) BFLY_MUL(13, 2, 3) BFLY_MUL(14, 4, 5)
             BFLY_MUL(15, 6, 7) BFLY_MUL(16, 8, 9) BFLY_MUL(17, 10, 11));
    else {
        fprintf(stderr, "unknown variant %s\n", k);
        return 2;
    }
    counters(&c1, &p1, &i1, &q1);
    double pc = (double)(p1 - p0), ec = (double)((c1 - c0) - (p1 - p0));
    double pi = (double)(q1 - q0), ei = (double)((i1 - i0) - (q1 - q0));
    double pit = pc > 0 ? pi / ((i1 - i0) / (double)iters) : 0, eit = iters - pit;  // iterations by core type
    printf("%-11s P %6.2f cyc/it (%4.0f%% of it)  E %6.2f cyc/it (%4.0f%% of it)\n", k,
           pit > 0 ? pc / pit : 0, 100 * pit / iters, eit > 0 ? ec / eit : 0, 100 * eit / iters);
    (void)ei;
    return 0;
}
