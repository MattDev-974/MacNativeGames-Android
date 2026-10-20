/* vk.h - Android Vulkan device, swapchain and command sink.
 *
 * This is the Android counterpart of gfx/nvn/nvn_mtl.m's *submission* half:
 * a device, a surface-backed swapchain, one command buffer per frame and the
 * handful of vkCmd* entries the NVN replay needs.  It deliberately mirrors the
 * shape of the Metal executor so the rest of the port can be moved over one
 * seam at a time:
 *
 *     Metal (macOS/iOS)              Vulkan (Android)
 *     ---------------------------    ----------------------------------
 *     MTLDevice                    -> VkDevice + VkPhysicalDevice
 *     CAMetalLayer / VK_KHR_surface-> VkSwapchainKHR
 *     MTLCommandQueue/CommandBuffer-> VkQueue + VkCommandBuffer
 *     MTL4RenderCommandEncoder     -> VkRenderPass + framebuffer + vkCmd*
 *     MTLSharedEvent / fence       -> VkFence + VkSemaphore
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <android/native_window.h>

/* Result of vk_bringup(); everything below is a no-op until it returns 0. */
int  vk_bringup(ANativeWindow *window);
void vk_teardown(void);
bool vk_ready(void);

/* The Android surface dies with the window; the device does not.  suspend()
 * parks the swapchain when APP_CMD_TERM_WINDOW arrives, resume() rebuilds it
 * for the new ANativeWindow without paying for a full device bring-up. */
void vk_suspend(void);
int  vk_resume(ANativeWindow *window);

/* Surface size changed (ANativeWindow geometry); recreated at the next begin. */
void vk_invalidate_swapchain(void);
int  vk_width(void);
int  vk_height(void);

/* One frame: begin(clear) -> [vk_cmd_*] -> end().  begin() acquires an image
 * and opens the render pass; the clear colour comes from the NVN command
 * stream, exactly like nvnCommandBufferClearColor defines a Metal pass. */
int  vk_frame_begin(float r, float g, float b, float a);
int  vk_frame_end(void);

/* NVN replay sinks.  These are the Vulkan twins of mtl_sub_* / the encoder
 * calls in nvn_mtl.m; addresses are guest GPU virtual addresses, resolved
 * through vk_pool_add() below. */
void vk_cmd_bind_vertex_buffer(uint64_t gpu_addr, uint32_t bytes);
void vk_cmd_set_uniform(uint64_t gpu_addr);
void vk_cmd_draw(uint32_t first, uint32_t count);
void vk_cmd_viewport(float x, float y, float w, float h);

/* Zero-copy memory pools: [base, base+size) becomes GPU-visible.  Vulkan has
 * no unified address space, so the pool is uploaded once into a VkBuffer and
 * guest addresses are rebased by (addr - base).  The real port replaces this
 * with VK_EXT_external_memory_host + a shared VkBuffer alias per pool. */
int  vk_pool_add(void *base, uint64_t size);
void vk_pool_del(void *base);
int  vk_pool_translate(uint64_t gpu_addr, uint64_t len, uint64_t *offset_out);

/* Vertex/uniform storage owned by the demo backend. */
typedef struct {
    uint64_t vertex_va;   /* guest address the game would pass to nvnCommandBufferBindVertexBuffer */
    uint64_t uniform_va;  /* guest address of the uniform block */
    uint32_t vertex_count;
} vk_demo_layout;
void vk_demo_bind(vk_demo_layout *layout);
