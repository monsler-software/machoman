#include "../src/dylibs/AppKit/metal_presentation.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static void check(bool yes, const char *message) {
    if (!yes) { std::fprintf(stderr, "Metal placement: %s\n", message); std::abort(); }
}
static void padded(unsigned w, unsigned h, unsigned scale) {
    const unsigned source_w = 512 * scale, source_h = 512 * scale, pitch = source_w + 7;
    std::vector<std::uint32_t> source(pitch * source_h, 0xff123456), output(w * h);
    for (unsigned y = 0; y < h * scale; ++y)
        for (unsigned x = 0; x < w * scale; ++x)
            source[y * pitch + x] = 0xff000000 | (y / scale << 12) | x / scale;
    ak::copy_metal_contents(output.data(), w, h, w, h, source.data(), source_w,
        source_h, pitch * 4, scale, 11);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            check(output[y * w + x] == (0xff000000 | (y << 12) | x),
                "top-left content keeps coordinates and excludes drawable padding");
}
int main() {
    padded(356, 356, 1); padded(446, 107, 1); padded(356, 356, 2);
    const std::uint32_t source[] = {1, 2, 3, 4, 5, 6, 7, 8}; // 4x2
    std::uint32_t output[16] = {};
    ak::copy_metal_contents(output, 2, 2, 2, 2, source, 4, 2, 16, 1, 0);
    check(output[0] == 1 && output[1] == 3 && output[2] == 5 && output[3] == 7, "default scales both axes");
    ak::copy_metal_contents(output, 2, 2, 2, 2, source, 4, 2, 16, 1, 3);
    check(output[0] == 2 && output[1] == 3 && output[2] == 6 && output[3] == 7, "center crops evenly");
    ak::copy_metal_contents(output, 2, 2, 2, 2, source, 4, 2, 16, 1, 5);
    check(output[0] == 3 && output[1] == 4 && output[2] == 7 && output[3] == 8, "right aligns source");
    ak::copy_metal_contents(output, 4, 4, 4, 4, source, 4, 2, 16, 1, 9);
    check(output[0] == 0 && output[7] == 0 && output[8] == 1 && output[15] == 8, "bottom placement leaves transparent area above");
    ak::copy_metal_contents(output, 4, 4, 4, 4, source, 4, 2, 16, 1, 1);
    check(output[0] == 0 && output[4] == 1 && output[11] == 8 && output[15] == 0, "aspect fit centers letterbox");
    ak::copy_metal_contents(output, 4, 4, 4, 4, source, 4, 2, 16, 1, 2);
    check(output[0] == 2 && output[3] == 3 && output[12] == 6 && output[15] == 7, "aspect fill crops with uniform scale");
    std::puts("Metal drawable placement pixels passed");
}
