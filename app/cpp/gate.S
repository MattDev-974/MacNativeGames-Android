/* gate.S - the HLE gate, Android/aarch64.
 *
 * A direct transliteration of games/totk/runtime/gate.S from MacNativeGames,
 * minus the Mach thread-pointer stack switch (Android threads already have a
 * large stack; the real port keeps the switch for the game's 8 KiB guest
 * stacks -- see docs/02-portage-android.md, "pile d'invite").
 *
 * The patcher rewrites every `svc` (and `mrs cntpct`/platform-register site)
 * into a `bl hle_gate`.  Because a gate may write any general-purpose
 * register -- syscall out-parameters land in x0..x8 and the counter target can
 * be a callee-saved register -- the whole integer and FP register file is
 * saved, the C dispatcher runs, and every register is read back.
 */
.text

    .p2align 4
    .globl hle_gate
    .type  hle_gate, %function
hle_gate:
    /* Entered with TPIDR_EL0 pointing at this thread's guest block.
     * TPIDRRO_EL0 cannot be used: on AArch64 it is read-only at EL0 (an MSR to
     * it is an illegal instruction, measured on the device), so the rewriter
     * emits `mrs xN, tpidr_el0` and the host swaps the register around each
     * call instead.  The saved Bionic TLS lives inside the block, at offset
     * 4096, so the swap never needs thread-local storage itself. */
    mrs  x9, tpidr_el0
    sub  sp, sp, #800
    str  x9, [sp, #792]        /* guest block */
    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    str  x18,      [sp, #144]
    stp  x19, x20, [sp, #152]
    stp  x21, x22, [sp, #168]
    stp  x23, x24, [sp, #184]
    stp  x25, x26, [sp, #200]
    stp  x27, x28, [sp, #216]
    stp  x29, x30, [sp, #232]
    stp  q0,  q1,  [sp, #256]
    stp  q2,  q3,  [sp, #288]
    stp  q4,  q5,  [sp, #320]
    stp  q6,  q7,  [sp, #352]
    stp  q8,  q9,  [sp, #384]
    stp  q10, q11, [sp, #416]
    stp  q12, q13, [sp, #448]
    stp  q14, q15, [sp, #480]
    stp  q16, q17, [sp, #512]
    stp  q18, q19, [sp, #544]
    stp  q20, q21, [sp, #576]
    stp  q22, q23, [sp, #608]
    stp  q24, q25, [sp, #640]
    stp  q26, q27, [sp, #672]
    stp  q28, q29, [sp, #704]
    stp  q30, q31, [sp, #736]
    mrs  x9, nzcv
    str  x9, [sp, #768]
    mrs  x9, fpsr
    str  x9, [sp, #776]
    mrs  x9, fpcr
    str  x9, [sp, #784]

    ldr  x10, [x9, #4096]      /* Bionic's TLS, saved at thread start */
    msr  tpidr_el0, x10

    mov  x0, x30          /* site: the return address the patcher left */
    mov  x1, sp           /* &regs[0] */
    bl   runtime_dispatch

    ldr  x9, [sp, #792]
    msr  tpidr_el0, x9         /* back to the guest block */

    ldr  x9, [sp, #784]
    msr  fpcr, x9
    ldr  x9, [sp, #776]
    msr  fpsr, x9
    ldr  x9, [sp, #768]
    msr  nzcv, x9
    ldp  q30, q31, [sp, #736]
    ldp  q28, q29, [sp, #704]
    ldp  q26, q27, [sp, #672]
    ldp  q24, q25, [sp, #640]
    ldp  q22, q23, [sp, #608]
    ldp  q20, q21, [sp, #576]
    ldp  q18, q19, [sp, #544]
    ldp  q16, q17, [sp, #512]
    ldp  q14, q15, [sp, #480]
    ldp  q12, q13, [sp, #448]
    ldp  q10, q11, [sp, #416]
    ldp  q8,  q9,  [sp, #384]
    ldp  q6,  q7,  [sp, #352]
    ldp  q4,  q5,  [sp, #320]
    ldp  q2,  q3,  [sp, #288]
    ldp  q0,  q1,  [sp, #256]
    ldp  x0,  x1,  [sp, #0]
    ldp  x2,  x3,  [sp, #16]
    ldp  x4,  x5,  [sp, #32]
    ldp  x6,  x7,  [sp, #48]
    ldp  x8,  x9,  [sp, #64]
    ldp  x10, x11, [sp, #80]
    ldp  x12, x13, [sp, #96]
    ldp  x14, x15, [sp, #112]
    ldp  x16, x17, [sp, #128]
    ldr  x18,      [sp, #144]
    ldp  x19, x20, [sp, #152]
    ldp  x21, x22, [sp, #168]
    ldp  x23, x24, [sp, #184]
    ldp  x25, x26, [sp, #200]
    ldp  x27, x28, [sp, #216]
    ldp  x29, x30, [sp, #232]
    add  sp, sp, #800
    ret
    .size hle_gate, . - hle_gate

/* guest_enter(stack_top, fn, arg): run fn(arg) on a different stack.
 * This is the guest/host stack boundary the real loader needs too: the game's
 * threads run on 8 KiB stacks that cannot hold the HLE implementation. */
    .p2align 4
    .globl guest_enter
    .type  guest_enter, %function
guest_enter:
    stp  x19, x20, [sp, #-16]!
    stp  x29, x30, [sp, #-16]!
    mov  x9, sp
    mov  sp, x0                 /* switch to the guest stack */
    stp  x9, xzr, [sp, #-16]!   /* remember the host stack */
    mov  x19, x1
    mov  x20, x2
    mov  x0, x20
    blr  x19
    ldr  x9, [sp], #16
    mov  sp, x9                 /* back to the host stack */
    ldp  x29, x30, [sp], #16
    ldp  x19, x20, [sp], #16
    ret
    .size guest_enter, . - guest_enter

    .section .note.GNU-stack,"",%progbits
