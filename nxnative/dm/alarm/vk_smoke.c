// nx2alarm-vk: the M2 Vulkan validation tool for the Arch Linux ARM port.
//
//   nx2alarm-vk info                      device, features, limits, BC formats, extensions
//   nx2alarm-vk smoke [--frames N] [--screenshot FILE] [--device N]
//                                         SDL3 window -> VkSurfaceKHR -> swapchain, clears an
//                                         animated colour and presents N frames on the real GPU
//   nx2alarm-vk bc                        decodes a BC1 and a BC3 block with a compute shader and
//                                         checks the sampled texel (textureCompressionBC proof)
//
// The output is the evidence for M2 in docs/M2_VERIFICATION.md: the driver is Turnip on the
// Adreno 630, the presentation path is the same VkSurfaceKHR/swapchain shape the backend uses,
// and the BC test is the format gate the README describes for Switch titles.
//
// It links the Vulkan loader (-lvulkan) and SDL3, unlike the shell (which dlopens the loader):
// this binary is a hardware test, not the shell, so a missing loader is a clear startup error.
//
// SPDX-License-Identifier: MIT
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bc_sample_comp.spv.h"

#define WINDOW_W 1280
#define WINDOW_H 720
#define FRAMES_IN_FLIGHT 2

static VkInstance inst;
static VkPhysicalDevice pd;
static VkPhysicalDeviceProperties props;
static VkPhysicalDeviceMemoryProperties mem_props;
static VkDevice dev;
static VkQueue queue;
static uint32_t qf;
static VkSurfaceKHR surf;   // smoke only (SDL creates it)
static int verbose;

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "nx2alarm-vk: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

#define VKCHK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) die("%s: %s (%d)", #x, vk_result_str(_r), (int)_r); } while (0)

static const char *vk_result_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    default: return "VkResult";
    }
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int has_layer(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *ls = calloc(n ? n : 1, sizeof *ls);
    vkEnumerateInstanceLayerProperties(&n, ls);
    for (uint32_t i = 0; i < n; i++) if (!strcmp(ls[i].layerName, name)) { free(ls); return 1; }
    free(ls);
    return 0;
}

// ---------------------------------------------------------------- instance / device

static VkInstance make_instance(const char *const *extra, uint32_t nextra, int want_debug) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nx2alarm-vk", 1, "nx2alarm", 1, VK_API_VERSION_1_3};
    const char *exts[16];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nextra && n < 15; i++) exts[n++] = extra[i];
    if (want_debug) exts[n++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, n, exts};
    if (want_debug && !has_layer("VK_LAYER_KHRONOS_validation")) {
        note("validation layer not installed: running without it (install vulkan-validation-layers or omit --validate)");
        want_debug = 0;
        ci.enabledExtensionCount = nextra;
    }
    if (want_debug) {
        ci.enabledLayerCount = 1;
        const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
        ci.ppEnabledLayerNames = layers;
    }
    VkInstance i;
    VKCHK(vkCreateInstance(&ci, NULL, &i));
    (void)want_debug;
    return i;
}

static void pick_device(int need_present) {
    uint32_t n = 0;
    VKCHK(vkEnumeratePhysicalDevices(inst, &n, NULL));
    if (!n) die("no Vulkan physical device (is vulkan-freedreno installed?)");
    VkPhysicalDevice *pds = calloc(n, sizeof *pds);
    VKCHK(vkEnumeratePhysicalDevices(inst, &n, pds));
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosen_qf = 0;
    for (uint32_t i = 0; i < n && chosen == VK_NULL_HANDLE; i++) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, NULL);
        VkQueueFamilyProperties *qs = calloc(nq ? nq : 1, sizeof *qs);
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qs);
        for (uint32_t q = 0; q < nq; q++) {
            if (!(qs[q].queueFlags & (need_present ? VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT))) continue;
            VkBool32 present = VK_FALSE;
            if (need_present && (vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surf, &present) != VK_SUCCESS || !present)) continue;
            chosen = pds[i];
            chosen_qf = q;
            break;
        }
        free(qs);
    }
    if (chosen == VK_NULL_HANDLE) { free(pds); die("no device with a %s queue", need_present ? "graphics+surface" : "compute"); }
    pd = chosen;
    qf = chosen_qf;
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);
    free(pds);
    note("device: %s  API %u.%u.%u  driver 0x%08x  queue family %u",
         props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
         VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, qf);
}

static void make_device(void) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
    const char *exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, NULL};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    // The M2 test does not need optional features; enable the ones the game backend needs when present, so device
    // creation fails here if a driver cannot provide them (a real Turnip can).
    f13.pNext = NULL;
    f12.pNext = NULL;
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, 1, exts, NULL};
    VkPhysicalDeviceFeatures2 want = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features w12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, NULL};
    VkPhysicalDeviceVulkan13Features w13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &w12};
    want.pNext = &w13;
    w12.timelineSemaphore = f12.timelineSemaphore;
    w12.descriptorIndexing = f12.descriptorIndexing;
    w12.imagelessFramebuffer = f12.imagelessFramebuffer;
    w12.bufferDeviceAddress = f12.bufferDeviceAddress;
    w13.dynamicRendering = f13.dynamicRendering;
    w13.synchronization2 = f13.synchronization2;
    VkPhysicalDeviceFeatures core = {0};
    core.textureCompressionBC = f2.features.textureCompressionBC;
    ci.pNext = &want;
    ci.pEnabledFeatures = &core;
    VKCHK(vkCreateDevice(pd, &ci, NULL, &dev));
    vkGetDeviceQueue(dev, qf, 0, &queue);
    note("features enabled: textureCompressionBC=%d dynamicRendering=%d sync2=%d timeline=%d descriptorIndexing=%d imagelessFB=%d",
         core.textureCompressionBC, w13.dynamicRendering, w13.synchronization2, w12.timelineSemaphore,
         w12.descriptorIndexing, w12.imagelessFramebuffer);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) return i;
    die("no memory type with flags 0x%x for bits 0x%x", (unsigned)want, bits);
    return 0;
}

static void cmd_begin(VkCommandBuffer cb) {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKCHK(vkBeginCommandBuffer(cb, &bi));
}

static void cmd_end_submit(VkCommandBuffer cb) {
    VKCHK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    VKCHK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VKCHK(vkQueueWaitIdle(queue));
}

static VkCommandPool new_pool(void) {
    VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf};
    VkCommandPool p;
    VKCHK(vkCreateCommandPool(dev, &ci, NULL, &p));
    return p;
}

static VkCommandBuffer one_cb(VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    VKCHK(vkAllocateCommandBuffers(dev, &ai, &cb));
    return cb;
}

static VkImage make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkDeviceMemory *mem) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt,
                            {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VKCHK(vkCreateImage(dev, &ci, NULL, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKCHK(vkAllocateMemory(dev, &ai, NULL, mem));
    VKCHK(vkBindImageMemory(dev, img, *mem, 0));
    return img;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src, dst, from, to,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void save_ppm(const char *path, uint32_t w, uint32_t h, const uint8_t *rgba) {
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write %s: %s", path, strerror(errno));
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t i = 0; i < w * h; i++) fwrite(&rgba[i * 4], 1, 3, f);
    if (fclose(f)) die("cannot close %s", path);
    note("wrote %s (%ux%u PPM P6)", path, w, h);
}

static void color_for_frame(uint32_t frame, VkClearColorValue *c) {
    float t = (float)(frame % 240) / 240.0f;
    c->float32[0] = 0.15f + 0.7f * fabsf(sinf(t * 6.2831853f));
    c->float32[1] = 0.15f + 0.7f * fabsf(sinf(t * 6.2831853f + 2.094f));
    c->float32[2] = 0.15f + 0.7f * fabsf(sinf(t * 6.2831853f + 4.188f));
    c->float32[3] = 1.0f;
}

// ---------------------------------------------------------------- info

static int cmd_info(void) {
    inst = make_instance(NULL, 0, 0);
    pick_device(0);
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, NULL};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, NULL};
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p11};
    VkPhysicalDeviceProperties2 px = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    vkGetPhysicalDeviceProperties2(pd, &px);

    note("\n== device\n%s  vendor 0x%04x  device 0x%04x  API %u.%u.%u", props.deviceName, props.vendorID, props.deviceID,
         VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    note("driver: %s %s  (driverID %d)", drv.driverName, drv.driverInfo, drv.driverID);
    note("limits: maxStorageBufferRange %u MiB  maxBoundDescriptorSets %u  maxColorAttachments %u  subgroupSize %u",
         props.limits.maxStorageBufferRange >> 20, props.limits.maxBoundDescriptorSets,
         props.limits.maxColorAttachments, p11.subgroupSize);
    note("memory: heap0 %llu MiB device-local (%u heaps, %u types)", (unsigned long long)(mem_props.memoryHeaps[0].size >> 20),
         mem_props.memoryHeapCount, mem_props.memoryTypeCount);

    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, NULL};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11f};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    note("\n== features the backend needs\n");
    note("textureCompressionBC %d  textureCompressionASTC_LDR %d  textureCompressionETC2 %d",
         f2.features.textureCompressionBC, f2.features.textureCompressionASTC_LDR, f2.features.textureCompressionETC2);
    note("dynamicRendering %d  synchronization2 %d  maintenance4 %d  imagelessFramebuffer %d",
         f13.dynamicRendering, f13.synchronization2, f13.maintenance4, f12.imagelessFramebuffer);
    note("timelineSemaphore %d  descriptorIndexing %d  bufferDeviceAddress %d  scalarBlockLayout %d  shaderFloat16 %d",
         f12.timelineSemaphore, f12.descriptorIndexing, f12.bufferDeviceAddress, f12.scalarBlockLayout, f12.shaderFloat16);
    note("storageBuffer16BitAccess %d  storageBuffer8BitAccess %d  geometryShader %d (transform feedback: VK_EXT_transform_feedback, see extensions)",
         f11f.storageBuffer16BitAccess, f12.storageBuffer8BitAccess, f2.features.geometryShader);

    static const VkFormat fmts[] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK,
                                    VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK};
    static const char *names[] = {"BC1", "BC3", "BC7", "BC7_sRGB", "ASTC8x8", "ETC2_RGBA8"};
    note("\n== texture formats (S=sampled, B=blit src, F=linear filter)\n");
    for (unsigned i = 0; i < sizeof fmts / sizeof *fmts; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, fmts[i], &fp);
        note("%-10s S%s B%s F%s", names[i],
             (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? " yes" : " NO ",
             (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ? " yes" : " NO ",
             (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? " yes" : " NO ");
    }

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);
    static const char *want[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_EXT_external_memory_dma_buf", "VK_KHR_external_memory_fd",
                                 "VK_EXT_descriptor_buffer", "VK_EXT_transform_feedback", "VK_EXT_custom_border_color",
                                 "VK_EXT_provoking_vertex", "VK_EXT_depth_clip_control", "VK_EXT_robustness2",
                                 "VK_EXT_index_type_uint8", "VK_EXT_vertex_attribute_divisor"};
    {
        uint32_t ni = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &ni, NULL);
        VkExtensionProperties *ie = calloc(ni ? ni : 1, sizeof *ie);
        vkEnumerateInstanceExtensionProperties(NULL, &ni, ie);
        int has_wl = 0, has_xlib = 0, has_xcb = 0;
        for (uint32_t i = 0; i < ni; i++) {
            if (!strcmp(ie[i].extensionName, "VK_KHR_wayland_surface")) has_wl = 1;
            if (!strcmp(ie[i].extensionName, "VK_KHR_xlib_surface")) has_xlib = 1;
            if (!strcmp(ie[i].extensionName, "VK_KHR_xcb_surface")) has_xcb = 1;
        }
        free(ie);
        note("\n== instance surface extensions: wayland %s  xlib %s  xcb %s", has_wl ? "yes" : "NO", has_xlib ? "yes" : "NO", has_xcb ? "yes" : "NO");
    }
    note("\n== device extensions (of %u)\n", ne);
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) {
        int found = 0;
        for (uint32_t j = 0; j < ne; j++) if (!strcmp(ex[j].extensionName, want[i])) found = 1;
        note("%s %s", found ? "yes" : "NO ", want[i]);
    }
    free(ex);

    // Hard gates of gfx_vk_core.c: a graphics+compute queue and dynamic rendering or imagelessFB+render pass 2.
    note("\n== backend gates\n");
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qs = calloc(nq, sizeof *qs);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs);
    int gfx_compute = 0;
    for (uint32_t i = 0; i < nq; i++) if ((qs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) gfx_compute = 1;
    free(qs);
    note("graphics+compute queue %s  dynamicRendering %s  => backend gate %s", gfx_compute ? "yes" : "NO",
         f13.dynamicRendering ? "yes" : "NO", (gfx_compute && f13.dynamicRendering) ? "PASS" : "CHECK");
    vkDestroyInstance(inst, NULL);
    return 0;
}

// ---------------------------------------------------------------- smoke

typedef struct {
    VkSwapchainKHR sc;
    VkFormat fmt;
    VkExtent2D ext;
    uint32_t nimg;
    VkImage img[8];
    VkImageView view[8];
    int initialized[8];
} swap_t;

static swap_t sw;
static VkCommandPool pool;
static int surface_format;   // -1 auto (prefer BGRA), 0 BGRA, 1 RGBA

static void swap_destroy(swap_t *s) {
    for (uint32_t i = 0; i < s->nimg; i++) if (s->view[i]) vkDestroyImageView(dev, s->view[i], NULL);
    if (s->sc) vkDestroySwapchainKHR(dev, s->sc, NULL);
    memset(s, 0, sizeof *s);
}

static void swap_create(SDL_Window *win) {
    VkSurfaceCapabilitiesKHR caps;
    VKCHK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    uint32_t nf = 0;
    VKCHK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
    if (!nf) die("surface reports no formats");
    VkSurfaceFormatKHR *fs = calloc(nf, sizeof *fs);
    VKCHK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fs));
    // Default: B8G8R8A8_UNORM (what Wayland/Turnip WSI usually offers first), then R8G8B8A8_UNORM. A surface that
    // offers only BGRA is exactly the case that needs the backend's conversion draw (GFX_VK_PORT.md M2b).
    int bgra = -1, rgba = -1;
    for (uint32_t i = 0; i < nf; i++) {
        if (fs[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (fs[i].format == VK_FORMAT_B8G8R8A8_UNORM && bgra < 0) bgra = (int)i;
        if (fs[i].format == VK_FORMAT_R8G8B8A8_UNORM && rgba < 0) rgba = (int)i;
    }
    int id = surface_format == 1 ? rgba : bgra;
    if (id < 0) id = bgra >= 0 ? bgra : rgba >= 0 ? rgba : 0;
    VkSurfaceFormatKHR pick = fs[id];
    {
        char list[256] = "";
        for (uint32_t i = 0; i < nf && i < 8; i++) {
            char one[40];
            snprintf(one, sizeof one, "%s%d", i ? ", " : "", (int)fs[i].format);
            strncat(list, one, sizeof list - strlen(list) - 1);
        }
        note("surface formats offered: %s%s", list, nf > 8 ? ", ..." : "");
    }
    free(fs);

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        ext.width = (uint32_t)(w > 0 ? w : WINDOW_W);
        ext.height = (uint32_t)(h > 0 ? h : WINDOW_H);
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    uint32_t n = caps.minImageCount + 1;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surf, n, pick.format, pick.colorSpace,
                                   ext, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                   VK_SHARING_MODE_EXCLUSIVE, 0, NULL, caps.currentTransform,
                                   VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE};
    VKCHK(vkCreateSwapchainKHR(dev, &ci, NULL, &sw.sc));
    sw.fmt = pick.format;
    sw.ext = ext;
    uint32_t ni = 8;
    VKCHK(vkGetSwapchainImagesKHR(dev, sw.sc, &ni, sw.img));
    sw.nimg = ni;
    for (uint32_t i = 0; i < ni; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, sw.img[i], VK_IMAGE_VIEW_TYPE_2D, sw.fmt,
                                    {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKCHK(vkCreateImageView(dev, &vi, NULL, &sw.view[i]));
    }
    note("swapchain: %ux%u  %u images  format %d (%s)  present mode FIFO", ext.width, ext.height, ni, (int)sw.fmt,
         sw.fmt == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8_UNORM" : sw.fmt == VK_FORMAT_R8G8B8A8_UNORM ? "R8G8B8A8_UNORM" : "other");
}

static int cmd_smoke(int frames, const char *shot, int validate) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) die("SDL_Init: %s", SDL_GetError());
    SDL_Window *win = SDL_CreateWindow("nx2alarm vk-smoke", WINDOW_W, WINDOW_H, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) die("SDL_CreateWindow: %s", SDL_GetError());
    if (!SDL_Vulkan_LoadLibrary(NULL)) die("SDL_Vulkan_LoadLibrary: %s", SDL_GetError());
    Uint32 next = 0;
    char const *const *exts = SDL_Vulkan_GetInstanceExtensions(&next);
    if (!exts || !next) die("SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
    note("SDL window: %s, Vulkan instance extensions: %u", SDL_GetCurrentVideoDriver(), next);
    inst = make_instance(exts, next, validate);
    if (!SDL_Vulkan_CreateSurface(win, inst, NULL, &surf)) die("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    note("VkSurfaceKHR created");
    pick_device(1);
    make_device();
    note("device ready");
    pool = new_pool();
    VkCommandBuffer cb = one_cb(pool);
    VkSemaphore avail = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHK(vkCreateSemaphore(dev, &si, NULL, &avail));
    VKCHK(vkCreateSemaphore(dev, &si, NULL, &done));
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VKCHK(vkCreateFence(dev, &fi, NULL, &fence));

    swap_create(win);
    note("swapchain ready; presenting...");
    uint64_t t0 = now_ns();
    uint32_t rendered = 0;
    int quit = 0;
    for (int frame = 0; !frames || frame < frames; frame++) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) quit = 1;
            if (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_Q)) quit = 1;
            if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                vkDeviceWaitIdle(dev);
                swap_destroy(&sw);
            }
        }
        if (quit) break;
        if (!sw.sc) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(win, &w, &h);
            if (w <= 0 || h <= 0) { SDL_Delay(16); frame--; continue; }
            swap_create(win);
        }
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, sw.sc, 1000000000ull, avail, VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(dev); swap_destroy(&sw); continue; }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) die("vkAcquireNextImageKHR: %s (%d)", vk_result_str(ar), (int)ar);
        if (idx >= sw.nimg) die("acquire returned image %u of %u", idx, sw.nimg);

        VkClearColorValue c;
        color_for_frame((uint32_t)frame, &c);
        VKCHK(vkResetCommandBuffer(cb, 0));
        cmd_begin(cb);
        image_barrier(cb, sw.img[idx], sw.initialized[idx] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, sw.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
        image_barrier(cb, sw.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VKCHK(vkEndCommandBuffer(cb));
        sw.initialized[idx] = 1;

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &avail, &wait_stage, 1, &cb, 1, &done};
        VKCHK(vkResetFences(dev, 1, &fence));
        VKCHK(vkQueueSubmit(queue, 1, &sub, fence));
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &done, 1, &sw.sc, &idx, NULL};
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        VKCHK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(dev);
            swap_destroy(&sw);
        } else if (pr != VK_SUCCESS) die("vkQueuePresentKHR: %s", vk_result_str(pr));
        rendered++;
    }
    uint64_t t1 = now_ns();
    double secs = (double)(t1 - t0) / 1e9;
    note("presented %u frame(s) in %.3f s (%.1f fps, includes startup/first-use costs)", rendered, secs, rendered / (secs > 0 ? secs : 1));

    if (shot) {
        // Pattern upload + readback (independent of the swapchain): proves the transfer path and gives the
        // screenshot artifact a recognizable frame instead of a solid clear colour.
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImage img = make_image(WINDOW_W, WINDOW_H, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &mem);
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)WINDOW_W * WINDOW_H * 4,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkBuffer srcbuf;
        VKCHK(vkCreateBuffer(dev, &bci, NULL, &srcbuf));
        VkMemoryRequirements br;
        vkGetBufferMemoryRequirements(dev, srcbuf, &br);
        VkDeviceMemory srcmem = VK_NULL_HANDLE;
        VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, br.size,
                                    mem_type(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHK(vkAllocateMemory(dev, &bai, NULL, &srcmem));
        VKCHK(vkBindBufferMemory(dev, srcbuf, srcmem, 0));
        uint8_t *px = NULL;
        VKCHK(vkMapMemory(dev, srcmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
        for (uint32_t y = 0; y < WINDOW_H; y++)
            for (uint32_t x = 0; x < WINDOW_W; x++) {
                uint8_t *q = &px[(y * WINDOW_W + x) * 4];
                q[0] = (uint8_t)(x * 255 / (WINDOW_W - 1));
                q[1] = (uint8_t)(y * 255 / (WINDOW_H - 1));
                q[2] = (uint8_t)(96 + 64 * (0.5 + 0.5 * sinf((x + y) * 0.05f)));
                q[3] = 255;
            }
        vkUnmapMemory(dev, srcmem);
        VkBufferCreateInfo rbci = bci;
        rbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer rbuf;
        VKCHK(vkCreateBuffer(dev, &rbci, NULL, &rbuf));
        VkMemoryRequirements rbr;
        vkGetBufferMemoryRequirements(dev, rbuf, &rbr);
        VkDeviceMemory rmem = VK_NULL_HANDLE;
        VkMemoryAllocateInfo rai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rbr.size,
                                    mem_type(rbr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHK(vkAllocateMemory(dev, &rai, NULL, &rmem));
        VKCHK(vkBindBufferMemory(dev, rbuf, rmem, 0));
        VKCHK(vkResetCommandBuffer(cb, 0));
        cmd_begin(cb);
        image_barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {WINDOW_W, WINDOW_H, 1}};
        vkCmdCopyBufferToImage(cb, srcbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        image_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &region);
        cmd_end_submit(cb);
        void *mapped = NULL;
        VKCHK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, &mapped));
        save_ppm(shot, WINDOW_W, WINDOW_H, mapped);
        vkUnmapMemory(dev, rmem);
        vkDestroyBuffer(dev, rbuf, NULL);
        vkFreeMemory(dev, rmem, NULL);
        vkDestroyBuffer(dev, srcbuf, NULL);
        vkFreeMemory(dev, srcmem, NULL);
        vkDestroyImage(dev, img, NULL);
        vkFreeMemory(dev, mem, NULL);
    }

    swap_destroy(&sw);
    vkDeviceWaitIdle(dev);
    vkDestroySemaphore(dev, avail, NULL);
    vkDestroySemaphore(dev, done, NULL);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    SDL_Vulkan_DestroySurface(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);
    SDL_Vulkan_UnloadLibrary();
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (quit) { note("quit by the user"); }
    return 0;
}

// ---------------------------------------------------------------- BC decode test

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } bc_img_t;

static int run_bc_format(const char *name, VkFormat fmt, const uint8_t *block, uint32_t block_bytes, uint32_t expect_rgba) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, fmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        note("%s: format not sampleable: SKIP", name);
        return 0;
    }
    VkCommandPool cp = new_pool();
    VkCommandBuffer cb = one_cb(cp);

    // --- 4x4 compressed image with a known block
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    VkImage img = make_image(4, 4, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &img_mem);
    VkDeviceMemory stage_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, block_bytes,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer stage;
    VKCHK(vkCreateBuffer(dev, &bci, NULL, &stage));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, stage, &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHK(vkAllocateMemory(dev, &mai, NULL, &stage_mem));
    VKCHK(vkBindBufferMemory(dev, stage, stage_mem, 0));
    void *m = NULL;
    VKCHK(vkMapMemory(dev, stage_mem, 0, VK_WHOLE_SIZE, 0, &m));
    memcpy(m, block, block_bytes);
    vkUnmapMemory(dev, stage_mem);

    VKCHK(vkResetCommandBuffer(cb, 0));
    cmd_begin(cb);
    image_barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
    vkCmdCopyBufferToImage(cb, stage, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    cmd_end_submit(cb);
    vkDestroyBuffer(dev, stage, NULL);
    vkFreeMemory(dev, stage_mem, NULL);

    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt,
                                 {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    VKCHK(vkCreateImageView(dev, &vci, NULL, &view));
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                               VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 0, VK_FALSE,
                               VK_COMPARE_OP_NEVER, 0, 0, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    VkSampler sampler;
    VKCHK(vkCreateSampler(dev, &sci, NULL, &sampler));

    // --- output storage buffer
    VkBufferCreateInfo obci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer outb;
    VKCHK(vkCreateBuffer(dev, &obci, NULL, &outb));
    VkMemoryRequirements oreq;
    vkGetBufferMemoryRequirements(dev, outb, &oreq);
    VkDeviceMemory outmem;
    VkMemoryAllocateInfo omai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, oreq.size,
                                 mem_type(oreq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHK(vkAllocateMemory(dev, &omai, NULL, &outmem));
    VKCHK(vkBindBufferMemory(dev, outb, outmem, 0));

    // --- compute pipeline
    VkDescriptorSetLayoutBinding binds[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dlci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, binds};
    VkDescriptorSetLayout dsl;
    VKCHK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    VkPipelineLayout pl;
    VKCHK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof bc_sample_comp_spv, bc_sample_comp_spv};
    VkShaderModule sm;
    VKCHK(vkCreateShaderModule(dev, &smci, NULL, &sm));
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL},
                                        pl, VK_NULL_HANDLE, -1};
    VkPipeline pipe;
    VKCHK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps};
    VkDescriptorPool dp;
    VKCHK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
    VkDescriptorSet ds;
    VKCHK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorImageInfo di = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo dbi = {outb, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &di, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL},
    };
    vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);

    VKCHK(vkResetCommandBuffer(cb, 0));
    cmd_begin(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    cmd_end_submit(cb);

    void *out = NULL;
    VKCHK(vkMapMemory(dev, outmem, 0, VK_WHOLE_SIZE, 0, &out));
    float texel[4];
    memcpy(texel, out, sizeof texel);
    vkUnmapMemory(dev, outmem);
    int r = (int)(texel[0] * 255.0f + 0.5f), g = (int)(texel[1] * 255.0f + 0.5f),
        b = (int)(texel[2] * 255.0f + 0.5f), a = (int)(texel[3] * 255.0f + 0.5f);
    int want_r = (expect_rgba >> 24) & 0xff, want_g = (expect_rgba >> 16) & 0xff, want_b = (expect_rgba >> 8) & 0xff, want_a = expect_rgba & 0xff;
    int ok = abs(r - want_r) <= 3 && abs(g - want_g) <= 3 && abs(b - want_b) <= 3 && abs(a - want_a) <= 3;
    note("%-4s sampled texel rgba(%d,%d,%d,%d), expected rgba(%d,%d,%d,%d): %s", name, r, g, b, a, want_r, want_g, want_b, want_a,
         ok ? "PASS" : "FAIL");

    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroySampler(dev, sampler, NULL);
    vkDestroyImageView(dev, view, NULL);
    vkDestroyImage(dev, img, NULL);
    vkFreeMemory(dev, img_mem, NULL);
    vkDestroyBuffer(dev, outb, NULL);
    vkFreeMemory(dev, outmem, NULL);
    vkDestroyCommandPool(dev, cp, NULL);
    return ok ? 0 : 1;
}

static int cmd_bc(void) {
    inst = make_instance(NULL, 0, 0);
    pick_device(0);
    make_device();
    // BC1: color0 = 0xF800 (565 red), color1 = 0x001F (565 blue), all indices 0 => 4x4 solid red.
    static const uint8_t bc1[8] = {0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00};
    // BC3: alpha block first (eight 0xFF bytes = alpha 1), then the BC1 colour block => opaque red.
    static const uint8_t bc3[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00};
    note("== BC texture decode (compute shader sampling a 4x4 block)");
    int bad = 0;
    bad |= run_bc_format("BC1", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, bc1, sizeof bc1, 0xFF0000FFu);
    bad |= run_bc_format("BC3", VK_FORMAT_BC3_UNORM_BLOCK, bc3, sizeof bc3, 0xFF0000FFu);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    note("BC decode: %s", bad ? "FAIL" : "PASS (BC1 and BC3 sampled as red, driver textureCompressionBC=yes)");
    return bad ? 1 : 0;
}

// ---------------------------------------------------------------- main

static void usage(void) {
    printf("usage: nx2alarm-vk <command> [options]\n"
           "\n"
           "commands:\n"
           "  info                                   Vulkan device, features, BC formats, extensions\n"
           "  smoke [--frames N] [--screenshot FILE] [--format bgra|rgba] [--validate]\n"
           "                                         SDL3 window + VkSurfaceKHR + swapchain; clears and\n"
           "                                         presents N frames (1-N per frame, 0/N = until quit),\n"
           "                                         optionally writes the last clear colour as a PPM\n"
           "  bc                                     sample a BC1/BC3 block with a compute shader\n"
           "\n"
           "This is the M2 hardware test tool; it needs a Vulkan ICD (vulkan-freedreno) and, for\n"
           "smoke, a display (Wayland/X11).\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return argc < 2 ? 2 : 0; }
    if (!strcmp(argv[1], "info")) return cmd_info();
    if (!strcmp(argv[1], "bc")) return cmd_bc();
    if (!strcmp(argv[1], "smoke")) {
        int frames = 120, validate = 0;
        const char *shot = NULL;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
            else if (!strcmp(argv[i], "--validate")) validate = 1;
            else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
                const char *v = argv[++i];
                if (!strcmp(v, "bgra")) surface_format = 0;
                else if (!strcmp(v, "rgba")) surface_format = 1;
                else if (!strcmp(v, "auto")) surface_format = -1;
                else { fprintf(stderr, "nx2alarm-vk: --format wants bgra, rgba or auto\n"); return 2; }
            }
            else if (!strcmp(argv[i], "-v")) verbose = 1;
            else { fprintf(stderr, "nx2alarm-vk: unknown option %s\n", argv[i]); usage(); return 2; }
        }
        if (frames < 1) frames = 1;
        return cmd_smoke(frames, shot, validate);
    }
    fprintf(stderr, "nx2alarm-vk: unknown command %s\n", argv[1]);
    usage();
    return 2;
}
