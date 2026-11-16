// ELF replacement for dm/host/hle_table.c: the libc passthrough table (C library functions consistent with AAPCS64 and with no
// struct layout dependency, connected directly to the host's libc). The upstream file writes the entries into the Mach-O section
// "__DATA,swhle", which GNU as cannot assemble; on ELF the same entries go into the "swhle" section, bounded by the linker's
// __start_swhle / __stop_swhle symbols and looked up by hle.c / getsectiondata (dm/android/compat/darwin_compat.c).
// The list must stay identical to hle_table.c: a function added there and missing here falls back to the named unimplemented stub.
#include "dm.h"
#include <math.h>
#include <ctype.h>
#include <time.h>

typedef struct { const char *name; void *fn; } hle_ent;
#define L(n) __attribute__((used, DM_SECTION("swhle"))) static const hle_ent hle_lib_##n = { #n, (void *)n };

L(memcpy) L(memset) L(memmove) L(memcmp) L(memchr) L(strlen) L(strnlen) L(strcmp) L(strncmp)
L(strcpy) L(strncpy) L(strcat) L(strncat) L(strchr) L(strrchr) L(strstr) L(strspn) L(strcspn)
L(strpbrk) L(strcasecmp) L(strncasecmp) L(atoi) L(atof) L(strtol) L(strtoul) L(strtoll) L(strtoull)
L(strtod) L(strtof) L(sqrtf) L(cbrtf) L(expf) L(acosf) L(cosf) L(sinf) L(atan2f) L(fmodf) L(cos)
L(sin) L(powf) L(atanf) L(tanf) L(asinf) L(log10) L(pow) L(logf) L(ldexpf) L(log10f) L(log)
L(ldexp) L(frexp) L(atan2) L(modff) L(exp2f) L(exp2) L(log2f) L(log2) L(lroundf)
L(mktime) L(timegm) L(gmtime_r) L(localtime_r) L(strftime)
