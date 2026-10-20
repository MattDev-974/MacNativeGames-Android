/* nvn.c - host side of the NVN seam plus the stand-in guest.
 *
 * Real port: hle_nvn.c (2 727 lines, 575 NVN entry points) keeps its shape and
 * the `mtl_*` seam becomes `vk_*`.  Here the entry points are reduced to the
 * five commands the demo needs, so the plumbing can be read end to end.
 */
#include "nvn.h"
#include "vk.h"

#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <time.h>
#include <unistd.h>

#define TAG "mnge-nvn"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define POOL_BYTES   (64u * 1024u)
#define STACK_BYTES  (256u * 1024u)

#define VTX_OFF   0u
#define UNI_OFF   1024u
#define CMD_OFF   4096u

static uint8_t *g_pool;
static uint8_t *g_stack_top;
static uint64_t g_frames;

/* ---------------------------------------------------------- memory pools */

int nvn_pool_add(void *base, uint64_t size)
{
    return vk_pool_add(base, size);
}

void nvn_pool_del(void *base)
{
    vk_pool_del(base);
}

int nvn_addr(uint64_t addr, uint64_t len, void **host, uint64_t *pool_base)
{
    uint64_t off = 0;
    /* zero-copy: on Android the guest pool is ordinary process memory, so the
     * GPU address is the host pointer.  The real port keeps this identity but
     * also aliases the pool into a VkBuffer (VK_EXT_external_memory_host). */
    if (!vk_pool_translate(addr, len, &off)) return 0;
    *host = (void *)(uintptr_t)addr;
    if (pool_base) *pool_base = addr - off;
    return 1;
}

/* ------------------------------------------------------------ HLE calls */

/* The demo's own HLE entry.  It must NOT go through gate.S: the gate now swaps
 * TPIDR_EL0 around the call and therefore requires a guest block on the calling
 * thread, which the renderer does not have.  The real game reaches the gate
 * from guest threads, which do. */
uint64_t hle_call(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3)
{
    uint64_t regs[31];
    memset(regs, 0, sizeof(regs));
    regs[0] = num;
    regs[1] = a1;
    regs[2] = a2;
    regs[3] = a3;
    runtime_dispatch(0, regs);
    return regs[0];
}

static uint64_t hle_submit(uint64_t cmd_addr)
{
    void *host = NULL;
    if (!nvn_addr(cmd_addr, sizeof(NvnCmdBuf), &host, NULL)) {
        LOGE("submit: command buffer %llx is not in a pool",
             (unsigned long long)cmd_addr);
        return 1;
    }
    nvn_execute((const NvnCmdBuf *)host);
    return 0;
}

static uint64_t hle_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The C side of the gate.  `regs` is the 800-byte frame gate.S built; a gate is
 * allowed to rewrite any of x0..x30, so the return value goes back in x0 and
 * any "out parameter" would be written to regs[n] directly. */
void runtime_dispatch(uint64_t site, uint64_t *regs)
{
    (void)site;
    uint64_t num = regs[0];
    uint64_t out = 1;

    switch (num) {
    case HLE_SUBMIT:    out = hle_submit(regs[1]); break;
    case HLE_POOL_BASE: out = (uint64_t)(uintptr_t)g_pool; break;
    case HLE_NOW_NS:    out = hle_now_ns(); break;
    case HLE_LOG:
        if (regs[1]) LOGI("guest: %s", (const char *)(uintptr_t)regs[1]);
        out = 0;
        break;
    default:
        LOGE("unhandled HLE call %llu (site %llx)", (unsigned long long)num,
             (unsigned long long)site);
        break;
    }
    regs[0] = out;
}

/* ------------------------------------------------------------- replay */

/* Diagnostic switches, read from `adb shell setprop debug.mnge.<name> N`.
 * Only debuggable builds may read debug.* properties; on any other platform
 * (the qemu host test) the default wins. */
static int prop_int(const char *key, int def)
{
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX];
    int n = __system_property_get(key, v);
    return n > 0 ? atoi(v) : def;
#else
    (void)key;
    return def;
#endif
}

int nvn_execute(const NvnCmdBuf *cb)
{
    float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    /* The clear defines the render pass, exactly like nvnCommandBufferClearColor
     * followed by BeginRenderPass on the Metal side. */
    for (uint32_t i = 0; i < cb->count && i < 64; i++)
        if (cb->cmd[i].op == NVN_CLEAR_COLOR)
            memcpy(clear, cb->cmd[i].f, sizeof(clear));

    int rc = vk_frame_begin(clear[0], clear[1], clear[2], clear[3]);
    if (rc != 0) {
        static int reported;
        if (reported++ < 5) LOGE("frame_begin returned %d (skipped)", rc);
        return rc;   /* swapchain recreated or lost: skip the frame */
    }

    int clear_only = prop_int("debug.mnge.clearonly", 0);

    for (uint32_t i = 0; i < cb->count && i < 64 && !clear_only; i++) {
        const NvnCmd *c = &cb->cmd[i];
        switch (c->op) {
        case NVN_SET_UBO:    vk_cmd_set_uniform(c->a); break;
        case NVN_BIND_VB:    vk_cmd_bind_vertex_buffer(c->a, c->b); break;
        case NVN_VIEWPORT:   vk_cmd_viewport(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case NVN_DRAW:       vk_cmd_draw(c->a, c->b); break;
        case NVN_CLEAR_COLOR: break;   /* consumed above */
        default: break;
        }
    }

    g_frames++;
    int end = vk_frame_end();
    if (end != 0) {
        static int reported;
        if (reported++ < 5) LOGE("frame_end returned %d", end);
    }
    return end;
}

/* --------------------------------------------------------------- guest */

/* This function is the stand-in for the game's rewritten NSO: it runs on the
 * guest stack and reaches the host only through hle_gate(). */
static void guest_frame(uintptr_t pool)
{
    float *vtx = (float *)(pool + VTX_OFF);
    float *uni = (float *)(pool + UNI_OFF);
    NvnCmdBuf *cb = (NvnCmdBuf *)(pool + CMD_OFF);

    /* a triangle, as the game would write it into its vertex pool */
    static const float tri[15] = {
         0.0f, -0.55f,  0.95f, 0.25f, 0.20f,
         0.51f, 0.45f,  0.20f, 0.85f, 0.45f,
        -0.51f, 0.45f,  0.35f, 0.45f, 0.95f,
    };
    memcpy(vtx, tri, sizeof(tri));

    uint64_t now = hle_call(HLE_NOW_NS, 0, 0, 0);
    uni[0] = (float)((double)now / 1e9);   /* the NVN uniform block */

    cb->count = 0;
    cb->cmd[cb->count++] = (NvnCmd){NVN_CLEAR_COLOR, 0, 0, 0, {0.04f, 0.05f, 0.09f, 1.0f}};
    cb->cmd[cb->count++] = (NvnCmd){NVN_SET_UBO, 0, (uint64_t)(pool + UNI_OFF), 0, {0}};
    cb->cmd[cb->count++] = (NvnCmd){NVN_BIND_VB, 0, (uint64_t)(pool + VTX_OFF), sizeof(tri), {0}};
    cb->cmd[cb->count++] = (NvnCmd){NVN_DRAW, 0, 0, 3, {0}};

    /* nvnQueueSubmitCommands */
    hle_call(HLE_SUBMIT, pool + CMD_OFF, 0, 0);
}

void nvn_guest_init(void)
{
    g_pool = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_pool == MAP_FAILED) { LOGE("pool mmap failed"); g_pool = NULL; return; }

    void *stack = mmap(NULL, STACK_BYTES, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) { LOGE("stack mmap failed"); return; }
    /* top of stack, 16-byte aligned, as the loader sets it from the NPDM */
    g_stack_top = (uint8_t *)stack + STACK_BYTES;
    g_stack_top = (uint8_t *)((uintptr_t)g_stack_top & ~(uintptr_t)0xF);

    nvn_pool_add(g_pool, POOL_BYTES);
    LOGI("guest pool at %p, %u KiB; guest stack %p",
         (void *)g_pool, POOL_BYTES / 1024, (void *)g_stack_top);
}

uint64_t nvn_guest_stack_top(void) { return (uint64_t)(uintptr_t)g_stack_top; }

void nvn_guest_frame(uint64_t time_ns)
{
    (void)time_ns;
    if (!g_pool || !g_stack_top) return;
    guest_enter(g_stack_top, guest_frame, (uintptr_t)g_pool);
}

void nvn_rebind_pool(void)
{
    if (g_pool) nvn_pool_add(g_pool, POOL_BYTES);
}

void nvn_init(void)   { g_frames = 0; }
void nvn_shutdown(void) { if (g_pool) { nvn_pool_del(g_pool); g_pool = NULL; } }
