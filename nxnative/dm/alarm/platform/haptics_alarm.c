// Linux platform: game rumble through SDL3, for player 1 on the first attached gamepad (dm/android/haptics_android.c is the Android Vibrator port).
// hle_haptics.c keeps the game's vibration state and hands each change to its sink on the game thread; the sink only copies the snapshot, and a worker
// thread issues SDL_RumbleGamepad from it, renewing the effect while the game holds a motor (SDL's rumble lasts the given milliseconds). No gamepad, or one
// without rumble: nothing is available, so IsVibrationPermitted answers false and the game does not offer vibration.
#include "dm.h"
#include "hle_haptics.h"
#include <SDL3/SDL.h>
#include <pthread.h>

#define TICK_MS 50        // the worker's period: effect renewals, the state change check and gamepad hotplug
#define RUMBLE_MS 150     // a little more than one tick: the next tick renews it while the game keeps the motor running
#define MOVE 400          // a new rumble call only when the strength moved this much (of 65535)

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static dm_haptic_state g_state;      // the latest snapshot from hle_haptics.c (read by the worker)
static int g_started, g_run = 1, g_enabled = 1;
static SDL_Gamepad *g_pad;
static Uint16 g_last_l, g_last_h;    // the strengths last sent, and when to renew the effect
static uint64_t g_renew_at;

static float amp(float x) { return x > 1 ? 1 : x > 0 ? x : 0; }   // hle_haptics.c already clamps, this is belt and braces
static void sink(dm_haptic_state s) {   // under hle_haptics.c's lock: hand the state over, no hardware call here
    if (s.player != 0) return;          // the Linux backend rumbles for player 1 only
    pthread_mutex_lock(&g_mu);
    if (s.epoch != g_state.epoch) { g_last_l = g_last_h = 0; g_renew_at = 0; }   // a new epoch drops everything older at once
    g_state = s;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
// Open the first gamepad that answers with rumble support (a zero-strength rumble is the probe SDL offers).
static void open_pad(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && !g_pad; i++) {
        SDL_Gamepad *p = SDL_OpenGamepad(ids[i]);
        if (!p) continue;
        if (SDL_RumbleGamepad(p, 0, 0, 0)) {
            g_pad = p;
            LOG("[haptics] gamepad %s: rumble on player 1", SDL_GetGamepadName(p) ? SDL_GetGamepadName(p) : "(unnamed)");
            dm_haptics_route(0, 1);
        } else {
            LOG("[haptics] gamepad %s has no rumble: vibration stays unavailable", SDL_GetGamepadName(p) ? SDL_GetGamepadName(p) : "(unnamed)");
            SDL_CloseGamepad(p);
        }
    }
    if (ids) SDL_free(ids);
}
static void close_pad(void) {
    if (!g_pad) return;
    SDL_RumbleGamepad(g_pad, 0, 0, 0);
    SDL_CloseGamepad(g_pad);
    g_pad = NULL;
    g_last_l = g_last_h = 0;
    dm_haptics_route(0, 0);
}
static void *worker(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "haptics");
    open_pad();
    if (!g_pad) LOG("[haptics] no rumble-capable gamepad: rumble is a no-op");
    int last_second = -1;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += TICK_MS * 1000000L;
        while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        if (g_run) pthread_cond_timedwait(&g_cv, &g_mu, &dl);
        int run = g_run, on = g_enabled;
        dm_haptic_state s = g_state;
        pthread_mutex_unlock(&g_mu);
        if (!run) break;
        SDL_PumpEvents();   // gamepad hotplug and a disconnected pad
        if (g_pad && !SDL_GamepadConnected(g_pad)) close_pad();
        if (!g_pad) {       // look for a gamepad once a second while there is none
            int sec = (int)(SDL_GetTicks() / 1000);
            if (sec != last_second) { last_second = sec; open_pad(); }
        }
        // A standard rumble pad: the left (low-frequency) motor follows motor 0, the right (high-frequency) one motor 1
        Uint16 l = (Uint16)(amp(s.motor[0].amplitude_low) * 65535.0f), h = (Uint16)(amp(s.motor[1].amplitude_high) * 65535.0f);
        if (!g_pad || !on || (!l && !h)) {
            if (g_pad && (g_last_l || g_last_h)) { SDL_RumbleGamepad(g_pad, 0, 0, 0); g_last_l = g_last_h = 0; }
            g_renew_at = 0;
            continue;
        }
        uint64_t t = SDL_GetTicks();
        int moved = (l > g_last_l ? l - g_last_l : g_last_l - l) > MOVE || (h > g_last_h ? h - g_last_h : g_last_h - h) > MOVE;
        if (t >= g_renew_at || moved) {
            if (!SDL_RumbleGamepad(g_pad, l, h, RUMBLE_MS)) { close_pad(); continue; }
            g_last_l = l; g_last_h = h;
            g_renew_at = t + TICK_MS;
        }
    }
    return NULL;
}

// Called by the front end once the host starts: registers the sink and starts the worker.
void dm_haptics_alarm_start(int enabled) {
    if (g_started) return;
    g_started = 1; g_enabled = enabled != 0; g_run = 1;
    dm_haptics_init(sink, g_enabled);
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {   // SDL3 returns true on success
        LOG("[RED] [haptics] SDL3 gamepad input is not available (%s): rumble is a no-op", SDL_GetError());
        g_run = 0;
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL)) { LOG("[RED] [haptics] cannot start the rumble thread"); g_run = 0; return; }
    pthread_detach(t);
}
void dm_haptics_alarm_set(int on) {
    pthread_mutex_lock(&g_mu);
    g_enabled = on != 0;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
void dm_haptics_alarm_stop(void) {
    if (!g_started) return;
    pthread_mutex_lock(&g_mu);
    g_run = 0;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    g_started = 0;
}
