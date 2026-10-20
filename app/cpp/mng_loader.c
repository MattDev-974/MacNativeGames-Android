/* mng_loader.c - map and relocate the rewritten game modules.
 *
 * A port of map_module() and relocate() from runtime/loader.c.  The three
 * differences from the Apple original are the ones that matter on Android:
 *
 *   - images are read from nativeLibraryDir and mapped anonymously, so the
 *     text can become PROT_EXEC without ever being writable (SELinux execmod);
 *   - the eight key slots are host-chosen byte offsets into a per-thread
 *     block, instead of Darwin's pthread_key_t-as-offset;
 *   - an unresolved strong import is *reported*, not fatal, so a bring-up run
 *     produces the list of services to implement rather than a dead end.
 */
#include "mng_loader.h"
#include "mng_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MISSING 512

static char g_missing[MAX_MISSING][128];
static unsigned g_missing_count;

const char *mng_loader_missing(unsigned i)
{
    return i < g_missing_count ? g_missing[i] : NULL;
}

unsigned mng_loader_missing_count(void) { return g_missing_count; }

static void note_missing(const char *name)
{
    if (g_missing_count >= MAX_MISSING) return;
    for (unsigned i = 0; i < g_missing_count; i++)
        if (!strcmp(g_missing[i], name)) return;
    snprintf(g_missing[g_missing_count], sizeof(g_missing[0]), "%s", name);
    g_missing_count++;
}

static unsigned hash_name(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static int make_hash(Module *m, char *err, size_t errsz)
{
    unsigned cap = 16;
    while (cap < m->nsym * 2) cap <<= 1;
    m->hash = calloc(cap, sizeof(unsigned));
    if (!m->hash) { snprintf(err, errsz, "%s: hash allocation", m->name); return -1; }
    m->hash_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        if (!m->symbols[i].defined) continue;
        const char *s = m->symbols[i].name;
        unsigned p = hash_name(s) & m->hash_mask;
        while (m->hash[p] && strcmp(s, m->symbols[m->hash[p]].name))
            p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
    return 0;
}

void mng_loader_free_hash(Module *mods, int n)
{
    for (int i = 0; i < n; i++) { free(mods[i].hash); mods[i].hash = NULL; }
}

static void *module_symbol(Module *m, const char *name)
{
    if (!m->hash) return NULL;
    unsigned p = hash_name(name) & m->hash_mask;
    while (m->hash[p]) {
        const Symbol *s = &m->symbols[m->hash[p]];
        if (!strcmp(name, s->name))
            return s->value < m->key_off ? m->base + s->value : NULL;
        p = (p + 1) & m->hash_mask;
    }
    return NULL;
}

void *mng_loader_symbol(Module *mods, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        void *p = module_symbol(&mods[i], name);
        if (p) return p;
    }
    return NULL;
}

int mng_loader_map_all(Module *mods, int n, const char *dir, int native_names,
                       char *err, size_t errsz)
{
    char path[4096];
    for (int i = 0; i < n; i++) {
        Module *m = &mods[i];
        /* On device the modules sit in nativeLibraryDir under the lib*.so names
         * Android extracts (the only executable, read-only location).  Elsewhere
         * they keep patch_nso.py's names next to their fixups.tsv. */
        if (snprintf(path, sizeof(path), native_names ? "%s/libgame_%s.so" : "%s/%s.patched.bin",
                     dir, m->name) >= (int)sizeof(path)) {
            snprintf(err, errsz, "image path too long");
            return -1;
        }

        MngLayout l;
        if (native_names) {
            /* The generated tables already carry the layout the fixups file
             * documents; the eight key indices are always 0..7. */
            memset(&l, 0, sizeof(l));
            l.patch_off = m->patch_off;
            l.key_page_off = m->key_off;
            l.key_page_size = 0x4000;
            l.text_end = m->text_end;
            l.ro_off = m->ro_off;
            l.data_off = m->data_off;
            l.k18 = 0; l.ktls = 1; l.ktlr = 2; l.kgate = 3;
            l.kres0 = 4; l.kres1 = 5; l.kaspect = 6; l.kaspectui = 7;
        } else {
            char tsv[4096];
            snprintf(tsv, sizeof(tsv), "%s/%s.fixups.tsv", dir, m->name);
            if (mng_layout_parse(tsv, &l) != 0) {
                snprintf(err, errsz, "%s: cannot read %s", m->name, tsv);
                return -1;
            }
        }
        /* The generated tables and the fixup file must agree; if they do not,
         * the image and the tables came from different builds. */
        if (l.patch_off != m->patch_off || l.key_page_off != m->key_off ||
            l.text_end != m->text_end || l.data_off != m->data_off) {
            snprintf(err, errsz,
                     "%s: fixups.tsv disagrees with runtime_tables.h "
                     "(patch %#llx/%#llx key %#llx/%#llx text_end %#llx/%#llx data %#llx/%#llx)",
                     m->name, (unsigned long long)l.patch_off, (unsigned long long)m->patch_off,
                     (unsigned long long)l.key_page_off, (unsigned long long)m->key_off,
                     (unsigned long long)l.text_end, (unsigned long long)m->text_end,
                     (unsigned long long)l.data_off, (unsigned long long)m->data_off);
            return -1;
        }

        uint64_t size = 0;
        void *base = NULL;
        if (mng_image_map(path, &l, &base, &size, err, errsz) != 0) return -1;
        m->base = base;
        m->size = size;

        if (mng_image_protect(base, size, &l, err, errsz) != 0) return -1;
        if (make_hash(m, err, errsz) != 0) return -1;
    }
    return 0;
}

/* Eight slots, one per key index patch_nso.py recorded.  Android has no
 * "pthread_key_t is a byte offset" convention, so the loader owns the layout:
 * slot i lives at byte i*8 of the guest thread block, and KGATE is written
 * with the address of the svc gate. */
void mng_loader_fill_keys(Module *mods, int n, const uint64_t keys[8])
{
    for (int i = 0; i < n; i++) {
        uint64_t *page = (uint64_t *)(mods[i].base + mods[i].key_off);
        for (int k = 0; k < 8; k++) page[k] = keys[k];
    }
}

int mng_loader_relocate(Module *mods, int n, MngOverrideFn override_fn,
                        MngRelocStats *st, char *err, size_t errsz)
{
    memset(st, 0, sizeof(*st));
    st->modules = (unsigned)n;

    for (int i = 0; i < n; i++) {
        Module *m = &mods[i];
        for (unsigned j = 0; j < m->nreloc; j++) {
            const Reloc *r = &m->relocs[j];

            if (r->where < m->data_off || r->where + 8 > m->key_off ||
                r->symbol >= m->nsym) {
                snprintf(err, errsz,
                         "%s reloc %u out of bounds (where %#llx data_off %#llx key_off %#llx symbol %u/%u)",
                         m->name, j, (unsigned long long)r->where,
                         (unsigned long long)m->data_off, (unsigned long long)m->key_off,
                         r->symbol, m->nsym);
                return -1;
            }

            uintptr_t v;
            if (r->kind == 1027) {                 /* relative */
                if (r->addend < 0 || (uint64_t)r->addend >= m->key_off) {
                    snprintf(err, errsz, "%s reloc %u: relative addend %#llx",
                             m->name, j, (unsigned long long)r->addend);
                    return -1;
                }
                v = (uintptr_t)m->base + (uint64_t)r->addend;
                m->relative++;
                st->relative++;
            } else {
                if (r->kind != 257 && r->kind != 1025 && r->kind != 1026) {
                    snprintf(err, errsz, "%s reloc %u: unknown kind %u", m->name, j, r->kind);
                    return -1;
                }
                const Symbol *s = &m->symbols[r->symbol];
                void *p = override_fn ? override_fn(s->name) : NULL;
                if (!p && s->defined)
                    p = s->value < m->key_off ? m->base + s->value : NULL;
                if (!p && !s->defined)
                    p = mng_loader_symbol(mods, n, s->name);

                if (!p) {
                    if (s->binding != 2) {          /* 2 = weak */
                        st->strong_missing++;
                        note_missing(s->name);
                        p = NULL;                   /* recorded, not fatal */
                    } else {
                        m->weak++;
                        st->weak++;
                    }
                }
                if (p && !s->defined) { m->cross++; st->cross++; }
                m->symbolic++;
                st->symbolic++;
                v = (uintptr_t)p + (uintptr_t)(r->kind == 1026 ? 0 : r->addend);
            }
            memcpy(m->base + r->where, &v, 8);
        }
    }
    return 0;
}
