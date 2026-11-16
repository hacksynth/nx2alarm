// Linux platform: the performance hints of dm/android/perf_android.c, as no-ops (M1).
// Android drives APerformanceHint (ADPF), the Adreno GPU turbo property and an adaptive frame rate from the present thread. Linux has no ADPF and no KGSL
// property API; the compositor already picks the display rate and the CPU/GPU governors are the system's. The functions the host calls stay, so the NVN
// replay and the frame pacing compile unchanged: they return the same values the host's own weak defaults would, and one log line says so once.
#include "dm.h"
#include <stdint.h>
#include <stdlib.h>

static void perf_log(void) {
    static int done;
    if (done) return;
    done = 1;
    LOG("[perf] ALARM: Android performance hints (ADPF, GPU turbo, adaptive frame rate) are not available: presents follow the display, clocks are the system's");
    const char *e = getenv("DM_PIN_REPLAY");
    if (e && *e && strcmp(e, "0")) LOG("[perf] DM_PIN_REPLAY is ignored on ALARM: the replay thread is left to the scheduler");
}

void dm_perf_game_mode(int mode) { (void)mode; perf_log(); }
void dm_perf_gpu_release(void) { perf_log(); }
void dm_perf_replay_thread(void) { perf_log(); }
void dm_perf_frame_cost(int64_t replay_ns, int64_t gpu_ns) { (void)replay_ns; (void)gpu_ns; perf_log(); }
uint64_t dm_present_period_adapt(uint64_t period) { perf_log(); return period; }   // the game's own period, as hle_nvn.c's weak default
void dm_perf_frame_start(uint64_t pace_ns, uint64_t pq_ns) { (void)pace_ns; (void)pq_ns; }
void dm_perf_frame(double period) { (void)period; perf_log(); }
// M2's Vulkan present calls this with the surface's window; no window frame-rate request exists on Linux (the compositor owns it).
void dm_perf_window(void *window, double fps) { (void)window; (void)fps; perf_log(); }
void dm_perf_present(double iv_ms) { (void)iv_ms; }
