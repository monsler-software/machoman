#include "../src/dylibs/AppKit/appkit.h"
#include <cstdio>

// Test-only observer: validates the pixels produced by real ARM64 drawRect:
// callbacks, including the ninth FP argument spilled to the guest stack.
extern "C" __attribute__((visibility("default"))) int bitmap_check(mm_id view, const std::uint32_t *expected, int width, int height) __asm__("_machoman_bitmap_check");
extern "C" int bitmap_check(mm_id view, const std::uint32_t *expected, int width, int height) {
    const auto &b = mmfw::as<ak::view_object>(view)->drawing;
    if (!b || b->width != width || b->height != height) return 1;
    for (std::size_t i=0;i<b->argb.size();++i) if (b->argb[i]!=expected[i]) {
        std::fprintf(stderr,"pixel %zu: got %08x expected %08x\n",i,b->argb[i],expected[i]); return 2;
    }
    return 0;
}
