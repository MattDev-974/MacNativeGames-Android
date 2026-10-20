/* loader_real_test.c - map and relocate the REAL game modules, off-device.
 *
 * Uses the runtime_tables.h that prepare_runtime.py generated from the verified
 * images, and the images themselves.  It does not run any game code: it proves
 * the loader's mapping and relocation stages against real data, and prints the
 * imports that no host implementation answers yet -- the work list for the
 * Horizon services.
 *
 *   ./run_loader.sh <rewritten-dir> <tables-dir>
 */
#include <stdio.h>
#include <stdlib.h>

#include "mng_loader.h"
#include "mng_gate.h"
#include "runtime_tables.h"

static int failures;
static unsigned svc_calls, timed;
static uint64_t last_num;

static uint64_t fake_svc(unsigned num, uint64_t *regs, const char *mod, uint64_t orig)
{
    (void)regs; (void)mod; (void)orig;
    svc_calls++; last_num = num;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <rewritten/v0 dir>\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);

    int n = (int)(sizeof(modules) / sizeof(modules[0]));
    printf("modules: %d\n", n);
    for (int i = 0; i < n; i++)
        printf("  %-8s size=%zu symbols=%u relocs=%u gates=%u\n", modules[i].name,
               modules[i].file_size, modules[i].nsym, modules[i].nreloc, modules[i].ngate);

    char err[1024] = {0};
    if (mng_loader_map_all(modules, (int)n, argv[1], 0, err, sizeof(err)) != 0) {
        printf("map FAILED: %s\n", err);
        return 1;
    }
    printf("\nmapped:\n");
    for (int i = 0; i < n; i++)
        printf("  %-8s at %p size=%zu\n", modules[i].name, modules[i].base, modules[i].size);

    /* Byte offsets of the eight TLS slots; the gate address itself goes in the
     * per-thread block, not here. */
    uint64_t keys[8];
    for (int i = 0; i < 8; i++) keys[i] = (uint64_t)i * 8;
    mng_loader_fill_keys(modules, (int)n, keys);

    static MngRelocStats st;
    printf("\nrelocating...\n");
    if (mng_loader_relocate(modules, (int)n, NULL, &st, err, sizeof(err)) != 0) {
        printf("relocate FAILED: %s\n", err);
        return 1;
    }
    printf("  relative   %llu\n", (unsigned long long)st.relative);
    printf("  symbolic   %llu\n", (unsigned long long)st.symbolic);
    printf("  cross-mod  %llu\n", (unsigned long long)st.cross);
    printf("  weak       %llu\n", (unsigned long long)st.weak);
    printf("  MISSING    %llu strong imports with no implementation\n",
           (unsigned long long)st.strong_missing);

    unsigned mc = mng_loader_missing_count();
    printf("\n%d distinct missing imports", mc);
    if (mc) printf(" (first 40):");
    printf("\n");
    for (unsigned i = 0; i < mc && i < 40; i++) printf("  %s\n", mng_loader_missing(i));

    /* ---- gate dispatch, exercised against the real table ---- */
    printf("\n== gates\n");
    svc_calls = timed = 0;
    mng_gate_init(modules, (int)n, fake_svc);
    printf("  installed %u gates\n", mng_gate_count());

    for (int i = 0; i < n; i++) {
        Module *m = &modules[i];
        for (unsigned j = 0; j < m->ngate; j++) {
            const Gate *g = &m->gates[j];
            uint64_t regs[31] = {0};
            uint64_t site = (uint64_t)(uintptr_t)m->base + g->ret;
            unsigned before = svc_calls;
            mng_gate_dispatch(site, regs);
            if (g->timer) {
                if (regs[g->arg] == 0) { printf("  FAIL timer gate %u wrote nothing\n", j); failures++; }
                timed++;
            } else if (svc_calls != before + 1 || last_num != g->arg) {
                printf("  FAIL svc gate %u dispatched %llu, expected %u\n", j,
                       (unsigned long long)last_num, g->arg);
                failures++;
            }
        }
    }
    printf("  %u svc gates and %u timer gates dispatch to the right target\n", svc_calls, timed);

    mng_loader_free_hash(modules, (int)n);
    printf("\n%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
