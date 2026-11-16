// main() of nx2alarm-host.
// The upstream host core's entry point is dm/host/main.c, which build_host.py compiles with -Dmain=dm_host_main;
// this wrapper gives the process its real entry point. Keeping it here means the Linux port needs no edit of the
// shared host code (the iOS front end calls dm_host_preload_start from its own main and keeps main.c as it is).
//
// `nx2alarm-host --tsd-offset` is the port pipeline's query: the guest thread slots live in one __thread variable
// of this executable (loader.c, DM_ALARM), and patch_nso.py (DM_TARGET=alarm) needs its offset from tpidr_el0 to
// rewrite guest code for it. It prints the decimal offset and exits without loading a game; port_alarm.py runs it
// right after building this binary, before the rewrite.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern __thread void *dm_alarm_tsd_block;   // loader.c (DM_ALARM): the per-thread guest slot block pointer
int dm_host_main(int argc, char **argv);

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--tsd-offset")) {
        uint64_t tp;
        __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
        printf("%td\n", (char *)&dm_alarm_tsd_block - (char *)(uintptr_t)tp);
        return 0;
    }
    return dm_host_main(argc, argv);
}
