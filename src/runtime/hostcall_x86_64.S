/* uint64_t mm_hostcall(mm_cpu_context *ctx, const void *fn)
 *
 * Calls a host C function with the guest's argument registers:
 *   x0-x5 -> rdi, rsi, rdx, rcx, r8, r9;  x6, x7 -> first two stack args
 *   [guest sp] .. [guest sp+40] -> the six stack args after those
 *   v0-v7 (low 64 bits) -> xmm0-xmm7;    al = 8 for variadic callees
 * and stores the results back the way an AArch64 callee would:
 *   rax -> x0;  xmm0 -> v0, xmm1 -> v1 (double and two-double returns)
 * rdx is deliberately NOT written to x1: most callees leave it holding the
 * third argument, and overwriting x1 corrupts the guest's view of a
 * register the resumed code may still be using (an objc IMP's _cmd, say).
 * A host function that really returns two words writes the second one
 * itself, through mm_objc_return_words_hi.
 * AArch64 passes everything past x7 on the stack, so those words are
 * copied too: a host function of up to fourteen integer arguments then
 * gets all of them. CFStringGetBytes is one (its ninth argument is where
 * it writes the byte count), and a host function reading a stack slot the
 * guest never filled is the hardest kind of bug to see — it returns, and
 * the answer is garbage. Copying words the callee does not take is
 * harmless: the caller owns that area of the stack.
 * Offsets below must match mm_cpu_context (checked by static_assert in loader.cpp). */
    .intel_syntax noprefix
    .text
    .globl mm_hostcall
    .type mm_hostcall, @function
mm_hostcall:
    push rbp
    mov  rbp, rsp
    push rbx
    push r12
    mov  rbx, rdi              /* ctx */
    mov  r12, rsi              /* fn */

    movsd xmm0, qword ptr [rbx + 384 + 0*16]
    movsd xmm1, qword ptr [rbx + 384 + 1*16]
    movsd xmm2, qword ptr [rbx + 384 + 2*16]
    movsd xmm3, qword ptr [rbx + 384 + 3*16]
    movsd xmm4, qword ptr [rbx + 384 + 4*16]
    movsd xmm5, qword ptr [rbx + 384 + 5*16]
    movsd xmm6, qword ptr [rbx + 384 + 6*16]
    movsd xmm7, qword ptr [rbx + 384 + 7*16]

    mov  rax, [rbx + 31*8]      /* guest sp: the arguments past x7 */
    push qword ptr [rax + 5*8]
    push qword ptr [rax + 4*8]
    push qword ptr [rax + 3*8]
    push qword ptr [rax + 2*8]
    push qword ptr [rax + 1*8]
    push qword ptr [rax + 0*8]
    push qword ptr [rbx + 7*8]  /* x7 -> second stack arg */
    push qword ptr [rbx + 6*8]  /* x6 -> first stack arg (8 pushes: rsp stays 16-aligned) */
    mov  rdi, [rbx + 0*8]
    mov  rsi, [rbx + 1*8]
    mov  rdx, [rbx + 2*8]
    mov  rcx, [rbx + 3*8]
    mov  r8,  [rbx + 4*8]
    mov  r9,  [rbx + 5*8]
    mov  eax, 8
    call r12
    add  rsp, 64

    mov  [rbx + 0*8], rax

    movsd qword ptr [rbx + 384 + 0*16], xmm0
    movsd qword ptr [rbx + 384 + 1*16], xmm1

    pop  r12
    pop  rbx
    pop  rbp
    ret
    .size mm_hostcall, .-mm_hostcall
    .section .note.GNU-stack,"",@progbits
