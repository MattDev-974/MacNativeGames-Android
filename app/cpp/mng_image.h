/* mng_image.h - see mng_image.c. Loader-facing description of one module. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MNG_KEY_COUNT 8

typedef struct {
    uint64_t patch_off;
    uint64_t key_page_off, key_page_size;
    uint64_t text_end, ro_off, data_off;
    int k18, ktls, ktlr, kgate, kres0, kres1, kaspect, kaspectui;
} MngLayout;

int mng_layout_parse(const char *tsv_path, MngLayout *out);
int mng_image_map(const char *path, const MngLayout *l, void **base_out,
                  uint64_t *image_size_out, char *err, size_t err_size);
int mng_image_protect(void *base, uint64_t size, const MngLayout *l,
                      char *err, size_t err_size);
void mng_image_set_keys(void *base, const MngLayout *l, const uint64_t keys[MNG_KEY_COUNT]);
uint64_t mng_image_key(void *base, const MngLayout *l, int index);
uint64_t mng_image_gate_slot(void *base, const MngLayout *l);
void mng_image_unmap(void *base, uint64_t size);
