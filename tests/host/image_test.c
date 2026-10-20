/* image_test.c - exercise mng_image.c without any game data.
 *
 * Builds a synthetic rewritten module (image + key page + patch area), maps it,
 * installs keys, and proves the permissions are what the loader promises:
 * text readable+executable and NOT writable, data writable, key page writable.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mng_image.h"

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_faulted;
static void on_fault(int sig) { (void)sig; g_faulted = 1; siglongjmp(g_jmp, 1); }

static int failures;

static void check(int cond, const char *what)
{
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

#define IMAGE_SIZE 0x40000u
#define TEXT_END   0x2a000u
#define DATA_OFF   0x30000u
#define KEY_OFF    0x38000u
#define PATCH_OFF  0x3c000u

static void write_tsv(const char *path, unsigned patch_off, unsigned key_off)
{
    FILE *f = fopen(path, "w");
    fprintf(f,
        "# PATCH_OFF\t0x%x\n# KEY_PAGE_OFF\t0x%x\n# KEY_PAGE_SIZE\t0x4000\n"
        "# TEXT_END\t0x%x\n# RO_OFF\t0x2b000\n# DATA_OFF\t0x%x\n"
        "# KEY_K18_IDX\t0\n# KEY_KTLS_IDX\t1\n# KEY_KTLR_IDX\t2\n# KEY_KGATE_IDX\t3\n"
        "# KEY_KRES0_IDX\t4\n# KEY_KRES1_IDX\t5\n# KEY_KASPECT_IDX\t6\n# KEY_KASPECTUI_IDX\t7\n",
        patch_off, key_off, TEXT_END, DATA_OFF);
    fclose(f);
}

int main(void)
{
    const char *bin = "fake.patched.bin", *tsv = "fake.fixups.tsv";
    char err[256] = {0};

    printf("== mng_layout_parse\n");
    write_tsv(tsv, PATCH_OFF, KEY_OFF);
    MngLayout l;
    check(mng_layout_parse(tsv, &l) == 0, "parses the tsv patch_nso.py writes");
    check(l.key_page_off == KEY_OFF && l.kgate == 3 && l.text_end == TEXT_END,
          "reads offsets and the KGATE slot index");

    write_tsv("bad.fixups.tsv", KEY_OFF, KEY_OFF);   /* patch area overlaps the key page */
    MngLayout bad;
    check(mng_layout_parse("bad.fixups.tsv", &bad) != 0, "rejects a patch area inside the key page");

    printf("== mng_image_map\n");
    uint8_t *img = calloc(1, IMAGE_SIZE);
    for (unsigned i = 0; i < IMAGE_SIZE; i++) img[i] = (uint8_t)(i * 31);
    img[0x100] = 0xAA;                      /* text byte */
    img[DATA_OFF + 0x10] = 0xBB;            /* data byte */
    FILE *f = fopen(bin, "wb");
    fwrite(img, 1, IMAGE_SIZE, f);
    fclose(f);
    free(img);

    void *base = NULL;
    uint64_t size = 0;
    check(mng_image_map(bin, &l, &base, &size, err, sizeof(err)) == 0, "maps the image");

    uint8_t *b = base;
    check(b[0x100] == 0xAA && b[DATA_OFF + 0x10] == 0xBB, "contents survive the mapping");

    printf("== key page\n");
    uint64_t keys[MNG_KEY_COUNT] = {
        0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888
    };
    mng_image_set_keys(base, &l, keys);
    check(mng_image_key(base, &l, 3) == 0x4444, "KGATE slot holds what the loader wrote");
    check(mng_image_key(base, &l, 0) == 0x1111, "K18 slot holds what the loader wrote");
    check(mng_image_gate_slot(base, &l) == (uint64_t)(uintptr_t)(b + KEY_OFF + 3 * 8),
          "gate slot address is base + key page + index*8");

    printf("== permissions\n");
    check(mng_image_protect(base, size, &l, err, sizeof(err)) == 0, "mprotect segments");

    struct sigaction sa = {0}, old;
    sa.sa_handler = on_fault;
    sigaction(SIGSEGV, &sa, &old);
    sigaction(SIGBUS, &sa, &old);

    g_faulted = 0;
    if (!sigsetjmp(g_jmp, 1)) {
        volatile uint64_t *text = (volatile uint64_t *)base;
        *text = 0xdead;                     /* must fault: text is not writable */
    }
    check(g_faulted, "writing to .text faults (R+X, W^X respected)");

    g_faulted = 0;
    if (!sigsetjmp(g_jmp, 1)) b[DATA_OFF + 0x20] = 0x5A;
    check(!g_faulted && b[DATA_OFF + 0x20] == 0x5A, "writing to .data succeeds");

    g_faulted = 0;
    if (!sigsetjmp(g_jmp, 1)) ((volatile uint8_t *)base)[KEY_OFF + 0x100] = 0x5A;
    check(!g_faulted, "writing the key page succeeds");

    g_faulted = 0;
    if (!sigsetjmp(g_jmp, 1)) { volatile uint8_t x = b[0x100]; (void)x; }
    check(!g_faulted, "reading .text succeeds");

    sigaction(SIGSEGV, &old, NULL);
    sigaction(SIGBUS, &old, NULL);

    mng_image_unmap(base, size);
    unlink(bin); unlink(tsv); unlink("bad.fixups.tsv");

    printf("\n%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
