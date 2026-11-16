// Linux platform: an AudioToolbox AudioUnit output unit on SDL3 (dm/android/compat/include/AudioToolbox/AudioToolbox.h).
// The Android port implements this unit on AAudio (dm/android/compat/audiotoolbox_aaudio.c); here SDL_OpenAudioDeviceStream plays the same pull model:
// SDL asks the unit's render callback for audio in its own format and the callback fills one AudioBuffer, so hle_audio.c's pull code is unchanged.
// A stop holds the stream (the callback plays silence and never enters the host); a start comes back out of the hold, and a stream that stopped calling
// back (a device that went away) is opened again, which is what hle_audio.c's watchdog expects. Device routing has no Linux counterpart: the default
// device follows the system mixer.
#include "dm.h"
#include <AudioToolbox/AudioToolbox.h>
#include <SDL3/SDL.h>
#include <sched.h>
#include <stdatomic.h>

struct dm_audio_component { int unused; };
struct dm_audio_unit {
    AudioStreamBasicDescription fmt; int have_fmt;
    AURenderCallbackStruct cb;
    int32_t frames_hint;             // kAudioDevicePropertyBufferFrameSize: the device buffer the host would like, a hint to SDL
    SDL_AudioStream *stream;         // the device stream of SDL_OpenAudioDeviceStream (NULL: not open)
    SDL_AudioDeviceID dev;
    _Atomic uint64_t sample_time;    // frames handed to the device so far
    uint64_t stop_time;              // sample_time at the unit's last stop: unchanged at the next start = the stream stopped calling back
    _Atomic int held, in_cb;         // held: stopped, the callback plays silence and never enters the host; in_cb: data callbacks running
    uint64_t held_frames;            // silence played since the stop
};
static struct dm_audio_component g_output;
static _Atomic int g_sdl_audio;      // 0 = not tried, 1 = SDL audio ready, -1 = SDL audio unavailable
static _Atomic int g_log_first;      // one line about the SDL output, not one per AudioOut

// SDL subsystems are reference counted: init once, never quit (the process owns it for good).
static int audio_ready(void) {
    int st = atomic_load(&g_sdl_audio);
    if (st) return st > 0;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { atomic_store(&g_sdl_audio, -1);   // SDL3 returns true on success
        LOG("[RED] audio: SDL3 audio is not available (%s): every output uses the silent sink", SDL_GetError()); return 0; }
    atomic_store(&g_sdl_audio, 1);
    return 1;
}

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d && d->componentType == kAudioUnitType_Output ? &g_output : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    if (c != &g_output || !out) return kAudio_UnimplementedError;
    *out = calloc(1, sizeof **out); return *out ? noErr : -1;
}
OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size) {
    (void)scope; (void)el;
    if (!u) return -50;
    switch (id) {
    case kAudioUnitProperty_StreamFormat:
        if (size < sizeof u->fmt || !data) return -50;
        memcpy(&u->fmt, data, sizeof u->fmt);
        if (u->fmt.mFormatID != kAudioFormatLinearPCM || (u->fmt.mFormatFlags & kAudioFormatFlagIsNonInterleaved))
            return kAudioUnitErr_FormatNotSupported;
        if (!(u->fmt.mFormatFlags & kAudioFormatFlagIsFloat) && u->fmt.mBitsPerChannel != 16)
            return kAudioUnitErr_FormatNotSupported;
        if (!u->fmt.mChannelsPerFrame || !u->fmt.mBytesPerFrame || u->fmt.mSampleRate <= 0)
            return kAudioUnitErr_FormatNotSupported;
        u->have_fmt = 1; return noErr;
    case kAudioUnitProperty_SetRenderCallback:
        if (size < sizeof u->cb || !data) return -50;
        memcpy(&u->cb, data, sizeof u->cb); return noErr;
    case kAudioDevicePropertyBufferFrameSize:
        if (size >= 4 && data) u->frames_hint = (int32_t)*(const UInt32 *)data;
        return noErr;
    default: return noErr;
    }
}
OSStatus AudioUnitInitialize(AudioUnit u) { return u && u->have_fmt && u->cb.inputProc ? noErr : -50; }

// One feed: the device asks for `additional` bytes in the unit's own format. The render callback fills one AudioBuffer over a stack chunk; a callback
// that fails (or a held unit) gives silence, as the AAudio port does.
enum { FEED_CAP = 16384 };
static void feed(AudioUnit u, SDL_AudioStream *stream, int additional) {
    int mbf = (int)u->fmt.mBytesPerFrame;
    while (additional > 0) {
        int bytes = additional < FEED_CAP ? additional : FEED_CAP;
        bytes -= bytes % mbf;
        if (bytes <= 0) bytes = mbf;
        int frames = bytes / mbf;
        uint8_t buf[FEED_CAP];
        if (atomic_load(&u->held)) {
            memset(buf, 0, (size_t)bytes);
            atomic_fetch_add(&u->sample_time, (uint64_t)frames);
            u->held_frames += (uint64_t)frames;
        } else {
            AudioBufferList bl = {1, {{u->fmt.mChannelsPerFrame, (UInt32)bytes, buf}}};
            AudioUnitRenderActionFlags fl = 0;
            struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
            AudioTimeStamp ts = {.mSampleTime = (Float64)atomic_fetch_add(&u->sample_time, (uint64_t)frames),
                                 .mHostTime = (UInt64)t.tv_sec * 1000000000ull + (UInt64)t.tv_nsec};
            if (!u->cb.inputProc || u->cb.inputProc(u->cb.inputProcRefCon, &fl, &ts, 0, (UInt32)frames, &bl))
                memset(buf, 0, (size_t)bytes);
        }
        SDL_PutAudioStreamData(stream, buf, bytes);
        additional -= bytes;
    }
}
static void SDLCALL on_audio(void *ud, SDL_AudioStream *stream, int additional, int total) {
    AudioUnit u = ud;
    (void)total;
    if (!u || !stream || additional <= 0) return;
    atomic_fetch_add(&u->in_cb, 1);
    feed(u, stream, additional);   // a held unit is served with silence (see feed)
    atomic_fetch_sub(&u->in_cb, 1);
}

static void close_stream(AudioUnit u) {
    if (u->stream) { SDL_DestroyAudioStream(u->stream); u->stream = NULL; u->dev = 0; }
}
OSStatus AudioOutputUnitStart(AudioUnit u) {
    if (!u || !u->have_fmt || !u->cb.inputProc) return -50;
    if (!audio_ready()) return -1;
    // A stop and start (the pause menu holds the game) comes back out of the hold on the same stream; a stream that stopped calling back while held,
    // or a paused device (the system moved the output), is opened again
    if (u->stream) {
        int callbacks = atomic_load(&u->held) ? atomic_load(&u->sample_time) != u->stop_time : 1;
        if (callbacks && !SDL_AudioDevicePaused(u->dev)) { atomic_store(&u->held, 0); return noErr; }
    }
    close_stream(u);
    atomic_store(&u->held, 0);
    u->held_frames = 0;
    SDL_AudioSpec spec = {
        .format = (u->fmt.mFormatFlags & kAudioFormatFlagIsFloat) ? SDL_AUDIO_F32 : SDL_AUDIO_S16,
        .channels = (int)u->fmt.mChannelsPerFrame,
        .freq = (int)u->fmt.mSampleRate,
    };
    // 5 ms callbacks unless the host asks otherwise (the AAudio port's frames hint): left to itself SDL buffers a whole device period
    int frames = u->frames_hint > 0 ? u->frames_hint : (int)(u->fmt.mSampleRate / 200);
    if (frames >= 64) {
        char v[16]; snprintf(v, sizeof v, "%d", frames);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, v);
    }
    u->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_audio, u);
    if (!u->stream) {
        LOG("[RED] audio: SDL3 could not open an output (%s): the silent sink stands in", SDL_GetError());
        return -1;
    }
    u->dev = SDL_GetAudioStreamDevice(u->stream);
    if (!SDL_ResumeAudioDevice(u->dev)) {
        LOG("[RED] audio: SDL3 could not start the output (%s): the silent sink stands in", SDL_GetError());
        close_stream(u);
        return -1;
    }
    if (!atomic_exchange(&g_log_first, 1))
        LOG("audio: SDL3 output %s (%d Hz, %d channels, %s)", SDL_GetAudioDeviceName(u->dev), (int)u->fmt.mSampleRate,
            (int)u->fmt.mChannelsPerFrame, (u->fmt.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "16-bit");
    return noErr;
}
// Stop holds the stream (silence, see feed): once it returns no callback is inside the host's render callback, and none enters it until the next start
// (held is set before in_cb is read, and each callback counts itself in before it reads it). SDL keeps the device running so its clock stays regular.
OSStatus AudioOutputUnitStop(AudioUnit u) {
    if (!u || !u->stream) return noErr;
    u->held_frames = 0; atomic_store(&u->held, 1);
    while (atomic_load(&u->in_cb)) sched_yield();
    u->stop_time = atomic_load(&u->sample_time);
    return noErr;
}
OSStatus AudioUnitUninitialize(AudioUnit u) { if (!u) return -50; close_stream(u); return noErr; }
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u) { if (u) { close_stream(u); free(u); } return noErr; }
