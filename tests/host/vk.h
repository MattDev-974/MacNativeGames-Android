/* Host stand-in for app/cpp/vk.h: only the seam nvn.c touches. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void vk_cmd_bind_vertex_buffer(uint64_t gpu_addr, uint32_t bytes);
void vk_cmd_set_uniform(uint64_t gpu_addr);
void vk_cmd_draw(uint32_t first, uint32_t count);
void vk_cmd_viewport(float x, float y, float w, float h);
int  vk_pool_add(void *base, uint64_t size);
void vk_pool_del(void *base);
int  vk_pool_translate(uint64_t gpu_addr, uint64_t len, uint64_t *offset_out);
int  vk_frame_begin(float r, float g, float b, float a);
int  vk_frame_end(void);
