#include <algorithm>
#include <cstdint>

struct ns_rect { double x, y, w, h; };
extern "C" __attribute__((visibility("default"))) int mm_NSIsEmptyRect(double, double, double, double) __asm__("_NSIsEmptyRect");
extern "C" int mm_NSIsEmptyRect(double, double, double w, double h) { return w <= 0 || h <= 0; }
extern "C" __attribute__((visibility("default"))) void mm_NSDivideRect(ns_rect *, ns_rect *, std::uint64_t, double, double, double, double, double) __asm__("_NSDivideRect");
extern "C" void mm_NSDivideRect(ns_rect *slice, ns_rect *remainder, std::uint64_t edge,
                                double x, double y, double w, double h, double amount) {
    ns_rect s{x,y,w,h}, r=s;
    amount=std::clamp(amount,0.,std::max(0.,edge==0||edge==2?w:h));
    switch(edge){
    case 0:s.w=amount;r.x+=amount;r.w-=amount;break;
    case 1:s.h=amount;r.y+=amount;r.h-=amount;break;
    case 2:s.x+=w-amount;s.w=amount;r.w-=amount;break;
    case 3:s.y+=h-amount;s.h=amount;r.h-=amount;break;
    default:return;
    }
    if(slice)*slice=s;if(remainder)*remainder=r;
}
