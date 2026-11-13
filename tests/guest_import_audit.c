#include <stdio.h>
#include <unistd.h>

extern void mm_audit_missing(void);
extern void mm_audit_weak(void) __attribute__((weak_import));
__attribute__((constructor)) static void initializer(void) { _exit(98); }
int main(void) {
    puts("audit must not execute this");
    if (mm_audit_weak) mm_audit_weak();
    mm_audit_missing();
    return 99;
}
