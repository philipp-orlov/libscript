// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The hand-written assembly at the edge of native code. It is alone in this file because it refers to C functions by
// name, which link-time optimization cannot see; CMakeLists.txt compiles this one file without it.

#if defined(SCRIPT_ARCH_ARM64)

// The stubs. `saved` in the bridge is x0-x7, then d0-d7, then the two result words. A jump back out needs x14 and x15 as
// they came.
__asm__(R"ASM(
    .text
    .p2align 2
    .globl scriptJitLazyStub
    .type scriptJitLazyStub, %function
scriptJitLazyStub:
    mov x17, #0
    b 1f
    .globl scriptJitLazyDelegateStub
    .type scriptJitLazyDelegateStub, %function
scriptJitLazyDelegateStub:
    mov x17, x14
1:
    stp x29, x30, [sp, #-192]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp d0, d1, [sp, #80]
    stp d2, d3, [sp, #96]
    stp d4, d5, [sp, #112]
    stp d6, d7, [sp, #128]
    stp xzr, xzr, [sp, #144]
    stp x14, x15, [sp, #160]
    mov x0, x20
    mov x1, x15
    add x2, sp, #16
    add x3, sp, #192
    mov x4, x17
    mov x5, x29
    bl scriptJitBridge
    mov x16, x0
    ldp x0, x1, [sp, #16]
    cbnz x16, 5f
    ldp x0, x1, [sp, #144]
5:
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldp d0, d1, [sp, #80]
    ldp d2, d3, [sp, #96]
    ldp d4, d5, [sp, #112]
    ldp d6, d7, [sp, #128]
    ldp x14, x15, [sp, #160]
    ldp x29, x30, [sp], #192
    cbz x16, 2f
    br x16
2:
    ret
    .size scriptJitLazyDelegateStub, .-scriptJitLazyDelegateStub

    // Everything a C call would clobber, kept: x0-x15, d0-d7 and d16-d29. Used by the next two.
    .macro SAVE_ALL
    stp x29, x30, [sp, #-336]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp x8, x9, [sp, #80]
    stp x10, x11, [sp, #96]
    stp x12, x13, [sp, #112]
    stp x14, x15, [sp, #128]
    stp d0, d1, [sp, #144]
    stp d2, d3, [sp, #160]
    stp d4, d5, [sp, #176]
    stp d6, d7, [sp, #192]
    stp d16, d17, [sp, #208]
    stp d18, d19, [sp, #224]
    stp d20, d21, [sp, #240]
    stp d22, d23, [sp, #256]
    stp d24, d25, [sp, #272]
    stp d26, d27, [sp, #288]
    stp d28, d29, [sp, #304]
    .endm
    .macro RESTORE_ALL
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldp x8, x9, [sp, #80]
    ldp x10, x11, [sp, #96]
    ldp x12, x13, [sp, #112]
    ldp x14, x15, [sp, #128]
    ldp d0, d1, [sp, #144]
    ldp d2, d3, [sp, #160]
    ldp d4, d5, [sp, #176]
    ldp d6, d7, [sp, #192]
    ldp d16, d17, [sp, #208]
    ldp d18, d19, [sp, #224]
    ldp d20, d21, [sp, #240]
    ldp d22, d23, [sp, #256]
    ldp d24, d25, [sp, #272]
    ldp d26, d27, [sp, #288]
    ldp d28, d29, [sp, #304]
    ldp x29, x30, [sp], #336
    .endm

    // x16: the engine's poll function. Answers in x16: nonzero means stop.
    .p2align 2
    .globl scriptJitPollStub
    .type scriptJitPollStub, %function
scriptJitPollStub:
    SAVE_ALL
    mov x0, x20
    blr x16
    sxtw x16, w0
    RESTORE_ALL
    ret
    .size scriptJitPollStub, .-scriptJitPollStub

    // void scriptJitEnter(rt, entry, ints, fps, stackArgs, stackCount, delegate, intResult, fpResult)
    .p2align 2
    .globl scriptJitEnter
    .type scriptJitEnter, %function
scriptJitEnter:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x20, x21, [sp, #16]
    str x22, [sp, #32]
    ldr x22, [x29, #48]
    mov x21, x7
    mov x20, x0
    mov x16, x1
    mov x14, x6
    mov x9, x2
    mov x10, x3
    add x11, x5, #1
    and x11, x11, #-2
    sub sp, sp, x11, lsl #3
    mov x12, #0
3:
    cmp x12, x5
    b.hs 4f
    ldr x13, [x4, x12, lsl #3]
    str x13, [sp, x12, lsl #3]
    add x12, x12, #1
    b 3b
4:
    ldp x0, x1, [x9]
    ldp x2, x3, [x9, #16]
    ldp x4, x5, [x9, #32]
    ldp x6, x7, [x9, #48]
    ldp d0, d1, [x10]
    ldp d2, d3, [x10, #16]
    ldp d4, d5, [x10, #32]
    ldp d6, d7, [x10, #48]
    blr x16
    stp x0, x1, [x21]
    str d0, [x22]
    mov sp, x29
    ldp x20, x21, [sp, #16]
    ldr x22, [sp, #32]
    ldp x29, x30, [sp], #48
    ret
    .size scriptJitEnter, .-scriptJitEnter

    // NativePair scriptJitCall5(rt, entry, a0, a1, a2, a3, a4): five integer arguments and a pair back, which is all a
    // dynamic language's function takes, without the tables scriptJitEnter reads them from.
    .p2align 2
    .globl scriptJitCall5
    .type scriptJitCall5, %function
scriptJitCall5:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x20, [sp, #16]
    mov x20, x0
    mov x16, x1
    mov x0, x2
    mov x1, x3
    mov x2, x4
    mov x3, x5
    mov x4, x6
    blr x16
    ldr x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
    .size scriptJitCall5, .-scriptJitCall5
)ASM");

#endif
