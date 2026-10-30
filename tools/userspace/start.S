// Static Darwin entry: preserve the kernel-provided stack before a C prologue.
.text
.p2align 2
.globl __start
__start:
    mov x0, sp
    // XNU's static argc/argv area is pointer-aligned, which can leave SP
    // at 8 modulo 16. Preserve its address in x0, but align the C stack.
    and x9, x0, #0xfffffffffffffff0
    mov sp, x9
    b _e5_main
