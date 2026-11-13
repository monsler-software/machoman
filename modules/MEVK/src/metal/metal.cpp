#include "machoman/objc_framework.h"
#include "machoman/runtime.h"
#include "metal_internal.h"
#include "mevk/buffer.h"
#include "mevk/command_buffer.h"
#include "mevk/context.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <condition_variable>
#include <deque>
#include <thread>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

MM_FW_CLASS(MEVKDevice);
MM_FW_CLASS(MEVKBuffer);
MM_FW_CLASS(MEVKCommandQueue);
MM_FW_CLASS(MEVKCommandBuffer);
MM_FW_CLASS(MEVKBlitCommandEncoder);

extern "C" {
MM_FW_EXPORT
    mm_id mevk_command_error_domain __asm__("_MTLCommandBufferErrorDomain");
MM_FW_EXPORT mm_id mevk_library_error_domain __asm__("_MTLLibraryErrorDomain");
// Attach guest protocol metadata after libobjc has registered the guest image.
void *metal_objc_get_protocol(const char *) __asm__("_objc_getProtocol");
bool metal_class_add_protocol(mm_objc_class *,
                              void *) __asm__("_class_addProtocol");
}

namespace mevk_metal {

bool trace() { return std::getenv("MACHOMAN_MEVK_TRACE") != nullptr; }
id string(const char *text) {
  return obj(send(reinterpret_cast<id>(cls("NSString")),
                  "stringWithUTF8String:", ret(text)));
}
id error_object(id domain, std::uint64_t code, const std::string &description) {
  id info =
      obj(send(reinterpret_cast<id>(cls("NSMutableDictionary")), "dictionary"));
  send(info, "setObject:forKey:", ret(string(description.c_str())),
       ret(string("NSLocalizedDescription")));
  return obj(send(reinterpret_cast<id>(cls("NSError")),
                  "errorWithDomain:code:userInfo:", ret(domain), code,
                  ret(info)));
}

std::uint64_t device_name(id self, SEL) {
  return ret(string(as<Device>(self)->context->device_info().name.c_str()));
}
std::uint64_t unsupported_capability(id, SEL, std::uint64_t) { return 0; }
std::uint64_t supports_sample_count(id, SEL, std::uint64_t count) {
  return count == 1;
}

std::uint64_t new_buffer(id self, SEL, std::uint64_t size,
                         std::uint64_t options) {
  // Backing is coherent for shared/managed buffers and remains inaccessible
  // through Metal's contents API for private buffers. Vulkan operations use
  // the backend allocation directly in all three modes.
  const auto storage = (options >> 4) & 0xf;
  const auto cache = options & 0xf;
  const auto hazards = (options >> 8) & 3;
  if ((options & ~std::uint64_t{0x3ff}) || storage > 2 || cache > 1 || hazards > 2) {
    std::fprintf(stderr,
                 "MEVK Metal: unsupported buffer resource options 0x%llx\n",
                 static_cast<unsigned long long>(options));
    return 0;
  }
  std::string error;
  auto backend = as<Device>(self)->context->create_buffer(size, error);
  if (!backend) {
    std::fprintf(stderr, "MEVK Metal: %s\n", error.c_str());
    return 0;
  }
  auto *result = make<Buffer>(MM_FW_CLASS_REF(MEVKBuffer));
  result->device.set(self);
  result->backend = std::move(backend);
  result->storage_mode = storage;
  result->cpu_cache_mode = cache;
  result->hazard_tracking_mode = hazards ? hazards : 2;
  return ret(result); // new... returns a retained object.
}
std::uint64_t new_buffer_bytes(id self, SEL sel, const void *bytes,
                               std::uint64_t size, std::uint64_t options) {
  if (!bytes && size) {
    std::fprintf(stderr,
                 "MEVK Metal: newBufferWithBytes requires nonnull bytes\n");
    return 0;
  }
  id result = obj(new_buffer(self, sel, size, options));
  if (result)
    std::memcpy(as<Buffer>(result)->backend->contents(), bytes,
                static_cast<std::size_t>(size));
  return ret(result);
}
std::uint64_t new_queue(id self, SEL) {
  auto *result = make<Queue>(MM_FW_CLASS_REF(MEVKCommandQueue));
  result->device.set(self);
  return ret(result);
}
std::uint64_t queue_device(id self, SEL) {
  return ret(as<Queue>(self)->device.value);
}
std::uint64_t buffer_device(id self, SEL) {
  return ret(as<Buffer>(self)->device.value);
}
std::uint64_t buffer_length(id self, SEL) {
  return as<Buffer>(self)->backend->size();
}
std::uint64_t buffer_contents(id self, SEL) {
  if (as<Buffer>(self)->storage_mode == 2) return 0;
  return ret(as<Buffer>(self)->backend->contents());
}
std::uint64_t shared_mode(id, SEL) { return 0; }
std::uint64_t buffer_cache_mode(id self, SEL) {
  return as<Buffer>(self)->cpu_cache_mode;
}
std::uint64_t buffer_storage_mode(id self, SEL) {
  return as<Buffer>(self)->storage_mode;
}
std::uint64_t buffer_modified(id self, SEL, std::uint64_t offset,
                              std::uint64_t length) {
  auto b = as<Buffer>(self)->backend;
  if (offset > b->size() || length > b->size() - offset)
    std::fprintf(stderr, "MEVK Metal: didModifyRange outside buffer\n");
  return 0;
}
std::uint64_t synchronize_resource(id self, SEL, id resource) {
  auto &encoder = *as<Encoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (!is_kind(resource, cls("MEVKBuffer")))
    encoder.state->fail("synchronizeResource requires a MEVK buffer");
  else
    encoder.state->resources.emplace_back(resource);
  return 0;
}
std::uint64_t tracked_mode(id self, SEL) {
  return as<Buffer>(self)->hazard_tracking_mode;
}
std::uint64_t resource_options(id self, SEL) {
  const auto *buffer = as<Buffer>(self);
  return (buffer->storage_mode << 4) | buffer->cpu_cache_mode |
         (buffer->hazard_tracking_mode << 8);
}

std::uint64_t queue_command_buffer(id self, SEL selector) {
  std::string error;
  auto backend = as<Device>(as<Queue>(self)->device.value)
                     ->context->create_command_buffer(error);
  if (!backend) {
    std::fprintf(stderr, "MEVK Metal: %s\n", error.c_str());
    return 0;
  }
  auto *result = make<Command>(MM_FW_CLASS_REF(MEVKCommandBuffer));
  result->queue.set(self);
  result->state = std::make_shared<CommandState>();
  result->state->backend = std::move(backend);
  result->retained_references=std::strstr(selector,"Unretained")==nullptr;
  return ret(autorelease(reinterpret_cast<id>(result)));
}
std::uint64_t command_queue(id self, SEL) {
  return ret(as<Command>(self)->queue.value);
}
std::uint64_t command_device(id self, SEL) {
  return ret(as<Queue>(as<Command>(self)->queue.value)->device.value);
}
std::uint64_t retained_references(id self, SEL) { return as<Command>(self)->retained_references?1u:0u; }
std::uint64_t current_allocated_size(id self,SEL){return as<Device>(self)->context->allocated_size();}
std::uint64_t recommended_working_set_size(id self,SEL){return as<Device>(self)->context->recommended_working_set_size();}

std::uint64_t blit_encoder(id self, SEL) {
  auto state = as<Command>(self)->state;
  std::lock_guard lock(state->mutex);
  if (!state->diagnostic.empty())
    return 0;
  if (state->commit_requested || state->encoder_active) {
    state->fail("cannot create a blit encoder after commit or while another "
                "encoder is active");
    return 0;
  }
  auto *result = make<Encoder>(MM_FW_CLASS_REF(MEVKBlitCommandEncoder));
  result->command.set(self);
  result->state = state;
  state->encoder_active = true;
  return ret(autorelease(reinterpret_cast<id>(result)));
}
std::uint64_t encoder_device(id self, SEL) {
  return command_device(as<Encoder>(self)->command.value, nullptr);
}
bool encoding(Encoder &encoder) {
  if (!encoder.state->diagnostic.empty())
    return false;
  if (encoder.ended || encoder.state->commit_requested) {
    encoder.state->fail(
        "cannot encode into an ended or committed blit encoder");
    return false;
  }
  return true;
}
std::uint64_t blit_copy(id self, SEL, id source, std::uint64_t source_offset,
                        id destination, std::uint64_t destination_offset,
                        std::uint64_t size) {
  auto &encoder = *as<Encoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (!is_kind(source, MM_FW_CLASS_REF(MEVKBuffer)) ||
      !is_kind(destination, MM_FW_CLASS_REF(MEVKBuffer))) {
    encoder.state->fail(
        "blit copy requires MEVK source and destination buffers");
    return 0;
  }
  std::string error;
  if (!encoder.state->backend->copy(as<Buffer>(source)->backend, source_offset,
                                    as<Buffer>(destination)->backend,
                                    destination_offset, size, error))
    encoder.state->fail(error);
  else {
    encoder.state->resources.emplace_back(source);
    encoder.state->resources.emplace_back(destination);
  }
  return 0;
}
std::uint64_t blit_fill(id self, SEL, id buffer, std::uint64_t offset,
                        std::uint64_t size, std::uint64_t value) {
  auto &encoder = *as<Encoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (!is_kind(buffer, MM_FW_CLASS_REF(MEVKBuffer))) {
    encoder.state->fail("blit fill requires a MEVK buffer");
    return 0;
  }
  std::string error;
  if (!encoder.state->backend->fill(as<Buffer>(buffer)->backend, offset, size,
                                    static_cast<std::uint8_t>(value), error))
    encoder.state->fail(error);
  else
    encoder.state->resources.emplace_back(buffer);
  return 0;
}
std::uint64_t end_encoding(id self, SEL) {
  auto &encoder = *as<Encoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (encoder.ended) {
    encoder.state->fail("endEncoding called twice");
    return 0;
  }
  encoder.ended = true;
  encoder.state->encoder_active = false;
  return 0;
}
std::uint64_t command_wait(id self, SEL) {
  OwnedId keep_alive(self);
  auto state = as<Command>(self)->state;
  std::vector<void *> handlers;
  std::vector<OwnedId> resources;
  std::function<bool(std::string &)> presentation;
  {
    std::unique_lock lock(state->mutex);
    if (state->completion_handled) {
      // A completion handler may itself call waitUntilCompleted.
      if (state->completing_thread != std::this_thread::get_id())
        state->completion_cv.wait(lock, [&] { return state->completion_done; });
      return 0;
    }
    std::string error;
    if (state->committed || state->diagnostic.empty())
      if (!state->backend->wait_until_completed(error))
        state->fail(error);
    if (!state->commit_requested)
      return 0;
    state->completion_handled = true;
    state->completing_thread = std::this_thread::get_id();
    handlers = std::move(state->completed_handlers);
    resources = std::move(state->resources);
    if (state->diagnostic.empty())
      presentation = std::move(state->presentation);
    else
      state->presentation = {};
  }
  if (presentation) {
    std::string error;
    if (!presentation(error)) {
      std::lock_guard lock(state->mutex);
      state->fail(error);
    }
  }
  const auto argument = ret(self);
  for (auto *block : handlers) {
    mm_objc_block_invoke(block, 1, &argument);
    mevk_block_release(block);
  }
  {
    std::lock_guard lock(state->mutex);
    state->completion_done = true;
    state->completing_thread = {};
  }
  state->completion_cv.notify_all();
  return 0;
}
namespace {
class CompletionService {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<OwnedId> pending;
  bool stopping = false;
  std::thread worker;
public:
  CompletionService() : worker([this] {
    for (;;) {
      OwnedId command;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return stopping || !pending.empty(); });
        if (pending.empty()) return;
        command = std::move(pending.front()); pending.pop_front();
      }
      autorelease_pool pool;
      command_wait(command.value, nullptr);
    }
  }) {}
  void enqueue(id command) {
    {
      std::lock_guard lock(mutex);
      if (!stopping) { pending.emplace_back(command); cv.notify_one(); return; }
    }
    command_wait(command, nullptr);
  }
  void stop() {
    { std::lock_guard lock(mutex); stopping = true; }
    cv.notify_one();
    if (worker.joinable()) worker.join();
  }
  ~CompletionService() { stop(); }
};
CompletionService &completion_service() { static CompletionService service; return service; }
}
void release_cached_device();
extern "C" MM_FW_EXPORT void machoman_dylib_stop() {
  completion_service().stop();
  // Release ObjC ownership while the runtime's host API is still alive.
  release_cached_device();
}
std::uint64_t command_commit(id self, SEL) {
  OwnedId keep_alive(self);
  auto state = as<Command>(self)->state;
  std::vector<void *> scheduled;
  {
    std::lock_guard lock(state->mutex);
    if (state->commit_requested) {
      state->fail("command buffer committed twice");
      return 0;
    }
    state->commit_requested = true;
    if (state->encoder_active)
      state->fail("commit requires ended encoders");
    if (state->diagnostic.empty()) {
      std::string error;
      if (!state->backend->commit(error))
        state->fail(error);
      else {
        state->committed = true;
        state->scheduled_handled = true;
        scheduled = std::move(state->scheduled_handlers);
      }
    }
  }
  const auto argument = ret(self);
  for (auto *block : scheduled) {
    mm_objc_block_invoke(block, 1, &argument);
    mevk_block_release(block);
  }
  if (trace() && state->committed)
    std::fprintf(stderr, "MEVK Metal: command buffer submitted\n");
  // The worker retains the command and its resources. Runtime guest callbacks
  // attach a guest context on native threads; commit can return immediately.
  completion_service().enqueue(self);
  return 0;
}
std::uint64_t add_handler(id self, SEL selector, void *block) {
  auto state = as<Command>(self)->state;
  std::lock_guard lock(state->mutex);
  if (state->commit_requested || !block) {
    state->fail("handlers require a block and an uncommitted command buffer");
    return 0;
  }
  void *copy = mevk_block_copy(block);
  if (std::strcmp(selector, "addScheduledHandler:") == 0)
    state->scheduled_handlers.push_back(copy);
  else
    state->completed_handlers.push_back(copy);
  return 0;
}
std::uint64_t wait_scheduled(id self, SEL) {
  auto state = as<Command>(self)->state;
  std::lock_guard lock(state->mutex);
  if (!state->committed)
    state->fail("waitUntilScheduled requires commit");
  return 0;
}
std::uint64_t command_status(id self, SEL) {
  auto state = as<Command>(self)->state;
  std::lock_guard lock(state->mutex);
  if (!state->diagnostic.empty())
    return 5;
  auto status = state->backend->status();
  if (status == mevk::CommandBufferStatus::committed &&
      state->scheduled_handled)
    return 3;
  return static_cast<std::uint64_t>(status);
}
std::uint64_t command_error(id self, SEL) {
  std::string message;
  {
    auto state = as<Command>(self)->state;
    std::lock_guard lock(state->mutex);
    message = state->diagnostic;
    if (message.empty())
      message = state->backend->error_message();
  }
  return message.empty()
             ? 0
             : ret(error_object(mevk_command_error_domain, 1, message));
}
std::uint64_t unsupported_command(id self, SEL selector, std::uint64_t = 0) {
  auto state = as<Command>(self)->state;
  std::lock_guard lock(state->mutex);
  state->fail(std::string(selector) + " is not implemented by MEVK yet");
  return 0;
}
std::uint64_t unsupported_device(id, SEL selector, std::uint64_t = 0) {
  std::fprintf(stderr, "MEVK Metal: %s is not implemented yet\n", selector);
  return 0;
}
std::uint64_t unsupported_library(id self, SEL selector, id, id *error) {
  const std::string message =
      std::string(selector) + ": this shader input is unsupported; precompiled "
                              "AIR metallib is supported";
  if (error)
    *error = error_object(mevk_library_error_domain, 1, message);
  return unsupported_device(self, selector);
}
std::uint64_t unsupported_source_library(id self, SEL selector, id source, id,
                                         id *error) {
  return unsupported_library(self, selector, source, error);
}

void register_classes() {
  Class root = cls("NSObject");
  Class device = mm_objc_define_class("MEVKDevice", root, sizeof(Device),
                                      MM_FW_CLASS_REF(MEVKDevice),
                                      MM_FW_META_REF(MEVKDevice));
  Class buffer = mm_objc_define_class("MEVKBuffer", root, sizeof(Buffer),
                                      MM_FW_CLASS_REF(MEVKBuffer),
                                      MM_FW_META_REF(MEVKBuffer));
  Class queue = mm_objc_define_class("MEVKCommandQueue", root, sizeof(Queue),
                                     MM_FW_CLASS_REF(MEVKCommandQueue),
                                     MM_FW_META_REF(MEVKCommandQueue));
  Class command = mm_objc_define_class(
      "MEVKCommandBuffer", root, sizeof(Command),
      MM_FW_CLASS_REF(MEVKCommandBuffer), MM_FW_META_REF(MEVKCommandBuffer));
  Class encoder =
      mm_objc_define_class("MEVKBlitCommandEncoder", root, sizeof(Encoder),
                           MM_FW_CLASS_REF(MEVKBlitCommandEncoder),
                           MM_FW_META_REF(MEVKBlitCommandEncoder));
  lifecycle<Device>(device);
  lifecycle<Buffer>(buffer);
  lifecycle<Queue>(queue);
  lifecycle<Command>(command);
  lifecycle<Encoder>(encoder);
  MM_METHODS(
      device, {"name", MM_IMP(device_name), "@16@0:8"},
      {"currentAllocatedSize",MM_IMP(current_allocated_size),"Q16@0:8"},
      {"recommendedMaxWorkingSetSize",MM_IMP(recommended_working_set_size),"Q16@0:8"},
      {"newBufferWithLength:options:", MM_IMP(new_buffer), "@32@0:8Q16Q24"},
      {"newBufferWithBytes:length:options:", MM_IMP(new_buffer_bytes),
       "@40@0:8^v16Q24Q32"},
      {"newCommandQueue", MM_IMP(new_queue), "@16@0:8"},
      {"supportsFamily:", MM_IMP(unsupported_capability), "B24@0:8Q16"},
      {"supportsFeatureSet:", MM_IMP(unsupported_capability), "B24@0:8Q16"},
      {"supportsTextureSampleCount:", MM_IMP(supports_sample_count),
       "B24@0:8Q16"},
      {"newDefaultLibrary", MM_IMP(unsupported_device), "@16@0:8"},
      {"newLibraryWithSource:options:error:",
       MM_IMP(unsupported_source_library), "@40@0:8@16@24^@32"},
      {"newLibraryWithData:error:", MM_IMP(unsupported_library),
       "@32@0:8@16^@24"},
      {"newLibraryWithFile:error:", MM_IMP(unsupported_library),
       "@32@0:8@16^@24"});
  MM_METHODS(
      buffer, {"device", MM_IMP(buffer_device), "@16@0:8"},
      {"length", MM_IMP(buffer_length), "Q16@0:8"},
      {"contents", MM_IMP(buffer_contents), "^v16@0:8"},
      {"storageMode", MM_IMP(buffer_storage_mode), "Q16@0:8"},
      {"didModifyRange:", MM_IMP(buffer_modified), "v32@0:8{_NSRange=QQ}16"},
      {"cpuCacheMode", MM_IMP(buffer_cache_mode), "Q16@0:8"},
      {"hazardTrackingMode", MM_IMP(tracked_mode), "Q16@0:8"},
      {"resourceOptions", MM_IMP(resource_options), "Q16@0:8"});
  MM_METHODS(queue, {"device", MM_IMP(queue_device), "@16@0:8"},
             {"commandBuffer", MM_IMP(queue_command_buffer), "@16@0:8"},
             {"commandBufferWithUnretainedReferences",
              MM_IMP(queue_command_buffer), "@16@0:8"});
  MM_METHODS(command, {"device", MM_IMP(command_device), "@16@0:8"},
             {"commandQueue", MM_IMP(command_queue), "@16@0:8"},
             {"retainedReferences", MM_IMP(retained_references), "B16@0:8"},
             {"blitCommandEncoder", MM_IMP(blit_encoder), "@16@0:8"},
             {"commit", MM_IMP(command_commit), "v16@0:8"},
             {"waitUntilCompleted", MM_IMP(command_wait), "v16@0:8"},
             {"status", MM_IMP(command_status), "Q16@0:8"},
             {"error", MM_IMP(command_error), "@16@0:8"},
             {"enqueue", MM_IMP(unsupported_command), "v16@0:8"},
             {"addCompletedHandler:", MM_IMP(add_handler), "v24@0:8@?16"},
             {"addScheduledHandler:", MM_IMP(add_handler), "v24@0:8@?16"},
             {"waitUntilScheduled", MM_IMP(wait_scheduled), "v16@0:8"},
             {"computeCommandEncoder", MM_IMP(unsupported_command), "@16@0:8"},
             {"renderCommandEncoderWithDescriptor:",
              MM_IMP(unsupported_command), "@24@0:8@16"});
  MM_METHODS(
      encoder, {"device", MM_IMP(encoder_device), "@16@0:8"},
      {"copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:",
       MM_IMP(blit_copy), "v56@0:8@16Q24@32Q40Q48"},
      {"fillBuffer:range:value:", MM_IMP(blit_fill),
       "v48@0:8@16{_NSRange=QQ}24C40"},
      {"synchronizeResource:", MM_IMP(synchronize_resource), "v24@0:8@16"},
      {"endEncoding", MM_IMP(end_encoding), "v16@0:8"});
  register_render_classes(device, command);
}

struct DeviceCache {
  std::mutex mutex;
  OwnedId device;
};
DeviceCache &cache() {
  static DeviceCache value;
  return value;
}
void release_cached_device() {
  auto &cached = cache();
  std::lock_guard lock(cached.mutex);
  cached.device.set(nullptr);
}

} // namespace mevk_metal

using namespace mevk_metal;
extern "C" {
mm_id metal_create_default_device() DARWIN(MTLCreateSystemDefaultDevice);
mm_id metal_create_default_device() {
  auto &cached = cache();
  std::lock_guard lock(cached.mutex);
  if (!cached.device.value) {
    std::string error;
    auto backend = mevk::Context::create(error);
    if (!backend) {
      std::fprintf(stderr, "MEVK Metal: %s\n", error.c_str());
      return nullptr;
    }
    auto *device = make<Device>(MM_FW_CLASS_REF(MEVKDevice));
    device->context = std::move(backend);
    id object = reinterpret_cast<id>(device);
    cached.device.set(object);
    release(object);
    std::fprintf(stderr, "MEVK Metal: using Vulkan device %s\n",
                 device->context->device_info().name.c_str());
  }
  return retain(cached.device.value);
}
mm_id metal_copy_devices() DARWIN(MTLCopyAllDevices);
mm_id metal_copy_devices() {
  id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
  id device = metal_create_default_device();
  if (device) {
    send(array, "addObject:", ret(device));
    release(device);
  }
  return retain(array);
}

static void metal_constructor() __attribute__((constructor));
static void metal_constructor() {
  register_classes();
  mevk_command_error_domain = retain(string("MTLCommandBufferErrorDomain"));
  mevk_library_error_domain = retain(string("MTLLibraryErrorDomain"));
}
MM_FW_EXPORT void machoman_dylib_init(const mm_host_api *) {
  register_metal_layer_if_available();
  struct Entry {
    Class c;
    const char *protocol;
  };
  const Entry entries[] = {
      {MM_FW_CLASS_REF(MEVKDevice), "MTLDevice"},
      {MM_FW_CLASS_REF(MEVKBuffer), "MTLBuffer"},
      {MM_FW_CLASS_REF(MEVKBuffer), "MTLResource"},
      {MM_FW_CLASS_REF(MEVKCommandQueue), "MTLCommandQueue"},
      {MM_FW_CLASS_REF(MEVKCommandBuffer), "MTLCommandBuffer"},
      {MM_FW_CLASS_REF(MEVKBlitCommandEncoder), "MTLBlitCommandEncoder"},
      {MM_FW_CLASS_REF(MEVKBlitCommandEncoder), "MTLCommandEncoder"},
      {cls("MEVKLibrary"), "MTLLibrary"},
      {cls("MEVKFunction"), "MTLFunction"},
      {cls("MEVKTexture"), "MTLTexture"},
      {cls("MEVKTexture"), "MTLResource"},
      {cls("MEVKRenderPipelineState"), "MTLRenderPipelineState"},
      {cls("MEVKRenderCommandEncoder"), "MTLRenderCommandEncoder"},
      {cls("MEVKRenderCommandEncoder"), "MTLCommandEncoder"},
      {cls("MEVKDrawable"), "CAMetalDrawable"},
      {cls("MEVKDrawable"), "MTLDrawable"},
  };
  for (const auto &entry : entries)
    if (void *protocol = metal_objc_get_protocol(entry.protocol))
      metal_class_add_protocol(entry.c, protocol);
}
} // extern "C"
