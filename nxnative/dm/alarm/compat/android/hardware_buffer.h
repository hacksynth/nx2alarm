// nx2alarm Linux shim: the AHardwareBuffer API the Vulkan backend names (dm/android/gfx_vk_core.c, gfx_vk_present.c).
//
// The backend's zero-copy memory pools (gfx_vk_core.c pool_zero_copy, slab_new, zc_uncached) are built on an AHardwareBuffer
// whose dma-buf is mmapped MAP_FIXED over the game's own pages and imported as a VkBuffer. Linux has no gralloc/allocator
// service and no VK_ANDROID_external_memory_android_hardware_buffer, so on the ARM Linux port allocation always fails and the
// backend's existing fallback runs: the pool keeps its anonymous pages and gets a GPU copy (VkBuffer + host-visible
// memory) that vk_pools_sync refreshes before every submission (gfx_vk_core.c "A pool the allocator refuses keeps its
// anonymous pages and gets a GPU copy refreshed before every submission"). Correct, but slow: every submission copies the
// dirty 64 KB blocks and every readback merges against a baseline.
//
// M2.1 replaces the failure with the real Linux path: /dev/dma_heap/system or /dev/dma_heap/system-uncached through
// DMA_HEAP_IOCTL_ALLOC, mmap MAP_FIXED over the guest pages, and the dma-buf fd imported with
// VK_EXT_external_memory_dma_buf + VK_KHR_external_memory_fd (see nxnative/dm/alarm/GFX_VK_PORT.md, "Memory pools").
// Only compiled for DM_ALARM: the Android build uses the NDK's <android/hardware_buffer.h>.
#pragma once
#include <stdint.h>

typedef struct AHardwareBuffer AHardwareBuffer;

// NDK layout (android/hardware_buffer.h, AHardwareBuffer_Desc): the backend fills every field with a positional initializer.
typedef struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

enum {
    AHARDWAREBUFFER_FORMAT_BLOB = 0x21,
};

// The two usage bits the backend asks for (AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN / CPU_WRITE_OFTEN / GPU_DATA_BUFFER); the
// values follow the NDK so a future dma-buf implementation can pass them through unchanged.
enum {
    AHARDWAREBUFFER_USAGE_CPU_READ_NEVER = 0,
    AHARDWAREBUFFER_USAGE_CPU_READ_RARELY = 1u << 0,
    AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN = 1u << 1,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER = 0,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY = 1u << 2,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN = 1u << 3,
    AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER = 1u << 10,
};

// Always refused: the backend then keeps anonymous guest pages plus a copied GPU buffer (see the header comment).
// Returns a negative errno-style value, as the NDK's implementation does on failure.
static inline int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out) {
    (void)desc;
    if (out) *out = 0;
    return -1;
}
static inline void AHardwareBuffer_release(AHardwareBuffer *buffer) { (void)buffer; }
// No gralloc handle to share; the backend only calls this after a successful allocate (never here).
static inline int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer, int socketFd) { (void)buffer; (void)socketFd; return -1; }
