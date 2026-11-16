// Linux platform: the movie decoder surface of the host (M1 has no video decoding).
// hle_movie.c implements the SDK's Decoder protocol itself: VP8 / VP9 with libvpx, Vorbis with stb_vorbis, H.264 through hle_movie_avc.h and AAC through
// AudioToolbox's AudioConverter. On Apple those two last decoders are VideoToolbox / CoreMedia (hle_movie_avc.c) and AudioToolbox; Android uses MediaCodec
// (dm/android/movie_avc.c, dm/android/compat/audioconverter_mediacodec.c). On ALARM neither exists in M1, so this file is the graceful stand-in: every H.264
// decode reports an error (the player gets no picture and skips the movie) and the AAC converter refuses to open (hle_movie.c already handles that and plays
// the track silent). Nothing here crashes or blocks; one [RED] line names the gap.
#include "dm.h"
#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include "hle_movie_avc.h"

// One line for the whole process: a movie may open several decoders (video and audio, one per stream).
static void movie_log(void) {
    static _Atomic int done;
    if (!atomic_exchange(&done, 1))
        LOG("[RED] movie decoding not implemented on ALARM yet: H.264 video and AAC audio are skipped (VP8 / VP9 / Vorbis still decode)");
}

// ---------- H.264 (hle_movie_avc.h): the host's VideoToolbox surface ----------
struct mavc { int rgba; };
mavc *mavc_new(void) { movie_log(); return calloc(1, sizeof(mavc)); }
void mavc_free(mavc *a) { free(a); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) { (void)a; (void)p; (void)n; (void)pts_us; return -1; }
int mavc_take(mavc *a, int flush, uint8_t *nv12, int w, int h, int64_t *pts_us) {
    (void)a; (void)flush; (void)nv12; (void)w; (void)h; if (pts_us) *pts_us = 0; return 0;
}
int mavc_queued(const mavc *a) { (void)a; return 0; }
void mavc_set_rgba(mavc *a) { if (a) a->rgba = 1; }
void mavc_drop(mavc *a) { (void)a; }
// mavc_held stays hle_movie.c's weak default (0: nothing is held in a decoder pipeline).

// ---------- AAC (AudioToolbox AudioConverter): refused, so hle_movie.c's md_aac_open reports silence ----------
OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) {
    (void)in; (void)out; if (c) *c = NULL; return kAudio_UnimplementedError;
}
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID id, UInt32 size, const void *data) {
    (void)c; (void)id; (void)size; (void)data; return kAudio_UnimplementedError;
}
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc proc, void *ref, UInt32 *packets, AudioBufferList *out, AudioStreamPacketDescription *desc) {
    (void)c; (void)proc; (void)ref; (void)out; (void)desc; if (packets) *packets = 0; return kAudio_UnimplementedError;
}
OSStatus AudioConverterDispose(AudioConverterRef c) { (void)c; return noErr; }
