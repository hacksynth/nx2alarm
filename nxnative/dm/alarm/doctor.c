// nx2alarm doctor: a device and prerequisite audit for the Arch Linux ARM port.
//
// It reads /proc and /sys for the board, CPU, RAM and dma-buf heaps, asks SDL3
// for the video/audio/display/gamepad state, and probes the Vulkan loader with
// dlopen() so the shell does not need -lvulkan or an installed ICD. Every check
// is PASS, WARN or FAIL: a missing Vulkan ICD, game data or audio device is a
// warning, a missing compiler or an unreadable CPU is a failure. Rows are also
// drawn by the shell window (front_alarm.c), so details stay on one line.
//
// The audit never exits or crashes on a missing device: every platform call is
// checked and a missing piece only changes a row's status.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "doctor.h"

#include <SDL3/SDL.h>

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __linux__
#include <link.h>
#endif

// ---------------------------------------------------------------- utilities

// Append printf output to a NUL-terminated buffer that is already in use.
static void str_append(char *buf, size_t cap, const char *fmt, ...) {
    size_t len = strlen(buf);
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
}

// Read a whole (small) file; returns false when it cannot be opened.
static bool read_file(const char *path, char *buf, size_t cap, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    if (out_len) *out_len = n;
    return true;
}

// Cut trailing NULs, newlines and spaces from a file's content in place.
static void trim_trailing(char *s, size_t *len) {
    while (*len > 0) {
        unsigned char c = (unsigned char)s[*len - 1];
        if (c == 0 || c == '\n' || c == '\r' || c == ' ') (*len)--;
        else break;
    }
    s[*len] = 0;
}

// Join the NUL-separated strings of /proc/device-tree/compatible with ", ".
static void join_nul_list(const char *raw, size_t raw_len, char *out, size_t cap) {
    out[0] = 0;
    size_t i = 0;
    while (i < raw_len) {
        size_t start = i;
        while (i < raw_len && raw[i] != 0) i++;
        if (i > start) {
            if (out[0]) str_append(out, cap, ", ");
            str_append(out, cap, "%.*s", (int)(i - start), raw + start);
        }
        i++;
    }
}

static void path_dirname(char *path) {
    char *slash = strrchr(path, '/');
    if (slash) *slash = 0;
}

// Concatenate a fixed suffix to a path without a format-truncation warning.
static void join_path(char *out, size_t cap, const char *base, const char *suffix) {
    snprintf(out, cap, "%s", base);
    str_append(out, cap, "%s", suffix);
}

// The repository root, found from the running binary: <repo>/build/alarm/nx2alarm.
static void repo_root(char *out, size_t cap) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) {
        snprintf(out, cap, ".");
        return;
    }
    self[n] = 0;
    for (int i = 0; i < 3; i++) path_dirname(self);
    snprintf(out, cap, "%s", self);
}

// Run a fixed command and keep its first output line; true only on exit 0.
static bool run_capture(const char *cmd, char *out, size_t cap) {
    out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    if (!fgets(out, (int)cap, p)) out[0] = 0;
    int rc = pclose(p);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = 0;
    return rc == 0 && out[0] != 0;
}

// Count files with a suffix in dir (names only); -1 when the directory is absent.
static int count_suffix_files(const char *dir, const char *suffix, char *first, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int count = 0;
    struct dirent *e;
    size_t slen = strlen(suffix);
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= slen) continue;
        if (strcmp(e->d_name + len - slen, suffix) != 0) continue;
        if (count == 0 && first) snprintf(first, cap, "%s", e->d_name);
        count++;
    }
    closedir(d);
    return count;
}

static void add_row(dm_report *rep, int section, dm_status status, const char *name, const char *fmt, ...) {
    if (rep->count >= DM_ALARM_MAX_ROWS) return;
    dm_row *r = &rep->rows[rep->count++];
    r->section = section;
    r->status = status;
    snprintf(r->name, sizeof r->name, "%s", name);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->detail, sizeof r->detail, fmt, ap);
    va_end(ap);
    switch (status) {
        case DM_ST_PASS: rep->n_pass++; break;
        case DM_ST_WARN: rep->n_warn++; break;
        case DM_ST_FAIL: rep->n_fail++; break;
        default: rep->n_info++; break;
    }
}

const char *dm_status_label(dm_status status) {
    switch (status) {
        case DM_ST_PASS: return "PASS";
        case DM_ST_WARN: return "WARN";
        case DM_ST_FAIL: return "FAIL";
        default: return "INFO";
    }
}

// ------------------------------------------------------------- device facts

static void board_rows(dm_report *rep) {
    char model[192];
    size_t len = 0;
    if (read_file("/proc/device-tree/model", model, sizeof model, &len)) {
        trim_trailing(model, &len);
        snprintf(rep->model, sizeof rep->model, "%s", model);
        add_row(rep, 0, DM_ST_PASS, "Board", "%s", model);
    } else {
        snprintf(rep->model, sizeof rep->model, "unknown");
        add_row(rep, 0, DM_ST_WARN, "Board", "not readable (/proc/device-tree/model missing)");
    }

    char raw[512], soc[320] = "";
    size_t raw_len = 0;
    if (read_file("/proc/device-tree/compatible", raw, sizeof raw, &raw_len)) {
        join_nul_list(raw, raw_len, soc, sizeof soc);
    }
    if (soc[0]) {
        if (strstr(soc, "sdm845")) str_append(soc, sizeof soc, " (Snapdragon 845/850 class)");
        snprintf(rep->soc, sizeof rep->soc, "%s", soc);
        add_row(rep, 0, DM_ST_PASS, "SoC", "%s", soc);
    } else {
        snprintf(rep->soc, sizeof rep->soc, "unknown");
        add_row(rep, 0, DM_ST_WARN, "SoC", "not readable (/proc/device-tree/compatible missing)");
    }
}

static void kernel_row(dm_report *rep) {
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(rep->kernel, sizeof rep->kernel, "%s %s", u.release, u.machine);
        add_row(rep, 0, DM_ST_PASS, "Kernel", "%s %s %s", u.sysname, u.release, u.machine);
    } else {
        snprintf(rep->kernel, sizeof rep->kernel, "unknown");
        add_row(rep, 0, DM_ST_FAIL, "Kernel", "uname() failed");
    }
}

// Core count, CPU parts (0x802 = A55-class, 0x803 = A75-class on SDM845/850)
// and the highest cpufreq max frequency. All straight from /proc and /sys.
static void cpu_row(dm_report *rep) {
    int cores = 0;
    struct { char part[12]; int count; } parts[4];
    int nparts = 0;
    char implementer[16] = "";
    bool readable = false;

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        readable = true;
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "processor", 9) && strchr(line, ':')) {
                cores++;
            } else if (!strncmp(line, "CPU implementer", 15)) {
                const char *colon = strchr(line, ':');
                if (colon && !implementer[0]) sscanf(colon + 1, "%15s", implementer);
            } else if (!strncmp(line, "CPU part", 8)) {
                const char *colon = strchr(line, ':');
                char part[12] = "";
                if (colon && sscanf(colon + 1, "%11s", part) == 1 && part[0]) {
                    int i = 0;
                    for (; i < nparts; i++) {
                        if (!strcmp(parts[i].part, part)) {
                            parts[i].count++;
                            break;
                        }
                    }
                    if (i == nparts && nparts < 4) {
                        snprintf(parts[nparts].part, sizeof parts[nparts].part, "%s", part);
                        parts[nparts].count = 1;
                        nparts++;
                    }
                }
            }
        }
        fclose(f);
    }

    char detail[192] = "";
    if (!readable || cores <= 0) {
        snprintf(rep->cpu, sizeof rep->cpu, "unknown");
        add_row(rep, 0, DM_ST_FAIL, "CPU", "unreadable (/proc/cpuinfo)");
        return;
    }
    str_append(detail, sizeof detail, "%d cores", cores);
    for (int i = 0; i < nparts; i++) {
        str_append(detail, sizeof detail, "%s%d x %s", i == 0 ? " (" : ", ", parts[i].count, parts[i].part);
    }
    if (nparts) str_append(detail, sizeof detail, ")");

    unsigned long max_khz = 0;
    for (int i = 0; i < 12; i++) {
        char path[128], buf[32];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        if (!read_file(path, buf, sizeof buf, NULL)) continue;
        unsigned long khz = strtoul(buf, NULL, 10);
        if (khz > max_khz) max_khz = khz;
    }
    if (max_khz) {
        str_append(detail, sizeof detail, "; up to %lu.%02lu GHz",
                   max_khz / 1000000UL, (max_khz % 1000000UL) / 10000UL);
    }
    if (implementer[0]) str_append(detail, sizeof detail, "; implementer %s", implementer);

    snprintf(rep->cpu, sizeof rep->cpu, "%s", detail);
    add_row(rep, 0, DM_ST_PASS, "CPU", "%s", detail);
}

static void ram_row(dm_report *rep) {
    unsigned long kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char key[64], unit[16];
        unsigned long value = 0;
        while (fscanf(f, "%63s %lu %15s", key, &value, unit) == 3) {
            if (!strcmp(key, "MemTotal:")) {
                kb = value;
                break;
            }
        }
        fclose(f);
    }
    if (!kb) {
        snprintf(rep->ram, sizeof rep->ram, "unknown");
        add_row(rep, 0, DM_ST_WARN, "RAM", "unreadable (/proc/meminfo)");
        return;
    }
    snprintf(rep->ram, sizeof rep->ram, "%lu MiB (%.1f GiB)", kb / 1024, (double)kb / 1048576.0);
    dm_status status = kb < 2UL * 1024 * 1024 ? DM_ST_WARN : DM_ST_PASS;
    add_row(rep, 0, status, "RAM", "%s total", rep->ram);
}

// ------------------------------------------------------------------- SDL3

static void sdl_version_strings(char *sdl, size_t sdl_cap, char *detail, size_t detail_cap) {
    int runtime = SDL_GetVersion();
    int compiled = SDL_VERSION;
    snprintf(sdl, sdl_cap, "%d.%d.%d", SDL_VERSIONNUM_MAJOR(runtime),
             SDL_VERSIONNUM_MINOR(runtime), SDL_VERSIONNUM_MICRO(runtime));
    snprintf(detail, detail_cap, "%d.%d.%d (runtime), compiled %d.%d.%d",
             SDL_VERSIONNUM_MAJOR(runtime), SDL_VERSIONNUM_MINOR(runtime), SDL_VERSIONNUM_MICRO(runtime),
             SDL_VERSIONNUM_MAJOR(compiled), SDL_VERSIONNUM_MINOR(compiled), SDL_VERSIONNUM_MICRO(compiled));
}

void dm_sdl_prepare_hints(void) {
    // Otherwise the Wayland driver loads libdecor-gtk for window decorations,
    // which pulls GTK/GIO into the process (on this device dconf then complains
    // about its read-only runtime dir on every check). GNOME decorates the
    // window through xdg-decoration instead. An explicit user setting wins.
    if (SDL_GetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR) == NULL) {
        SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0");
    }
}

// Video, display, audio and gamepad state. Only the subsystems this call
// initialized itself are quit again, so the shell can collect while running.
static void sdl_rows(dm_report *rep, bool with_sdl) {
    dm_sdl_prepare_hints();
    char sdl[64], detail[160];
    sdl_version_strings(sdl, sizeof sdl, detail, sizeof detail);
    snprintf(rep->sdl, sizeof rep->sdl, "%s", sdl);
    add_row(rep, 0, DM_ST_PASS, "SDL3", "%s", detail);

    if (!with_sdl) {
        snprintf(rep->display, sizeof rep->display, "not probed (headless)");
        add_row(rep, 0, DM_ST_INFO, "Video", "not probed (headless mode)");
        add_row(rep, 0, DM_ST_INFO, "Display", "not probed (headless mode)");
        add_row(rep, 0, DM_ST_INFO, "Audio", "not probed (headless mode)");
        add_row(rep, 0, DM_ST_INFO, "Gamepads", "not probed (headless mode)");
        return;
    }

    bool own_video = false;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) own_video = SDL_InitSubSystem(SDL_INIT_VIDEO);
    const char *video_driver = SDL_GetCurrentVideoDriver();
    if (video_driver) {
        add_row(rep, 0, DM_ST_PASS, "Video", "%s (%d driver(s) compiled in)",
                video_driver, SDL_GetNumVideoDrivers());
    } else {
        add_row(rep, 0, DM_ST_WARN, "Video", "unavailable: %s", SDL_GetError());
    }

    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *mode = display ? SDL_GetDesktopDisplayMode(display) : NULL;
    if (mode && mode->w > 0 && mode->h > 0) {
        snprintf(rep->display, sizeof rep->display, "%dx%d @ %.1f Hz", mode->w, mode->h,
                 (double)mode->refresh_rate);
        add_row(rep, 0, DM_ST_PASS, "Display", "%s (display %u)", rep->display, (unsigned)display);
    } else {
        snprintf(rep->display, sizeof rep->display, "none");
        add_row(rep, 0, DM_ST_WARN, "Display", "no desktop display mode: %s", SDL_GetError());
    }

    bool own_audio = false;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) own_audio = SDL_InitSubSystem(SDL_INIT_AUDIO);
    const char *audio_driver = SDL_GetCurrentAudioDriver();
    if (audio_driver) {
        const char *device = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        add_row(rep, 0, DM_ST_PASS, "Audio", "%s driver%s%s", audio_driver,
                device && device[0] ? ", default device: " : "", device && device[0] ? device : "");
    } else {
        add_row(rep, 0, DM_ST_WARN, "Audio", "no audio driver: %s", SDL_GetError());
    }

    bool own_pad = false;
    if (SDL_WasInit(SDL_INIT_GAMEPAD) == 0) own_pad = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (SDL_WasInit(SDL_INIT_GAMEPAD) != 0) {
        int count = 0;
        SDL_JoystickID *pads = SDL_GetGamepads(&count);
        SDL_free(pads);
        add_row(rep, 0, DM_ST_PASS, "Gamepads", "%d connected", count < 0 ? 0 : count);
    } else {
        add_row(rep, 0, DM_ST_WARN, "Gamepads", "gamepad subsystem unavailable: %s", SDL_GetError());
    }

    if (own_pad) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (own_audio) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (own_video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

// ----------------------------------------------------------------- Vulkan

// Minimal Vulkan declarations: doctor.c must not include the repository's
// Vulkan headers (it is also compiled by the host build), and it never links
// libvulkan, so only the flat structures the probe reads are spelled out here.
typedef int32_t dm_vk_result;
typedef struct dm_vk_instance_t *dm_vk_instance;
typedef struct dm_vk_physical_t *dm_vk_physical;

typedef struct {
    int32_t s_type;
    const void *next;
    const char *app_name;
    uint32_t app_version;
    const char *engine_name;
    uint32_t engine_version;
    uint32_t api_version;
} dm_vk_app_info;

typedef struct {
    int32_t s_type;
    const void *next;
    uint32_t flags;
    const dm_vk_app_info *app_info;
    uint32_t layer_count;
    const char *const *layers;
    uint32_t extension_count;
    const char *const *extensions;
} dm_vk_instance_info;

typedef struct {
    uint32_t api_version;
    uint32_t driver_version;
    uint32_t vendor_id;
    uint32_t device_id;
    int32_t device_type;
    char device_name[256];
    unsigned char rest[1024];
} dm_vk_phys_props;

#define DM_VK_SUCCESS 0
#define DM_VK_STRUCTURE_TYPE_APPLICATION_INFO 0
#define DM_VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define DM_VK_API_VERSION_1_0 ((uint32_t)1u << 22)

typedef void *(*dm_vk_gipa_fn)(void *instance, const char *name);
typedef dm_vk_result (*dm_vk_enum_version_fn)(uint32_t *version);
typedef dm_vk_result (*dm_vk_create_instance_fn)(const dm_vk_instance_info *info, const void *allocator,
                                                 dm_vk_instance *instance);
typedef dm_vk_result (*dm_vk_enum_phys_fn)(dm_vk_instance instance, uint32_t *count, dm_vk_physical *devices);
typedef void (*dm_vk_get_props_fn)(dm_vk_physical device, dm_vk_phys_props *props);
typedef void (*dm_vk_destroy_instance_fn)(dm_vk_instance instance, const void *allocator);

typedef struct {
    bool loaded;
    bool instance_created;
    uint32_t loader_version;    // 0 when the loader did not report one (Vulkan 1.0)
    dm_vk_result create_result;
    uint32_t device_count;
    char device_names[224];
    char loader_path[128];
    char load_error[160];
} dm_vk_probe;

static const char *vk_result_name(dm_vk_result result) {
    switch (result) {
        case 0: return "VK_SUCCESS";
        case -1: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case -2: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case -3: return "VK_ERROR_INITIALIZATION_FAILED";
        case -4: return "VK_ERROR_DEVICE_LOST";
        case -5: return "VK_ERROR_MEMORY_MAP_FAILED";
        case -6: return "VK_ERROR_LAYER_NOT_PRESENT";
        case -7: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case -8: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case -9: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case -10: return "VK_ERROR_TOO_MANY_OBJECTS";
        case -11: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        default: return "unknown result";
    }
}

static void vk_version_string(uint32_t version, char *out, size_t cap) {
    // Variant 0 is assumed; every loader version used here is variant 0.
    snprintf(out, cap, "%u.%u.%u", (version >> 22) & 0x7Fu, (version >> 12) & 0x3FFu, version & 0xFFFu);
}

static void probe_vulkan(dm_vk_probe *p) {
    memset(p, 0, sizeof *p);
    p->create_result = -1;
    snprintf(p->loader_path, sizeof p->loader_path, "libvulkan.so.1");

    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char *error = dlerror();
        snprintf(p->load_error, sizeof p->load_error, "%s", error ? error : "not found");
        return;
    }
    p->loaded = true;

#ifdef RTLD_DI_LINKMAP
    struct link_map *map = NULL;
    if (dlinfo(lib, RTLD_DI_LINKMAP, &map) == 0 && map && map->l_name && map->l_name[0]) {
        const char *slash = strrchr(map->l_name, '/');
        snprintf(p->loader_path, sizeof p->loader_path, "%s", slash ? slash + 1 : map->l_name);
    }
#endif

    dm_vk_gipa_fn gipa = (dm_vk_gipa_fn)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) {
        snprintf(p->load_error, sizeof p->load_error, "vkGetInstanceProcAddr missing");
        dlclose(lib);
        return;
    }

    dm_vk_enum_version_fn enum_version = (dm_vk_enum_version_fn)gipa(NULL, "vkEnumerateInstanceVersion");
    if (enum_version) enum_version(&p->loader_version);

    dm_vk_create_instance_fn create_instance =
        (dm_vk_create_instance_fn)gipa(NULL, "vkCreateInstance");
    if (!create_instance) {
        snprintf(p->load_error, sizeof p->load_error, "vkCreateInstance missing");
        dlclose(lib);
        return;
    }

    dm_vk_app_info app;
    memset(&app, 0, sizeof app);
    app.s_type = DM_VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.app_name = "nx2alarm";
    app.app_version = 1;
    app.engine_name = "nx2alarm";
    app.engine_version = 1;
    app.api_version = DM_VK_API_VERSION_1_0;

    dm_vk_instance_info create_info;
    memset(&create_info, 0, sizeof create_info);
    create_info.s_type = DM_VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.app_info = &app;

    dm_vk_instance instance = NULL;
    p->create_result = create_instance(&create_info, NULL, &instance);
    if (p->create_result != DM_VK_SUCCESS || !instance) {
        dlclose(lib);
        return;
    }
    p->instance_created = true;

    dm_vk_enum_phys_fn enum_phys =
        (dm_vk_enum_phys_fn)gipa(instance, "vkEnumeratePhysicalDevices");
    dm_vk_get_props_fn get_props =
        (dm_vk_get_props_fn)gipa(instance, "vkGetPhysicalDeviceProperties");
    dm_vk_destroy_instance_fn destroy_instance =
        (dm_vk_destroy_instance_fn)gipa(instance, "vkDestroyInstance");

    if (enum_phys) {
        uint32_t count = 0;
        if (enum_phys(instance, &count, NULL) == DM_VK_SUCCESS && count > 0) {
            dm_vk_physical devices[8];
            uint32_t want = count > 8 ? 8 : count;
            if (enum_phys(instance, &want, devices) == DM_VK_SUCCESS) {
                p->device_count = want;
                for (uint32_t i = 0; i < want; i++) {
                    dm_vk_phys_props props;
                    memset(&props, 0, sizeof props);
                    if (get_props) get_props(devices[i], &props);
                    str_append(p->device_names, sizeof p->device_names, "%s%s", i ? ", " : "",
                               props.device_name[0] ? props.device_name : "unnamed device");
                }
            }
        }
    }

    if (destroy_instance) destroy_instance(instance, NULL);
    dlclose(lib);
}

static void vulkan_rows(dm_report *rep) {
    dm_vk_probe probe;
    probe_vulkan(&probe);

    char loader_version[32] = "1.0.0";
    if (probe.loader_version) vk_version_string(probe.loader_version, loader_version, sizeof loader_version);

    if (!probe.loaded) {
        add_row(rep, 1, DM_ST_WARN, "Vulkan loader", "not installed: %s", probe.load_error);
    } else {
        add_row(rep, 1, DM_ST_PASS, "Vulkan loader", "%s (%s)", probe.loader_path, loader_version);
    }

    if (!probe.loaded) {
        add_row(rep, 1, DM_ST_WARN, "Vulkan device", "not probed (no loader)");
        snprintf(rep->vulkan, sizeof rep->vulkan, "not installed (no libvulkan.so.1)");
    } else if (!probe.instance_created) {
        add_row(rep, 1, DM_ST_WARN, "Vulkan device", "no instance: %d (%s)",
                (int)probe.create_result, vk_result_name(probe.create_result));
        snprintf(rep->vulkan, sizeof rep->vulkan, "loader %s; instance failed (%s)",
                 loader_version, vk_result_name(probe.create_result));
    } else if (probe.device_count == 0) {
        add_row(rep, 1, DM_ST_WARN, "Vulkan device", "0 physical devices (loader works; no ICD installed)");
        snprintf(rep->vulkan, sizeof rep->vulkan, "loader %s; 0 physical devices", loader_version);
    } else {
        add_row(rep, 1, DM_ST_PASS, "Vulkan device", "%u device(s): %s",
                probe.device_count, probe.device_names);
        snprintf(rep->vulkan, sizeof rep->vulkan, "loader %s; %u device(s): %s",
                 loader_version, probe.device_count, probe.device_names);
    }
}

// ICD manifests: the Vulkan loader reads one JSON file per installed driver.
static void icd_row(dm_report *rep) {
    static const char *dirs[] = {
        "/usr/share/vulkan/icd.d",
        "/etc/vulkan/icd.d",
        "/usr/local/share/vulkan/icd.d",
    };
    int total = 0;
    char found[288] = "";
    char absent[192] = "";

    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char first[96] = "";
        int n = count_suffix_files(dirs[i], ".json", first, sizeof first);
        if (n < 0) {
            if (absent[0]) str_append(absent, sizeof absent, ", ");
            str_append(absent, sizeof absent, "%s", dirs[i]);
        } else if (n > 0) {
            total += n;
            str_append(found, sizeof found, "%s%s/%s", found[0] ? ", " : "", dirs[i], first);
        }
    }

    const char *xdg = getenv("XDG_DATA_DIRS");
    if (xdg) {
        char data_dirs[PATH_MAX];
        snprintf(data_dirs, sizeof data_dirs, "%s", xdg);
        char *save = NULL;
        for (char *dir = strtok_r(data_dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
            char path[PATH_MAX], first[96] = "";
            snprintf(path, sizeof path, "%s/vulkan/icd.d", dir);
            int n = count_suffix_files(path, ".json", first, sizeof first);
            if (n > 0) {
                total += n;
                str_append(found, sizeof found, "%s%s/%s", found[0] ? ", " : "", path, first);
            }
        }
    }

    const char *override = getenv("VK_DRIVER_FILES");
    if (!override || !override[0]) override = getenv("VK_ICD_FILENAMES");
    if (override && override[0]) {
        str_append(found, sizeof found, "%sVK_DRIVER_FILES=%s", found[0] ? ", " : "", override);
        total++;
    }

    if (total > 0) {
        add_row(rep, 1, DM_ST_PASS, "Vulkan ICD", "%d manifest(s): %s", total, found);
    } else {
        add_row(rep, 1, DM_ST_WARN, "Vulkan ICD",
                "no manifests%s%s%s; Vulkan stays unavailable until a driver is installed",
                absent[0] ? " (" : "", absent, absent[0] ? ")" : "");
    }
}

// ------------------------------------------------------------------- Linux

static void dma_heap_row(dm_report *rep) {
    DIR *d = opendir("/dev/dma_heap");
    if (!d) {
        add_row(rep, 1, DM_ST_WARN, "dma-buf heap",
                "absent (/dev/dma_heap missing); Vulkan zero-copy falls back to copied pools");
        return;
    }
    int count = 0;
    char names[160] = "";
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (count < 3) str_append(names, sizeof names, "%s%s", names[0] ? ", " : "", e->d_name);
        count++;
    }
    closedir(d);
    if (count > 0) add_row(rep, 1, DM_ST_PASS, "dma-buf heap", "/dev/dma_heap: %d heap(s): %s", count, names);
    else add_row(rep, 1, DM_ST_WARN, "dma-buf heap", "/dev/dma_heap exists but is empty");
}

static void toolchain_row(dm_report *rep) {
    char gcc[160] = "";
    char sdl_pkg[64] = "";
    bool have_gcc = run_capture("gcc --version 2>/dev/null", gcc, sizeof gcc);
    bool have_sdl_pkg = run_capture("pkg-config --modversion sdl3 2>/dev/null", sdl_pkg, sizeof sdl_pkg);

    char detail[288] = "";
    str_append(detail, sizeof detail, "%s", have_gcc ? gcc : "gcc not found");
    str_append(detail, sizeof detail, "; pkg-config sdl3 %s", have_sdl_pkg ? sdl_pkg : "not found");

    dm_status status = (have_gcc && have_sdl_pkg) ? DM_ST_PASS : DM_ST_FAIL;
    add_row(rep, 1, status, "Toolchain", "%s", detail);
}

static void build_outputs_row(dm_report *rep, const char *repo) {
    char self[PATH_MAX];
    bool have_self = false;
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = 0;
        have_self = true;
    }

    char host[PATH_MAX];
    join_path(host, sizeof host, repo, "/build/alarm/nx2alarm-host");
    bool have_host = access(host, X_OK) == 0;

    char detail[288] = "";
    if (have_self) {
        const char *slash = strrchr(self, '/');
        str_append(detail, sizeof detail, "shell: %s", slash ? slash + 1 : self);
    } else {
        str_append(detail, sizeof detail, "shell: self path unknown");
    }
    str_append(detail, sizeof detail, "; host: %s", have_host ? "build/alarm/nx2alarm-host" : "not built yet (build_host.py)");

    add_row(rep, 1, have_host ? DM_ST_PASS : DM_ST_WARN, "Build outputs", "%s", detail);
}

static void game_data_rows(dm_report *rep, const char *repo) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/root";

    char keys[4][PATH_MAX];
    join_path(keys[0], PATH_MAX, repo, "/tools/prod.keys");
    join_path(keys[1], PATH_MAX, home, "/.local/share/nx2alarm/keys/prod.keys");
    join_path(keys[2], PATH_MAX, home, "/.switch/prod.keys");
    join_path(keys[3], PATH_MAX, home, "/.config/nx2alarm/prod.keys");

    const char *found = NULL;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (access(keys[i], F_OK) == 0) {
            found = keys[i];
            break;
        }
    }
    if (found) {
        add_row(rep, 1, DM_ST_PASS, "Game keys", "prod.keys: %s", found);
    } else {
        add_row(rep, 1, DM_ST_WARN, "Game keys",
                "not installed (no prod.keys; user-owned keys are needed only for gameplay)");
    }

    // Game dumps: the ignored .gamedump/ folder of this checkout, or the XDG data dir.
    char games[PATH_MAX];
    join_path(games, sizeof games, home, "/.local/share/nx2alarm/games");
    const char *locations[2] = { repo, games };
    int dumps = 0;
    for (size_t i = 0; i < 2; i++) {
        char path[PATH_MAX];
        join_path(path, sizeof path, locations[i], i == 0 ? "/.gamedump" : "");
        DIR *d = opendir(path);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            dumps++;
        }
        closedir(d);
    }
    if (dumps > 0) add_row(rep, 1, DM_ST_PASS, "Game data", "%d game folder(s) found", dumps);
    else add_row(rep, 1, DM_ST_WARN, "Game data",
                 "not installed (no game dumps; M1 shell and doctor do not need them)");
}

static void mesa_row(dm_report *rep) {
    static const char *lib_dirs[] = {
        "/usr/lib",
        "/usr/lib64",
        "/usr/lib/aarch64-linux-gnu",
    };
    char version[32] = "";
    for (size_t i = 0; i < sizeof lib_dirs / sizeof lib_dirs[0] && !version[0]; i++) {
        DIR *d = opendir(lib_dirs[i]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            static const char prefix[] = "libgallium-";
            if (strncmp(e->d_name, prefix, sizeof prefix - 1) != 0) continue;
            const char *start = e->d_name + sizeof prefix - 1;
            size_t len = strcspn(start, "-");
            if (len > 0 && len < sizeof version) {
                memcpy(version, start, len);
                version[len] = 0;
            }
            break;
        }
        closedir(d);
    }
    if (version[0]) {
        add_row(rep, 1, DM_ST_PASS, "Mesa", "%s (libgallium present; OpenGL works)", version);
    } else if (access("/usr/share/glvnd/egl_vendor.d/50_mesa.json", F_OK) == 0) {
        add_row(rep, 1, DM_ST_PASS, "Mesa", "EGL vendor manifest present (OpenGL via Mesa)");
    } else {
        add_row(rep, 1, DM_ST_WARN, "Mesa", "not detected");
    }
}

// ---------------------------------------------------------------- public API

void dm_doctor_collect(dm_report *rep, bool with_sdl) {
    memset(rep, 0, sizeof *rep);
    char repo[PATH_MAX];
    repo_root(repo, sizeof repo);

    // Device summary.
    board_rows(rep);
    kernel_row(rep);
    cpu_row(rep);
    ram_row(rep);
    sdl_rows(rep, with_sdl);

    // Prerequisites for the port.
    vulkan_rows(rep);
    icd_row(rep);
    dma_heap_row(rep);
    toolchain_row(rep);
    build_outputs_row(rep, repo);
    game_data_rows(rep, repo);
    mesa_row(rep);
}

void dm_doctor_print(const dm_report *rep) {
    static const char *const titles[2] = { "device", "prerequisites" };
    printf("nx2alarm doctor -- device and prerequisite audit\n");
    printf("nx2alarm %s\n\n", DM_ALARM_VERSION);
    for (int section = 0; section < 2; section++) {
        printf("== %s ==\n", titles[section]);
        for (int i = 0; i < rep->count; i++) {
            if (rep->rows[i].section != section) continue;
            printf("[%-4s] %-26s %s\n", dm_status_label(rep->rows[i].status),
                   rep->rows[i].name, rep->rows[i].detail);
        }
        printf("\n");
    }
    printf("summary: %d pass, %d warn, %d fail, %d not checked\n",
           rep->n_pass, rep->n_warn, rep->n_fail, rep->n_info);
    if (rep->n_fail > 0) {
        printf("result: FAIL (%d critical check(s) failed)\n", rep->n_fail);
    } else if (rep->n_warn > 0) {
        printf("result: OK (warnings only; they do not fail doctor)\n");
    } else {
        printf("result: OK\n");
    }
}

int dm_doctor_exit_code(const dm_report *rep) {
    return rep->n_fail > 0 ? 1 : 0;
}

int dm_alarm_doctor_command(void) {
    dm_report report;
    dm_doctor_collect(&report, true);
    dm_doctor_print(&report);
    return dm_doctor_exit_code(&report);
}
