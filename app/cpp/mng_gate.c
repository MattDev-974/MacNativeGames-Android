/* mng_gate.c - resolve a gate return address to a Horizon call.
 *
 * Port of runtime_dispatch() from runtime/loader.c.  The guest reaches here
 * through `bl hle_gate`: gate.S saved x0..x30 and passed the return address in
 * x30 as `site`.  patch_nso.py recorded, for every rewritten site, the offset
 * the return address will have (g->ret) and what the site was (g->arg: the svc
 * number, or the target register for a counter read).
 */
#include "mng_gate.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static Module *g_mods;
static int g_nmods;
static MngSvcFn g_svc;
static unsigned g_gates;

void mng_gate_init(Module *mods, int n, MngSvcFn svc)
{
    g_mods = mods;
    g_nmods = n;
    g_svc = svc;
    g_gates = 0;
    for (int i = 0; i < n; i++) g_gates += mods[i].ngate;
}

unsigned mng_gate_count(void) { return g_gates; }

uint64_t mng_gate_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    /* 19.2 MHz: one nanosecond is 12/625 of a tick. */
    return ns / 625ull * 12ull + (ns % 625ull) * 12ull / 625ull;
}

void mng_gate_dispatch(uint64_t site, uint64_t *regs)
{
    for (int i = 0; i < g_nmods; i++) {
        Module *m = &g_mods[i];
        for (unsigned j = 0; j < m->ngate; j++) {
            const Gate *g = &m->gates[j];
            if ((uint64_t)(uintptr_t)m->base + g->ret != site) continue;
            if (g->timer) {
                if (g->arg >= 31 || g->arg == 18) {
                    fprintf(stderr, "[gate] timer target register %u is invalid\n", g->arg);
                    abort();
                }
                regs[g->arg] = mng_gate_ticks();
                return;
            }
            if (g_svc) g_svc(g->arg, regs, m->name, g->original);
            return;
        }
    }
    fprintf(stderr, "[gate] unknown return address %#llx\n", (unsigned long long)site);
    abort();
}
