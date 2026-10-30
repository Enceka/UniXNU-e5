// Static Darwin entry: preserve the kernel-provided stack before a C prologue.
.text
.p2align 2
.globl __start
__start:
    mov x0, sp
    b _e5_main
