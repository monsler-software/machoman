#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "setjmp: %s\n", message); exit(1); }
}

/* Use the SDK's exact size and only int alignment, with adjacent canaries. */
static struct { uint32_t before; jmp_buf buffer; unsigned char after[64]; } plain;
static struct { uint32_t before; sigjmp_buf buffer; unsigned char after[64]; } masked;

__attribute__((noinline, noreturn)) static void jump_deeper(int depth, int variant, int value) {
    volatile unsigned char stack[384];
    memset((void *)stack, depth, sizeof stack);
    if (depth) jump_deeper(depth - 1, variant, value);
    if (variant == 0) longjmp(plain.buffer, value);
    if (variant == 1) _longjmp(plain.buffer, value);
    siglongjmp(masked.buffer, value);
}

static void canaries(void) {
    check(plain.before == 0xa5a5a5a5u && masked.before == 0xa5a5a5a5u, "leading canaries");
    for (unsigned i = 0; i < sizeof plain.after; ++i)
        check(plain.after[i] == 0xa5 && masked.after[i] == 0xa5, "SDK buffer bounds");
}

extern int register_roundtrip(void *buffer);
#if defined(__x86_64__)
__asm__(
    ".text\n.globl _register_roundtrip\n_register_roundtrip:\n"
    "pushq %rbp\npushq %rbx\npushq %r12\npushq %r13\npushq %r14\npushq %r15\n"
    "subq $8, %rsp\nmovq %rdi, (%rsp)\n"
    "movq $19, %rbx\nmovq $29, %rbp\nmovq $12, %r12\nmovq $13, %r13\nmovq $14, %r14\nmovq $15, %r15\n"
    "callq __setjmp\ntestl %eax, %eax\njnz 1f\n"
    "xorq %rbx, %rbx\nxorq %rbp, %rbp\nxorq %r12, %r12\nxorq %r13, %r13\nxorq %r14, %r14\nxorq %r15, %r15\n"
    "movq (%rsp), %rdi\nxorl %esi, %esi\ncallq __longjmp\nud2\n"
    "1: cmpl $1, %eax\njne 2f\ncmpq $19, %rbx\njne 2f\ncmpq $29, %rbp\njne 2f\n"
    "cmpq $12, %r12\njne 2f\ncmpq $13, %r13\njne 2f\ncmpq $14, %r14\njne 2f\ncmpq $15, %r15\njne 2f\n"
    "movl $1, %eax\njmp 3f\n2: xorl %eax, %eax\n"
    "3: addq $8, %rsp\npopq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %rbx\npopq %rbp\nretq\n");
#else
__asm__(
    ".text\n.globl _register_roundtrip\n.p2align 2\n_register_roundtrip:\n"
    "stp x29, x30, [sp, #-192]!\nstp x19, x20, [sp, #16]\nstp x21, x22, [sp, #32]\n"
    "stp x23, x24, [sp, #48]\nstp x25, x26, [sp, #64]\nstp x27, x28, [sp, #80]\n"
    "stp d8, d9, [sp, #96]\nstp d10, d11, [sp, #112]\nstp d12, d13, [sp, #128]\nstp d14, d15, [sp, #144]\n"
    "str x0, [sp, #160]\n"
    ".irp n,19,20,21,22,23,24,25,26,27,28,29\nmov x\\n, #\\n\n.endr\n"
    ".irp n,8,9,10,11,12,13,14,15\nmov x9, #\\n\nfmov d\\n, x9\n.endr\n"
    "bl __setjmp\ncbnz w0, 1f\n"
    ".irp n,19,20,21,22,23,24,25,26,27,28,29\nmov x\\n, #0\n.endr\n"
    ".irp n,8,9,10,11,12,13,14,15\nfmov d\\n, xzr\n.endr\n"
    "ldr x0, [sp, #160]\nmov w1, #0\nbl __longjmp\nbrk #0\n"
    "1: cmp w0, #1\nb.ne 2f\n"
    ".irp n,19,20,21,22,23,24,25,26,27,28,29\ncmp x\\n, #\\n\nb.ne 2f\n.endr\n"
    ".irp n,8,9,10,11,12,13,14,15\nfmov x9, d\\n\ncmp x9, #\\n\nb.ne 2f\n.endr\n"
    "mov w0, #1\nb 3f\n2: mov w0, #0\n"
    "3: ldp x19, x20, [sp, #16]\nldp x21, x22, [sp, #32]\nldp x23, x24, [sp, #48]\n"
    "ldp x25, x26, [sp, #64]\nldp x27, x28, [sp, #80]\n"
    "ldp d8, d9, [sp, #96]\nldp d10, d11, [sp, #112]\nldp d12, d13, [sp, #128]\nldp d14, d15, [sp, #144]\n"
    "ldp x29, x30, [sp], #192\nret\n");
#endif

int main(void) {
    memset(&plain, 0xa5, sizeof plain);
    memset(&masked, 0xa5, sizeof masked);
    sigset_t original, blocked, empty;
    sigemptyset(&empty);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    check(sigprocmask(SIG_SETMASK, &blocked, &original) == 0, "initial mask");
    volatile uint64_t local = 0x123456789abcdef0ull;
    int value = setjmp(plain.buffer);
    if (!value) { sigprocmask(SIG_SETMASK, &empty, NULL); jump_deeper(3, 0, 42); }
    check(value == 42 && local == 0x123456789abcdef0ull, "nested return and stack locals");
    sigset_t current;
    sigprocmask(SIG_SETMASK, NULL, &current);
    check(sigismember(&current, SIGUSR1), "setjmp restores signal mask");
    value = _setjmp(plain.buffer);
    if (!value) { sigprocmask(SIG_SETMASK, &empty, NULL); jump_deeper(3, 1, 0); }
    check(value == 1, "zero longjmp value becomes one");
    sigprocmask(SIG_SETMASK, NULL, &current);
    check(!sigismember(&current, SIGUSR1), "_setjmp leaves signal mask");
    for (volatile int save = 0; save <= 1; ++save) {
        sigprocmask(SIG_SETMASK, &blocked, NULL);
        value = sigsetjmp(masked.buffer, save);
        if (!value) { sigprocmask(SIG_SETMASK, &empty, NULL); jump_deeper(2, 2, -7); }
        check(value == -7, "negative siglongjmp value");
        sigprocmask(SIG_SETMASK, NULL, &current);
        check(sigismember(&current, SIGUSR1) == save, "sigsetjmp optional signal mask");
    }
    check(register_roundtrip(plain.buffer), "callee-saved registers and stack restore");
#if defined(__x86_64__)
    uint32_t original_mxcsr, restored_mxcsr;
    uint16_t original_control, restored_control;
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(original_mxcsr), "=m"(original_control));
    const uint32_t target_mxcsr = (original_mxcsr & ~0x6000u) | 0x4000u;
    const uint16_t target_control = (uint16_t)((original_control & ~0xc00u) | 0x800u);
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(target_mxcsr), "m"(target_control));
    value = _setjmp(plain.buffer);
    if (!value) {
        __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(original_mxcsr), "m"(original_control));
        _longjmp(plain.buffer, 1);
    }
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(restored_mxcsr), "=m"(restored_control));
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(original_mxcsr), "m"(original_control));
    check(restored_mxcsr == target_mxcsr && restored_control == target_control, "native FP control restore");
#endif
    canaries();
    sigprocmask(SIG_SETMASK, &original, NULL);
    puts("SDK setjmp bounds, register restoration, nested jumps and signal masks passed");
    return 0;
}
