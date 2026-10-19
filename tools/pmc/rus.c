// rus PID...: per-process instructions, cycles and user time, split into P cores and the rest.
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <mach/mach_time.h>
int main(int argc, char **argv) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    for (int i = 1; i < argc; i++) {
        struct rusage_info_v6 r;
        if (proc_pid_rusage(atoi(argv[i]), RUSAGE_INFO_V6, (rusage_info_t *)&r)) { perror("rusage"); continue; }
        double ut = (double)r.ri_user_time * tb.numer / tb.denom / 1e9;
        double put = (double)r.ri_user_ptime * tb.numer / tb.denom / 1e9;
        printf("%s inst %llu pinst %llu cyc %llu pcyc %llu utime %.3f putime %.3f stime %.3f faults %llu\n", argv[i],
               (unsigned long long)r.ri_instructions, (unsigned long long)r.ri_pinstructions,
               (unsigned long long)r.ri_cycles, (unsigned long long)r.ri_pcycles, ut, put, (double)r.ri_system_time * tb.numer / tb.denom / 1e9, (unsigned long long)r.ri_pageins);
    }
    return 0;
}
