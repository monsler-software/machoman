#include "../src/dylibs/AppKit/cocoa_layout.h"
#include <cstdio>
#include <cstdlib>
using namespace ak::layout;
void require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
bool near(double a, double b) { return std::abs(a - b) < .02; }
int main() {
    double x = 80, w = 20;
    resize_axis(x, w, 100, 50, 1);
    require(near(x, 130) && near(w, 20), "right-anchored button follows resize");
    x = 10;
    w = 80;
    resize_axis(x, w, 100, 50, 2);
    require(near(x, 10) && near(w, 130), "flexible content preserves margins");
    std::vector<frame> f{{475, 293, 24, 24}, {228, 20, 271, 261, .0001, .0001}, {290, 296, 95, 23}, {228, 293, 55, 24}};
    std::vector<constraint> c{{-1, 6, 0, 6, 0, 1, 20},      {-1, 4, 0, 4, 0, 1, 20}, {-1, 6, 1, 6, 0, 1, 20},
                              {1, 3, -1, 3, 0, 1, 20},      {0, 3, 1, 4, 0, 1, 8},   {-1, 4, 2, 4, 0, 1, 20},
                              {-1, 4, 3, 4, 0, 1, 20},      {2, 5, 3, 6, 0, 1, 8},   {0, 5, 2, 6, 1, 1, 8},
                              {1, 7, -1, 0, 1, 1, 240, 999}};
    solve(f, 489, 337, c);
    require(near(f[0].x + f[0].w, 469), "help button stays inside right edge");
    require(near(f[0].y + f[0].h, 317) && near(f[2].y + f[2].h, 317) && near(f[3].y + f[3].h, 317),
            "bottom buttons share baseline");
    require(near(f[1].y, 20) && near(f[1].x + f[1].w, 469), "scroll view obeys container edges");
    require(near(f[0].h, 24) && near(f[1].h, 265), "scroll view stretches while button keeps intrinsic size");
    solve(f, 589, 437, c);
    require(near(f[0].x + f[0].w, 569) && near(f[0].y + f[0].h, 417), "constraints survive another resize");
    require(f[1].width_stay == .0001 && f[1].height_stay == .0001, "solve preserves intrinsic-size weights");
    std::vector<frame> adjacent{{0, 0, 20, 20, .0001, .0001}, {20, 0, 20, 20, .0001, .0001}};
    solve(adjacent, 1167, 567, {{0, 5, -1, 5}, {0, 3, -1, 3}, {0, 4, -1, 4},
                             {1, 5, 0, 6}, {1, 6, -1, 6}, {1, 3, -1, 3}, {1, 4, -1, 4},
                             {0, 7, 1, 7}});
    for (auto &a : adjacent) a = align_edges(a);
    require(adjacent[0].h == 567 && adjacent[1].h == 567, "bitmap height matches rounded drawing bounds");
    require(adjacent[0].x + adjacent[0].w == adjacent[1].x && adjacent[1].x + adjacent[1].w == 1167,
            "rounding preserves shared edges without gaps");
    std::vector<frame> one{{0, 0, 20, 20}};
    solve(one, 100, 100, {{0, 7, -1, 0, 1, 1, 40, 999}});
    require(near(one[0].w, 40), "minimum-size inequality remains active");
    puts("Cocoa layout anchoring, intrinsic sizes and resize constraints passed");
}
