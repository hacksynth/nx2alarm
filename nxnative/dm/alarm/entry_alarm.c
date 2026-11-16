// nx2alarm: command line entry point.
//
//   nx2alarm [shell] [--headless] [--frames N] [--screenshot FILE]
//   nx2alarm doctor
//   nx2alarm version
//   nx2alarm help
//
// No command means `shell`. Unknown commands and options print the usage and
// exit 2, so a typo never opens a window by accident.
#include "alarm.h"
#include "doctor.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dm_shell_options_init(dm_shell_options *options) {
    memset(options, 0, sizeof *options);
    options->headless = false;
    options->frames = 0;
    options->screenshot = NULL;
}

void dm_alarm_usage(FILE *out, const char *argv0) {
    fprintf(out, "usage: %s [command] [options]\n", argv0);
    fprintf(out, "\n");
    fprintf(out, "commands:\n");
    fprintf(out, "  shell      run the SDL3 shell window (the default with no command)\n");
    fprintf(out, "  doctor     print the device and prerequisite audit; exit 1 on a FAIL\n");
    fprintf(out, "  version    print the nx2alarm, compiler and SDL3 versions\n");
    fprintf(out, "  help       print this text\n");
    fprintf(out, "\n");
    fprintf(out, "shell options:\n");
    fprintf(out, "  --headless          no video: print the device summary and exit 0\n");
    fprintf(out, "  --frames N          render N frames and exit (0 or absent: run until quit)\n");
    fprintf(out, "  --screenshot FILE   save the first rendered frame as a PPM P6 file\n");
    fprintf(out, "  -h, --help          print this text\n");
    fprintf(out, "\n");
    fprintf(out, "controls: Q / Esc quit, R refresh the audit, F11 toggle fullscreen\n");
}

static void print_version(void) {
    int runtime = SDL_GetVersion();
    printf("nx2alarm %s\n", DM_ALARM_VERSION);
    printf("native aarch64 Linux shell for nx2android\n");
    printf("  compiler : gcc %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
    printf("  SDL3     : compiled %d.%d.%d, runtime %d.%d.%d\n",
           SDL_VERSIONNUM_MAJOR(SDL_VERSION), SDL_VERSIONNUM_MINOR(SDL_VERSION),
           SDL_VERSIONNUM_MICRO(SDL_VERSION),
           SDL_VERSIONNUM_MAJOR(runtime), SDL_VERSIONNUM_MINOR(runtime),
           SDL_VERSIONNUM_MICRO(runtime));
    printf("  built    : %s %s\n", __DATE__, __TIME__);
    printf("  targets  : shell window, doctor; no Android NDK, no .NET, no game data needed\n");
}

static bool parse_frames(const char *text, int *frames) {
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || end == NULL || *end != 0) return false;
    if (value < 0 || value > 1000000L) return false;
    *frames = (int)value;
    return true;
}

static int shell_bad_argument(const char *argv0, const char *argument) {
    fprintf(stderr, "nx2alarm shell: unknown or invalid option '%s'\n", argument);
    dm_alarm_usage(stderr, argv0);
    return 2;
}

static int run_shell(int argc, char **argv, int first) {
    dm_shell_options options;
    dm_shell_options_init(&options);

    for (int i = first; i < argc; i++) {
        const char *argument = argv[i];
        if (strcmp(argument, "--headless") == 0) {
            options.headless = true;
        } else if (strcmp(argument, "--frames") == 0) {
            if (i + 1 >= argc || !parse_frames(argv[i + 1], &options.frames)) {
                return shell_bad_argument(argv[0], "--frames needs a frame count");
            }
            i++;
        } else if (strncmp(argument, "--frames=", 9) == 0) {
            if (!parse_frames(argument + 9, &options.frames)) {
                return shell_bad_argument(argv[0], argument);
            }
        } else if (strcmp(argument, "--screenshot") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == 0) {
                return shell_bad_argument(argv[0], "--screenshot needs a file name");
            }
            options.screenshot = argv[++i];
        } else if (strncmp(argument, "--screenshot=", 13) == 0) {
            if (argument[13] == 0) return shell_bad_argument(argv[0], argument);
            options.screenshot = argument + 13;
        } else if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            dm_alarm_usage(stdout, argv[0]);
            return 0;
        } else {
            return shell_bad_argument(argv[0], argument);
        }
    }
    return dm_alarm_shell_run(&options);
}

static int run_doctor(int argc, char **argv, int first) {
    for (int i = first; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            dm_alarm_usage(stdout, argv[0]);
            return 0;
        }
        fprintf(stderr, "nx2alarm doctor: unknown option '%s'\n", argv[i]);
        dm_alarm_usage(stderr, argv[0]);
        return 2;
    }
    return dm_alarm_doctor_command();
}

int main(int argc, char **argv) {
    const char *command = "shell";
    int first = 1;

    if (argc > 1) {
        if (argv[1][0] != '-') {
            command = argv[1];
            first = 2;
        } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            dm_alarm_usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(argv[1], "--version") == 0) {
            print_version();
            return 0;
        }
        // Other leading options belong to the default `shell` command.
    }

    if (strcmp(command, "shell") == 0) return run_shell(argc, argv, first);
    if (strcmp(command, "doctor") == 0) return run_doctor(argc, argv, first);
    if (strcmp(command, "version") == 0) {
        if (first < argc) {
            fprintf(stderr, "nx2alarm version: unexpected argument '%s'\n", argv[first]);
            dm_alarm_usage(stderr, argv[0]);
            return 2;
        }
        print_version();
        return 0;
    }
    if (strcmp(command, "help") == 0) {
        dm_alarm_usage(stdout, argv[0]);
        return 0;
    }

    fprintf(stderr, "nx2alarm: unknown command '%s'\n", command);
    dm_alarm_usage(stderr, argv[0]);
    return 2;
}
