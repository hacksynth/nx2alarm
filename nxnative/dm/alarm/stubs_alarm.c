// Weak platform stubs for the native Linux host core (nx2alarm-host).
// Every function here is a __attribute__((weak)) default implementation of a symbol the upstream host core references but that
// belongs to a platform layer: the real implementations live in dm/alarm/platform/ (audio, movie, saves, LAN, haptics, perf,
// assets), in the SDL3 front end (front_*) or, later, in the Vulkan backend. A strong definition anywhere in the link replaces
// these, so the host links and starts before those layers exist.
//
// M1 scope: the host binary has no window, no input and no game directory, and main.c dies with the usage message before any
// of these run. The defaults are the "no front end attached" behavior described in dm/host/front.h: there is no CAMetalLayer,
// no screen rate, and a software-keyboard request reports "cancelled" instead of blocking a thread forever.
#include "front.h"
#include <stddef.h>

// Front end (Mac: dm/host/front_mac.m; iOS: dm/ios; Linux: dm/alarm/front_alarm.c for the shell binary, not part of this link).
// No window: run the guest directly on the calling thread, as front.h documents for a window-less front end.
__attribute__((weak)) void front_run(void (*guest)(void)) { guest(); }
// No screen: an unknown refresh rate (0) and no rate switching.
__attribute__((weak)) int front_screen_max_fps(void) { return 0; }
__attribute__((weak)) void front_screen_match(int fps) { (void)fps; }
// The CAMetalLayer of a window (NULL: nothing to present to). The null graphics backend does not present, so nothing calls it
// today; the weak definition keeps a front-end-free host linkable if a backend starts asking for it.
__attribute__((weak)) void *front_metal_layer(void) { return NULL; }
// Software keyboard: with no window nothing is shown and -1 is returned (front.h). out is left as the caller gave it.
__attribute__((weak)) int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    return -1;
}
