// Linux platform: the filesystem-backed replacement for Android's AssetManager (APK assets).
// dm/android/overlay_theme.c and front_android.c load the on-screen gamepad's theme and font through AAssetManager; on Linux the same files are read from a
// data directory: $DM_ALARM_DATA when set, else <executable dir>/assets (the build dir, where a packaged data folder sits beside the binary), else
// ./assets. Every call returns NULL when the directory or the file is missing, which the callers already treat as "drawn without a theme".
#pragma once
#include <stddef.h>
#include <sys/types.h>

typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
enum { AASSET_MODE_UNKNOWN = 0, AASSET_MODE_BUFFER = 1, AASSET_MODE_STREAMING = 2, AASSET_MODE_RANDOM = 3 };   // as android/asset_manager.h

// The process-wide manager (NULL when no data directory exists); the Linux counterpart of dm_front_assets() in front_android.c
AAssetManager *dm_front_assets(void);
AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode);
off_t AAsset_getLength(AAsset *asset);
off_t AAsset_getRemainingLength(AAsset *asset);
int AAsset_read(AAsset *asset, void *buf, size_t count);
void AAsset_close(AAsset *asset);
