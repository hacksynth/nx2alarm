// nx2alarm shell: the SDL3 front end of the Arch Linux ARM port.
//
// M1 draws a dark status screen with the repository's bitmap font
// (dm/android/pixel_font.h + pixel_font_latin1.h): the device summary and the
// prerequisite checks from doctor.c, as a support artifact for the port.
// Controls: Q / Esc quit, R refresh the audit, F11 toggle fullscreen.
//
// Modes: --headless prints the summary without touching SDL video; --frames N
// renders N frames and exits; --screenshot FILE saves the first frame as PPM
// P6 through SDL_RenderReadPixels. Missing display, renderer or audio never
// crash the shell: it prints the reason and falls back to the headless summary.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "alarm.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The host core's front-end interface: the Linux shell provides front_run() and
// front_text_input() so dm/host/main.c and hle_swkbd.c link against this file.
#include "../host/front.h"

// The repository's bitmap font. PF_LATIN1 is a static glyph pack in the format
// pixel_font.h documents; the shell only has to draw ASCII/Latin-1 text.
#include "../android/pixel_font.h"
#include "../android/pixel_font_latin1.h"

// ------------------------------------------------------------------ palette

#define DM_COL_BG       0xFF0F1216u   // window background
#define DM_COL_PANEL    0xFF171C23u   // header / footer bars
#define DM_COL_RULE     0xFF2A313Bu   // divider lines
#define DM_COL_TEXT     0xFFE6EDF3u   // normal text
#define DM_COL_DIM      0xFF8B949Eu   // labels, details
#define DM_COL_ACCENT   0xFF79C0FFu   // title and section labels
#define DM_COL_PASS     0xFF7EE787u
#define DM_COL_WARN     0xFFE3B341u
#define DM_COL_FAIL     0xFFFF7B72u
#define DM_COL_INFO     0xFFA5B4C4u

#define DM_MARGIN       20
#define DM_HEADER_H     64
#define DM_FOOTER_H     30

// ------------------------------------------------------------ text drawing

// A locked streaming texture: the shell paints the whole frame in software so
// the pixel font's glyph plotter has direct pixel access, then uploads it once.
typedef struct {
    int w, h, pitch;    // pitch in bytes
    Uint32 *pixels;
} dm_canvas;

static void canvas_pixel(dm_canvas *canvas, int x, int y, Uint32 color) {
    if (x < 0 || y < 0 || x >= canvas->w || y >= canvas->h) return;
    canvas->pixels[(size_t)y * (size_t)(canvas->pitch / 4) + (size_t)x] = color;
}

static void canvas_rect(dm_canvas *canvas, int x, int y, int w, int h, Uint32 color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > canvas->w ? canvas->w : x + w;
    int y1 = y + h > canvas->h ? canvas->h : y + h;
    for (int row = y0; row < y1; row++) {
        Uint32 *line = canvas->pixels + (size_t)row * (size_t)(canvas->pitch / 4);
        for (int column = x0; column < x1; column++) line[column] = color;
    }
}

typedef struct {
    dm_canvas *canvas;
    Uint32 color;
} dm_pen;

static void pen_plot(void *user, int x, int y) {
    dm_pen *pen = user;
    canvas_pixel(pen->canvas, x, y, pen->color);
}

static int text_width(const char *text, int scale) {
    return pf_width(PF_LATIN1, text) * scale;
}

// The pack covers Latin-1 and U+FFFD only. System-provided text (an audio
// device name, a path) may be UTF-8 with other scripts; draw '?' for those so
// the shell never shows a row of fallback boxes. Latin-1 is re-encoded as
// UTF-8, which is what pf_draw() expects.
static void sanitize_text(const char *text, char *out, size_t cap) {
    const char *cursor = text;
    size_t used = 0;
    while (*cursor && used + 4 < cap) {
        uint32_t code_point = pf_next(&cursor);
        if (code_point < 0x80) {
            out[used++] = (char)code_point;
        } else if (code_point >= 0xA0 && code_point <= 0xFF) {
            out[used++] = (char)(0xC0 | (code_point >> 6));
            out[used++] = (char)(0x80 | (code_point & 0x3F));
        } else {
            out[used++] = '?';
        }
    }
    out[used] = 0;
}

// Draw a string with its top-left corner at (x, top); 'top' is the top of a
// capital, so the baseline is PF_CAP * scale rows below it.
static void text_at(dm_canvas *canvas, int x, int top, int scale, Uint32 color, const char *text) {
    char printable[320];
    sanitize_text(text, printable, sizeof printable);
    dm_pen pen = { canvas, color };
    pf_draw(PF_LATIN1, printable, x, top + PF_CAP * scale, scale, pen_plot, &pen);
}

// Draw a string clipped to [x, right); a "..." marks the cut.
static void text_fit(dm_canvas *canvas, int x, int top, int scale, Uint32 color, int right, const char *text) {
    char buf[320];
    sanitize_text(text, buf, sizeof buf);
    int available = right - x;
    if (available <= 0) return;
    if (text_width(buf, scale) > available) {
        int dots = text_width("...", scale);
        int target = available - dots;
        while (buf[0] && text_width(buf, scale) > target) pf_chop(buf, strlen(buf));
        size_t len = strlen(buf);
        if (len + 3 < sizeof buf) snprintf(buf + len, sizeof buf - len, "...");
    }
    text_at(canvas, x, top, scale, color, buf);
}

static Uint32 status_color(dm_status status) {
    switch (status) {
        case DM_ST_PASS: return DM_COL_PASS;
        case DM_ST_WARN: return DM_COL_WARN;
        case DM_ST_FAIL: return DM_COL_FAIL;
        default: return DM_COL_INFO;
    }
}

// ---------------------------------------------------------------- the frame

static void draw_section_label(dm_canvas *canvas, int x, int *y, int line_height, const char *label) {
    text_at(canvas, x, *y, 1, DM_COL_ACCENT, label);
    canvas_rect(canvas, x, *y + 12, canvas->w - 2 * x, 1, DM_COL_RULE);
    *y += line_height;
}

// Paint one full frame: header, both audit sections, footer.
static void draw_frame(dm_canvas *canvas, const dm_report *report) {
    canvas_rect(canvas, 0, 0, canvas->w, canvas->h, DM_COL_BG);
    canvas_rect(canvas, 0, 0, canvas->w, DM_HEADER_H, DM_COL_PANEL);
    canvas_rect(canvas, 0, DM_HEADER_H - 2, canvas->w, 2, DM_COL_ACCENT);

    // Header: title, subtitle, version and SDL on the right.
    text_at(canvas, DM_MARGIN, 10, 3, DM_COL_ACCENT, "nx2alarm");
    text_at(canvas, DM_MARGIN, 45, 1, DM_COL_DIM,
            "Arch Linux ARM port of nx2android - device and prerequisite audit");

    char version[64];
    snprintf(version, sizeof version, "v%s", DM_ALARM_VERSION);
    text_at(canvas, canvas->w - DM_MARGIN - text_width(version, 1), 12, 1, DM_COL_TEXT, version);
    char sdl[64];
    snprintf(sdl, sizeof sdl, "SDL %.20s", report->sdl);
    text_at(canvas, canvas->w - DM_MARGIN - text_width(sdl, 1), 30, 1, DM_COL_DIM, sdl);

    // Choose a text size that fits all rows; the window can be any size, but
    // the frame is always laid out for DM_ALARM_WINDOW_H.
    int lines = 2;
    for (int i = 0; i < report->count; i++) lines++;
    int available = DM_ALARM_WINDOW_H - DM_HEADER_H - DM_FOOTER_H - 24;
    int scale = 2;
    int line_height = 24;
    if (lines * line_height > available) {
        scale = 1;
        line_height = 14;
    }

    // Column widths, so names and details line up.
    int device_name_col = 0;
    int check_status_col = text_width("[WARN]", scale) + 12;
    int check_name_col = 0;
    for (int i = 0; i < report->count; i++) {
        const dm_row *row = &report->rows[i];
        int width = text_width(row->name, scale);
        if (row->section == 0 && width > device_name_col) device_name_col = width;
        if (row->section == 1 && width > check_name_col) check_name_col = width;
    }
    device_name_col += 16;
    check_name_col += 16;

    int y = DM_HEADER_H + 14;
    draw_section_label(canvas, DM_MARGIN, &y, line_height, "DEVICE");
    for (int i = 0; i < report->count; i++) {
        const dm_row *row = &report->rows[i];
        if (row->section != 0) continue;
        text_at(canvas, DM_MARGIN, y, scale, DM_COL_DIM, row->name);
        text_fit(canvas, DM_MARGIN + device_name_col, y, scale, DM_COL_TEXT,
                 canvas->w - DM_MARGIN, row->detail);
        y += line_height;
    }

    y += 8;
    draw_section_label(canvas, DM_MARGIN, &y, line_height, "PREREQUISITES (PASS / WARN / FAIL)");
    for (int i = 0; i < report->count; i++) {
        const dm_row *row = &report->rows[i];
        if (row->section != 1) continue;
        char label[8];
        snprintf(label, sizeof label, "[%s]", dm_status_label(row->status));
        text_at(canvas, DM_MARGIN, y, scale, status_color(row->status), label);
        text_at(canvas, DM_MARGIN + check_status_col, y, scale, DM_COL_TEXT, row->name);
        text_fit(canvas, DM_MARGIN + check_status_col + check_name_col, y, scale, DM_COL_DIM,
                 canvas->w - DM_MARGIN, row->detail);
        y += line_height;
    }

    // Footer: controls on the left, pass/warn/fail totals on the right.
    canvas_rect(canvas, 0, canvas->h - DM_FOOTER_H, canvas->w, DM_FOOTER_H, DM_COL_PANEL);
    text_at(canvas, DM_MARGIN, canvas->h - DM_FOOTER_H + 10, 1, DM_COL_DIM,
            "Q / Esc: quit    R: refresh    F11: fullscreen");
    char totals[96];
    snprintf(totals, sizeof totals, "%d pass   %d warn   %d fail", report->n_pass, report->n_warn,
             report->n_fail);
    text_at(canvas, canvas->w - DM_MARGIN - text_width(totals, 1), canvas->h - DM_FOOTER_H + 10, 1,
            DM_COL_TEXT, totals);
}

// ------------------------------------------------------------------ PPM out

// Save the current render target as a binary PPM (P6). The frame is read back
// through the renderer, not from our own buffer, so this also exercises the
// same path a future Vulkan swapchain readback would.
static int save_ppm(SDL_Renderer *renderer, const char *path) {
    SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
    if (!shot) {
        fprintf(stderr, "nx2alarm shell: SDL_RenderReadPixels failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_Surface *rgb = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_XRGB8888);
    if (!rgb) {
        fprintf(stderr, "nx2alarm shell: SDL_ConvertSurface failed: %s\n", SDL_GetError());
        SDL_DestroySurface(shot);
        return -1;
    }
    SDL_DestroySurface(shot);

    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "nx2alarm shell: cannot open screenshot '%s': %s\n", path, strerror(errno));
        SDL_DestroySurface(rgb);
        return -1;
    }

    bool ok = fprintf(file, "P6\n%d %d\n255\n", rgb->w, rgb->h) > 0;
    for (int row = 0; ok && row < rgb->h; row++) {
        const Uint32 *line =
            (const Uint32 *)((const Uint8 *)rgb->pixels + (size_t)row * (size_t)rgb->pitch);
        for (int column = 0; column < rgb->w; column++) {
            Uint32 pixel = line[column];
            unsigned char bytes[3] = {
                (unsigned char)((pixel >> 16) & 0xFFu),
                (unsigned char)((pixel >> 8) & 0xFFu),
                (unsigned char)(pixel & 0xFFu),
            };
            if (fwrite(bytes, 1, sizeof bytes, file) != sizeof bytes) {
                ok = false;
                break;
            }
        }
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "nx2alarm shell: cannot write screenshot '%s'\n", path);
        remove(path);
        SDL_DestroySurface(rgb);
        return -1;
    }
    printf("nx2alarm shell: wrote screenshot %s (%dx%d PPM P6)\n", path, rgb->w, rgb->h);
    SDL_DestroySurface(rgb);
    return 0;
}

// ----------------------------------------------------------- headless mode

// The summary never touches SDL video: it is safe on a serial console, under
// SDL_VIDEODRIVER=dummy, or when the compositor is gone.
static int shell_headless(const dm_shell_options *options, const char *reason) {
    dm_report report;
    dm_doctor_collect(&report, false);

    printf("nx2alarm %s -- headless summary (%s)\n", DM_ALARM_VERSION, reason);
    printf("  board   : %s\n", report.model);
    printf("  soc     : %s\n", report.soc);
    printf("  cpu     : %s\n", report.cpu);
    printf("  ram     : %s\n", report.ram);
    printf("  kernel  : %s\n", report.kernel);
    printf("  sdl     : %s\n", report.sdl);
    printf("  display : %s\n", report.display);
    printf("  vulkan  : %s\n", report.vulkan);
    printf("  checks  : %d pass, %d warn, %d fail, %d not checked\n",
           report.n_pass, report.n_warn, report.n_fail, report.n_info);
    for (int i = 0; i < report.count; i++) {
        const dm_row *row = &report.rows[i];
        if (row->section != 1) continue;
        printf("    [%-4s] %s: %s\n", dm_status_label(row->status), row->name, row->detail);
    }
    if (options->screenshot) {
        printf("  screenshot: skipped in headless mode (it needs an SDL renderer)\n");
    }
    printf("  no window created (headless)\n");
    return 0;
}

// -------------------------------------------------------------- shell loop

int dm_alarm_shell_run(const dm_shell_options *options) {
    if (options->headless) return shell_headless(options, "headless mode");

    dm_sdl_prepare_hints();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "nx2alarm shell: SDL video unavailable: %s\n", SDL_GetError());
        fprintf(stderr, "nx2alarm shell: falling back to the headless summary\n");
        return shell_headless(options, "no video device");
    }

    SDL_Window *window = SDL_CreateWindow("nx2alarm", DM_ALARM_WINDOW_W, DM_ALARM_WINDOW_H,
                                          SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "nx2alarm shell: cannot create window: %s\n", SDL_GetError());
        fprintf(stderr, "nx2alarm shell: falling back to the headless summary\n");
        SDL_Quit();
        return shell_headless(options, "no window");
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    const char *renderer_name = renderer ? SDL_GetRendererName(renderer) : NULL;
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
        renderer_name = renderer ? SDL_GetRendererName(renderer) : NULL;
    }
    if (!renderer) {
        fprintf(stderr, "nx2alarm shell: no SDL renderer: %s\n", SDL_GetError());
        fprintf(stderr, "nx2alarm shell: falling back to the headless summary\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return shell_headless(options, "no renderer");
    }

    // The UI is painted into this streaming texture, then drawn into a target
    // texture so SDL_RenderReadPixels can save it before it is scaled to the
    // window (the window may be any size; the frame keeps its aspect ratio).
    SDL_Texture *frame_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                                   SDL_TEXTUREACCESS_STREAMING,
                                                   DM_ALARM_WINDOW_W, DM_ALARM_WINDOW_H);
    SDL_Texture *target_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                                    SDL_TEXTUREACCESS_TARGET,
                                                    DM_ALARM_WINDOW_W, DM_ALARM_WINDOW_H);
    if (!frame_texture || !target_texture) {
        fprintf(stderr, "nx2alarm shell: cannot create textures: %s\n", SDL_GetError());
        if (frame_texture) SDL_DestroyTexture(frame_texture);
        if (target_texture) SDL_DestroyTexture(target_texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return shell_headless(options, "no textures");
    }
    SDL_SetTextureScaleMode(frame_texture, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode(target_texture, SDL_SCALEMODE_NEAREST);
    SDL_SetRenderVSync(renderer, 1);

    dm_report report;
    dm_doctor_collect(&report, true);

    printf("nx2alarm shell: window %dx%d (resizable), renderer %s\n",
           DM_ALARM_WINDOW_W, DM_ALARM_WINDOW_H, renderer_name ? renderer_name : "unknown");
    printf("nx2alarm shell: device: %s\n", report.model);
    printf("nx2alarm shell: controls: Q/Esc quit, R refresh, F11 fullscreen\n");
    if (options->screenshot) {
        size_t len = strlen(options->screenshot);
        if (len < 4 || strcmp(options->screenshot + len - 4, ".ppm") != 0) {
            fprintf(stderr, "nx2alarm shell: warning: screenshot '%s' does not end in .ppm, "
                            "writing PPM P6 anyway\n", options->screenshot);
        }
    }

    int exit_code = 0;
    int rendered = 0;
    bool screenshot_done = false;
    bool quit = false;

    while (!quit) {
        bool refresh = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                quit = true;
            } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                       event.window.windowID == SDL_GetWindowID(window)) {
                quit = true;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                switch (event.key.scancode) {
                    case SDL_SCANCODE_Q:
                    case SDL_SCANCODE_ESCAPE:
                        quit = true;
                        break;
                    case SDL_SCANCODE_R:
                        refresh = true;
                        break;
                    case SDL_SCANCODE_F11: {
                        bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                        SDL_SetWindowFullscreen(window, !fullscreen);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        if (refresh) dm_doctor_collect(&report, true);

        // Paint the frame into the streaming texture.
        void *pixels = NULL;
        int pitch = 0;
        dm_canvas canvas = { 0, 0, 0, NULL };
        if (!SDL_LockTexture(frame_texture, NULL, &pixels, &pitch) || !pixels) {
            fprintf(stderr, "nx2alarm shell: cannot lock the frame texture: %s\n", SDL_GetError());
            exit_code = 1;
            break;
        }
        canvas.w = DM_ALARM_WINDOW_W;
        canvas.h = DM_ALARM_WINDOW_H;
        canvas.pitch = pitch;
        canvas.pixels = pixels;
        draw_frame(&canvas, &report);
        SDL_UnlockTexture(frame_texture);

        // Draw the frame into the target texture (for readback), then scale it
        // to the window with its aspect kept.
        bool want_screenshot = options->screenshot != NULL && !screenshot_done;
        SDL_SetRenderTarget(renderer, target_texture);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, frame_texture, NULL, NULL);
        if (want_screenshot) {
            screenshot_done = true;
            if (save_ppm(renderer, options->screenshot) != 0) exit_code = 1;
        }
        SDL_SetRenderTarget(renderer, NULL);

        int output_w = DM_ALARM_WINDOW_W;
        int output_h = DM_ALARM_WINDOW_H;
        SDL_GetRenderOutputSize(renderer, &output_w, &output_h);
        if (output_w <= 0 || output_h <= 0) {
            output_w = DM_ALARM_WINDOW_W;
            output_h = DM_ALARM_WINDOW_H;
        }
        float scale_x = (float)output_w / (float)DM_ALARM_WINDOW_W;
        float scale_y = (float)output_h / (float)DM_ALARM_WINDOW_H;
        float fit = scale_x < scale_y ? scale_x : scale_y;
        SDL_FRect destination = {
            ((float)output_w - (float)DM_ALARM_WINDOW_W * fit) * 0.5f,
            ((float)output_h - (float)DM_ALARM_WINDOW_H * fit) * 0.5f,
            (float)DM_ALARM_WINDOW_W * fit,
            (float)DM_ALARM_WINDOW_H * fit,
        };
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, target_texture, NULL, &destination);
        SDL_RenderPresent(renderer);

        rendered++;
        if (options->frames > 0 && rendered >= options->frames) break;
        if (options->screenshot && options->frames == 0 && screenshot_done) break;
        SDL_Delay(16);
    }

    printf("nx2alarm shell: rendered %d frame(s)\n", rendered);

    SDL_DestroyTexture(frame_texture);
    SDL_DestroyTexture(target_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}

// --------------------------------------------------------- host-core glue

// The host core (dm/host/main.c) runs the guest through this interface. The M1
// shell has no separate UI thread, so the guest runs on the calling thread just
// as in the Android and iOS front ends when there is no window.
void front_run(void (*guest)(void)) {
    if (guest) guest();
}

// Text entry for the game's software keyboard (hle_swkbd.c): the M1 shell does
// not show an on-screen keyboard yet, so this reports "nothing shown" (-1),
// the same answer the front ends give when there is no window.
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial,
                     int max_chars, char *out, int out_cap) {
    (void)title;
    (void)sub;
    (void)guide;
    (void)initial;
    (void)max_chars;
    (void)out;
    (void)out_cap;
    return -1;
}
