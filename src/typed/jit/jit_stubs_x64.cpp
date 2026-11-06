// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The hand-written assembly at the edge of native code, for x86-64. It is alone in this file because it refers to C
// functions by name, which link-time optimization cannot see; CMakeLists.txt compiles this one file without it.

#if defined(SCRIPT_ARCH_X64) && !defined(_WIN32)

// `saved` in the bridge is rdi, rsi, rdx, rcx, r8, r9, then xmm0-7, then the two result words. A jump back out needs r10 and
// r11 as they came.
__asm__(R"ASM(
    .text
    .p2align 4
    .globl scriptJitLazyStub
    .type scriptJitLazyStub, @function
scriptJitLazyStub:
    xor %eax, %eax
    jmp 1f
    .globl scriptJitLazyDelegateStub
    .type scriptJitLazyDelegateStub, @function
scriptJitLazyDelegateStub:
    mov %r10, %rax
1:
    push %rbp
    mov %rsp, %rbp
    sub $144, %rsp
    mov %rdi, 0(%rsp)
    mov %rsi, 8(%rsp)
    mov %rdx, 16(%rsp)
    mov %rcx, 24(%rsp)
    mov %r8, 32(%rsp)
    mov %r9, 40(%rsp)
    movsd %xmm0, 48(%rsp)
    movsd %xmm1, 56(%rsp)
    movsd %xmm2, 64(%rsp)
    movsd %xmm3, 72(%rsp)
    movsd %xmm4, 80(%rsp)
    movsd %xmm5, 88(%rsp)
    movsd %xmm6, 96(%rsp)
    movsd %xmm7, 104(%rsp)
    movq $0, 112(%rsp)
    movq $0, 120(%rsp)
    mov %r10, 128(%rsp)
    mov %r11, 136(%rsp)
    mov %r15, %rdi
    mov %r11, %rsi
    mov %rsp, %rdx
    lea 16(%rbp), %rcx
    mov %rax, %r8
    mov %rbp, %r9
    call scriptJitBridge
    mov %rax, %r11
    test %r11, %r11
    jz 2f
    // Compiled just now: in with the arguments as they came.
    mov 0(%rsp), %rdi
    mov 8(%rsp), %rsi
    mov 16(%rsp), %rdx
    mov 24(%rsp), %rcx
    mov 32(%rsp), %r8
    mov 40(%rsp), %r9
    movsd 48(%rsp), %xmm0
    movsd 56(%rsp), %xmm1
    movsd 64(%rsp), %xmm2
    movsd 72(%rsp), %xmm3
    movsd 80(%rsp), %xmm4
    movsd 88(%rsp), %xmm5
    movsd 96(%rsp), %xmm6
    movsd 104(%rsp), %xmm7
    mov 128(%rsp), %r10
    mov %r11, %rax
    mov 136(%rsp), %r11
    leave
    jmp *%rax
2:
    mov 112(%rsp), %rax
    mov 120(%rsp), %rdx
    movsd 48(%rsp), %xmm0
    leave
    ret
    .size scriptJitLazyDelegateStub, .-scriptJitLazyDelegateStub

    // Answers in rax: nonzero means stop. Keeps every register allocation can use.
    .p2align 4
    .globl scriptJitPollStub
    .type scriptJitPollStub, @function
scriptJitPollStub:
    push %rbp
    mov %rsp, %rbp
    sub $144, %rsp
    mov %rsi, 0(%rsp)
    mov %rdi, 8(%rsp)
    mov %r8, 16(%rsp)
    mov %r9, 24(%rsp)
    movsd %xmm0, 32(%rsp)
    movsd %xmm1, 40(%rsp)
    movsd %xmm2, 48(%rsp)
    movsd %xmm3, 56(%rsp)
    movsd %xmm4, 64(%rsp)
    movsd %xmm5, 72(%rsp)
    movsd %xmm6, 80(%rsp)
    movsd %xmm7, 88(%rsp)
    movsd %xmm8, 96(%rsp)
    movsd %xmm9, 104(%rsp)
    movsd %xmm10, 112(%rsp)
    movsd %xmm11, 120(%rsp)
    movsd %xmm12, 128(%rsp)
    movsd %xmm13, 136(%rsp)
    mov %r15, %rdi
    call scriptJitPoll
    movslq %eax, %rax
    mov 0(%rsp), %rsi
    mov 8(%rsp), %rdi
    mov 16(%rsp), %r8
    mov 24(%rsp), %r9
    movsd 32(%rsp), %xmm0
    movsd 40(%rsp), %xmm1
    movsd 48(%rsp), %xmm2
    movsd 56(%rsp), %xmm3
    movsd 64(%rsp), %xmm4
    movsd 72(%rsp), %xmm5
    movsd 80(%rsp), %xmm6
    movsd 88(%rsp), %xmm7
    movsd 96(%rsp), %xmm8
    movsd 104(%rsp), %xmm9
    movsd 112(%rsp), %xmm10
    movsd 120(%rsp), %xmm11
    movsd 128(%rsp), %xmm12
    movsd 136(%rsp), %xmm13
    leave
    ret
    .size scriptJitPollStub, .-scriptJitPollStub

    // void scriptJitEnter(rt, entry, ints, fps, stackArgs, stackCount, delegate, intResults, fpResult)
    .p2align 4
    .globl scriptJitEnter
    .type scriptJitEnter, @function
scriptJitEnter:
    push %rbp
    mov %rsp, %rbp
    push %r15
    push %rbx
    mov %rdi, %r15
    mov %rsi, %rax
    mov %rdx, %r10
    mov %rcx, %r11
    // Stack words, keeping the stack aligned at the call.
    mov %r9, %rbx
    add $1, %rbx
    and $-2, %rbx
    shl $3, %rbx
    sub %rbx, %rsp
    xor %ebx, %ebx
3:
    cmp %r9, %rbx
    jae 4f
    mov (%r8, %rbx, 8), %rdi
    mov %rdi, (%rsp, %rbx, 8)
    add $1, %rbx
    jmp 3b
4:
    movsd 0(%r11), %xmm0
    movsd 8(%r11), %xmm1
    movsd 16(%r11), %xmm2
    movsd 24(%r11), %xmm3
    movsd 32(%r11), %xmm4
    movsd 40(%r11), %xmm5
    movsd 48(%r11), %xmm6
    movsd 56(%r11), %xmm7
    mov 0(%r10), %rdi
    mov 8(%r10), %rsi
    mov 16(%r10), %rdx
    mov 24(%r10), %rcx
    mov 32(%r10), %r8
    mov 40(%r10), %r9
    mov 16(%rbp), %r10
    call *%rax
    mov 24(%rbp), %rcx
    mov %rax, 0(%rcx)
    mov %rdx, 8(%rcx)
    mov 32(%rbp), %rcx
    movsd %xmm0, (%rcx)
    lea -16(%rbp), %rsp
    pop %rbx
    pop %r15
    pop %rbp
    ret
    .size scriptJitEnter, .-scriptJitEnter
)ASM");

#endif
