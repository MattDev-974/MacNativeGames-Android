/* nvn.h - the NVN command stream and the HLE seam.
 *
 * In MacNativeGames the game's own ARM64 code records NVN commands
 * (nvnCommandBuffer*) that nvn_mtl.m replays on Metal, and every syscall goes
 * through the gate in runtime/gate.S into runtime/services.c.  On Android both
 * halves keep their shape:
 *
 *   guest (game code)          host (this file + vk.c)
 *   -------------------        ---------------------------------------------
 *   nvnCommandBufferClearColor -> NVN_CLEAR_COLOR  -> vk_frame_begin()
 *   nvnCommandBufferBind...    -> NVN_BIND_VB      -> vk_cmd_bind_vertex_buffer()
 *   nvnCommandBufferDraw...    -> NVN_DRAW         -> vk_cmd_draw()
 *   nvnQueueSubmitCommands     -> HLE_SUBMIT       -> nvn_execute()
 *
 * The demo guest below is a stand-in for the rewritten NSO: it is real ARM64
 * code running on its own stack, calling the same gate a patched `svc` site
 * calls, writing into a zero-copy pool and submitting a command buffer.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- HLE memory pools (nvn_mtl.h: mtl_pool_add / mtl_addr) --------------- */
int  nvn_pool_add(void *base, uint64_t size);
void nvn_pool_del(void *base);
int  nvn_addr(uint64_t addr, uint64_t len, void **host, uint64_t *pool_base);

/* ---- Command stream ------------------------------------------------------ */
enum {
    NVN_CLEAR_COLOR = 1,
    NVN_SET_UBO,
    NVN_BIND_VB,
    NVN_DRAW,
    NVN_VIEWPORT,
};

typedef struct {
    uint32_t op;
    uint32_t pad;
    uint64_t a;      /* guest GPU address or index */
    uint64_t b;      /* size or count */
    float    f[4];
} NvnCmd;

typedef struct {
    uint32_t count;
    uint32_t pad;
    NvnCmd   cmd[64];
} NvnCmdBuf;

/* ---- HLE call numbers (the values the patcher writes into the svc sites) -- */
enum {
    HLE_SUBMIT    = 0x10,
    HLE_POOL_BASE = 0x11,
    HLE_NOW_NS    = 0x12,
    HLE_LOG       = 0x13,
};

/* ---- The gate (gate.S) and its C side ------------------------------------ */
uint64_t hle_gate(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);
void     runtime_dispatch(uint64_t site, uint64_t *regs);
uint64_t hle_call(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);

/* ---- Host side ----------------------------------------------------------- */
void  nvn_init(void);
void  nvn_shutdown(void);
void  nvn_rebind_pool(void);              /* after the graphics device is (re)created */
int   nvn_execute(const NvnCmdBuf *cb);   /* replays one submit on Vulkan */

/* ---- The guest ----------------------------------------------------------- */
void  nvn_guest_init(void);               /* allocate guest stack + pool     */
void  nvn_guest_frame(uint64_t time_ns);  /* record + submit one frame       */
uint64_t nvn_guest_stack_top(void);

/* Stack trampoline in gate.S: run fn(arg) on a different stack. */
void guest_enter(void *stack_top, void (*fn)(uintptr_t), uintptr_t arg);
