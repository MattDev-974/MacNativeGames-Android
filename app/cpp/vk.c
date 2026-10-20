/* vk.c - Vulkan 1.1 bring-up and the per-frame command sink.
 *
 * Deliberately dependency-free (no VMA, no volk): the Android port has to stay
 * buildable with the NDK alone, the same way the macOS side only needs Xcode.
 * This is the "graphics executor" slice of the port; see docs/03-nvn-vulkan.md.
 */
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

#include "vk.h"

#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <stdlib.h>

#include "shaders_spv.h"

#define TAG "mnge-vk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MAX_FRAMES_IN_FLIGHT 2
#define UNIFORM_BYTES 256u
#define POOL_BYTES   (128u * 1024u)

typedef struct {
    uint64_t base;
    uint64_t size;
} PoolEntry;

typedef struct {
    VkInstance        instance;
    VkPhysicalDevice  phys;
    VkDevice          dev;
    VkQueue           queue;
    uint32_t          qfam;
    VkSurfaceKHR      surface;

    VkSwapchainKHR    swap;
    VkFormat          format;
    VkColorSpaceKHR   colorspace;
    VkExtent2D        extent;
    uint32_t          nimg;
    VkImage           img[8];
    VkImageView       img_view[8];
    VkFramebuffer     img_fb[8];

    VkRenderPass      rp;
    VkCommandPool     cpool;
    VkCommandBuffer   cmd[MAX_FRAMES_IN_FLIGHT];

    VkSemaphore       sem_acquire[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore       sem_render[8];
    VkFence           fence[MAX_FRAMES_IN_FLIGHT];
    VkFence           fence_acq[MAX_FRAMES_IN_FLIGHT];
    bool              acq_fence;
    bool              dead;

    VkDescriptorSetLayout dsl;
    VkDescriptorPool      dpool;
    VkDescriptorSet       dset[MAX_FRAMES_IN_FLIGHT];
    VkPipelineLayout      playout;
    VkPipeline            pipe;

    VkBuffer       frame_buf[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory frame_mem[MAX_FRAMES_IN_FLIGHT];
    void          *frame_map[MAX_FRAMES_IN_FLIGHT];

    PoolEntry      pools[32];
    int            npools;

    ANativeWindow *window;
    int            width, height;      /* swapchain extent */
    int            win_w, win_h;       /* last surface size we reacted to */
    bool           swap_dirty;

    uint32_t       frame;
    uint32_t       image_index;
    bool           in_frame;
    bool           skip_pass;

    vk_demo_layout demo;
} VkCtx;

static VkCtx g;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int prop_int(const char *key, int def)
{
    char v[PROP_VALUE_MAX];
    int n = __system_property_get(key, v);
    return n > 0 ? atoi(v) : def;
}

/* ------------------------------------------------------------------ utils */

static uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer *buf, VkDeviceMemory *mem, void **map)
{
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g.dev, &bi, NULL, buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, *buf, &req);
    uint32_t type = find_memory(req.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;

    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(g.dev, &ai, NULL, mem) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(g.dev, *buf, *mem, 0) != VK_SUCCESS) return false;
    if (map && vkMapMemory(g.dev, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) return false;
    return true;
}

static void destroy_buffer(VkBuffer *buf, VkDeviceMemory *mem)
{
    if (*buf) vkDestroyBuffer(g.dev, *buf, NULL);
    if (*mem) vkFreeMemory(g.dev, *mem, NULL);
    *buf = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
}

/* --------------------------------------------------------------- swapchain */

static void destroy_swapchain(void)
{
    for (uint32_t i = 0; i < g.nimg; i++) {
        if (g.img_fb[i])     vkDestroyFramebuffer(g.dev, g.img_fb[i], NULL);
        if (g.img_view[i])   vkDestroyImageView(g.dev, g.img_view[i], NULL);
        if (g.sem_render[i]) vkDestroySemaphore(g.dev, g.sem_render[i], NULL);
        g.img_fb[i] = VK_NULL_HANDLE;
        g.img_view[i] = VK_NULL_HANDLE;
        g.sem_render[i] = VK_NULL_HANDLE;
    }
    g.nimg = 0;
    if (g.swap) { vkDestroySwapchainKHR(g.dev, g.swap, NULL); g.swap = VK_NULL_HANDLE; }
}

/* The render pass is format-bound, and the format is a property of the
 * surface, not of the swapchain: pick it once, before anything is created.
 * (Creating a throwaway swapchain just to learn the format breaks some Mali
 * blobs: the created-then-destroyed swapchain leaves the device unusable.) */
static bool pick_format(void)
{
    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &nfmt, NULL);
    if (nfmt == 0) return false;
    VkSurfaceFormatKHR *fmts = calloc(nfmt, sizeof(*fmts));
    if (!fmts) return false;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &nfmt, fmts);

    LOGI("surface offers %u format(s):", nfmt);
    for (uint32_t i = 0; i < nfmt; i++)
        LOGI("  fmt[%u] = VkFormat %d, colour space %d", i, (int)fmts[i].format,
             (int)fmts[i].colorSpace);

    int forced = prop_int("debug.mnge.fmtidx", -1);
    VkSurfaceFormatKHR pick = fmts[0];
    int have_pick = 0;
    if (forced >= 0 && (uint32_t)forced < nfmt) { pick = fmts[forced]; have_pick = 1; }
    for (uint32_t i = 0; i < nfmt && !have_pick; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { pick = fmts[i]; have_pick = 1; }
    for (uint32_t i = 0; i < nfmt && !have_pick; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fmts[i]; have_pick = 1; }
    free(fmts);

    if (pick.format == VK_FORMAT_UNDEFINED) pick.format = VK_FORMAT_R8G8B8A8_UNORM;
    g.format = pick.format;
    g.colorspace = pick.colorSpace;
    LOGI("chosen format %d, colour space %d", (int)g.format, (int)g.colorspace);
    return true;
}

static bool create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        ext.width  = (uint32_t)g.width;
        ext.height = (uint32_t)g.height;
    }
    /* At APP_CMD_INIT_WINDOW the surface is often not laid out yet and reports
     * 1x1.  Building a swapchain at that size gives a black screen forever, so
     * wait for a real size and retry on the next frame. */
    if (ext.width < 2 || ext.height < 2) return false;

    LOGI("caps: minImageCount %u, maxImageCount %u, currentExtent %ux%u", caps.minImageCount,
         caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height);
    LOGI("caps: transforms supported %#x current %#x, compositeAlpha %#x, usageFlags %#x",
         caps.supportedTransforms, caps.currentTransform, caps.supportedCompositeAlpha,
         caps.supportedUsageFlags);
    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = g.surface;
    ci.minImageCount = want;
    ci.imageFormat = g.format;
    ci.imageColorSpace = g.colorspace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    /* debug.mnge.idtransform=1 forces IDENTITY, which is legal but lets the
     * compositor rotate; some Mali blobs mishandle it when the panel is 90deg. */
    ci.preTransform = prop_int("debug.mnge.idtransform", 0)
                      ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                      : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                        : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & 0xf);
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(g.dev, &ci, NULL, &g.swap) != VK_SUCCESS) return false;
    g.extent = ext;

    /* vkGetSwapchainImagesKHR is an in/out count: the second call needs the
     * capacity as input.  Passing a zeroed g.nimg here reports zero images,
     * no framebuffer is ever created, and the first render pass dereferences
     * VK_NULL_HANDLE. */
    uint32_t n = 0;
    if (vkGetSwapchainImagesKHR(g.dev, g.swap, &n, NULL) != VK_SUCCESS || n == 0)
        return false;
    if (n > 8) n = 8;
    g.nimg = n;
    if (vkGetSwapchainImagesKHR(g.dev, g.swap, &g.nimg, g.img) != VK_SUCCESS)
        return false;
    if (g.nimg == 0) return false;

    VkImageViewCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = g.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;

    VkSemaphoreCreateInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < g.nimg; i++) {
        vi.image = g.img[i];
        if (vkCreateImageView(g.dev, &vi, NULL, &g.img_view[i]) != VK_SUCCESS) return false;
        if (vkCreateSemaphore(g.dev, &si, NULL, &g.sem_render[i]) != VK_SUCCESS) return false;

        VkFramebufferCreateInfo fi = {0};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = g.rp;
        fi.attachmentCount = 1;
        fi.pAttachments = &g.img_view[i];
        fi.width = ext.width;
        fi.height = ext.height;
        fi.layers = 1;
        if (vkCreateFramebuffer(g.dev, &fi, NULL, &g.img_fb[i]) != VK_SUCCESS) return false;
    }

    g.width  = (int)ext.width;
    g.height = (int)ext.height;
    g.swap_dirty = false;
    LOGI("swapchain %ux%u fmt=%d images=%u", ext.width, ext.height, (int)g.format, g.nimg);
    return true;
}

/* ----------------------------------------------------------------- objects */

static bool create_render_pass(void)
{
    VkAttachmentDescription color = {0};
    color.format = g.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep[2] = {{0}};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp = {0};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    return vkCreateRenderPass(g.dev, &rp, NULL, &g.rp) == VK_SUCCESS;
}

static bool create_pipeline(void)
{
    VkDescriptorSetLayoutBinding b = {0};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo dl = {0};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 1;
    dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(g.dev, &dl, NULL, &g.dsl) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.dsl;
    if (vkCreatePipelineLayout(g.dev, &pl, NULL, &g.playout) != VK_SUCCESS) return false;

    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm = {0};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

    if (tri_vert_spv_len & 3u) { LOGE("vertex SPIR-V is not word aligned"); return false; }
    sm.codeSize = tri_vert_spv_len;
    sm.pCode = tri_vert_spv;
    if (vkCreateShaderModule(g.dev, &sm, NULL, &vm) != VK_SUCCESS) return false;
    if (tri_frag_spv_len & 3u) { LOGE("fragment SPIR-V is not word aligned"); return false; }
    sm.codeSize = tri_frag_spv_len;
    sm.pCode = tri_frag_spv;
    if (vkCreateShaderModule(g.dev, &sm, NULL, &fm) != VK_SUCCESS) return false;

    VkPipelineShaderStageCreateInfo stages[2] = {{0}};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vm;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fm;

    VkVertexInputBindingDescription vbind = {0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vattr[2] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT,    0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8},
    };

    VkPipelineVertexInputStateCreateInfo vin = {0};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &vbind;
    vin.vertexAttributeDescriptionCount = 2;
    vin.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vs = {0};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1;
    vs.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState ba = {0};
    ba.colorWriteMask = 0xf;
    ba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp = {0};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vs;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = g.playout;
    gp.renderPass = g.rp;
    gp.subpass = 0;

    VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &gp, NULL, &g.pipe);
    vkDestroyShaderModule(g.dev, vm, NULL);
    vkDestroyShaderModule(g.dev, fm, NULL);
    return r == VK_SUCCESS;
}

static bool create_frame_resources(void)
{
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo dpi = {0};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = MAX_FRAMES_IN_FLIGHT;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(g.dev, &dpi, NULL, &g.dpool) != VK_SUCCESS) return false;

    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT] = {g.dsl, g.dsl};
    VkDescriptorSetAllocateInfo dsa = {0};
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = g.dpool;
    dsa.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    dsa.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(g.dev, &dsa, g.dset) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo ca = {0};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = g.cpool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    VkSemaphoreCreateInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkAllocateCommandBuffers(g.dev, &ca, &g.cmd[i]) != VK_SUCCESS) return false;
        if (vkCreateSemaphore(g.dev, &si, NULL, &g.sem_acquire[i]) != VK_SUCCESS) return false;
        if (vkCreateFence(g.dev, &fi, NULL, &g.fence[i]) != VK_SUCCESS) return false;
        fi.flags = 0;
        if (vkCreateFence(g.dev, &fi, NULL, &g.fence_acq[i]) != VK_SUCCESS) return false;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!create_buffer(POOL_BYTES + UNIFORM_BYTES,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           &g.frame_buf[i], &g.frame_mem[i], &g.frame_map[i]))
            return false;

        VkDescriptorBufferInfo dbi = {g.frame_buf[i], 0, UNIFORM_BYTES};
        VkWriteDescriptorSet w = {0};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = g.dset[i];
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
    }
    return true;
}

/* ------------------------------------------------------------------ public */

bool vk_ready(void) { return g.dev != VK_NULL_HANDLE && g.swap != VK_NULL_HANDLE; }
int  vk_width(void)  { return g.width; }
int  vk_height(void) { return g.height; }

void vk_invalidate_swapchain(void)
{
    pthread_mutex_lock(&g_lock);
    g.swap_dirty = true;
    pthread_mutex_unlock(&g_lock);
}

int vk_pool_add(void *base, uint64_t size)
{
    if (g.npools >= (int)(sizeof(g.pools) / sizeof(g.pools[0]))) return 0;
    g.pools[g.npools].base = (uint64_t)(uintptr_t)base;
    g.pools[g.npools].size = size;
    g.npools++;
    return 1;
}

void vk_pool_del(void *base)
{
    for (int i = 0; i < g.npools; i++) {
        if (g.pools[i].base == (uint64_t)(uintptr_t)base) {
            g.pools[i] = g.pools[--g.npools];
            return;
        }
    }
}

/* Guest GPU address -> offset inside the demo's uploaded copy of its pool.
 * A real port aliases the pool (VK_EXT_external_memory_host / AHardwareBuffer)
 * instead of copying, so the CPU never duplicates the game's 17 GB of data. */
int vk_pool_translate(uint64_t gpu_addr, uint64_t len, uint64_t *offset_out)
{
    for (int i = 0; i < g.npools; i++) {
        uint64_t base = g.pools[i].base;
        if (gpu_addr >= base && gpu_addr - base <= g.pools[i].size &&
            len <= g.pools[i].size - (gpu_addr - base)) {
            *offset_out = gpu_addr - base;
            return 1;
        }
    }
    return 0;
}

int vk_bringup(ANativeWindow *window)
{
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (g.dev) { pthread_mutex_unlock(&g_lock); return 0; }
    memset(&g, 0, sizeof(g));
    g.window = window;
    g.width  = ANativeWindow_getWidth(window);
    g.height = ANativeWindow_getHeight(window);
    g.win_w  = g.width;
    g.win_h  = g.height;

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MacNativeGames";
    app.applicationVersion = 1;
    app.pEngineName = "mnge-android";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_1;

    const char *ext[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = ext;
    LOGI("step 0/9: pre-init (clearonly=%d nopipe=%d fmtidx=%d idtransform=%d emptysubmit=%d)",
         prop_int("debug.mnge.clearonly", 0), prop_int("debug.mnge.nopipe", 0),
         prop_int("debug.mnge.fmtidx", -1), prop_int("debug.mnge.idtransform", 0),
         prop_int("debug.mnge.emptysubmit", 0));
    LOGI("step 1/9: vkCreateInstance (apiVersion 1.1)");
    VkResult r0 = vkCreateInstance(&ici, NULL, &g.instance);
    if (r0 != VK_SUCCESS) {
        /* Vulkan 1.0-only drivers are rare on Android but exist. */
        LOGI("1.1 refused (%d), retrying with 1.0", (int)r0);
        app.apiVersion = VK_API_VERSION_1_0;
        r0 = vkCreateInstance(&ici, NULL, &g.instance);
    }
    if (r0 != VK_SUCCESS) { LOGE("vkCreateInstance failed: %d", (int)r0); goto out; }

    LOGI("step 2/9: vkCreateAndroidSurfaceKHR");
    VkAndroidSurfaceCreateInfoKHR sci = {0};
    sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    sci.window = window;
    VkResult r1 = vkCreateAndroidSurfaceKHR(g.instance, &sci, NULL, &g.surface);
    if (r1 != VK_SUCCESS) { LOGE("android surface failed: %d", (int)r1); goto out; }

    LOGI("step 3/9: enumerate devices");
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(g.instance, &ndev, NULL);
    if (!ndev) { LOGE("no Vulkan device"); goto out; }
    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    vkEnumeratePhysicalDevices(g.instance, &ndev, devs);

    bool found = false;
    for (uint32_t d = 0; d < ndev && !found; d++) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, NULL);
        VkQueueFamilyProperties *qs = calloc(nq, sizeof(*qs));
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qs);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], q, g.surface, &present);
            if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                g.phys = devs[d];
                g.qfam = q;
                found = true;
                break;
            }
        }
        free(qs);
    }
    free(devs);
    if (!found) { LOGE("no graphics+present queue family"); goto out; }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.phys, &props);
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(g.phys, NULL, &n, NULL);
        VkExtensionProperties *e = calloc(n, sizeof(*e));
        vkEnumerateDeviceExtensionProperties(g.phys, NULL, &n, e);
        LOGI("device extensions (%u):", n);
        for (uint32_t i = 0; i < n; i++)
            LOGI("  %s (rev %u)", e[i].extensionName, e[i].specVersion);
        free(e);
    }
    {
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceFeatures(g.phys, &f);
        LOGI("features: BC=%d ASTC_LDR=%d ETC2=%d", f.textureCompressionBC,
             f.textureCompressionASTC_LDR, f.textureCompressionETC2);
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.phys, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &fp);
        LOGI("format BC1_RGBA: optimal=%#x linear=%#x", fp.optimalTilingFeatures, fp.linearTilingFeatures);
        vkGetPhysicalDeviceFormatProperties(g.phys, VK_FORMAT_BC7_UNORM_BLOCK, &fp);
        LOGI("format BC7:     optimal=%#x", fp.optimalTilingFeatures);
        vkGetPhysicalDeviceFormatProperties(g.phys, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &fp);
        LOGI("format ASTC4x4: optimal=%#x", fp.optimalTilingFeatures);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
        LOGI("memory: %u heaps, %u types", mp.memoryHeapCount, mp.memoryTypeCount);
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
            LOGI("  heap[%u] %.2f GB %s", i, mp.memoryHeaps[i].size / 1e9,
                 (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "device-local" : "");
        VkPhysicalDeviceProperties p2;
        vkGetPhysicalDeviceProperties(g.phys, &p2);
        LOGI("limits: maxStorageBufferRange %u MB, maxMemoryAllocationCount %u, maxImageDimension2D %u",
             p2.limits.maxStorageBufferRange >> 20, p2.limits.maxMemoryAllocationCount,
             p2.limits.maxImageDimension2D);
    }
    LOGI("GPU: %s (Vulkan %u.%u.%u)", props.deviceName,
         VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
         VK_VERSION_PATCH(props.apiVersion));

    LOGI("step 4/9: vkCreateDevice (queue family %u)", g.qfam);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VkResult r2 = vkCreateDevice(g.phys, &dci, NULL, &g.dev);
    if (r2 != VK_SUCCESS) { LOGE("vkCreateDevice failed: %d", (int)r2); g.dev = VK_NULL_HANDLE; goto out; }
    vkGetDeviceQueue(g.dev, g.qfam, 0, &g.queue);

    VkCommandPoolCreateInfo cpi = {0};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = g.qfam;
    if (vkCreateCommandPool(g.dev, &cpi, NULL, &g.cpool) != VK_SUCCESS) goto out;

    LOGI("step 5/9: command pool ok");

    if (prop_int("debug.mnge.probe", 0)) {
        /* Isolate device+queue health from the swapchain entirely: submit an
         * empty command buffer with no semaphore and no fence. */
        VkCommandBufferAllocateInfo pai = {0};
        pai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        pai.commandPool = g.cpool;
        pai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        pai.commandBufferCount = 1;
        VkCommandBuffer pcb = VK_NULL_HANDLE;
        VkResult pr = vkAllocateCommandBuffers(g.dev, &pai, &pcb);
        LOGI("probe: alloc cmd %d", (int)pr);
        VkCommandBufferBeginInfo pbi = {0};
        pbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(pcb, &pbi);
        vkEndCommandBuffer(pcb);
        VkSubmitInfo psi = {0};
        psi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        psi.commandBufferCount = 1;
        psi.pCommandBuffers = &pcb;
        VkResult sr0 = vkQueueSubmit(g.queue, 1, &psi, VK_NULL_HANDLE);
        LOGI("probe: bare queue submit = %d", (int)sr0);
        LOGI("probe: queue wait idle = %d", (int)vkQueueWaitIdle(g.queue));
        vkFreeCommandBuffers(g.dev, g.cpool, 1, &pcb);
    }
    if (!pick_format()) { LOGE("step 6/9: no usable surface format"); goto out; }
    LOGI("step 6/9: format %d", (int)g.format);
    if (!create_render_pass()) { LOGE("step 7/9: create_render_pass failed"); goto out; }
    if (!create_swapchain()) { LOGE("step 7/9: create_swapchain failed"); goto out; }
    LOGI("step 7/9: render pass ok, swapchain %ux%u", g.extent.width, g.extent.height);
    if (!create_pipeline()) { LOGE("step 8/9: create_pipeline failed"); goto out; }
    if (!create_frame_resources()) { LOGE("step 8/9: create_frame_resources failed"); goto out; }
    LOGI("step 9/9: frames in flight ok");
    {
        /* If the device is already gone here, the fault is in swapchain
         * creation, not in anything we record. */
        VkResult idle = vkDeviceWaitIdle(g.dev);
        LOGI("device idle probe: %d (0 = still alive)", (int)idle);
    }

    LOGI("Vulkan ready: %dx%d", g.width, g.height);
    rc = 0;
out:
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void vk_suspend(void)
{
    pthread_mutex_lock(&g_lock);
    if (g.dev) {
        vkDeviceWaitIdle(g.dev);
        destroy_swapchain();
    }
    if (g.surface) { vkDestroySurfaceKHR(g.instance, g.surface, NULL); g.surface = VK_NULL_HANDLE; }
    g.in_frame = false;
    pthread_mutex_unlock(&g_lock);
}

int vk_resume(ANativeWindow *window)
{
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (!g.dev) { pthread_mutex_unlock(&g_lock); return vk_bringup(window); }

    g.window = window;
    g.width  = ANativeWindow_getWidth(window);
    g.height = ANativeWindow_getHeight(window);
    g.win_w  = g.width;
    g.win_h  = g.height;

    VkAndroidSurfaceCreateInfoKHR sci = {0};
    sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    sci.window = window;
    if (vkCreateAndroidSurfaceKHR(g.instance, &sci, NULL, &g.surface) != VK_SUCCESS) goto out;
    if (!create_swapchain()) goto out;
    rc = 0;
out:
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void vk_teardown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g.dev) {
        vkDeviceWaitIdle(g.dev);
        destroy_swapchain();
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            destroy_buffer(&g.frame_buf[i], &g.frame_mem[i]);
            if (g.sem_acquire[i]) vkDestroySemaphore(g.dev, g.sem_acquire[i], NULL);
            if (g.fence[i])       vkDestroyFence(g.dev, g.fence[i], NULL);
            if (g.fence_acq[i])   vkDestroyFence(g.dev, g.fence_acq[i], NULL);
        }
        if (g.pipe)    vkDestroyPipeline(g.dev, g.pipe, NULL);
        if (g.playout) vkDestroyPipelineLayout(g.dev, g.playout, NULL);
        if (g.dpool)   vkDestroyDescriptorPool(g.dev, g.dpool, NULL);
        if (g.dsl)     vkDestroyDescriptorSetLayout(g.dev, g.dsl, NULL);
        if (g.rp)      vkDestroyRenderPass(g.dev, g.rp, NULL);
        if (g.cpool)   vkDestroyCommandPool(g.dev, g.cpool, NULL);
        vkDestroyDevice(g.dev, NULL);
    }
    if (g.surface)  vkDestroySurfaceKHR(g.instance, g.surface, NULL);
    if (g.instance) vkDestroyInstance(g.instance, NULL);
    memset(&g, 0, sizeof(g));
    pthread_mutex_unlock(&g_lock);
}

int vk_frame_begin(float r, float gg, float b, float a)
{
    if (!g.dev) return -1;

    if (g.dead) return -1;
    if (g.swap_dirty || g.swap == VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g.dev);
        destroy_swapchain();
        if (!create_swapchain()) return 1;
    }

    if (g.window) {
        int w = ANativeWindow_getWidth(g.window);
        int h = ANativeWindow_getHeight(g.window);
        if (w > 1 && h > 1 && (w != g.win_w || h != g.win_h)) {
            g.win_w = w;
            g.win_h = h;
            g.swap_dirty = true;
        }
    }

    uint32_t slot = g.frame % MAX_FRAMES_IN_FLIGHT;
    vkWaitForFences(g.dev, 1, &g.fence[slot], VK_TRUE, UINT64_MAX);

    g.acq_fence = prop_int("debug.mnge.acqfence", 1) != 0;
    VkResult ar;
    if (g.acq_fence) {
        /* Some Mali blobs (G610 on MIUI) return VK_ERROR_DEVICE_LOST from
         * vkQueueSubmit when it waits on a WSI acquire semaphore.  Acquiring
         * with a fence and stalling the CPU instead is spec-equivalent and
         * works there. */
        ar = vkAcquireNextImageKHR(g.dev, g.swap, 1000000000ull,
                                   VK_NULL_HANDLE, g.fence_acq[slot], &g.image_index);
        if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) {
            vkWaitForFences(g.dev, 1, &g.fence_acq[slot], VK_TRUE, UINT64_MAX);
            vkResetFences(g.dev, 1, &g.fence_acq[slot]);
        }
    } else {
        ar = vkAcquireNextImageKHR(g.dev, g.swap, 1000000000ull,
                                   g.sem_acquire[slot], VK_NULL_HANDLE, &g.image_index);
    }
    if (ar == VK_TIMEOUT) return 1;   /* surface not ready; try again next tick */
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { g.swap_dirty = true; return 1; }
    {
        static int n;
        if (n++ < 3)
            LOGI("acquire: result %d, image %u of %u, slot %u", (int)ar, g.image_index,
                 g.nimg, slot);
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return -1;
    if (g.image_index >= g.nimg || !g.img_fb[g.image_index]) {
        LOGE("acquired image %u has no framebuffer (nimg=%u)", g.image_index, g.nimg);
        return -1;
    }
    vkResetFences(g.dev, 1, &g.fence[slot]);

    vkResetCommandBuffer(g.cmd[slot], 0);
    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g.cmd[slot], &bi);

    VkClearValue cv;
    memset(&cv, 0, sizeof(cv));
    cv.color.float32[0] = r; cv.color.float32[1] = gg;
    cv.color.float32[2] = b; cv.color.float32[3] = a;

    VkRenderPassBeginInfo rbi = {0};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = g.rp;
    rbi.framebuffer = g.img_fb[g.image_index];
    rbi.renderArea.extent = g.extent;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &cv;
    g.skip_pass = prop_int("debug.mnge.emptysubmit", 0) != 0;
    if (!g.skip_pass)
        vkCmdBeginRenderPass(g.cmd[slot], &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = {0, 0, (float)g.extent.width, (float)g.extent.height, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, g.extent};
    vkCmdSetViewport(g.cmd[slot], 0, 1, &vp);
    vkCmdSetScissor(g.cmd[slot], 0, 1, &sc);
    if (!prop_int("debug.mnge.nopipe", 0))
        vkCmdBindPipeline(g.cmd[slot], VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe);

    g.in_frame = true;
    return 0;
}

void vk_cmd_viewport(float x, float y, float w, float h)
{
    if (!g.in_frame) return;
    VkViewport vp = {x, y, w, h, 0.0f, 1.0f};
    vkCmdSetViewport(g.cmd[g.frame % MAX_FRAMES_IN_FLIGHT], 0, 1, &vp);
}

void vk_cmd_bind_vertex_buffer(uint64_t gpu_addr, uint32_t bytes)
{
    if (!g.in_frame) return;
    uint64_t off = 0;
    if (!vk_pool_translate(gpu_addr, bytes, &off)) {
        LOGE("vb: guest address %llx not in a pool", (unsigned long long)gpu_addr);
        return;
    }
    uint32_t slot = g.frame % MAX_FRAMES_IN_FLIGHT;
    memcpy((uint8_t *)g.frame_map[slot] + UNIFORM_BYTES + off,
           (const void *)(uintptr_t)gpu_addr, bytes);
    VkDeviceSize vboff = UNIFORM_BYTES + off;
    vkCmdBindVertexBuffers(g.cmd[slot], 0, 1, &g.frame_buf[slot], &vboff);
}

void vk_cmd_set_uniform(uint64_t gpu_addr)
{
    if (!g.in_frame) return;
    uint32_t slot = g.frame % MAX_FRAMES_IN_FLIGHT;
    memcpy(g.frame_map[slot], (const void *)(uintptr_t)gpu_addr, UNIFORM_BYTES);
    vkCmdBindDescriptorSets(g.cmd[slot], VK_PIPELINE_BIND_POINT_GRAPHICS, g.playout,
                            0, 1, &g.dset[slot], 0, NULL);
}

void vk_cmd_draw(uint32_t first, uint32_t count)
{
    if (!g.in_frame) return;
    vkCmdDraw(g.cmd[g.frame % MAX_FRAMES_IN_FLIGHT], count, 1, first, 0);
}

int vk_frame_end(void)
{
    if (!g.in_frame) return -1;
    uint32_t slot = g.frame % MAX_FRAMES_IN_FLIGHT;
    if (!g.skip_pass) vkCmdEndRenderPass(g.cmd[slot]);
    VkResult er = vkEndCommandBuffer(g.cmd[slot]);
    if (er != VK_SUCCESS) { LOGE("EndCommandBuffer failed: %d", (int)er); return -1; }

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = g.acq_fence ? 0 : 1;
    si.pWaitSemaphores = &g.sem_acquire[slot];
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd[slot];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.sem_render[g.image_index];
    VkResult sr = vkQueueSubmit(g.queue, 1, &si, g.fence[slot]);
    if (sr != VK_SUCCESS) {
        /* The fence was reset and will never be signalled: stop rather than
         * deadlock on the next vkWaitForFences. */
        LOGE("QueueSubmit failed: %d (skip_pass=%d)", (int)sr, (int)g.skip_pass);
        g.dead = true;
        return -1;
    }

    VkPresentInfoKHR pi = {0};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.sem_render[g.image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swap;
    pi.pImageIndices = &g.image_index;
    VkResult r = vkQueuePresentKHR(g.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.swap_dirty = true;

    g.in_frame = false;
    g.frame++;
    return 0;
}

void vk_demo_bind(vk_demo_layout *layout)
{
    g.demo = *layout;
}
