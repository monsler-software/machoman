#include <resolv.h>

// These global entry points expose no Darwin resolver-state layout. Use
// the host resolver's actual configuration and networking implementation.
extern "C" int darwin_res_init() __asm__("_res_9_init");
extern "C" int darwin_res_init() { return res_init(); }
extern "C" void darwin_res_close() __asm__("_res_9_close");
extern "C" void darwin_res_close() { res_close(); }

extern "C" int darwin_res_query(const char *, int, int, unsigned char *, int) __asm__("_res_9_query");
extern "C" int darwin_res_query(const char *name, int cls, int type, unsigned char *answer, int size) {
    return res_query(name, cls, type, answer, size);
}
extern "C" int darwin_res_search(const char *, int, int, unsigned char *, int) __asm__("_res_9_search");
extern "C" int darwin_res_search(const char *name, int cls, int type, unsigned char *answer, int size) {
    return res_search(name, cls, type, answer, size);
}
