// nx2alarm Linux port: SDL3 window and VkSurfaceKHR for the Vulkan backend (see gfx_vk_alarm.h and GFX_VK_PORT.md).
//
// What it replaces from the Android port:
//   - dm_front_window() + ANativeWindow_getWidth/Height (dm/android/front_android.c:59, gfx_vk_present.c:169): the SDL window
//     and SDL_GetWindowSizeInPixels.
//   - VkAndroidSurfaceCreateInfoKHR + vkCreateAndroidSurfaceKHR (gfx_vk_present.c:172): SDL_Vulkan_CreateSurface.
//   - VK_KHR_android_surface in the instance (gfx_vk_core.c:161): the extensions SDL_Vulkan_GetInstanceExtensions() asks for.
//   - perf_android.c dm_perf_window()'s request for a display rate: the Linux compositor owns the mode; logged and left alone.
//   - overlay_theme.c's APK asset reads (dm_overlay_font/dm_overlay_theme): the built-in Latin-1 font, no theme.
//   - perf_android.c's dm_perf_frame/dm_perf_frame_cost: weak no-ops (dm/alarm/platform/perf_alarm.c may provide strong ones).
//
// Threading: the front end calls vk_alarm_set_window()/vk_alarm_window_lost() on its main thread; the backend calls
// dm_front_window() and the size accessors from its present thread, and vk_alarm_create_surface() from the present thread
// under g_present_mu (gfx_vk_present.c). SDL window queries are used read-only from there; SDL's window size and flags are
// stable enough for the backend's "size changed -> remake the swapchain" check.
#include "gfx_vk.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <string.h>
#include "overlay_layout.h"
#include "pixel_font_latin1.h"

static SDL_Window *g_win;
static int g_vk_loaded;   // SDL_Vulkan_LoadLibrary() succeeded (refcounted by SDL; never unloaded: the devices outlive us)

// gfx_vk_present.c: the backend drops its swapchain and surface for a window that is going away (main thread, g_present_mu).
void vk_window_lost(void);

// ---------------- window ----------------
void vk_alarm_set_window(SDL_Window *w) {
    if (w == g_win) return;
    if (g_win) vk_window_lost();   // the old window's swapchain and surface go before its SDL window does
    g_win = w;
    if (w) {
        if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_VULKAN)) {
            static int said;
            if (!said++) VKLOG("[RED] the SDL window was not created with SDL_WINDOW_VULKAN: SDL_Vulkan_CreateSurface will fail (the shell must make a Vulkan window for this backend)");
        }
        if (!g_vk_loaded) { if (!SDL_Vulkan_LoadLibrary(NULL)) VKLOG("[RED] SDL_Vulkan_LoadLibrary: %s", SDL_GetError()); else g_vk_loaded = 1; }
    }
}
SDL_Window *vk_alarm_window(void) { return g_win; }
void vk_alarm_window_lost(void) { vk_window_lost(); }

// 0: no window, answered nothing, minimized or hidden: the backend makes no swapchain and shows nothing until it is back
static int win_pixels(int *w, int *h) {
    if (!g_win || (SDL_GetWindowFlags(g_win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))) return 0;
    int ww = 0, hh = 0;
    if (!SDL_GetWindowSizeInPixels(g_win, &ww, &hh) || ww <= 0 || hh <= 0) return 0;
    if (w) *w = ww;
    if (h) *h = hh;
    return 1;
}
int vk_alarm_window_pixels(int *w, int *h) { return win_pixels(w, h); }
// dm/android/gfx_vk_present.c's window plumbing (compat/android/native_window.h)
int32_t ANativeWindow_getWidth(ANativeWindow *w) { int ww = 0; return w && win_pixels(&ww, NULL) ? ww : 0; }
int32_t ANativeWindow_getHeight(ANativeWindow *w) { int hh = 0; return w && win_pixels(NULL, &hh) ? hh : 0; }
ANativeWindow *dm_front_window(void) { return g_win; }   // (gfx_vk_present.c has a weak NULL default for the Android build)

int vk_alarm_refresh_hz(void) {
    SDL_DisplayID d = g_win ? SDL_GetDisplayForWindow(g_win) : SDL_GetPrimaryDisplay();
    if (!d) return 0;
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(d);
    if (!m || m->refresh_rate <= 0) return 0;
    return (int)lroundf(m->refresh_rate);
}

void vk_alarm_toggle_fullscreen(void) {
    if (!g_win) return;
    int on = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!SDL_SetWindowFullscreen(g_win, !on)) VKLOG("[RED] fullscreen %s: %s", on ? "off" : "on", SDL_GetError());
}

// ---------------- Vulkan instance and surface ----------------
int vk_alarm_instance_exts(const char **names, int max) {
    if (max <= 0) return 0;
    if (!g_vk_loaded) {
        if (!SDL_Vulkan_LoadLibrary(NULL)) {
            static int said;
            if (!said++) VKLOG("[RED] SDL_Vulkan_LoadLibrary: %s (only VK_KHR_surface will be asked for: no window surface)", SDL_GetError());
            return 0;
        }
        g_vk_loaded = 1;
    }
    Uint32 n = 0;
    const char *const *e = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!e) {
        static int said;
        if (!said++) VKLOG("[RED] SDL_Vulkan_GetInstanceExtensions: %s (is the SDL video subsystem initialized? only VK_KHR_surface will be asked for)", SDL_GetError());
        return 0;
    }
    int k = 0;
    for (Uint32 i = 0; i < n && k < max; i++) {
        if (!e[i] || !*e[i]) continue;
        int dup = 0;
        for (int j = 0; j < k; j++) if (!strcmp(names[j], e[i])) dup = 1;
        if (!dup) names[k++] = e[i];
    }
    return k;
}

int vk_alarm_create_surface(VkInstance instance, VkSurfaceKHR *surface) {
    if (!g_win) return -1;
    if (!(SDL_GetWindowFlags(g_win) & SDL_WINDOW_VULKAN)) {
        static int said;
        if (!said++) VKLOG("[RED] no Vulkan surface: the SDL window was not created with SDL_WINDOW_VULKAN");
        return -1;
    }
    if (!g_vk_loaded) {
        if (!SDL_Vulkan_LoadLibrary(NULL)) {
            static int said;
            if (!said++) VKLOG("[RED] no Vulkan surface: SDL_Vulkan_LoadLibrary: %s", SDL_GetError());
            return -1;
        }
        g_vk_loaded = 1;
    }
    if (!SDL_Vulkan_CreateSurface(g_win, instance, NULL, surface)) {
        static int said;
        if (!said++) VKLOG("[RED] SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return -1;
    }
    return 0;
}

// ---------------- the Android layer's other hooks this backend calls ----------------
// perf_android.c asked the window for a display rate (an Android SurfaceFlinger/DisplayManager path, perf_android.c:370). Linux
// compositors pick the mode themselves, and SDL has no portable way to request one: log it once per change and do nothing.
// Weak, because dm/alarm/platform/perf_alarm.c provides the host binary's strong no-op (both may be linked in the M2 build).
__attribute__((weak)) void dm_perf_window(ANativeWindow *w, double fps) {
    (void)w;
    static double last = -1;
    if (last < 0 || fabs(fps - last) >= 1) { last = fps; VKLOG("display rate request %.1f Hz: the compositor picks the mode on Linux (no Android window rate API)", fps); }
}
// perf_android.c's frame pacing/ADPF bookkeeping: nothing to report on Linux (weak: dm/alarm/platform/perf_alarm.c may implement them)
__attribute__((weak)) void dm_perf_frame(double period) { (void)period; }
__attribute__((weak)) void dm_perf_frame_cost(int64_t replay_ns, int64_t gpu_ns) { (void)replay_ns; (void)gpu_ns; }

// overlay_theme.c reads the gamepad theme and the CJK font from the APK's assets (AAssetManager, Android-only). The Linux build
// has no APK assets, so the overlay draws the gamepad's procedural form (gfx_vk_present.c) and Latin-1 text with the built-in
// font. Weak, so a future Linux asset path can replace it without touching this file.
__attribute__((weak)) const uint8_t *dm_overlay_font(void) { return PF_LATIN1; }
__attribute__((weak)) const ov_theme *dm_overlay_theme(int layout) { (void)layout; return NULL; }
__attribute__((weak)) void dm_overlay_theme_done(const ov_theme *t) { (void)t; }
