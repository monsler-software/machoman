#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static volatile sig_atomic_t stage, handled, nested;
static const uint64_t readable = 789;

/* Only 512 bytes belong to the guest fiber; the prefix is another
 * allocation's contents. Import traps must use the runtime recovery stack. */
static _Alignas(16) unsigned char fiber_memory[8192 + 512];
extern int fiber_hostcall(void *top);
__asm__(
    ".text\n.globl _fiber_hostcall\n_fiber_hostcall:\n"
    "pushq %r12\nmovq %rsp, %r12\nmovq %rdi, %rsp\n"
    "callq _getpid\nmovq %r12, %rsp\npopq %r12\nretq\n");

static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "native signals: %s\n", message); exit(1); }
}

static void handler(int signo, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    check(info && uc && uc->uc_mcontext, "valid signal arguments");
    check(info->si_signo == signo, "siginfo signo");
    check(uc->uc_mcsize == sizeof(*uc->uc_mcontext), "Darwin mcontext size");
    mcontext_t mc = uc->uc_mcontext;
    if (nested) {
        check(signo == SIGILL, "nested SIGILL");
        mc->__ss.__rip += 2;
        ++handled;
        return;
    }
    if (stage == 1) {
        check(signo == SIGSEGV && info->si_addr == NULL, "null memory fault");
        check(mc->__es.__faultvaddr == 0 && mc->__es.__trapno == 14, "page fault state");
        check((mc->__ss.__rflags & 1) != 0, "saved carry flag");
        uint64_t xmm;
        memcpy(&xmm, &mc->__fs.__fpu_xmm0, sizeof xmm);
        check(xmm == 321, "saved XMM register");
        mc->__ss.__rip += 2; /* mov (%rax), %eax */
        mc->__ss.__rax = 123;
        xmm = 456;
        memcpy(&mc->__fs.__fpu_xmm0, &xmm, sizeof xmm);
        mc->__fs.__fpu_mxcsr = (mc->__fs.__fpu_mxcsr & ~0x6000u) | 0x4000u;
        nested = 1;
        __asm__ volatile("ud2");
        nested = 0;
        __asm__ volatile("fninit" ::: "memory");
    } else if (stage == 2) {
        check(signo == SIGSEGV, "repair signal");
        mc->__ss.__rax = (uintptr_t)&readable; /* Retry at the same RIP. */
    } else if (stage == 3) {
        check(signo == SIGILL, "illegal instruction signal");
        mc->__ss.__rip += 2;
    } else {
        check(stage == 4 && signo == SIGTRAP, "breakpoint signal");
        /* int3 has already advanced RIP by one byte. */
    }
    ++handled;
}

int main(void) {
    memset(fiber_memory, 0xa5, sizeof fiber_memory);
    for (int i = 0; i < 16; ++i)
        check(fiber_hostcall(fiber_memory + sizeof fiber_memory) == getpid(), "import call on small fiber");
    for (unsigned i = 0; i < 8192; ++i)
        check(fiber_memory[i] == 0xa5, "runtime signal frame stays outside small guest fiber");
    struct sigaction action = {0};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO;
    check(sigaction(SIGSEGV, &action, NULL) == 0, "install SIGSEGV");
    check(sigaction(SIGILL, &action, NULL) == 0, "install SIGILL");
    check(sigaction(SIGTRAP, &action, NULL) == 0, "install SIGTRAP");
    stage = 1;
    uint64_t result, xmm, redzone;
    unsigned char carry;
    uint32_t mxcsr, original_mxcsr;
    long double x87;
    __asm__ volatile("stmxcsr %0" : "=m"(original_mxcsr));
    __asm__ volatile(
        "movq $654, -64(%%rsp)\n"
        "movq $321, %%rdx\n"
        "movq %%rdx, %%xmm0\n"
        "fld1\n"
        "xor %%eax, %%eax\n"
        "stc\n"
        "mov (%%rax), %%eax\n"
        "setc %3\n"
        "movq %%xmm0, %1\n"
        "movq -64(%%rsp), %2\n"
        "stmxcsr %4\n"
        "fstpt %5\n"
        : "=a"(result), "=r"(xmm), "=r"(redzone), "=qm"(carry), "=m"(mxcsr), "=m"(x87)
        : : "rdx", "xmm0", "cc", "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(original_mxcsr));
    check(result == 123 && xmm == 456 && carry, "edited registers and restored flags");
    check(redzone == 654, "interrupted red zone preserved");
    check((mxcsr & 0x6000u) == 0x4000u && x87 == 1.0L, "x87 and MXCSR restored");
    stage = 2;
    __asm__ volatile("xor %%eax, %%eax; mov (%%rax), %%eax" : "=a"(result) : : "cc", "memory");
    check(result == readable, "retry with repaired register");
    stage = 3;
    __asm__ volatile("ud2");
    stage = 4;
    __asm__ volatile("int3");
    check(handled == 5, "all synchronous and nested signals handled");
    puts("Native Darwin signals, nested handlers, context edits, red zone and FPU restoration passed");
    return 0;
}
