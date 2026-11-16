// nx2alarm Linux shim: <vulkan/vulkan.h>.
//
// The repository's Vulkan-Headers copy (dm/third_party/vulkan-headers/include) has no vulkan.h umbrella header: it ships
// vk_platform.h, vulkan_core.h and vulkan_vi.h only, because the Android build gets vulkan.h from the NDK's sysroot. On Linux
// the backend includes <vulkan/vulkan.h> (gfx_vk.h), so this shim provides it from the headers the repository does ship.
// The lookup of vulkan_core.h below skips this directory (there is no vulkan_core.h here) and finds
// dm/third_party/vulkan-headers/include/vulkan/vulkan_core.h through the next -I.
#pragma once
#include <vulkan/vulkan_core.h>
