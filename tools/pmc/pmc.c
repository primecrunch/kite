// pmc SECS EVENT...: system-wide counts per CPU of the named kpep events over SECS seconds
// (kperf/kperfdata private frameworks; needs root). Prints one line per CPU.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>

typedef struct kpep_db kpep_db;
typedef struct kpep_config kpep_config;
typedef struct kpep_event kpep_event;
#define KPC_CLASS_FIXED_MASK 1u
#define KPC_CLASS_CONFIGURABLE_MASK 2u
#define KPC_MAX_COUNTERS 32

static int (*kpc_set_counting)(uint32_t);
static int (*kpc_set_config)(uint32_t, uint64_t *);
static int (*kpc_get_cpu_counters)(int, uint32_t, int *, uint64_t *);
static int (*kpc_force_all_ctrs_set)(int);
static uint32_t (*kpc_get_counter_count)(uint32_t);
static int (*kpep_db_create)(const char *, kpep_db **);
static int (*kpep_config_create)(kpep_db *, kpep_config **);
static int (*kpep_config_force_counters)(kpep_config *);
static int (*kpep_db_event)(kpep_db *, const char *, kpep_event **);
static int (*kpep_config_add_event)(kpep_config *, kpep_event **, uint32_t, uint32_t *);
static int (*kpep_config_kpc_classes)(kpep_config *, uint32_t *);
static int (*kpep_config_kpc_count)(kpep_config *, size_t *);
static int (*kpep_config_kpc_map)(kpep_config *, size_t *, size_t);
static int (*kpep_config_kpc)(kpep_config *, uint64_t *, size_t);

#define LOAD(h, f) do { *(void **)&f = dlsym(h, #f); if (!f) { fprintf(stderr, "no %s\n", #f); return 1; } } while (0)

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: pmc SECS EVENT...\n"); return 2; }
    void *kp = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY);
    void *kd = dlopen("/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata", RTLD_LAZY);
    if (!kp || !kd) { fprintf(stderr, "dlopen failed\n"); return 1; }
    LOAD(kp, kpc_set_counting); LOAD(kp, kpc_set_config); LOAD(kp, kpc_get_cpu_counters);
    LOAD(kp, kpc_force_all_ctrs_set); LOAD(kp, kpc_get_counter_count);
    LOAD(kd, kpep_db_create); LOAD(kd, kpep_config_create); LOAD(kd, kpep_config_force_counters);
    LOAD(kd, kpep_db_event); LOAD(kd, kpep_config_add_event); LOAD(kd, kpep_config_kpc_classes);
    LOAD(kd, kpep_config_kpc_count); LOAD(kd, kpep_config_kpc_map); LOAD(kd, kpep_config_kpc);
    kpep_db *db; kpep_config *cfg;
    if (kpep_db_create(NULL, &db) || kpep_config_create(db, &cfg) || kpep_config_force_counters(cfg)) {
        fprintf(stderr, "kpep setup failed\n"); return 1;
    }
    int nev = argc - 2;
    for (int i = 0; i < nev; i++) {
        kpep_event *ev;
        uint32_t err = 0;
        if (kpep_db_event(db, argv[i + 2], &ev) || kpep_config_add_event(cfg, &ev, 0, &err)) {
            fprintf(stderr, "event %s: not available (%u)\n", argv[i + 2], err); return 1;
        }
    }
    uint32_t classes = 0; size_t regc = 0, map[KPC_MAX_COUNTERS] = {0}; uint64_t regs[KPC_MAX_COUNTERS] = {0};
    kpep_config_kpc_classes(cfg, &classes);
    kpep_config_kpc_count(cfg, &regc);
    kpep_config_kpc_map(cfg, map, sizeof map);
    kpep_config_kpc(cfg, regs, sizeof regs);
    if (kpc_force_all_ctrs_set(1)) { fprintf(stderr, "kpc_force_all_ctrs_set failed (root?)\n"); return 1; }
    if ((classes & KPC_CLASS_CONFIGURABLE_MASK) && regc && kpc_set_config(classes, regs)) {
        fprintf(stderr, "kpc_set_config failed\n"); return 1;
    }
    if (kpc_set_counting(classes | KPC_CLASS_FIXED_MASK)) { fprintf(stderr, "kpc_set_counting failed\n"); return 1; }
    int ncpu = 0; size_t sz = sizeof ncpu; sysctlbyname("hw.ncpu", &ncpu, &sz, NULL, 0);
    uint32_t nctr = kpc_get_counter_count(classes | KPC_CLASS_FIXED_MASK);
    uint32_t nfix = kpc_get_counter_count(KPC_CLASS_FIXED_MASK);  // fixed counters come first
    uint64_t *a = calloc((size_t)ncpu * nctr, 8), *b = calloc((size_t)ncpu * nctr, 8);
    int cur;
    kpc_get_cpu_counters(1, classes | KPC_CLASS_FIXED_MASK, &cur, a);
    sleep(atoi(argv[1]));
    kpc_get_cpu_counters(1, classes | KPC_CLASS_FIXED_MASK, &cur, b);
    kpc_set_counting(0);
    kpc_force_all_ctrs_set(0);
    printf("cpu cycles instructions");
    for (int i = 0; i < nev; i++) printf(" %s", argv[i + 2]);
    printf("\n");
    for (int c = 0; c < ncpu; c++) {
        uint64_t *x = a + (size_t)c * nctr, *y = b + (size_t)c * nctr;
        printf("%d %llu %llu", c, (unsigned long long)(y[0] - x[0]), (unsigned long long)(y[1] - x[1]));
        for (int i = 0; i < nev; i++) {
            size_t k = nfix + map[i];
            printf(" %llu", (unsigned long long)(y[k] - x[k]));
        }
        printf("\n");
    }
    return 0;
}
