// nx2alarm: the native Arch Linux ARM (aarch64) shell of nx2android.
// Shared declarations of the command line (entry_alarm.c), the SDL3 shell
// (front_alarm.c) and the device audit (doctor.c).
#ifndef DM_ALARM_H
#define DM_ALARM_H

#include <stdbool.h>
#include <stdio.h>

// Version of the shell binary; keep it in step with docs/PORTING_PLAN.md's milestones.
#define DM_ALARM_VERSION "0.1.0-m1"

// The shell's render size, in pixels. The window is resizable; the frame is
// rendered at this size and scaled to the window with its aspect kept.
#define DM_ALARM_WINDOW_W 1280
#define DM_ALARM_WINDOW_H 720

// Audit report rows shown by `nx2alarm doctor` and in the shell window.
#define DM_ALARM_MAX_ROWS 40

typedef enum {
    DM_ST_PASS = 0,     // present and working
    DM_ST_WARN,         // missing or degraded, but not fatal for M1
    DM_ST_FAIL,         // a prerequisite `doctor` fails on (exit 1)
    DM_ST_INFO          // not checked in this mode (e.g. headless)
} dm_status;

typedef struct {
    int section;                // 0 = device summary, 1 = prerequisite check
    char name[40];              // short check name, e.g. "Vulkan loader"
    char detail[288];           // measured value / explanation, English, one line
    dm_status status;
} dm_row;

typedef struct {
    dm_row rows[DM_ALARM_MAX_ROWS];
    int count;
    int n_pass, n_warn, n_fail, n_info;
    // Compact device values, also used by the headless shell summary.
    char model[192];
    char soc[320];
    char cpu[192];
    char kernel[160];
    char ram[64];
    char sdl[96];
    char display[128];
    char vulkan[320];
} dm_report;

typedef struct {
    bool headless;              // --headless: no SDL video, print the summary and exit
    int frames;                 // --frames N: render N frames then exit; 0 = until quit
    const char *screenshot;     // --screenshot FILE: save the first frame as PPM P6, or NULL
} dm_shell_options;

void dm_shell_options_init(dm_shell_options *opt);

// Set the SDL hints shared by the shell and doctor before any SDL subsystem starts.
void dm_sdl_prepare_hints(void);

// front_alarm.c
int dm_alarm_shell_run(const dm_shell_options *opt);

// doctor.c
void dm_doctor_collect(dm_report *rep, bool with_sdl);
void dm_doctor_print(const dm_report *rep);
int dm_doctor_exit_code(const dm_report *rep);
int dm_alarm_doctor_command(void);
const char *dm_status_label(dm_status status);

// entry_alarm.c
void dm_alarm_usage(FILE *out, const char *argv0);

#endif  // DM_ALARM_H
