// nx2alarm Linux shim: <android/log.h>.
//
// The Vulkan backend does not include it (it logs through dm.h's LOG, which writes to stderr); other Android-only files of
// the tree do (dm/android/front_android.c, dm/android/compat/audiotoolbox_aaudio.c). This shim exists so a file that is
// compile-checked on Linux and still includes <android/log.h> gets the same text on stderr instead of the Android log, with
// no Android runtime. Compile-only: no liblog is linked.
#pragma once
#include <stdarg.h>
#include <stdio.h>

typedef enum android_LogPriority {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
} android_LogPriority;

static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[dm] %s: ", tag ? tag : "?");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}
static inline int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    (void)prio;
    fprintf(stderr, "[dm] %s: ", tag ? tag : "?");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    return 0;
}
static inline int __android_log_write(int prio, const char *tag, const char *text) {
    return __android_log_print(prio, tag, "%s", text);
}
