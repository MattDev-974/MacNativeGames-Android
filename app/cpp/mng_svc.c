/* mng_svc.c - Horizon system calls, the Android side.
 *
 * runtime/services.c on Apple implements the same numbers.  This is the
 * bring-up subset: it answers the calls the SDK makes before it needs
 * graphics, logs every distinct call once, and never fails a boot on an
 * unimplemented one -- the log then names exactly what is missing.
 */
#include "mng_svc.h"

#include <android/log.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define TAG "mnge-svc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static unsigned char g_seen[256];
static unsigned g_distinct;

unsigned mng_svc_distinct(void) { return g_distinct; }

static const char *svc_name(unsigned n)
{
    switch (n) {
    case 0x01: return "SetHeapSize";
    case 0x02: return "SetMemoryPermission";
    case 0x03: return "SetMemoryAttribute";
    case 0x04: return "MapMemory";
    case 0x05: return "UnmapMemory";
    case 0x06: return "QueryMemory";
    case 0x07: return "ExitProcess";
    case 0x08: return "CreateThread";
    case 0x09: return "StartThread";
    case 0x0a: return "ExitThread";
    case 0x0b: return "SleepThread";
    case 0x0c: return "GetThreadPriority";
    case 0x0d: return "SetThreadPriority";
    case 0x0e: return "GetThreadCoreMask";
    case 0x0f: return "SetThreadCoreMask";
    case 0x10: return "GetCurrentProcessorNumber";
    case 0x11: return "SignalEvent";
    case 0x12: return "ClearEvent";
    case 0x13: return "MapSharedMemory";
    case 0x14: return "UnmapSharedMemory";
    case 0x15: return "CreateTransferMemory";
    case 0x16: return "CloseHandle";
    case 0x17: return "ResetSignal";
    case 0x18: return "WaitSynchronization";
    case 0x19: return "CancelSynchronization";
    case 0x1a: return "ArbitrateLock";
    case 0x1b: return "ArbitrateUnlock";
    case 0x1c: return "WaitProcessWideKeyAtomic";
    case 0x1d: return "SignalProcessWideKey";
    case 0x1e: return "GetSystemTick";
    case 0x1f: return "ConnectToNamedPort";
    case 0x20: return "SendSyncRequestLight";
    case 0x21: return "SendSyncRequest";
    case 0x22: return "SendSyncRequestWithUserBuffer";
    case 0x23: return "SendAsyncRequestWithUserBuffer";
    case 0x24: return "GetProcessId";
    case 0x25: return "GetThreadId";
    case 0x26: return "Break";
    case 0x27: return "OutputDebugString";
    case 0x28: return "ReturnFromException";
    case 0x29: return "GetInfo";
    case 0x50: return "SetThreadActivity";
    case 0x51: return "GetThreadContext3";
    case 0x70: return "CreateSession";
    case 0x71: return "AcceptSession";
    case 0x72: return "ReplyAndReceive";
    default:   return "?";
    }
}

uint64_t mng_svc(unsigned num, uint64_t *r, const char *module, uint64_t original)
{
    if (num < 256 && !g_seen[num]) {
        g_seen[num] = 1;
        g_distinct++;
        LOGI("svc %#04x %-28s from %s+%#llx", num, svc_name(num), module,
             (unsigned long long)original);
    }

    switch (num) {
    case 0x01:                      /* SetHeapSize: r1 in, r1 out */
        r[0] = 0;
        return 0;
    case 0x1e:                      /* GetSystemTick */
        r[0] = 0;
        return 0;
    case 0x24:                      /* GetProcessId */
        r[0] = 0;
        r[1] = (uint64_t)getpid();
        return 0;
    case 0x25:                      /* GetThreadId */
        r[0] = 0;
        r[1] = (uint64_t)gettid();
        return 0;
    case 0x27: {                    /* OutputDebugString */
        const char *s = (const char *)(uintptr_t)r[0];
        if (s && (uintptr_t)s > 0x1000) LOGI("guest: %s", s);
        r[0] = 0;
        return 0;
    }
    case 0x07:                      /* ExitProcess */
        LOGE("guest called ExitProcess");
        r[0] = 0;
        return 0;
    default:
        r[0] = 0;
        return 0;
    }
}
