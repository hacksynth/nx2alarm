// nx2alarm Linux port: the Vulkan backend's window and surface glue (replaces the Android ANativeWindow / VkAndroidSurfaceKHR
// path of dm/android/gfx_vk_present.c on Linux).
//
// The backend keeps its Android-shaped window plumbing: gfx_vk_present.c calls dm_front_window() and ANativeWindow_getWidth/
// Height (compat/android/native_window.h aliases the window to an SDL_Window and reads its pixel size). This file owns that
// window and the VkSurfaceKHR:
//   - vk_alarm_set_window(): the front end hands over the SDL window it renders into. It must have been created with
//     SDL_WINDOW_VULKAN (SDL_Vulkan_CreateSurface refuses any other window); the shell's own GL/SDL_Renderer window is not
//     usable for this backend.
//   - vk_alarm_create_surface(): SDL_Vulkan_CreateSurface (Wayland/X11/... picked by SDL at run time) instead of
//     vkCreateAndroidSurfaceKHR.
//   - vk_alarm_instance_exts(): the platform instance extensions SDL asks for (VK_KHR_surface + VK_KHR_wayland_surface, ...),
//     which gfx_vk_core.c vk_init() enables instead of VK_KHR_android_surface.
//   - vk_alarm_window_pixels()/vk_alarm_refresh_hz(): the swapchain extent and the display's rate (SDL display mode),
//     replacing ANativeWindow_getWidth/Height and Android's display-rate query (perf_android.c dm_perf_window).
// The Android build never sees this file (compiled only with -DDM_ALARM, see GFX_VK_PORT.md).
#pragma once
#if defined(DM_ALARM)
#include <vulkan/vulkan.h>   // types only: gfx_vk.h includes this after defining VK_NO_PROTOTYPES

struct SDL_Window;

// The window the backend presents into (an SDL_WINDOW_VULKAN window). NULL: no window, the backend stays headless (frames
// are counted and DM_VK_SHOT shots are written, but no swapchain is made). Called by the front end on the main thread.
void vk_alarm_set_window(struct SDL_Window *window);
struct SDL_Window *vk_alarm_window(void);
void vk_alarm_window_lost(void);   // the window is going away: the backend drops its swapchain and surface

// The window's pixel size (HiDPI safe); 0 when there is no window or it is minimized/hidden, and the backend then shows nothing.
int vk_alarm_window_pixels(int *w, int *h);
int vk_alarm_refresh_hz(void);   // the display mode's rate, 0 when unknown
void vk_alarm_toggle_fullscreen(void);

// The Vulkan instance extensions to enable (VK_KHR_surface plus the window system's). Returns the count, 0 when SDL cannot
// say (no video driver yet); *names then holds nothing and the caller enables VK_KHR_surface alone.
int vk_alarm_instance_exts(const char **names, int max);
// Create the window's VkSurfaceKHR; 0 on success, negative on failure (logged once with SDL's error).
int vk_alarm_create_surface(VkInstance instance, VkSurfaceKHR *surface);
#endif
