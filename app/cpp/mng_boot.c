/* mng_boot.c - bring the game's own code up on Android.
 *
 * The sequence is the one runtime/loader.c performs on Apple: map the three
 * modules, apply the relocations, install the gate table, then call
 * nn::init::Start with the SDK's own startup callbacks.  What differs is that
 * every unimplemented Horizon call is reported instead of aborting, so a
 * bring-up run yields the list of what to implement next.
 */
#include "mng_boot.h"
#include "mng_gate.h"
#include "mng_loader.h"
#include "mng_svc.h"
#include "mng_tables.h"

#include <android/log.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "mnge-boot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

typedef void (*StartFn)(uint64_t, uint64_t, void (*)(void), void (*)(void), void (*)(void));

static Module *g_mods;
static unsigned g_nmods;
static volatile int g_active;

/* A fault inside the game's own code is otherwise anonymous: Android's
 * tombstone only names libtotk.so.  This reports the guest module and offset,
 * which is what makes the next step knowable. */
/* Saved before the guest block goes into TPIDR_EL0.  The fault handler runs on
 * the faulting thread, and on the guest thread TPIDR_EL0 is *not* Bionic's TLS:
 * every libc call in the handler (snprintf, dladdr, open, write) would fault
 * again and the process would die with no report at all. */
static volatile uint64_t g_host_tls;

static void fault_handler(int sig, siginfo_t *si, void *ctx)
{
    if (g_host_tls) __asm__ volatile("msr tpidr_el0, %0" :: "r"(g_host_tls) : "memory");
    ucontext_t *u = (ucontext_t *)ctx;
    uint64_t pc = (uint64_t)u->uc_mcontext.pc;
    uint64_t lr = (uint64_t)u->uc_mcontext.regs[30];
    const char *mod = "host";
    uint64_t off = pc;
    for (unsigned i = 0; i < g_nmods; i++) {
        uint64_t b = (uint64_t)(uintptr_t)g_mods[i].base;
        if (pc >= b && pc < b + g_mods[i].size) { mod = g_mods[i].name; off = pc - b; break; }
    }
    /* Where exactly: dladdr turns the pc into "library+offset", which
     * llvm-addr2line resolves to a file and line off-device. */
    char where[256] = "?";
    Dl_info di;
    if (dladdr((void *)(uintptr_t)pc, &di) && di.dli_fbase) {
        const char *name = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
        snprintf(where, sizeof(where), "%s+%#llx", name ? name + 1 : "?",
                 (unsigned long long)(pc - (uint64_t)(uintptr_t)di.dli_fbase));
    }
    char line[512];
    int n = snprintf(line, sizeof(line),
        "signal %d at %p\npc=%s+%#llx  (%s)\nlr=%#llx\nx0=%#llx x1=%#llx x2=%#llx\n"
        "x18=%#llx x19=%#llx sp=%#llx\n",
        sig, si->si_addr, mod, (unsigned long long)off, where,
        (unsigned long long)lr,
        (unsigned long long)u->uc_mcontext.regs[0],
        (unsigned long long)u->uc_mcontext.regs[1],
        (unsigned long long)u->uc_mcontext.regs[2],
        (unsigned long long)u->uc_mcontext.regs[18],
        (unsigned long long)u->uc_mcontext.regs[19],
        (unsigned long long)u->uc_mcontext.sp);
    __android_log_write(ANDROID_LOG_ERROR, "mnge-fault", line);
    /* The log socket may not flush before the process dies; the file does. */
    int fd = open("/data/data/org.macnativegames.totk/files/fault.txt",
                  O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; fsync(fd); close(fd); }
    usleep(300000);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_fault_handler(void)
{
    /* A stack overflow cannot be reported on the stack that just overflowed:
     * give the kernel a dedicated signal stack first. */
    static char altstack[SIGSTKSZ * 4] __attribute__((aligned(16)));
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGSYS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

/* A clean exit_group cannot be caught by a signal handler; atexit can. */
static void on_exit(void)
{
    if (g_host_tls) __asm__ volatile("msr tpidr_el0, %0" :: "r"(g_host_tls) : "memory");
    int fd = open("/data/data/org.macnativegames.totk/files/exit.txt",
                  O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { ssize_t w = write(fd, "exit() was called\n", 18); (void)w; fsync(fd); close(fd); }
    __android_log_write(ANDROID_LOG_ERROR, "mnge-boot", "exit() was called");
}

int mng_boot_active(void) { return g_active; }

/* Per-guest-thread block.  The rewriter addresses the key slots directly at
 * slot_index*8 (KGATE is index 511, so byte 4088), which is why the block is a
 * full page plus room for the host state the gate needs. */
#define GUEST_KEY_BYTES 4096u
#define GUEST_TLS_OFF   GUEST_KEY_BYTES      /* Bionic's TPIDR_EL0, for the gate */
#define GUEST_SCRATCH   (GUEST_KEY_BYTES + 8)

typedef struct {
    uint8_t  keys[GUEST_KEY_BYTES];
    uint64_t host_tls;
    uint8_t  scratch[2048];
} GuestBlock;

static __thread GuestBlock t_blk __attribute__((aligned(16)));

/* BLOCKER (see docs/06, finding 6): the rewriter emits `mrs xN, tpidrro_el0`
 * into every gate block, because on Darwin the OS points that register at the
 * thread block.  On AArch64 TPIDRRO_EL0 is architecturally READ-ONLY at EL0 --
 * an `msr` is an illegal instruction (SIGILL, verified on the device) -- and
 * Linux never sets it for user threads.  The port therefore needs:
 *   1. patch_nso.py to emit `mrs xN, tpidr_el0` instead (same encoding family,
 *      one instruction swapped in place);
 *   2. gate.S to swap TPIDR_EL0 to Bionic's value before calling C and back
 *      afterwards, keeping the saved value inside the guest block itself so the
 *      swap never needs thread-local storage.
 * Until that is done, entering nn::init::Start would dereference a garbage
 * thread pointer. */
static int guest_tls_available(void) { return 1; }

static int8_t g_sdk_started;

/* Any host callback the guest invokes runs with the guest block in TPIDR_EL0,
 * so it must swap back to Bionic's before touching libc (LOG included). */
static uint64_t tls_to_host(void)
{
    uint64_t guest;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(guest));
    uint64_t host = ((GuestBlock *)(uintptr_t)guest)->host_tls;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(host) : "memory");
    return guest;
}

static void tls_to_guest(uint64_t guest)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(guest) : "memory");
}

static void loader_ready(void)
{
    uint64_t g = tls_to_host();
    LOGI("loader_ready: the SDK finished its first stage");
    tls_to_guest(g);
}

static void module_inits(void)
{
    uint64_t g = tls_to_host();
    for (unsigned i = 0; i < g_nmods; i++) {
        Module *m = &g_mods[i];
        uint64_t count = m->init_size / 8;
        uint64_t *arr = (uint64_t *)(void *)(m->base + m->init_array);
        LOGI("running %s constructors (%llu)", m->name, (unsigned long long)count);
        for (uint64_t k = 0; k < count; k++) {
            void (*fn)(void) = (void (*)(void))(uintptr_t)arr[k];
            if (fn) fn();
        }
    }
    tls_to_guest(g);
}

static void module_finis(void) { LOGI("module finis"); }

/* The block is installed last: from then on libc must not be touched on this
 * thread except through the gate or an explicit swap. */
__attribute__((no_stack_protector))
static void enter_guest(void *entry)
{
    uint64_t host;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(host));

    uint64_t *slots = (uint64_t *)(void *)t_blk.keys;
    slots[511] = (uint64_t)(uintptr_t)&hle_gate;     /* KGATE */
    uint64_t scratch = (uint64_t)(uintptr_t)t_blk.scratch;
    slots[508] = scratch;                            /* K18  */
    slots[509] = scratch;                            /* KTLS */
    slots[510] = scratch;                            /* KTLR */
    t_blk.host_tls = host;

    g_host_tls = host;
    uint64_t block = (uint64_t)(uintptr_t)&t_blk;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(block) : "memory");

    /* The first argument is the main thread's handle.  The Apple build passes
     * h_new(O_THREAD, t); until a handle table exists here, 1 is the least
     * implausible value (0 is the invalid-handle sentinel). */
    ((StartFn)entry)(1, 0, loader_ready, module_inits, module_finis);
    /* nn::init::Start does not return; the thread is unusable afterwards. */
    for (;;) __asm__ volatile("wfi");
}

static void *guest_main(void *arg)
{
    (void)arg;

    void *entry = mng_loader_symbol(g_mods, (int)g_nmods,
                                    "_ZN2nn4init5StartEmmPFvvES2_S2_");
    if (!entry) {
        LOGE("nn::init::Start not found in any module");
        g_active = 0;
        return NULL;
    }
    LOGI("entering nn::init::Start at %p (handle 1)", entry);

    g_active = 1;
    enter_guest(entry);
    return NULL;
}

/* Heartbeat: if the process is killed rather than faulting, the last beat
 * dates the death against whatever the system logs at the same moment. */
static void *beat(void *a)
{
    (void)a;
    for (int i = 1;; i++) {
        sleep(2);
        long rss = 0;
        FILE *f = fopen("/proc/self/statm", "r");
        if (f) { long sz; if (fscanf(f, "%ld %ld", &sz, &rss) != 2) rss = 0; fclose(f); }
        LOGI("heartbeat %d rss=%ld MB", i, rss * 4 / 1024);
    }
    return NULL;
}

int mng_boot_run(const char *module_dir)
{
    if (g_mods) return 0;

    g_nmods = mng_game_module_count();
    if (!g_nmods) {
        LOGE("no game tables compiled in (build with --game-tree)");
        return -1;
    }
    g_mods = (Module *)(uintptr_t)mng_game_modules();

    install_fault_handler();
    atexit(on_exit);

    char err[1024] = {0};
    if (mng_loader_map_all(g_mods, (int)g_nmods, module_dir, 1, err, sizeof(err)) != 0) {
        LOGE("map failed: %s", err);
        g_mods = NULL;
        return -1;
    }
    LOGI("mapped %u modules from %s", g_nmods, module_dir);

    uint64_t keys[8];
    for (int i = 0; i < 8; i++) keys[i] = (uint64_t)i * 8;
    mng_loader_fill_keys(g_mods, (int)g_nmods, keys);

    static MngRelocStats st;
    if (mng_loader_relocate(g_mods, (int)g_nmods, NULL, &st, err, sizeof(err)) != 0) {
        LOGE("relocate failed: %s", err);
        g_mods = NULL;
        return -1;
    }
    LOGI("relocated: relative %llu, symbolic %llu, cross %llu, weak %llu, missing %llu",
         (unsigned long long)st.relative, (unsigned long long)st.symbolic,
         (unsigned long long)st.cross, (unsigned long long)st.weak,
         (unsigned long long)st.strong_missing);

    mng_gate_init(g_mods, (int)g_nmods, mng_svc);
    LOGI("gate table installed: %u gates", mng_gate_count());

    g_sdk_started = 0;

    if (!guest_tls_available()) {
        LOGE("not entering nn::init::Start: the gate still reads TPIDRRO_EL0, "
             "which EL0 cannot write (docs/06, finding 6)");
        return 0;
    }

    /* 8 MB is plenty for the SDK's start path; 32 MB asked a phone to hand
     * over more contiguous address space than it likes. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    {
        long rss = 0, size = 0;
        FILE *f = fopen("/proc/self/statm", "r");
        if (f && fscanf(f, "%ld %ld", &size, &rss) == 2)
            LOGI("RSS before guest: %ld MB (%ld MB virtual)", rss * 4 / 1024, size * 4 / 1024);
        if (f) fclose(f);
    }

    pthread_t th;
    if (pthread_create(&th, &attr, guest_main, NULL) != 0) {
        LOGE("cannot create the guest thread");
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_detach(th);
    pthread_attr_destroy(&attr);

    pthread_t hb;
    if (pthread_create(&hb, NULL, beat, NULL) == 0) pthread_detach(hb);
    return 0;
}
