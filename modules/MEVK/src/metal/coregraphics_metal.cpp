#include "machoman/objc_host.h"
#include "machoman/runtime.h"
#include "machoman/translator.h"
#include "synthesise.h"
#include <cstdint>
#include <cstring>

namespace {
struct Rect { double x, y, width, height; };
mm_cpu_context *context() {
  const auto *api = mm_objc_host_api();
  return api && api->current_context ? api->current_context() : nullptr;
}
Rect display_bounds(std::uint32_t display) {
  auto *ctx = context();
  if (display != 1 || !ctx) return {};
  // Use the same display geometry as NSScreen, including headless mode.
  // Host-to-host sends return flattened geometry in the context on both ISAs.
  std::uint64_t saved[4][2];
  std::memcpy(saved, ctx->v, sizeof saved);
  auto *screen = reinterpret_cast<mm_id>(mm_objc_send0(
      reinterpret_cast<mm_id>(mm_objc_class_named("NSScreen")), "mainScreen"));
  mm_objc_send0(screen, "frame");
  double values[4]{};
  if (screen) for (unsigned i = 0; i < 4; ++i)
    std::memcpy(&values[i], &ctx->v[i][0], sizeof(double));
  std::memcpy(ctx->v, saved, sizeof saved);
  return {values[0], values[1], values[2], values[3]};
}
Rect rect_argument(double x, double y, double width, double height) {
  auto *ctx = context();
  Rect rect{x, y, width, height};
  // Darwin x86_64 passes this 32-byte aggregate on the stack. Darwin arm64
  // passes its four homogeneous members in d0..d3.
  if (ctx && ctx->native_mode)
    std::memcpy(&rect, reinterpret_cast<const void *>(ctx->native_x[MM_RSP] + 8), sizeof rect);
  return rect;
}
}

extern "C" {
mm_id metal_create_default_device() __asm__("_MTLCreateSystemDefaultDevice");

__attribute__((visibility("default")))
mm_id metal_copy_display_device(std::uint32_t display)
    __asm__("_CGDirectDisplayCopyCurrentMetalDevice");
mm_id metal_copy_display_device(std::uint32_t display) {
  // AppKit exposes one display with NSScreenNumber == 1.
  return display == 1 ? metal_create_default_device() : nullptr;
}

MM_OBJC_API std::uint32_t cg_main_display() __asm__("_CGMainDisplayID");
std::uint32_t cg_main_display() { return 1; }

MM_OBJC_API std::uint64_t cg_display_bounds(std::uint64_t first, std::uint32_t second)
    __asm__("_CGDisplayBounds");
std::uint64_t cg_display_bounds(std::uint64_t first, std::uint32_t second) {
  auto *ctx = context();
  const bool native = ctx && ctx->native_mode;
  const Rect rect = display_bounds(native ? second : static_cast<std::uint32_t>(first));
  if (native) {
    // Intel uses a hidden first argument for the result; ARM returns an HFA.
    std::memcpy(reinterpret_cast<void *>(first), &rect, sizeof rect);
    return first;
  }
  if (ctx) {
    const double values[] = {rect.x, rect.y, rect.width, rect.height};
    for (unsigned i = 0; i < 4; ++i) {
      std::memcpy(&ctx->v[i][0], &values[i], sizeof(double));
      ctx->v[i][1] = 0;
    }
    ctx->host_result_in_context = 1;
  }
  return 0;
}

MM_OBJC_API double cg_rect_width(double x, double y, double width, double height)
    __asm__("_CGRectGetWidth");
double cg_rect_width(double x, double y, double width, double height) {
  return rect_argument(x, y, width, height).width;
}
MM_OBJC_API double cg_rect_height(double x, double y, double width, double height)
    __asm__("_CGRectGetHeight");
double cg_rect_height(double x, double y, double width, double height) {
  return rect_argument(x, y, width, height).height;
}

MM_OBJC_API std::uint32_t cg_display_mask(std::uint32_t display)
    __asm__("_CGDisplayIDToOpenGLDisplayMask");
std::uint32_t cg_display_mask(std::uint32_t display) { return display == 1 ? 1 : 0; }

__attribute__((visibility("default")))
void *machoman_resolve_symbol(const char *name, unsigned flags) {
  return mm_synthesise_symbol(name, flags);
}
}
