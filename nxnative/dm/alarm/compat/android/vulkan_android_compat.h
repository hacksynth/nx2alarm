// nx2alarm Linux shim: the Android-only Vulkan declarations the Vulkan backend names, which the Vulkan-Headers copy in
// dm/third_party/vulkan-headers omits (it ships only vulkan_core.h, vk_platform.h and vulkan_vi.h: the platform headers
// vulkan_android.h / vulkan_platform.h are not there, and the Android build gets them from the NDK).
//
// The backend names three of them:
//   - VkAndroidSurfaceCreateInfoKHR + vkCreateAndroidSurfaceKHR
//         gfx_vk_present.c swap_make(): replaced for DM_ALARM by vk_alarm_create_surface() (SDL3, gfx_vk_alarm.c); the
//         declarations stay so the Android code path compiles unchanged and the pointer is loaded (as NULL) with the
//         other instance entry points.
//   - VkAndroidHardwareBufferPropertiesANDROID + VkImportAndroidHardwareBufferInfoANDROID +
//     vkGetAndroidHardwareBufferPropertiesANDROID
//         gfx_vk_core.c slab_new() / pool_zero_copy() / zcf_free_now(), gfx_vk_present.c vk_image_from_ahb(): the
//         zero-copy pool import. Unreachable on Linux because compat/android/hardware_buffer.h refuses every
//         allocation, so these are compile-only (the code path is kept byte-identical for the Android build).
// Only compiled for DM_ALARM.
#pragma once
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>

typedef struct VkAndroidSurfaceCreateInfoKHR {
    VkStructureType sType;              // VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR
    const void *pNext;
    VkFlags flags;
    ANativeWindow *window;
} VkAndroidSurfaceCreateInfoKHR;

typedef struct VkImportAndroidHardwareBufferInfoANDROID {
    VkStructureType sType;              // VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID
    const void *pNext;
    AHardwareBuffer *buffer;
} VkImportAndroidHardwareBufferInfoANDROID;

typedef struct VkAndroidHardwareBufferPropertiesANDROID {
    VkStructureType sType;              // VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID
    void *pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeBits;
} VkAndroidHardwareBufferPropertiesANDROID;

typedef VkResult (VKAPI_PTR *PFN_vkCreateAndroidSurfaceKHR)(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *pCreateInfo,
                                                             const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface);
typedef VkResult (VKAPI_PTR *PFN_vkGetAndroidHardwareBufferPropertiesANDROID)(VkDevice device, const AHardwareBuffer *buffer,
                                                                              VkAndroidHardwareBufferPropertiesANDROID *pProperties);
#endif
