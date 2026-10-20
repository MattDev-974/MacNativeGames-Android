/* main.c - host test: run the guest, the gate and the NVN replay off-device.
 *
 * Built for aarch64 and executed under qemu-user, so the hand-written assembly
 * in gate.S and the guest stack switch are exercised for real, without a phone.
 */
#include <stdio.h>
#include <stdint.h>
#include "nvn.h"
#include "stub_check.h"

long test_regs(void);

int main(void)
{
    int failures = 0;

    printf("== register preservation across hle_gate\n");
    long bad = test_regs();
    if (bad) { printf("  FAIL: %ld registers came back changed\n", bad); failures++; }
    else     printf("  ok: x19..x28 and q8/q9/q15 preserved\n");

    printf("== guest frame -> NVN replay\n");
    nvn_guest_init();
    nvn_guest_frame(0);

    int errors = (int)(intptr_t)0;
    {
        /* the pool base the HLE handed to the guest */
        uint64_t pool;
        void *host = 0;
        pool = hle_call(0x11 /* HLE_POOL_BASE */, 0, 0, 0);
        if (!pool) { printf("  FAIL: HLE_POOL_BASE returned 0\n"); failures++; }
        if (!nvn_addr(pool, 64, &host, 0)) { printf("  FAIL: pool not addressable\n"); failures++; }
        errors = stub_check(pool + 0, pool + 1024);
    }

    if (errors) { printf("  FAIL: %d replay assertion(s)\n", errors); failures++; }
    else        printf("  ok: clear, uniform, vertex buffer, draw(0,3) all correct\n");

    printf("\n%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
