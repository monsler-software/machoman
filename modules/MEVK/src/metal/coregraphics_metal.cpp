#include "machoman/objc_host.h"
#include "synthesise.h"
#include <cstdint>

extern "C" {
mm_id metal_create_default_device() __asm__("_MTLCreateSystemDefaultDevice");

__attribute__((visibility("default")))
mm_id metal_copy_display_device(std::uint32_t display)
    __asm__("_CGDirectDisplayCopyCurrentMetalDevice");
mm_id metal_copy_display_device(std::uint32_t display) {
  // AppKit exposes one display with NSScreenNumber == 1.
  return display == 1 ? metal_create_default_device() : nullptr;
}

__attribute__((visibility("default")))
void *machoman_resolve_symbol(const char *name, unsigned flags) {
  return mm_synthesise_symbol(name, flags);
}
}
