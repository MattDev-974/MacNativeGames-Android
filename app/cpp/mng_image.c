/* mng_image.c - map one rewritten NSO image and install its key page.
 *
 * This is the part of runtime/loader.c that talks to the kernel, rewritten for
 * Linux/Android.  It is deliberately free of game knowledge: everything it
 * needs comes from the <name>.fixups.tsv that patch_nso.py wrote next to the
 * image, so it can be tested on a synthetic module (tests/host/image_test.c).
 *
 *   0x0                     .text            R+X
 *   TEXT_END                .rodata          R
 *   DATA_OFF                .data / .bss     R+W
 *   KEY_PAGE_OFF (0x4000)   8 host key slots R+W
 *   PATCH_OFF               patch area      R+W
 */
#include "mng_image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE 0x4000u

static uint64_t align_up(uint64_t v) { return (v + PAGE - 1) & ~(uint64_t)(PAGE - 1); }

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || end == s) return 0;
    *out = (uint64_t)v;
    return 1;
}

int mng_layout_parse(const char *path, MngLayout *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(out, 0, sizeof(*out));

    char line[512];
    int have_off = 0, have_key = 0;
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash == line) {                       /* "# NAME\tvalue" */
            char name[64];
            unsigned long long value;
            if (sscanf(line + 1, " %63s %llx", name, &value) != 2) continue;
            if      (!strcmp(name, "PATCH_OFF"))     out->patch_off = value;
            else if (!strcmp(name, "KEY_PAGE_OFF"))  { out->key_page_off = value; have_off = 1; }
            else if (!strcmp(name, "KEY_PAGE_SIZE")) out->key_page_size = value;
            else if (!strcmp(name, "TEXT_END"))      out->text_end = value;
            else if (!strcmp(name, "RO_OFF"))        out->ro_off = value;
            else if (!strcmp(name, "DATA_OFF"))      out->data_off = value;
            else if (!strncmp(name, "KEY_", 4) && strstr(name, "_IDX")) {
                int idx = (int)value;
                if      (!strcmp(name, "KEY_K18_IDX"))       out->k18 = idx, have_key = 1;
                else if (!strcmp(name, "KEY_KTLS_IDX"))      out->ktls = idx;
                else if (!strcmp(name, "KEY_KTLR_IDX"))      out->ktlr = idx;
                else if (!strcmp(name, "KEY_KGATE_IDX"))     out->kgate = idx;
                else if (!strcmp(name, "KEY_KRES0_IDX"))     out->kres0 = idx;
                else if (!strcmp(name, "KEY_KRES1_IDX"))     out->kres1 = idx;
                else if (!strcmp(name, "KEY_KASPECT_IDX"))   out->kaspect = idx;
                else if (!strcmp(name, "KEY_KASPECTUI_IDX")) out->kaspectui = idx;
            }
        }
    }
    fclose(f);

    if (!have_off || !have_key || !out->key_page_size) return -1;
    if (out->key_page_size != PAGE) return -1;          /* one page, eight slots */
    if (!(out->text_end <= out->data_off)) return -1;
    if (out->key_page_off < out->data_off) return -1;
    if (out->patch_off < out->key_page_off + out->key_page_size) return -1;
    if (out->text_end > out->key_page_off) return -1;
    return 0;
}

int mng_image_map(const char *path, const MngLayout *l, void **base_out,
                  uint64_t *image_size_out, char *err, size_t err_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { snprintf(err, err_size, "open %s: %s", path, strerror(errno)); return -1; }

    struct stat st;
    if (fstat(fd, &st) != 0) { snprintf(err, err_size, "stat: %s", strerror(errno)); close(fd); return -1; }

    /* The image must hold the code plus the appended key page and patch area;
     * a shorter file means patch_nso.py did not finish. */
    if ((uint64_t)st.st_size < l->patch_off) {
        snprintf(err, err_size, "%s is %lld bytes, layout says at least %llu",
                 path, (long long)st.st_size, (unsigned long long)l->patch_off);
        close(fd);
        return -1;
    }

    uint64_t size = align_up((uint64_t)st.st_size);

    /* Anonymous, not file-backed: the key page is written in place and the
     * text must end up executable.  On Android a writable file mapping cannot
     * become executable (execmod is denied by SELinux). */
    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        snprintf(err, err_size, "mmap %llu: %s", (unsigned long long)size, strerror(errno));
        close(fd);
        return -1;
    }

    uint8_t *p = mem;
    uint64_t left = (uint64_t)st.st_size;
    while (left) {
        ssize_t n = read(fd, p, left > (1u << 20) ? (1u << 20) : (size_t)left);
        if (n <= 0) {
            snprintf(err, err_size, "read %s: %s", path, n ? strerror(errno) : "short file");
            munmap(mem, size);
            close(fd);
            return -1;
        }
        p += n;
        left -= (uint64_t)n;
    }
    close(fd);

    *base_out = mem;
    *image_size_out = size;
    return 0;
}

int mng_image_protect(void *base, uint64_t size, const MngLayout *l, char *err, size_t err_size)
{
    uint8_t *p = base;
    struct { uint64_t from, to; int prot; } seg[] = {
        { 0,                 l->text_end,     PROT_READ | PROT_EXEC },
        { l->text_end,       l->data_off,     PROT_READ },
        { l->data_off,       size,            PROT_READ | PROT_WRITE },
    };
    for (unsigned i = 0; i < sizeof(seg) / sizeof(seg[0]); i++) {
        uint64_t from = seg[i].from & ~(uint64_t)(PAGE - 1);
        uint64_t to = align_up(seg[i].to);
        if (to <= from) continue;
        if (to > size) to = size;
        /* never remove execute from the text to satisfy a neighbour: extend
         * downward only when the previous segment already covers it */
        if (mprotect(p + from, to - from, seg[i].prot) != 0) {
            snprintf(err, err_size, "mprotect [%#llx,%#llx) prot %d: %s",
                     (unsigned long long)from, (unsigned long long)to, seg[i].prot,
                     strerror(errno));
            return -1;
        }
    }
    return 0;
}

void mng_image_set_keys(void *base, const MngLayout *l, const uint64_t keys[MNG_KEY_COUNT])
{
    uint64_t *page = (uint64_t *)((uint8_t *)base + l->key_page_off);
    for (int i = 0; i < MNG_KEY_COUNT; i++) page[i] = keys[i];
}

uint64_t mng_image_key(void *base, const MngLayout *l, int index)
{
    if (index < 0 || index >= MNG_KEY_COUNT) return 0;
    return ((const uint64_t *)((uint8_t *)base + l->key_page_off))[index];
}

uint64_t mng_image_gate_slot(void *base, const MngLayout *l)
{
    return (uint64_t)(uintptr_t)((uint8_t *)base + l->key_page_off + (uint64_t)l->kgate * 8);
}

void mng_image_unmap(void *base, uint64_t size)
{
    if (base) munmap(base, size);
}
