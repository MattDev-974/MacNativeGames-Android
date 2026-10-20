/* mng_gate.h - the C side of the svc gate, Android/aarch64. */
#pragma once
#include <stdint.h>
#include "mng_loader.h"

/* Horizon call handler.  Returns the value the guest expects in whatever
 * register the call documents; out-parameters are written into regs[] like
 * runtime_svc() does on the Apple side. */
typedef uint64_t (*MngSvcFn)(unsigned num, uint64_t *regs, const char *module,
                            uint64_t original);

void     mng_gate_init(Module *mods, int n, MngSvcFn svc);
void     mng_gate_dispatch(uint64_t site, uint64_t *regs);
uint64_t mng_gate_ticks(void);          /* CNTPCT at 19.2 MHz, as the Switch */

/* gate.S: saves every register, resolves the call, writes the results back. */
uint64_t hle_gate(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);
unsigned mng_gate_count(void);
