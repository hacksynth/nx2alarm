// nx2alarm Linux shim: Android's ANativeWindow as an SDL3 window.
//
// The Vulkan backend's presentation side (dm/android/gfx_vk_present.c) asks dm_front_window() for an ANativeWindow, reads
// its size and creates a VkSurfaceKHR from it (VkAndroidSurfaceCreateInfoKHR + vkCreateAndroidSurfaceKHR). On Linux the
// window is the SDL3 window of the shell (nxnative/dm/alarm/gfx_vk_alarm.c): this header aliases the opaque Android type to
// it and provides the two size accessors the backend uses. Surface creation itself is replaced by
// vk_alarm_create_surface() (SDL_Vulkan_CreateSurface), see gfx_vk_alarm.h; the ANativeWindow alias exists so the
// Android-shaped window plumbing in gfx_vk_present.c stays as it is.
// Only compiled for DM_ALARM: the Android build uses the NDK's <android/native_window.h>.
#pragma once
#include <stdint.h>

struct SDL_Window;
typedef struct SDL_Window ANativeWindow;

// Pixel size (SDL_GetWindowSizeInPixels); 0 for a window that is not shown or is minimized, which makes the backend skip
// the swapchain and show nothing until it comes back.
int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
