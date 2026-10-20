/* asm_test.S - does hle_gate really preserve every register the guest expects?
 *
 * Puts a distinct sentinel in x19..x28 (callee-saved) and q8..q15 (callee-saved
 * SIMD), crosses the gate, and counts the ones that came back changed.  This is
 * the property the whole port rests on: a patched `svc` site must be invisible
 * to the game's compiled code.
 */
.text
    .p2align 4
    .globl test_regs
    .type  test_regs, %function
test_regs:
    stp  x29, x30, [sp, #-16]!
    mov  x29, sp
    stp  x19, x20, [sp, #-16]!
    stp  x21, x22, [sp, #-16]!
    stp  x23, x24, [sp, #-16]!
    stp  x25, x26, [sp, #-16]!
    stp  x27, x28, [sp, #-16]!

    mov  x19, #0x1000
    mov  x20, #0x2000
    mov  x21, #0x3000
    mov  x22, #0x4000
    mov  x23, #0x5000
    mov  x24, #0x6000
    mov  x25, #0x7000
    mov  x26, #0x8000
    mov  x27, #0x9000
    mov  x28, #0xA000

    /* q8..q15: two lanes each, easy to compare */
    movi v8.4s,  #0x11
    movi v9.4s,  #0x22
    movi v10.4s, #0x33
    movi v11.4s, #0x44
    movi v12.4s, #0x55
    movi v13.4s, #0x66
    movi v14.4s, #0x77
    movi v15.4s, #0x88

    /* HLE_LOG with a null pointer: returns 0, logs nothing, clobbers x0..x18 */
    mov  x0, #0x13
    mov  x1, #0
    mov  x2, #0
    mov  x3, #0
    bl   hle_gate

    mov  x9, #0
    mov  x10, #0x1000
    cmp  x19, x10
    cinc x9, x9, ne
    mov  x10, #0x2000
    cmp  x20, x10
    cinc x9, x9, ne
    mov  x10, #0x3000
    cmp  x21, x10
    cinc x9, x9, ne
    mov  x10, #0x4000
    cmp  x22, x10
    cinc x9, x9, ne
    mov  x10, #0x5000
    cmp  x23, x10
    cinc x9, x9, ne
    mov  x10, #0x6000
    cmp  x24, x10
    cinc x9, x9, ne
    mov  x10, #0x7000
    cmp  x25, x10
    cinc x9, x9, ne
    mov  x10, #0x8000
    cmp  x26, x10
    cinc x9, x9, ne
    mov  x10, #0x9000
    cmp  x27, x10
    cinc x9, x9, ne
    mov  x10, #0xA000
    cmp  x28, x10
    cinc x9, x9, ne

    /* umov w10, v8.s[0] etc. would need lane extraction; compare with vsub */
    movi v0.4s, #0x11
    cmeq v0.4s, v8.4s, v0.4s
    uminv s1, v0.4s
    fmov w10, s1
    cmn  w10, #1
    cinc x9, x9, ne

    movi v0.4s, #0x22
    cmeq v0.4s, v9.4s, v0.4s
    uminv s1, v0.4s
    fmov w10, s1
    cmn  w10, #1
    cinc x9, x9, ne

    movi v0.4s, #0x88
    cmeq v0.4s, v15.4s, v0.4s
    uminv s1, v0.4s
    fmov w10, s1
    cmn  w10, #1
    cinc x9, x9, ne

    mov  x0, x9

    ldp  x27, x28, [sp], #16
    ldp  x25, x26, [sp], #16
    ldp  x23, x24, [sp], #16
    ldp  x21, x22, [sp], #16
    ldp  x19, x20, [sp], #16
    ldp  x29, x30, [sp], #16
    ret
    .size test_regs, . - test_regs

    .section .note.GNU-stack,"",%progbits
