#include "metal_internal.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <unordered_map>

MM_FW_CLASS(MEVKLibrary);
MM_FW_CLASS(MEVKFunction);
MM_FW_CLASS(MEVKTexture);
MM_FW_CLASS(MEVKRenderPipelineState);
MM_FW_CLASS(MEVKRenderCommandEncoder);
MM_FW_CLASS(MTLTextureDescriptor);
MM_FW_CLASS(MTLRenderPipelineDescriptor);
MM_FW_CLASS(MTLRenderPipelineColorAttachmentDescriptor);
MM_FW_CLASS(MTLRenderPipelineColorAttachmentDescriptorArray);
MM_FW_CLASS(MTLRenderPassDescriptor);
MM_FW_CLASS(MTLRenderPassColorAttachmentDescriptor);
MM_FW_CLASS(MTLRenderPassColorAttachmentDescriptorArray);
MM_FW_CLASS(MEVKLayerState);
MM_FW_CLASS(MEVKDrawable);
extern "C" {
MM_FW_EXPORT
    mm_id mevk_pipeline_error_domain __asm__("_MTLRenderPipelineErrorDomain");
mm_id metal_create_default_device() __asm__("_MTLCreateSystemDefaultDevice");
mm_id metal_get_association(mm_id,
                            const void *) __asm__("_objc_getAssociatedObject");
void metal_set_association(mm_id, const void *, mm_id,
                           std::uint64_t) __asm__("_objc_setAssociatedObject");
}

namespace mevk_metal {
namespace {
struct Library {
  mm_objc_object header;
  Label label;
  OwnedId device;
  std::shared_ptr<std::vector<mevk::ShaderFunction>> functions;
};
struct Function {
  mm_objc_object header;
  Label label;
  OwnedId device;
  std::shared_ptr<const mevk::ShaderFunction> shader;
};
struct TextureObject {
  mm_objc_object header;
  Label label;
  OwnedId device;
  std::shared_ptr<mevk::Texture> backend;
  std::uint64_t storage = 2, usage = 1;
  bool framebuffer_only = false;
};
struct TextureDescriptor {
  mm_objc_object header;
  Label label;
  std::uint64_t width = 1, height = 1, depth = 1, pixel_format = 70, type = 2,
                mips = 1, samples = 1, array = 1, storage = 2, usage = 1;
};
struct PipelineColor {
  mm_objc_object header;
  Label label;
  std::uint64_t format = 0, enabled = 0, src_rgb = 1, dst_rgb = 0,
                src_alpha = 1, dst_alpha = 0, op_rgb = 0, op_alpha = 0,
                mask = 15;
};
struct PassColor {
  mm_objc_object header;
  Label label;
  OwnedId texture;
  std::uint64_t load = 0, store = 1, level = 0, slice = 0, plane = 0;
  std::array<double, 4> clear{0, 0, 0, 0};
};
struct AttachmentArray {
  mm_objc_object header;
  Label label;
  std::vector<OwnedId> values;
};
struct PipelineDescriptor {
  mm_objc_object header;
  Label label;
  OwnedId vertex, fragment, colors, vertex_descriptor;
  std::uint64_t samples = 1, depth_format = 0, stencil_format = 0;
};
struct PassDescriptor {
  mm_objc_object header;
  Label label;
  OwnedId colors, depth, stencil;
  std::uint64_t width = 0, height = 0, array_length = 1;
};
struct PipelineObject {
  mm_objc_object header;
  Label label;
  OwnedId device;
  std::shared_ptr<mevk::RenderPipeline> backend;
};
struct RenderEncoder {
  mm_objc_object header;
  Label label;
  OwnedId command;
  std::shared_ptr<CommandState> state;
  std::shared_ptr<mevk::Context> context;
  std::shared_ptr<mevk::RenderPipeline> pipeline;
  mevk::RenderBindings bindings;
  mevk::Viewport viewport;
  mevk::Scissor scissor;
  bool ended = false;
};
struct LayerState {
  mm_objc_object header;
  Label label;
  OwnedId device;
  double width = 0, height = 0;
  std::uint64_t format = 80;
  bool framebuffer_only = true;
  std::recursive_mutex mutex;
  std::vector<std::shared_ptr<mevk::Texture>> drawables;
  std::chrono::steady_clock::time_point timing_since = std::chrono::steady_clock::now();
  std::uint64_t timing_count = 0, timing_readbacks = 0;
  double timing_total_ms = 0, timing_max_ms = 0;
};
struct Drawable {
  mm_objc_object header;
  Label label;
  OwnedId layer, texture;
  double presented_time = 0;
};

template <class T, auto Field> std::uint64_t get(id self, SEL) {
  return static_cast<std::uint64_t>(as<T>(self)->*Field);
}
template <class T, auto Field>
std::uint64_t set(id self, SEL, std::uint64_t value) {
  as<T>(self)->*Field = value;
  return 0;
}
template <class T, auto Field> std::uint64_t get_object(id self, SEL) {
  return ret((as<T>(self)->*Field).value);
}
template <class T, auto Field>
std::uint64_t set_object(id self, SEL, id value) {
  (as<T>(self)->*Field).set(value);
  return 0;
}
// Explicit setter names avoid assuming the capitalization of Objective-C
// selectors.
template <class T, auto Field>
void uint_property(Class c, const char *getter, const char *setter) {
  mm_objc_add_method(c, getter, MM_IMP((get<T, Field>)), "Q16@0:8");
  mm_objc_add_method(c, setter, MM_IMP((set<T, Field>)), "v24@0:8Q16");
}
template <class T, auto Field>
std::uint64_t set_bool(id self, SEL, std::uint64_t value) {
  as<T>(self)->*Field = (value & 0xff) != 0;
  return 0;
}
template <class T, auto Field>
void bool_property(Class c, const char *getter, const char *setter) {
  mm_objc_add_method(c, getter, MM_IMP((get<T, Field>)), "B16@0:8");
  mm_objc_add_method(c, setter, MM_IMP((set_bool<T, Field>)), "v24@0:8B16");
}
template <class T, auto Field>
void object_property(Class c, const char *getter, const char *setter) {
  mm_objc_add_method(c, getter, MM_IMP((get_object<T, Field>)), "@16@0:8");
  mm_objc_add_method(c, setter, MM_IMP((set_object<T, Field>)), "v24@0:8@16");
}
std::uint64_t array_at(id self, SEL, std::uint64_t index) {
  auto &values = as<AttachmentArray>(self)->values;
  return index < values.size()
             ? ret(values[static_cast<std::size_t>(index)].value)
             : 0;
}
id attachment_array(bool pass) {
  auto *array = make<AttachmentArray>(
      pass ? MM_FW_CLASS_REF(MTLRenderPassColorAttachmentDescriptorArray)
           : MM_FW_CLASS_REF(MTLRenderPipelineColorAttachmentDescriptorArray));
  for (unsigned i = 0; i < 8; ++i) {
    id object = pass ? reinterpret_cast<id>(make<PassColor>(MM_FW_CLASS_REF(
                           MTLRenderPassColorAttachmentDescriptor)))
                     : reinterpret_cast<id>(make<PipelineColor>(MM_FW_CLASS_REF(
                           MTLRenderPipelineColorAttachmentDescriptor)));
    array->values.emplace_back(object);
    release(object);
  }
  return reinterpret_cast<id>(array);
}
std::uint64_t pipeline_colors(id self, SEL) {
  auto *descriptor = as<PipelineDescriptor>(self);
  if (!descriptor->colors.value) {
    id array = attachment_array(false);
    descriptor->colors.set(array);
    release(array);
  }
  return ret(descriptor->colors.value);
}
std::uint64_t pass_colors(id self, SEL) {
  auto *descriptor = as<PassDescriptor>(self);
  if (!descriptor->colors.value) {
    id array = attachment_array(true);
    descriptor->colors.set(array);
    release(array);
  }
  return ret(descriptor->colors.value);
}
std::uint64_t pass_depth(id self, SEL) {
  auto &value = as<PassDescriptor>(self)->depth;
  if (!value.value) {
    auto *p = make<PassColor>(
        MM_FW_CLASS_REF(MTLRenderPassColorAttachmentDescriptor));
    value.set(reinterpret_cast<id>(p));
    release(reinterpret_cast<id>(p));
  }
  return ret(value.value);
}
std::uint64_t pass_stencil(id self, SEL) {
  auto &value = as<PassDescriptor>(self)->stencil;
  if (!value.value) {
    auto *p = make<PassColor>(
        MM_FW_CLASS_REF(MTLRenderPassColorAttachmentDescriptor));
    value.set(reinterpret_cast<id>(p));
    release(reinterpret_cast<id>(p));
  }
  return ret(value.value);
}
std::uint64_t pass_factory(id c, SEL) {
  return ret(autorelease(
      reinterpret_cast<id>(make<PassDescriptor>(reinterpret_cast<Class>(c)))));
}
mm_two_doubles clear_get(id self, SEL) {
  auto color = as<PassColor>(self)->clear;
  mm_objc_return_doubles_hi(color[2], color[3]);
  return {color[0], color[1]};
}
std::uint64_t clear_set(id self, SEL, double r, double g, double b, double a) {
  as<PassColor>(self)->clear = {r, g, b, a};
  return 0;
}
std::uint64_t texture_factory(id c, SEL, std::uint64_t format,
                              std::uint64_t width, std::uint64_t height,
                              std::uint64_t mipmapped) {
  auto *descriptor = make<TextureDescriptor>(reinterpret_cast<Class>(c));
  descriptor->pixel_format = format;
  descriptor->width = width;
  descriptor->height = height;
  if ((mipmapped & 0xff) && width && height)
    descriptor->mips = static_cast<std::uint64_t>(std::floor(
                           std::log2(double(std::max(width, height))))) +
                       1;
  return ret(autorelease(reinterpret_cast<id>(descriptor)));
}

std::uint64_t library_file(id self, SEL, id path, id *error) {
  if (error)
    *error = nullptr;
  const char *name =
      path ? reinterpret_cast<const char *>(send(path, "UTF8String")) : nullptr;
  std::string message;
  std::vector<std::uint8_t> data;
  if (name) {
    std::ifstream file(name, std::ios::binary | std::ios::ate);
    auto size = file.tellg();
    if (file && size > 0 && size <= 16 * 1024 * 1024) {
      data.resize(static_cast<std::size_t>(size));
      file.seekg(0);
      file.read(reinterpret_cast<char *>(data.data()), size);
      if (!file)
        data.clear();
    }
  }
  if (data.empty())
    message = "cannot read metallib file";
  static std::mutex cache_mutex;
  static std::unordered_map<std::string,
                            std::weak_ptr<std::vector<mevk::ShaderFunction>>>
      cache;
  std::shared_ptr<std::vector<mevk::ShaderFunction>> functions;
  if (!data.empty()) {
    std::string key(reinterpret_cast<const char *>(data.data()), data.size());
    std::lock_guard lock(cache_mutex);
    for (auto it = cache.begin(); it != cache.end();) {
      if (it->second.expired())
        it = cache.erase(it);
      else
        ++it;
    }
    if (auto it = cache.find(key); it != cache.end())
      functions = it->second.lock();
    if (!functions) {
      functions = std::make_shared<std::vector<mevk::ShaderFunction>>();
      if (mevk::load_metallib(data, *functions, message))
        cache[key] = functions;
      else
        functions.reset();
    }
  }
  if (!functions) {
    if (error)
      *error = error_object(mevk_library_error_domain, 1, message);
    std::fprintf(stderr, "MEVK Metal library: %s (%s)\n", message.c_str(),
                 name ? name : "nil");
    return 0;
  }
  auto *library = make<Library>(MM_FW_CLASS_REF(MEVKLibrary));
  library->device.set(self);
  library->functions = std::move(functions);
  std::fprintf(stderr, "MEVK Metal: translated %zu AIR functions from %s\n",
               library->functions->size(), name);
  return ret(library);
}
std::uint64_t library_url(id self, SEL sel, id url, id *error) {
  return library_file(self, sel, obj(send(url, "path")), error);
}
std::uint64_t library_default_bundle(id self, SEL sel, id bundle, id *error) {
  id path = obj(send(bundle, "pathForResource:ofType:", ret(string("default")),
                     ret(string("metallib"))));
  return library_file(self, sel, path, error);
}
std::uint64_t library_default(id self, SEL sel) {
  id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
  return library_default_bundle(self, sel, bundle, nullptr);
}
std::uint64_t library_names(id self, SEL) {
  id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
  for (auto &f : *as<Library>(self)->functions)
    send(array, "addObject:", ret(string(f.name.c_str())));
  return ret(array);
}
std::uint64_t library_function(id self, SEL, id name) {
  const char *requested =
      name ? reinterpret_cast<const char *>(send(name, "UTF8String")) : nullptr;
  if (!requested)
    return 0;
  auto *library = as<Library>(self);
  for (auto &f : *library->functions)
    if (f.name == requested) {
      auto *result = make<Function>(MM_FW_CLASS_REF(MEVKFunction));
      result->device.set(library->device.value);
      result->shader =
          std::shared_ptr<const mevk::ShaderFunction>(library->functions, &f);
      return ret(result);
    }
  return 0;
}
std::uint64_t function_name(id self, SEL) {
  return ret(string(as<Function>(self)->shader->name.c_str()));
}
std::uint64_t function_type(id self, SEL) {
  return as<Function>(self)->shader->stage == mevk::ShaderStage::vertex ? 1 : 2;
}
std::uint64_t new_texture(id self, SEL, id descriptor) {
  if (!is_kind(descriptor, MM_FW_CLASS_REF(MTLTextureDescriptor))) {
    std::fprintf(stderr, "MEVK Metal: invalid texture descriptor\n");
    return 0;
  }
  auto *d = as<TextureDescriptor>(descriptor);
  if (d->width > UINT32_MAX || d->height > UINT32_MAX ||
      d->pixel_format > UINT32_MAX || d->depth != 1 || d->type != 2 ||
      d->mips != 1 || d->samples != 1 || d->array != 1 || d->storage > 2 ||
      (d->usage & ~std::uint64_t{5})) {
    std::fprintf(stderr, "MEVK Metal: unsupported texture "
                         "dimensions/type/mips/samples/storage/usage\n");
    return 0;
  }
  std::string error;
  auto backend = as<Device>(self)->context->create_texture(
      static_cast<std::uint32_t>(d->width),
      static_cast<std::uint32_t>(d->height),
      static_cast<std::uint32_t>(d->pixel_format), error);
  if (!backend) {
    std::fprintf(stderr, "MEVK Metal: %s\n", error.c_str());
    return 0;
  }
  auto *texture = make<TextureObject>(MM_FW_CLASS_REF(MEVKTexture));
  texture->device.set(self);
  texture->backend = std::move(backend);
  texture->storage = d->storage;
  texture->usage = d->usage;
  return ret(texture);
}
std::uint64_t texture_width(id self, SEL) {
  return as<TextureObject>(self)->backend->width();
}
std::uint64_t texture_height(id self, SEL) {
  return as<TextureObject>(self)->backend->height();
}
std::uint64_t texture_format(id self, SEL) {
  return as<TextureObject>(self)->backend->pixel_format();
}
std::uint64_t one(id, SEL) { return 1; }
std::uint64_t two(id, SEL) { return 2; }
std::uint64_t texture_replace(id self, SEL, const std::uint64_t *region,
                              std::uint64_t mip, const void *bytes,
                              std::uint64_t stride) {
  auto *t = as<TextureObject>(self);
  std::string error;
  if (!region || mip || region[2] || region[5] != 1 || region[0] > UINT32_MAX ||
      region[1] > UINT32_MAX || region[3] > UINT32_MAX ||
      region[4] > UINT32_MAX || t->framebuffer_only)
    error = "unsupported texture upload region/mip/framebufferOnly";
  else
    t->backend->replace(static_cast<std::uint32_t>(region[0]),
                        static_cast<std::uint32_t>(region[1]),
                        static_cast<std::uint32_t>(region[3]),
                        static_cast<std::uint32_t>(region[4]), bytes, stride,
                        error);
  if (!error.empty())
    std::fprintf(stderr, "MEVK Metal: %s\n", error.c_str());
  return 0;
}
std::uint64_t texture_get_bytes(id self, SEL, void *bytes, std::uint64_t stride,
                                const std::uint64_t *region,
                                std::uint64_t mip) {
  auto *t = as<TextureObject>(self);
  std::string error;
  auto bpp = t->backend->pixel_format() == 10 ? 1u : 4u;
  if (!bytes || !region || mip || region[2] || region[5] != 1 ||
      region[0] > t->backend->width() ||
      region[3] > t->backend->width() - region[0] ||
      region[1] > t->backend->height() ||
      region[4] > t->backend->height() - region[1] ||
      stride < region[3] * bpp ||
      (region[4] &&
       stride > std::numeric_limits<std::size_t>::max() / region[4]) ||
      t->framebuffer_only) {
    std::fprintf(
        stderr, "MEVK Metal: invalid getBytes region/stride/framebufferOnly\n");
    return 0;
  }
  std::vector<std::uint8_t> pixels;
  if (!t->backend->read_pixels(pixels, error)) {
    std::fprintf(stderr, "MEVK Metal: %s\n", error.c_str());
    return 0;
  }
  for (std::uint64_t row = 0; row < region[4]; ++row)
    std::memcpy(static_cast<std::uint8_t *>(bytes) + row * stride,
                pixels.data() +
                    ((row + region[1]) * t->backend->width() + region[0]) * bpp,
                static_cast<std::size_t>(region[3] * bpp));
  return 0;
}

std::uint64_t new_pipeline(id self, SEL, id descriptor, id *error) {
  if (error)
    *error = nullptr;
  std::string message;
  auto *d = is_kind(descriptor, MM_FW_CLASS_REF(MTLRenderPipelineDescriptor))
                ? as<PipelineDescriptor>(descriptor)
                : nullptr;
  std::shared_ptr<mevk::RenderPipeline> backend;
  if (!d || !is_kind(d->vertex.value, MM_FW_CLASS_REF(MEVKFunction)) ||
      !is_kind(d->fragment.value, MM_FW_CLASS_REF(MEVKFunction)))
    message = "pipeline requires vertex and fragment AIR functions";
  else {
    auto *colors =
        as<AttachmentArray>(obj(pipeline_colors(descriptor, nullptr)));
    auto *color = as<PipelineColor>(colors->values[0].value);
    bool extra = false;
    for (std::size_t i = 1; i < colors->values.size(); ++i)
      if (as<PipelineColor>(colors->values[i].value)->format)
        extra = true;
    if (extra || d->samples != 1 || d->depth_format || d->stencil_format ||
        d->vertex_descriptor.value || color->format > UINT32_MAX ||
        color->src_rgb > 10 || color->dst_rgb > 10 || color->src_alpha > 10 ||
        color->dst_alpha > 10 || color->op_rgb > 4 || color->op_alpha > 4 ||
        color->mask > 15)
      message = "unsupported pipeline attachments, vertex layout, samples or "
                "blend state";
    else {
      mevk::BlendState blend;
      blend.enabled = color->enabled != 0;
      blend.source_rgb = static_cast<std::uint32_t>(color->src_rgb);
      blend.destination_rgb = static_cast<std::uint32_t>(color->dst_rgb);
      blend.source_alpha = static_cast<std::uint32_t>(color->src_alpha);
      blend.destination_alpha = static_cast<std::uint32_t>(color->dst_alpha);
      blend.rgb_operation = static_cast<std::uint32_t>(color->op_rgb);
      blend.alpha_operation = static_cast<std::uint32_t>(color->op_alpha);
      blend.write_mask = static_cast<std::uint32_t>(color->mask);
      if (as<Function>(d->vertex.value)->device.value != self ||
          as<Function>(d->fragment.value)->device.value != self)
        message = "pipeline functions belong to another device";
      else
        backend = as<Device>(self)->context->create_render_pipeline(
            *as<Function>(d->vertex.value)->shader,
            *as<Function>(d->fragment.value)->shader,
            static_cast<std::uint32_t>(color->format), blend, message);
    }
  }
  if (!backend) {
    if (error)
      *error = error_object(mevk_pipeline_error_domain, 1, message);
    std::fprintf(stderr, "MEVK Metal pipeline: %s\n", message.c_str());
    return 0;
  }
  auto *result = make<PipelineObject>(MM_FW_CLASS_REF(MEVKRenderPipelineState));
  result->device.set(self);
  result->backend = std::move(backend);
  if (trace())
    std::fprintf(stderr, "MEVK Metal: created render pipeline %s + %s\n",
                 as<Function>(d->vertex.value)->shader->name.c_str(),
                 as<Function>(d->fragment.value)->shader->name.c_str());
  return ret(result);
}
bool encoding(RenderEncoder &encoder) {
  if (!encoder.state->diagnostic.empty())
    return false;
  if (encoder.ended || encoder.state->commit_requested) {
    encoder.state->fail("render encoder is ended or committed");
    return false;
  }
  return true;
}
std::uint64_t render_encoder(id self, SEL, id descriptor) {
  auto *command = as<Command>(self);
  auto state = command->state;
  std::lock_guard lock(state->mutex);
  if (!state->diagnostic.empty())
    return 0;
  if (state->encoder_active || state->commit_requested ||
      !is_kind(descriptor, MM_FW_CLASS_REF(MTLRenderPassDescriptor))) {
    state->fail(
        "invalid render pass descriptor or overlapping/committed encoder");
    return 0;
  }
  auto *d = as<PassDescriptor>(descriptor);
  auto *colors = as<AttachmentArray>(obj(pass_colors(descriptor, nullptr)));
  auto *color = as<PassColor>(colors->values[0].value);
  bool extra = false;
  for (std::size_t i = 1; i < colors->values.size(); ++i)
    if (as<PassColor>(colors->values[i].value)->texture.value)
      extra = true;
  if (extra || !is_kind(color->texture.value, MM_FW_CLASS_REF(MEVKTexture)) ||
      color->load > 2 || color->store > 1 || color->level || color->slice ||
      color->plane || d->array_length != 1 ||
      (d->depth.value && as<PassColor>(d->depth.value)->texture.value) ||
      (d->stencil.value && as<PassColor>(d->stencil.value)->texture.value)) {
    state->fail("render pass requires one supported color texture without "
                "depth/stencil/resolve");
    return 0;
  }
  auto *target = as<TextureObject>(color->texture.value);
  id device = as<Queue>(command->queue.value)->device.value;
  if (target->device.value != device ||
      (d->width && d->width != target->backend->width()) ||
      (d->height && d->height != target->backend->height())) {
    state->fail("render target device or dimensions mismatch");
    return 0;
  }
  std::string error;
  if (!state->backend->begin_render(target->backend, color->load != 1,
                                    color->clear, error)) {
    state->fail(error);
    return 0;
  }
  auto *encoder =
      make<RenderEncoder>(MM_FW_CLASS_REF(MEVKRenderCommandEncoder));
  encoder->command.set(self);
  encoder->state = state;
  encoder->context = as<Device>(device)->context;
  encoder->viewport = {
      0, 0, double(target->backend->width()), double(target->backend->height()),
      0, 1};
  encoder->scissor = {0, 0, target->backend->width(),
                      target->backend->height()};
  state->encoder_active = true;
  state->resources.emplace_back(color->texture.value);
  return ret(autorelease(reinterpret_cast<id>(encoder)));
}
std::uint64_t render_end(id self, SEL) {
  auto &encoder = *as<RenderEncoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (encoder.ended) {
    encoder.state->fail("render endEncoding called twice");
    return 0;
  }
  std::string error;
  if (encoder.state->diagnostic.empty() &&
      !encoder.state->backend->end_render(error))
    encoder.state->fail(error);
  encoder.ended = true;
  encoder.state->encoder_active = false;
  return 0;
}
std::uint64_t render_device(id self, SEL) {
  return ret(
      as<Queue>(
          as<Command>(as<RenderEncoder>(self)->command.value)->queue.value)
          ->device.value);
}
std::uint64_t render_pipeline(id self, SEL, id pipeline) {
  auto &encoder = *as<RenderEncoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (!is_kind(pipeline, MM_FW_CLASS_REF(MEVKRenderPipelineState)))
    encoder.state->fail("invalid render pipeline state");
  else {
    encoder.pipeline = as<PipelineObject>(pipeline)->backend;
    encoder.state->resources.emplace_back(pipeline);
  }
  return 0;
}
std::uint64_t render_buffer(id self, SEL selector, id buffer,
                            std::uint64_t offset, std::uint64_t index) {
  auto &encoder = *as<RenderEncoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (index >= 16 || offset > UINT32_MAX ||
      (buffer && !is_kind(buffer, cls("MEVKBuffer")))) {
    encoder.state->fail("invalid render buffer/index/offset");
    return 0;
  }
  auto slot = static_cast<std::size_t>(index) +
              (std::strstr(selector, "Fragment") ? 16u : 0u);
  encoder.bindings.buffers[slot] =
      buffer ? as<Buffer>(buffer)->backend : nullptr;
  encoder.bindings.offsets[slot] = static_cast<std::uint32_t>(offset);
  if (buffer)
    encoder.state->resources.emplace_back(buffer);
  return 0;
}
std::uint64_t render_buffer_offset(id self, SEL selector, std::uint64_t offset,
                                   std::uint64_t index) {
  auto &encoder = *as<RenderEncoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (index >= 16 || offset > UINT32_MAX) {
    encoder.state->fail("invalid render buffer offset/index");
    return 0;
  }
  encoder.bindings.offsets[static_cast<std::size_t>(index) +
                           (std::strstr(selector, "Fragment") ? 16u : 0u)] =
      static_cast<std::uint32_t>(offset);
  return 0;
}
std::uint64_t render_bytes(id self, SEL selector, const void *bytes,
                           std::uint64_t size, std::uint64_t index) {
  auto &encoder = *as<RenderEncoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (!bytes || !size || size > 4096 || index >= 16) {
    encoder.state->fail("invalid inline render bytes (maximum 4096)");
    return 0;
  }
  std::string error;
  auto buffer = encoder.context->create_buffer(size, error);
  if (!buffer) {
    encoder.state->fail(error);
    return 0;
  }
  std::memcpy(buffer->contents(), bytes, static_cast<std::size_t>(size));
  auto slot = static_cast<std::size_t>(index) +
              (std::strstr(selector, "Fragment") ? 16u : 0u);
  encoder.bindings.buffers[slot] = std::move(buffer);
  encoder.bindings.offsets[slot] = 0;
  return 0;
}
std::uint64_t render_texture(id self, SEL selector, id texture,
                             std::uint64_t index) {
  auto &encoder = *as<RenderEncoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (index >= 16 ||
      (texture && !is_kind(texture, MM_FW_CLASS_REF(MEVKTexture)))) {
    encoder.state->fail("invalid shader texture/index");
    return 0;
  }
  if (texture && as<TextureObject>(texture)->framebuffer_only) {
    encoder.state->fail("framebufferOnly texture cannot be sampled");
    return 0;
  }
  auto slot = static_cast<std::size_t>(index) +
              (std::strstr(selector, "Fragment") ? 16u : 0u);
  encoder.bindings.textures[slot] =
      texture ? as<TextureObject>(texture)->backend : nullptr;
  if (texture)
    encoder.state->resources.emplace_back(texture);
  return 0;
}
std::uint64_t render_viewport(id self, SEL, const mevk::Viewport *viewport) {
  auto &encoder = *as<RenderEncoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (encoding(encoder)) {
    if (viewport)
      encoder.viewport = *viewport;
    else
      encoder.state->fail("null viewport");
  }
  return 0;
}
std::uint64_t render_scissor(id self, SEL, const std::uint64_t *rectangle) {
  auto &encoder = *as<RenderEncoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (!rectangle) {
    encoder.state->fail("null scissor");
    return 0;
  }
  for (unsigned i = 0; i < 4; ++i)
    if (rectangle[i] > UINT32_MAX) {
      encoder.state->fail("scissor exceeds Vulkan integer range");
      return 0;
    }
  encoder.scissor = {static_cast<std::uint32_t>(rectangle[0]),
                     static_cast<std::uint32_t>(rectangle[1]),
                     static_cast<std::uint32_t>(rectangle[2]),
                     static_cast<std::uint32_t>(rectangle[3])};
  return 0;
}
std::uint64_t draw_common(id self, std::uint64_t primitive, std::uint64_t start,
                          std::uint64_t count, std::uint64_t instances,
                          std::uint64_t base_instance, id indices,
                          std::uint64_t offset, std::uint64_t index_type,
                          std::int64_t base_vertex) {
  auto &encoder = *as<RenderEncoder>(self);
  std::lock_guard lock(encoder.state->mutex);
  if (!encoding(encoder))
    return 0;
  if (primitive > 4 || start > UINT32_MAX || count > UINT32_MAX ||
      instances > UINT32_MAX || base_instance > UINT32_MAX || index_type > 1 ||
      base_vertex < INT32_MIN || base_vertex > INT32_MAX ||
      (indices && !is_kind(indices, cls("MEVKBuffer")))) {
    encoder.state->fail("unsupported draw parameters or index buffer");
    return 0;
  }
  std::string error;
  if (!encoder.state->backend->draw(
          encoder.pipeline, encoder.bindings, encoder.viewport, encoder.scissor,
          static_cast<std::uint32_t>(primitive),
          static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(count),
          static_cast<std::uint32_t>(instances),
          static_cast<std::uint32_t>(base_instance),
          indices ? as<Buffer>(indices)->backend : nullptr, offset,
          static_cast<std::uint32_t>(index_type),
          static_cast<std::int32_t>(base_vertex), error))
    encoder.state->fail(error);
  if (indices)
    encoder.state->resources.emplace_back(indices);
  return 0;
}
std::uint64_t draw(id self, SEL, std::uint64_t type, std::uint64_t start,
                   std::uint64_t count) {
  return draw_common(self, type, start, count, 1, 0, nullptr, 0, 0, 0);
}
std::uint64_t draw_instances(id self, SEL, std::uint64_t type,
                             std::uint64_t start, std::uint64_t count,
                             std::uint64_t instances) {
  return draw_common(self, type, start, count, instances, 0, nullptr, 0, 0, 0);
}
std::uint64_t draw_base(id self, SEL, std::uint64_t type, std::uint64_t start,
                        std::uint64_t count, std::uint64_t instances,
                        std::uint64_t base) {
  return draw_common(self, type, start, count, instances, base, nullptr, 0, 0,
                     0);
}
std::uint64_t indexed(id self, SEL, std::uint64_t type, std::uint64_t count,
                      std::uint64_t index_type, id buffer,
                      std::uint64_t offset) {
  return draw_common(self, type, 0, count, 1, 0, buffer, offset, index_type, 0);
}
std::uint64_t indexed_instances(id self, SEL, std::uint64_t type,
                                std::uint64_t count, std::uint64_t index_type,
                                id buffer, std::uint64_t offset,
                                std::uint64_t instances) {
  return draw_common(self, type, 0, count, instances, 0, buffer, offset,
                     index_type, 0);
}
std::uint64_t indexed_base(id self, SEL, std::uint64_t type,
                           std::uint64_t count, std::uint64_t index_type,
                           id buffer, std::uint64_t offset,
                           std::uint64_t instances, std::int64_t base_vertex,
                           std::uint64_t base_instance) {
  return draw_common(self, type, 0, count, instances, base_instance, buffer,
                     offset, index_type, base_vertex);
}

char layer_key;
LayerState *layer_state(id layer) {
  id associated = metal_get_association(layer, &layer_key);
  if (associated)
    return as<LayerState>(associated);
  auto *state = make<LayerState>(MM_FW_CLASS_REF(MEVKLayerState));
  metal_set_association(layer, &layer_key, reinterpret_cast<id>(state), 1);
  release(reinterpret_cast<id>(state));
  return state;
}
std::uint64_t layer_device(id self, SEL) {
  auto *state = layer_state(self); std::lock_guard lock(state->mutex);
  return ret(state->device.value);
}
std::uint64_t layer_set_device(id self, SEL, id device) {
  if (device && !is_kind(device, cls("MEVKDevice"))) {
    std::fprintf(stderr, "MEVK Metal: invalid CAMetalLayer device\n");
    return 0;
  }
  auto *state = layer_state(self); std::lock_guard lock(state->mutex);
  state->device.set(device);
  state->drawables.clear();
  return 0;
}
std::uint64_t layer_format(id self, SEL) { auto *state = layer_state(self); std::lock_guard lock(state->mutex); return state->format; }
std::uint64_t layer_set_format(id self, SEL, std::uint64_t format) {
  auto *state = layer_state(self); std::lock_guard lock(state->mutex);
  state->format = format;
  state->drawables.clear();
  return 0;
}
std::uint64_t layer_framebuffer(id self, SEL) {
  auto *state = layer_state(self); std::lock_guard lock(state->mutex);
  return state->framebuffer_only ? 1 : 0;
}
std::uint64_t layer_set_framebuffer(id self, SEL, std::uint64_t value) {
  auto *state = layer_state(self); std::lock_guard lock(state->mutex);
  state->framebuffer_only = (value & 0xff) != 0;
  return 0;
}
mm_two_doubles layer_size(id self, SEL) {
  auto *state = layer_state(self);
  std::lock_guard lock(state->mutex);
  return {state->width, state->height};
}
std::uint64_t layer_set_size(id self, SEL, double width, double height) {
  if (!std::isfinite(width) || !std::isfinite(height) || width < 0 ||
      height < 0 || width > UINT32_MAX || height > UINT32_MAX) {
    std::fprintf(stderr, "MEVK Metal: invalid drawable size\n");
    return 0;
  }
  auto *state = layer_state(self);
  std::lock_guard lock(state->mutex);
  if (state->width != width || state->height != height) state->drawables.clear();
  state->width = width;
  state->height = height;
  return 0;
}
std::uint64_t next_drawable(id self, SEL) {
  auto *state = layer_state(self);
  std::unique_lock lock(state->mutex);
  id device = state->device.value;
  if (!device) {
    id default_device = metal_create_default_device();
    state->device.set(default_device);
    release(default_device);
    device = state->device.value;
  }
  if (!device)
    return 0;
  if (state->width <= 0 || state->height <= 0) {
    auto *api = mm_objc_host_api();
    auto *context =
        api && api->current_context ? api->current_context() : nullptr;
    if (context) {
      send(self, "bounds");
      double width = 0, height = 0;
      std::memcpy(&width, &context->v[2][0], 8);
      std::memcpy(&height, &context->v[3][0], 8);
      layer_set_size(self, nullptr, width, height);
    }
  }
  if (state->width <= 0 || state->height <= 0 || state->format > UINT32_MAX)
    return 0;
  std::string error;
  std::shared_ptr<mevk::Texture> texture;
  for (auto &candidate : state->drawables) if (candidate.use_count() == 1) { texture = candidate; break; }
  // Three render slots plus three WSI copies can retain sources concurrently.
  // Extra leases allow the presenter to recycle completed WSI slots.
  if (!texture && state->drawables.size() >= 8) return 0;
  if (!texture) {
    texture = as<Device>(device)->context->create_texture(
        static_cast<std::uint32_t>(std::ceil(state->width)),
        static_cast<std::uint32_t>(std::ceil(state->height)),
        static_cast<std::uint32_t>(state->format), error);
    if (texture) state->drawables.push_back(texture);
  }
  if (!texture) {
    std::fprintf(stderr, "MEVK Metal drawable: %s\n", error.c_str());
    return 0;
  }
  auto *resource = make<TextureObject>(MM_FW_CLASS_REF(MEVKTexture));
  resource->device.set(device);
  resource->backend = std::move(texture);
  resource->usage = 4;
  resource->framebuffer_only = state->framebuffer_only;
  auto *drawable = make<Drawable>(MM_FW_CLASS_REF(MEVKDrawable));
  drawable->layer.set(self);
  drawable->texture.set(reinterpret_cast<id>(resource));
  release(reinterpret_cast<id>(resource));
  return ret(autorelease(reinterpret_cast<id>(drawable)));
}
bool present_impl(id drawable, std::string &error, bool &readback) {
  auto *d = as<Drawable>(drawable);
  auto *texture = as<TextureObject>(d->texture.value);
  const char *mode = std::getenv("MACHOMAN_MEVK_PRESENT");
  if (!mode || std::strcmp(mode, "cpu") != 0) {
    using GpuHook = int (*)(id, const void *);
    const auto gpu = reinterpret_cast<GpuHook>(dlsym(RTLD_DEFAULT, "machoman_appkit_present_metal_texture"));
    const int result = gpu ? gpu(d->layer.value, &texture->backend) : -1;
    if (result > 0) {
      if (trace()) std::fprintf(stderr, "MEVK Metal: GPU present %ux%u drawable (no readback)\n", texture->backend->width(), texture->backend->height());
      return true;
    }
    if (result == 0 || (mode && std::strcmp(mode, "vulkan") == 0)) {
      error = "direct Vulkan presentation failed"; return false;
    }
  }
  std::vector<std::uint8_t> pixels;
  readback = true;
  if (!texture->backend->read_pixels(pixels, error))
    return false;
  auto format = texture->backend->pixel_format();
  if (format == 70 || format == 71)
    for (std::size_t i = 0; i < pixels.size(); i += 4)
      std::swap(pixels[i], pixels[i + 2]);
  if (format != 70 && format != 71 && format != 80 && format != 81) {
    error = "unsupported drawable presentation format";
    return false;
  }
  using Hook =
      int (*)(id, const void *, std::uint32_t, std::uint32_t, std::uint64_t);
  auto hook = reinterpret_cast<Hook>(
      dlsym(RTLD_DEFAULT, "machoman_appkit_present_metal"));
  if (!hook) {
    error = "AppKit Metal presentation hook is unavailable";
    return false;
  }
  if (!hook(d->layer.value, pixels.data(), texture->backend->width(),
            texture->backend->height(),
            std::uint64_t{texture->backend->width()} * 4)) {
    error = "AppKit could not present Metal pixels";
    return false;
  }
  if (trace())
    std::fprintf(stderr, "MEVK Metal: presented %ux%u drawable\n",
                 texture->backend->width(), texture->backend->height());
  return true;
}
bool present(id drawable, std::string &error) {
  const bool timing = std::getenv("MACHOMAN_APPKIT_FRAME_TIMING") != nullptr;
  const auto begin = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  bool readback = false;
  const bool ok = present_impl(drawable, error, readback);
  if (timing && ok) {
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
    auto *d = as<Drawable>(drawable); auto *state = layer_state(d->layer.value);
    std::lock_guard lock(state->mutex);
    ++state->timing_count; state->timing_readbacks += readback;
    state->timing_total_ms += ms; state->timing_max_ms = std::max(state->timing_max_ms, ms);
    const double seconds = std::chrono::duration<double>(end - state->timing_since).count();
    if (seconds >= 3) {
      const auto stats = as<Device>(as<TextureObject>(d->texture.value)->device.value)->context->allocation_stats();
      std::fprintf(stderr, "MEVK output: layer=%p seconds=%.3f frames=%llu mean_ms=%.3f max_ms=%.3f readbacks=%llu drawable_pool=%zu buffers=%llu/%llu commands=%llu/%llu textures=%llu\n",
          static_cast<void *>(d->layer.value), seconds, static_cast<unsigned long long>(state->timing_count),
          state->timing_total_ms / static_cast<double>(state->timing_count), state->timing_max_ms,
          static_cast<unsigned long long>(state->timing_readbacks), state->drawables.size(),
          static_cast<unsigned long long>(stats.buffers_created), static_cast<unsigned long long>(stats.buffers_reused),
          static_cast<unsigned long long>(stats.commands_created), static_cast<unsigned long long>(stats.commands_reused),
          static_cast<unsigned long long>(stats.textures_created));
      state->timing_count = state->timing_readbacks = 0; state->timing_total_ms = state->timing_max_ms = 0;
      state->timing_since = end;
    }
  }
  return ok;
}
std::uint64_t drawable_present(id self, SEL) {
  std::string error;
  if (!present(self, error))
    std::fprintf(stderr, "MEVK Metal: %s\n", error.c_str());
  return 0;
}
std::uint64_t command_present(id self, SEL, id drawable) {
  auto state = as<Command>(self)->state;
  std::lock_guard lock(state->mutex);
  if (state->commit_requested ||
      !is_kind(drawable, MM_FW_CLASS_REF(MEVKDrawable)) ||
      state->presentation) {
    state->fail("invalid drawable or repeated/late presentDrawable");
    return 0;
  }
  auto owned = std::make_shared<OwnedId>(drawable);
  state->presentation = [owned](std::string &error) {
    return present(owned->value, error);
  };
  return 0;
}
} // namespace

void register_render_classes(Class device, Class command) {
  Class root = cls("NSObject");
#define DEFINE(T, name)                                                        \
  Class name = mm_objc_define_class(                                           \
      #name, root, sizeof(T), MM_FW_CLASS_REF(name), MM_FW_META_REF(name));    \
  lifecycle<T>(name)
  DEFINE(Library, MEVKLibrary);
  DEFINE(Function, MEVKFunction);
  DEFINE(TextureObject, MEVKTexture);
  DEFINE(PipelineObject, MEVKRenderPipelineState);
  DEFINE(RenderEncoder, MEVKRenderCommandEncoder);
  DEFINE(TextureDescriptor, MTLTextureDescriptor);
  DEFINE(PipelineDescriptor, MTLRenderPipelineDescriptor);
  DEFINE(PipelineColor, MTLRenderPipelineColorAttachmentDescriptor);
  DEFINE(AttachmentArray, MTLRenderPipelineColorAttachmentDescriptorArray);
  DEFINE(PassDescriptor, MTLRenderPassDescriptor);
  DEFINE(PassColor, MTLRenderPassColorAttachmentDescriptor);
  DEFINE(AttachmentArray, MTLRenderPassColorAttachmentDescriptorArray);
  DEFINE(LayerState, MEVKLayerState);
  DEFINE(Drawable, MEVKDrawable);
#undef DEFINE
  MM_METHODS(
      device,
      {"newLibraryWithFile:error:", MM_IMP(library_file), "@32@0:8@16^@24"},
      {"newLibraryWithURL:error:", MM_IMP(library_url), "@32@0:8@16^@24"},
      {"newDefaultLibrary", MM_IMP(library_default), "@16@0:8"},
      {"newDefaultLibraryWithBundle:error:", MM_IMP(library_default_bundle),
       "@32@0:8@16^@24"},
      {"newTextureWithDescriptor:", MM_IMP(new_texture), "@24@0:8@16"},
      {"newRenderPipelineStateWithDescriptor:error:", MM_IMP(new_pipeline),
       "@32@0:8@16^@24"});
  MM_METHODS(command,
             {"renderCommandEncoderWithDescriptor:", MM_IMP(render_encoder),
              "@24@0:8@16"},
             {"presentDrawable:", MM_IMP(command_present), "v24@0:8@16"});
  MM_METHODS(
      MEVKLibrary,
      {"device", MM_IMP((get_object<Library, &Library::device>)), "@16@0:8"},
      {"functionNames", MM_IMP(library_names), "@16@0:8"},
      {"newFunctionWithName:", MM_IMP(library_function), "@24@0:8@16"});
  MM_METHODS(
      MEVKFunction, {"name", MM_IMP(function_name), "@16@0:8"},
      {"functionType", MM_IMP(function_type), "Q16@0:8"},
      {"device", MM_IMP((get_object<Function, &Function::device>)), "@16@0:8"});
  MM_METHODS(MEVKRenderPipelineState,
             {"device",
              MM_IMP((get_object<PipelineObject, &PipelineObject::device>)),
              "@16@0:8"});
  MM_METHODS(
      MEVKTexture,
      {"device", MM_IMP((get_object<TextureObject, &TextureObject::device>)),
       "@16@0:8"},
      {"width", MM_IMP(texture_width), "Q16@0:8"},
      {"height", MM_IMP(texture_height), "Q16@0:8"},
      {"pixelFormat", MM_IMP(texture_format), "Q16@0:8"},
      {"depth", MM_IMP(one), "Q16@0:8"},
      {"arrayLength", MM_IMP(one), "Q16@0:8"},
      {"mipmapLevelCount", MM_IMP(one), "Q16@0:8"},
      {"sampleCount", MM_IMP(one), "Q16@0:8"},
      {"textureType", MM_IMP(two), "Q16@0:8"},
      {"storageMode", MM_IMP((get<TextureObject, &TextureObject::storage>)),
       "Q16@0:8"},
      {"usage", MM_IMP((get<TextureObject, &TextureObject::usage>)), "Q16@0:8"},
      {"replaceRegion:mipmapLevel:withBytes:bytesPerRow:",
       MM_IMP(texture_replace),
       "v88@0:8{MTLRegion={MTLOrigin=QQQ}{MTLSize=QQQ}}16Q64^v72Q80"},
      {"getBytes:bytesPerRow:fromRegion:mipmapLevel:",
       MM_IMP(texture_get_bytes),
       "v88@0:8^v16Q24{MTLRegion={MTLOrigin=QQQ}{MTLSize=QQQ}}32Q80"});
  MM_CLASS_METHODS(
      MTLTextureDescriptor,
      {"texture2DDescriptorWithPixelFormat:width:height:mipmapped:",
       MM_IMP(texture_factory), "@48@0:8Q16Q24Q32B40"});
#define U(c, T, g, s, f) uint_property<T, &T::f>(c, g, s)
  U(MTLTextureDescriptor, TextureDescriptor, "width", "setWidth:", width);
  U(MTLTextureDescriptor, TextureDescriptor, "height", "setHeight:", height);
  U(MTLTextureDescriptor, TextureDescriptor, "depth", "setDepth:", depth);
  U(MTLTextureDescriptor, TextureDescriptor, "pixelFormat",
    "setPixelFormat:", pixel_format);
  U(MTLTextureDescriptor, TextureDescriptor, "textureType",
    "setTextureType:", type);
  U(MTLTextureDescriptor, TextureDescriptor, "mipmapLevelCount",
    "setMipmapLevelCount:", mips);
  U(MTLTextureDescriptor, TextureDescriptor, "sampleCount",
    "setSampleCount:", samples);
  U(MTLTextureDescriptor, TextureDescriptor, "arrayLength",
    "setArrayLength:", array);
  U(MTLTextureDescriptor, TextureDescriptor, "storageMode",
    "setStorageMode:", storage);
  U(MTLTextureDescriptor, TextureDescriptor, "usage", "setUsage:", usage);
  U(MTLRenderPipelineDescriptor, PipelineDescriptor, "sampleCount",
    "setSampleCount:", samples);
  U(MTLRenderPipelineDescriptor, PipelineDescriptor, "rasterSampleCount",
    "setRasterSampleCount:", samples);
  U(MTLRenderPipelineDescriptor, PipelineDescriptor,
    "depthAttachmentPixelFormat",
    "setDepthAttachmentPixelFormat:", depth_format);
  U(MTLRenderPipelineDescriptor, PipelineDescriptor,
    "stencilAttachmentPixelFormat",
    "setStencilAttachmentPixelFormat:", stencil_format);
  U(MTLRenderPipelineColorAttachmentDescriptor, PipelineColor, "pixelFormat",
    "setPixelFormat:", format);
  bool_property<PipelineColor, &PipelineColor::enabled>(
      MTLRenderPipelineColorAttachmentDescriptor, "isBlendingEnabled",
      "setBlendingEnabled:");
  U(MTLRenderPipelineColorAttachmentDescriptor, PipelineColor,
    "sourceRGBBlendFactor", "setSourceRGBBlendFactor:", src_rgb);
  U(MTLRenderPipelineColorAttachmentDescriptor, PipelineColor,
    "destinationRGBBlendFactor", "setDestinationRGBBlendFactor:", dst_rgb);
  U(MTLRenderPipelineColorAttachmentDescriptor, PipelineColor,
    "sourceAlphaBlendFactor", "setSourceAlphaBlendFactor:", src_alpha);
  U(MTLRenderPipelineColorAttachmentDescriptor, PipelineColor,
    "destinationAlphaBlendFactor",
    "setDestinationAlphaBlendFactor:", dst_alpha);
  U(MTLRenderPipelineColorAttachmentDescriptor, PipelineColor,
    "rgbBlendOperation", "setRgbBlendOperation:", op_rgb);
  U(MTLRenderPipelineColorAttachmentDescriptor, PipelineColor,
    "alphaBlendOperation", "setAlphaBlendOperation:", op_alpha);
  U(MTLRenderPipelineColorAttachmentDescriptor, PipelineColor, "writeMask",
    "setWriteMask:", mask);
  U(MTLRenderPassColorAttachmentDescriptor, PassColor, "loadAction",
    "setLoadAction:", load);
  U(MTLRenderPassColorAttachmentDescriptor, PassColor, "storeAction",
    "setStoreAction:", store);
  U(MTLRenderPassColorAttachmentDescriptor, PassColor, "level",
    "setLevel:", level);
  U(MTLRenderPassColorAttachmentDescriptor, PassColor, "slice",
    "setSlice:", slice);
  U(MTLRenderPassColorAttachmentDescriptor, PassColor, "depthPlane",
    "setDepthPlane:", plane);
  U(MTLRenderPassDescriptor, PassDescriptor, "renderTargetWidth",
    "setRenderTargetWidth:", width);
  U(MTLRenderPassDescriptor, PassDescriptor, "renderTargetHeight",
    "setRenderTargetHeight:", height);
  U(MTLRenderPassDescriptor, PassDescriptor, "renderTargetArrayLength",
    "setRenderTargetArrayLength:", array_length);
#undef U
  object_property<PipelineDescriptor, &PipelineDescriptor::vertex>(
      MTLRenderPipelineDescriptor, "vertexFunction", "setVertexFunction:");
  object_property<PipelineDescriptor, &PipelineDescriptor::fragment>(
      MTLRenderPipelineDescriptor, "fragmentFunction", "setFragmentFunction:");
  object_property<PipelineDescriptor, &PipelineDescriptor::vertex_descriptor>(
      MTLRenderPipelineDescriptor, "vertexDescriptor", "setVertexDescriptor:");
  object_property<PassColor, &PassColor::texture>(
      MTLRenderPassColorAttachmentDescriptor, "texture", "setTexture:");
  MM_METHODS(MTLRenderPipelineDescriptor,
             {"colorAttachments", MM_IMP(pipeline_colors), "@16@0:8"});
  MM_METHODS(MTLRenderPassDescriptor,
             {"colorAttachments", MM_IMP(pass_colors), "@16@0:8"},
             {"depthAttachment", MM_IMP(pass_depth), "@16@0:8"},
             {"stencilAttachment", MM_IMP(pass_stencil), "@16@0:8"});
  MM_CLASS_METHODS(MTLRenderPassDescriptor,
                   {"renderPassDescriptor", MM_IMP(pass_factory), "@16@0:8"});
  for (Class array : {MTLRenderPassColorAttachmentDescriptorArray,
                      MTLRenderPipelineColorAttachmentDescriptorArray})
    MM_METHODS(array,
               {"objectAtIndexedSubscript:", MM_IMP(array_at), "@24@0:8Q16"},
               {"objectAtIndex:", MM_IMP(array_at), "@24@0:8Q16"});
  MM_METHODS(
      MTLRenderPassColorAttachmentDescriptor,
      {"clearColor", MM_IMP(clear_get), "{MTLClearColor=dddd}16@0:8"},
      {"setClearColor:", MM_IMP(clear_set), "v48@0:8{MTLClearColor=dddd}16"});
  MM_METHODS(
      MEVKRenderCommandEncoder, {"device", MM_IMP(render_device), "@16@0:8"},
      {"endEncoding", MM_IMP(render_end), "v16@0:8"},
      {"setRenderPipelineState:", MM_IMP(render_pipeline), "v24@0:8@16"},
      {"setVertexBuffer:offset:atIndex:", MM_IMP(render_buffer),
       "v40@0:8@16Q24Q32"},
      {"setFragmentBuffer:offset:atIndex:", MM_IMP(render_buffer),
       "v40@0:8@16Q24Q32"},
      {"setVertexBytes:length:atIndex:", MM_IMP(render_bytes),
       "v40@0:8^v16Q24Q32"},
      {"setFragmentBytes:length:atIndex:", MM_IMP(render_bytes),
       "v40@0:8^v16Q24Q32"},
      {"setVertexBufferOffset:atIndex:", MM_IMP(render_buffer_offset),
       "v32@0:8Q16Q24"},
      {"setFragmentBufferOffset:atIndex:", MM_IMP(render_buffer_offset),
       "v32@0:8Q16Q24"},
      {"setFragmentTexture:atIndex:", MM_IMP(render_texture), "v32@0:8@16Q24"},
      {"setVertexTexture:atIndex:", MM_IMP(render_texture), "v32@0:8@16Q24"},
      {"setViewport:", MM_IMP(render_viewport),
       "v64@0:8{MTLViewport=dddddd}16"},
      {"setScissorRect:", MM_IMP(render_scissor),
       "v48@0:8{MTLScissorRect=QQQQ}16"},
      {"drawPrimitives:vertexStart:vertexCount:", MM_IMP(draw),
       "v40@0:8Q16Q24Q32"},
      {"drawPrimitives:vertexStart:vertexCount:instanceCount:",
       MM_IMP(draw_instances), "v48@0:8Q16Q24Q32Q40"},
      {"drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
       MM_IMP(draw_base), "v56@0:8Q16Q24Q32Q40Q48"},
      {"drawIndexedPrimitives:indexCount:indexType:indexBuffer:"
       "indexBufferOffset:",
       MM_IMP(indexed), "v56@0:8Q16Q24Q32@40Q48"},
      {"drawIndexedPrimitives:indexCount:indexType:indexBuffer:"
       "indexBufferOffset:instanceCount:",
       MM_IMP(indexed_instances), "v64@0:8Q16Q24Q32@40Q48Q56"},
      {"drawIndexedPrimitives:indexCount:indexType:indexBuffer:"
       "indexBufferOffset:instanceCount:baseVertex:baseInstance:",
       MM_IMP(indexed_base), "v80@0:8Q16Q24Q32@40Q48Q56q64Q72"});
  MM_METHODS(
      MEVKDrawable,
      {"texture", MM_IMP((get_object<Drawable, &Drawable::texture>)),
       "@16@0:8"},
      {"layer", MM_IMP((get_object<Drawable, &Drawable::layer>)), "@16@0:8"},
      {"present", MM_IMP(drawable_present), "v16@0:8"});
  mevk_pipeline_error_domain = retain(string("MTLRenderPipelineErrorDomain"));
}

void register_metal_layer_if_available() {
  if (Class c = cls("CAMetalLayer")) {
    static std::mutex mutex;
    static Class registered = nullptr;
    std::lock_guard lock(mutex);
    if (registered == c)
      return;
    registered = c;
    MM_METHODS(
        c, {"device", MM_IMP(layer_device), "@16@0:8"},
        {"setDevice:", MM_IMP(layer_set_device), "v24@0:8@16"},
        {"pixelFormat", MM_IMP(layer_format), "Q16@0:8"},
        {"setPixelFormat:", MM_IMP(layer_set_format), "v24@0:8Q16"},
        {"framebufferOnly", MM_IMP(layer_framebuffer), "B16@0:8"},
        {"setFramebufferOnly:", MM_IMP(layer_set_framebuffer), "v24@0:8B16"},
        {"drawableSize", MM_IMP(layer_size), "{CGSize=dd}16@0:8"},
        {"setDrawableSize:", MM_IMP(layer_set_size), "v32@0:8{CGSize=dd}16"},
        {"nextDrawable", MM_IMP(next_drawable), "@16@0:8"});
  }
}
} // namespace mevk_metal
extern "C" MM_FW_EXPORT void machoman_mevk_register_layer() {
  mevk_metal::register_metal_layer_if_available();
}
