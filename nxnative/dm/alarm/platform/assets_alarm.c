// Linux platform: filesystem-backed Android AssetManager (see assets_alarm.h).
// The assets are the files an APK would carry under assets/: overlay/theme.tsv, overlay/<layout>.png and font/ui.bin. $DM_ALARM_DATA names the directory
// they live in; unset, the directory beside the executable (build/alarm/assets) and then ./assets are tried. Nothing is an error here: a missing asset
// answers NULL and the caller draws without it.
#include "dm.h"
#include "assets_alarm.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct AAssetManager { char root[1200]; };
struct AAsset { FILE *f; off_t len; };

static struct AAssetManager g_mgr;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static AAssetManager *g_assets;   // NULL: no data directory

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static void resolve(void) {
    const char *e = getenv("DM_ALARM_DATA");
    if (e && *e && is_dir(e)) { snprintf(g_mgr.root, sizeof g_mgr.root, "%s", e); g_assets = &g_mgr; }
    else {
        char exe[512], dir[1200];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char *slash = strrchr(exe, '/'); if (slash) { *slash = 0; snprintf(dir, sizeof dir, "%s/assets", exe); }
            else snprintf(dir, sizeof dir, "assets");
            if (is_dir(dir)) { snprintf(g_mgr.root, sizeof g_mgr.root, "%s", dir); g_assets = &g_mgr; }
        }
        if (!g_assets && is_dir("assets")) { snprintf(g_mgr.root, sizeof g_mgr.root, "assets"); g_assets = &g_mgr; }
    }
    if (g_assets) LOG("[assets] reading assets from %s", g_mgr.root);
    else LOG("[assets] no asset directory ($DM_ALARM_DATA unset and no assets/ beside the executable): overlay themes and fonts are not loaded");
}
AAssetManager *dm_front_assets(void) { pthread_once(&g_once, resolve); return g_assets; }

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode) {
    (void)mode;
    if (!mgr || !filename || !*filename || strstr(filename, "..")) return NULL;
    char file[512], path[2400];
    snprintf(file, sizeof file, "%.*s", (int)sizeof file - 1, filename);
    snprintf(path, sizeof path, "%s/%s", mgr->root, file);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(path, sizeof path, "%s/assets/%s", mgr->root, file); f = fopen(path, "rb"); }
    if (!f) return NULL;
    AAsset *a = calloc(1, sizeof *a);
    if (!a) { fclose(f); return NULL; }
    a->f = f;
    if (fseeko(f, 0, SEEK_END) == 0) { a->len = ftello(f); if (fseeko(f, 0, SEEK_SET) != 0) { fclose(f); free(a); return NULL; } }
    else { a->len = 0; }
    return a;
}
off_t AAsset_getLength(AAsset *asset) { return asset ? asset->len : 0; }
off_t AAsset_getRemainingLength(AAsset *asset) {
    if (!asset) return 0;
    off_t p = ftello(asset->f);
    return p < 0 || p >= asset->len ? 0 : asset->len - p;
}
int AAsset_read(AAsset *asset, void *buf, size_t count) {
    if (!asset || !buf || !count) return 0;
    size_t n = fread(buf, 1, count, asset->f);
    return n > 0x7fffffff ? 0x7fffffff : (int)n;
}
void AAsset_close(AAsset *asset) { if (asset) { if (asset->f) fclose(asset->f); free(asset); } }
