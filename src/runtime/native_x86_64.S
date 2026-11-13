/* Restore a Darwin x86_64 register file and enter mapped Mach-O code.
 * The return sentinel and host imports are non-executable trap addresses;
 * the signal guard saves registers and returns to the host run loop. */
    .intel_syntax noprefix
    .text
    .globl mm_native_enter
    .type mm_native_enter, @function
mm_native_enter:
    mov rax, rdi
    mov qword ptr fs:[mm_native_entry_pc@tpoff], rsi
    fxrstor [rax + 1328]
    push qword ptr [rax + 1040]
    popfq
    mov rsp, [rax + 912 + 4*8]
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    movdqu xmm\n, [rax + 1056 + \n*16]
    .endr
    mov rcx, [rax + 912 + 1*8]
    mov rdx, [rax + 912 + 2*8]
    mov rbx, [rax + 912 + 3*8]
    mov rbp, [rax + 912 + 5*8]
    mov rsi, [rax + 912 + 6*8]
    mov rdi, [rax + 912 + 7*8]
    mov r8,  [rax + 912 + 8*8]
    mov r9,  [rax + 912 + 9*8]
    mov r10, [rax + 912 + 10*8]
    mov r11, [rax + 912 + 11*8]
    mov r12, [rax + 912 + 12*8]
    mov r13, [rax + 912 + 13*8]
    mov r14, [rax + 912 + 14*8]
    mov r15, [rax + 912 + 15*8]
    mov rax, [rax + 912]
    /* Keep the interrupted function's red zone intact on signal return. */
    jmp qword ptr fs:[mm_native_entry_pc@tpoff]
    .size mm_native_enter, .-mm_native_enter
    .section .note.GNU-stack,"",@progbits
