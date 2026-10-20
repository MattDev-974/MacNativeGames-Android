/* mng_loader.h - loader-side types and the platform-neutral relocation pass.
 *
 * The types mirror runtime/runtime.h of MacNativeGames, because the generated
 * runtime_tables.h uses designated initialisers with those exact field names.
 * Everything here is portable C: mapping goes through mng_image.c, and the
 * symbol/relocation pass is a direct port of relocate() from loader.c.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t where; unsigned kind, symbol; int64_t addend; } Reloc;
typedef struct { const char *name; uint64_t value, size; unsigned defined, binding, type; } Symbol;
typedef struct { uint64_t ret, original; unsigned timer, arg; } Gate;

typedef struct {
    const char *name, *digest;
    size_t file_size, size;
    uint64_t text_end, ro_off, data_off, patch_off, key_off;
    uint64_t init, ctor_flag, init_array, init_size;
    const Symbol *symbols; unsigned nsym;
    const Reloc *relocs; unsigned nreloc;
    const Gate *gates; unsigned ngate;
    unsigned *hash, hash_mask;
    uint8_t *base;
    unsigned relative, symbolic, cross, weak;
} Module;

typedef struct {
    uint64_t relative, symbolic, cross, weak, strong_missing;
    unsigned modules;
} MngRelocStats;

/* Resolve a host replacement for an imported name (Horizon service, libc...).
 * Return NULL when the host does not implement it yet: the loader then reports
 * the name instead of failing, which is how the missing-service list is built. */
typedef void *(*MngOverrideFn)(const char *name);

int  mng_loader_map_all(Module *mods, int n, const char *dir, int native_names,
                        char *err, size_t errsz);
void mng_loader_fill_keys(Module *mods, int n, const uint64_t keys[8]);
int  mng_loader_relocate(Module *mods, int n, MngOverrideFn override_fn,
                         MngRelocStats *st, char *err, size_t errsz);
void *mng_loader_symbol(Module *mods, int n, const char *name);
void mng_loader_free_hash(Module *mods, int n);

/* Names of the imports that resolved to nothing, for the report. */
const char *mng_loader_missing(unsigned index);
unsigned    mng_loader_missing_count(void);
