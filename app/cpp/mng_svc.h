#pragma once
#include <stdint.h>
uint64_t mng_svc(unsigned num, uint64_t *regs, const char *module, uint64_t original);
unsigned mng_svc_distinct(void);
