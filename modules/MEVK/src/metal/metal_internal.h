#pragma once
#include "machoman/objc_framework.h"
#include "machoman/runtime.h"
#include "mevk/buffer.h"
#include "mevk/command_buffer.h"
#include "mevk/context.h"
#include "mevk/render.h"
#include <cstdio>
#include <functional>
#include <condition_variable>
#include <thread>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
extern "C" {
void *mevk_block_copy(const void *) __asm__("__Block_copy");
void mevk_block_release(const void *) __asm__("__Block_release");
extern mm_id mevk_library_error_domain __asm__("_MTLLibraryErrorDomain");
}
namespace mevk_metal {
using namespace mmfw;
struct OwnedId {
  id value = nullptr;
  OwnedId() = default;
  explicit OwnedId(id object) : value(retain(object)) {}
  OwnedId(const OwnedId &) = delete;
  OwnedId &operator=(const OwnedId &) = delete;
  OwnedId(OwnedId &&other) noexcept
      : value(std::exchange(other.value, nullptr)) {}
  OwnedId &operator=(OwnedId &&other) noexcept {
    if (this != &other) { release(value); value = std::exchange(other.value, nullptr); }
    return *this;
  }
  ~OwnedId() { release(value); }
  void set(id object) {
    id old = value;
    value = retain(object);
    release(old);
  }
};

struct Label {
  std::mutex mutex;
  bool present = false;
  std::string text;
};

struct Device {
  mm_objc_object header;
  Label label;
  std::shared_ptr<mevk::Context> context;
};
struct Buffer {
  mm_objc_object header;
  Label label;
  OwnedId device;
  std::shared_ptr<mevk::Buffer> backend;
  std::uint64_t storage_mode = 0;
  std::uint64_t cpu_cache_mode = 0, hazard_tracking_mode = 2;
};
struct Queue {
  mm_objc_object header;
  Label label;
  OwnedId device;
};
struct CommandState {
  std::mutex mutex;
  std::shared_ptr<mevk::CommandBuffer> backend;
  bool encoder_active = false;
  bool committed = false;
  bool commit_requested = false;
  std::string diagnostic;
  std::vector<OwnedId> resources;
  std::vector<void *> scheduled_handlers, completed_handlers;
  bool scheduled_handled = false, completion_handled = false;
  bool completion_done = false;
  std::thread::id completing_thread;
  std::condition_variable completion_cv;
  std::function<bool(std::string &)> presentation;
  ~CommandState() {
    // Keep the Objective-C wrappers, as well as backend allocations, alive
    // until GPU completion. Members release resources after this wait.
    if (committed) {
      std::string error;
      backend->wait_until_completed(error);
    }
    for (auto *block : scheduled_handlers)
      mevk_block_release(block);
    for (auto *block : completed_handlers)
      mevk_block_release(block);
  }
  void fail(const std::string &message) {
    if (diagnostic.empty()) {
      diagnostic = message;
      std::fprintf(stderr, "MEVK Metal: %s\n", diagnostic.c_str());
    }
  }
};
struct Command {
  mm_objc_object header;
  Label label;
  OwnedId queue;
  std::shared_ptr<CommandState> state;
  bool retained_references=true;
};
struct Encoder {
  mm_objc_object header;
  Label label;
  OwnedId command;
  std::shared_ptr<CommandState> state;
  bool ended = false; // Protected by state->mutex.
};

bool trace();
id string(const char *text);
id error_object(id domain, std::uint64_t code, const std::string &description);
void register_render_classes(Class device, Class command);
void register_metal_layer_if_available();
bool encoding(Encoder &encoder);
template <typename T> std::uint64_t label_get(id self, SEL) {
  std::string text;
  {
    auto &label = as<T>(self)->label;
    std::lock_guard lock(label.mutex);
    if (!label.present)
      return 0;
    text = label.text;
  }
  return ret(string(text.c_str()));
}
template <typename T> std::uint64_t label_set(id self, SEL, id value) {
  const char *text =
      value ? reinterpret_cast<const char *>(send(value, "UTF8String"))
            : nullptr;
  auto &label = as<T>(self)->label;
  std::lock_guard lock(label.mutex);
  label.present = value != nullptr;
  label.text = text ? text : "";
  return 0;
}
template <typename T> void lifecycle(Class c) {
  register_lifecycle<T>(c);
  MM_METHODS(c, {"label", MM_IMP(label_get<T>), "@16@0:8"},
             {"setLabel:", MM_IMP(label_set<T>), "v24@0:8@16"});
}

} // namespace mevk_metal
