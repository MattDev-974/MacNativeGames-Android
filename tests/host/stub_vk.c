/* stub_vk.c - records what the NVN replay would have asked the GPU to do. */
#include <stdio.h>
#include <string.h>
#include "vk.h"

typedef struct {
    int      frame_begin, frame_end;
    float    clear[4];
    int      have_clear, have_ubo, have_vb, have_draw, have_viewport;
    uint64_t vb_addr, ubo_addr;
    uint32_t vb_bytes, draw_first, draw_count;
    float    viewport[4];
    int      errors;
    uint8_t  vertices[60];
    int      pool_added;
} Rec;

Rec g_rec;

static void fail(const char *msg)
{
    fprintf(stderr, "  FAIL: %s\n", msg);
    g_rec.errors++;
}

void vk_cmd_bind_vertex_buffer(uint64_t gpu_addr, uint32_t bytes)
{
    if (bytes != 60) fail("vertex buffer size != 60");
    memset(g_rec.vertices, 0, sizeof(g_rec.vertices));
    if (bytes <= sizeof(g_rec.vertices))
        memcpy(g_rec.vertices, (const void *)(uintptr_t)gpu_addr, bytes);
    g_rec.have_vb = 1;
    g_rec.vb_addr = gpu_addr;
    g_rec.vb_bytes = bytes;
}

void vk_cmd_set_uniform(uint64_t gpu_addr)
{
    g_rec.have_ubo = 1;
    g_rec.ubo_addr = gpu_addr;
}

void vk_cmd_draw(uint32_t first, uint32_t count)
{
    g_rec.have_draw = 1;
    g_rec.draw_first = first;
    g_rec.draw_count = count;
}

void vk_cmd_viewport(float x, float y, float w, float h)
{
    g_rec.have_viewport = 1;
    g_rec.viewport[0] = x; g_rec.viewport[1] = y;
    g_rec.viewport[2] = w; g_rec.viewport[3] = h;
}

int vk_frame_begin(float r, float g, float b, float a)
{
    g_rec.frame_begin++;
    g_rec.clear[0] = r; g_rec.clear[1] = g; g_rec.clear[2] = b; g_rec.clear[3] = a;
    g_rec.have_clear = 1;
    return 0;
}

int vk_frame_end(void)
{
    g_rec.frame_end++;
    return 0;
}

/* same containment rule as the real implementation */
static struct { uint64_t base, size; } g_pools[8];
static int g_npools;

int vk_pool_add(void *base, uint64_t size)
{
    g_pools[g_npools].base = (uint64_t)(uintptr_t)base;
    g_pools[g_npools].size = size;
    g_npools++;
    g_rec.pool_added = 1;
    return 1;
}

void vk_pool_del(void *base) { (void)base; }

int vk_pool_translate(uint64_t gpu_addr, uint64_t len, uint64_t *offset_out)
{
    for (int i = 0; i < g_npools; i++) {
        uint64_t base = g_pools[i].base;
        if (gpu_addr >= base && gpu_addr - base <= g_pools[i].size &&
            len <= g_pools[i].size - (gpu_addr - base)) {
            *offset_out = gpu_addr - base;
            return 1;
        }
    }
    return 0;
}

/* All the assertions the replay must satisfy, in one place. */
int stub_check(uint64_t expected_vb, uint64_t expected_ubo)
{
    static const float tri[15] = {
         0.0f, -0.55f,  0.95f, 0.25f, 0.20f,
         0.51f, 0.45f,  0.20f, 0.85f, 0.45f,
        -0.51f, 0.45f,  0.35f, 0.45f, 0.95f,
    };
    if (g_rec.frame_begin != 1) fail("frame_begin count != 1");
    if (g_rec.frame_end != 1)   fail("frame_end count != 1");
    if (!g_rec.pool_added)      fail("pool never registered");
    if (!g_rec.have_clear)      fail("no clear colour reached the pass");
    if (!g_rec.have_ubo)        fail("uniform block not bound");
    if (!g_rec.have_vb)         fail("vertex buffer not bound");
    if (!g_rec.have_draw)       fail("no draw recorded");
    if (g_rec.draw_first != 0 || g_rec.draw_count != 3) fail("draw is not first=0 count=3");
    if (g_rec.vb_addr != expected_vb)   fail("vertex buffer address is wrong");
    if (g_rec.ubo_addr != expected_ubo) fail("uniform address is wrong");
    if (g_rec.clear[0] < 0.039f || g_rec.clear[0] > 0.041f) fail("clear.r is not 0.04");
    if (g_rec.clear[2] < 0.089f || g_rec.clear[2] > 0.091f) fail("clear.b is not 0.09");
    if (g_rec.clear[3] != 1.0f) fail("clear.a is not 1.0");
    if (memcmp(g_rec.vertices, tri, sizeof(tri)) != 0) fail("vertex data did not survive the pool round-trip");
    return g_rec.errors;
}
