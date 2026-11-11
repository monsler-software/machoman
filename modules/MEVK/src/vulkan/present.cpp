#if defined(__linux__)
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include "internal.h"
#include "mevk/present.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace mevk {
PresentationRegion presentation_region(unsigned width, unsigned height,
    unsigned out_width, unsigned out_height, double logical_width,
    double logical_height, double scale, unsigned placement) {
    PresentationRegion r;
    if (!width || !height || !out_width || !out_height ||
        !std::isfinite(logical_width) || !std::isfinite(logical_height) ||
        !std::isfinite(scale) || logical_width <= 0 || logical_height <= 0 || scale <= 0 || placement > 11) return r;
    double w = width / scale, h = height / scale, left = 0, top = 0;
    if (placement == 0) { w = logical_width; h = logical_height; }
    else if (placement == 1 || placement == 2) {
        const double f = placement == 1 ? std::min(logical_width / w, logical_height / h)
                                         : std::max(logical_width / w, logical_height / h);
        w *= f; h *= f; left = (logical_width - w) / 2; top = (logical_height - h) / 2;
    } else {
        if (placement == 3 || placement == 4 || placement == 8) left = (logical_width - w) / 2;
        else if (placement == 5 || placement == 6 || placement == 7) left = logical_width - w;
        if (placement == 3 || placement == 6 || placement == 10) top = (logical_height - h) / 2;
        else if (placement == 7 || placement == 8 || placement == 9) top = logical_height - h;
    }
    const double x0 = std::max(0., left), y0 = std::max(0., top);
    const double x1 = std::min(logical_width, left + w), y1 = std::min(logical_height, top + h);
    if (x1 <= x0 || y1 <= y0) return r;
    auto source = [](double v, unsigned limit) { return static_cast<std::int32_t>(std::clamp(std::lround(v), 0l, static_cast<long>(limit))); };
    r.sx0 = source((x0 - left) / w * width, width); r.sx1 = source((x1 - left) / w * width, width);
    r.sy0 = source((y0 - top) / h * height, height); r.sy1 = source((y1 - top) / h * height, height);
    r.dx0 = source(x0 / logical_width * out_width, out_width); r.dx1 = source(x1 / logical_width * out_width, out_width);
    r.dy0 = source(y0 / logical_height * out_height, out_height); r.dy1 = source(y1 / logical_height * out_height, out_height);
    r.empty = r.sx0 >= r.sx1 || r.sy0 >= r.sy1 || r.dx0 >= r.dx1 || r.dy0 >= r.dy1;
    return r;
}

struct WaylandPresenter::Impl {
    std::shared_ptr<Context::Impl> context;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    unsigned width = 0, height = 0;
    unsigned requested_width = 0, requested_height = 0;
    bool dirty = true;
    struct Image { VkImage image = VK_NULL_HANDLE; VkSemaphore ready = VK_NULL_HANDLE; bool initialized = false; };
    std::vector<Image> images;
    struct Frame {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        std::shared_ptr<Texture> source;
    };
    std::array<Frame, 3> frames;
    PresentationStats counts;
    void destroy_swapchain() {
        for (auto image : images) if (image.ready) vkDestroySemaphore(context->device, image.ready, nullptr);
        images.clear();
        if (swapchain) vkDestroySwapchainKHR(context->device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
    bool resize(unsigned wanted_width, unsigned wanted_height, std::string &error) {
        VkSurfaceCapabilitiesKHR caps{};
        if (!vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context->physical_device, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", error)) return false;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { error = "MEVK: swapchain lacks transfer destination support"; return false; }
        std::uint32_t count = 0;
        if (!vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(context->physical_device, surface, &count, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR", error)) return false;
        std::vector<VkSurfaceFormatKHR> formats(count);
        if (!vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(context->physical_device, surface, &count, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR", error)) return false;
        VkSurfaceFormatKHR selected{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        for (auto f : formats) if (f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_UNDEFINED)) {
            selected = f; if (selected.format == VK_FORMAT_UNDEFINED) selected.format = VK_FORMAT_B8G8R8A8_UNORM;
            if (selected.format == VK_FORMAT_B8G8R8A8_UNORM) break;
        }
        // Preserve the UI's already encoded pixel values: avoid implicit sRGB conversion.
        if (selected.format == VK_FORMAT_UNDEFINED) { error = "MEVK: no UNORM Wayland swapchain format"; return false; }
        VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(context->physical_device, selected.format, &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) { error = "MEVK: swapchain format cannot be blitted"; return false; }
        VkExtent2D extent{wanted_width, wanted_height};
        if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
        else {
            extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (!extent.width || !extent.height) { error = "MEVK: zero swapchain extent"; return false; }
        if (!vk_check(vkGetPhysicalDeviceSurfacePresentModesKHR(context->physical_device, surface, &count, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR", error)) return false;
        std::vector<VkPresentModeKHR> modes(count);
        if (!vk_check(vkGetPhysicalDeviceSurfacePresentModesKHR(context->physical_device, surface, &count, modes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR", error)) return false;
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        for (auto available : modes) if (available == VK_PRESENT_MODE_MAILBOX_KHR) mode = available;
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (auto candidate : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & candidate) { alpha = candidate; break; }
        auto image_count = std::max(3u, caps.minImageCount);
        if (caps.maxImageCount) image_count = std::min(image_count, caps.maxImageCount);
        VkSwapchainCreateInfoKHR info{}; info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface; info.minImageCount = image_count; info.imageFormat = selected.format; info.imageColorSpace = selected.colorSpace;
        info.imageExtent = extent; info.imageArrayLayers = 1; info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; info.preTransform = caps.currentTransform;
        info.compositeAlpha = alpha; info.presentMode = mode; info.clipped = VK_TRUE; info.oldSwapchain = swapchain;
        VkSwapchainKHR next = VK_NULL_HANDLE;
        if (!vk_check(vkCreateSwapchainKHR(context->device, &info, nullptr, &next), "vkCreateSwapchainKHR", error)) return false;
        // Waiting is confined to resize/teardown, never the regular frame path.
        if (swapchain && !vk_check(vkQueueWaitIdle(context->queue), "vkQueueWaitIdle(resize)", error)) { vkDestroySwapchainKHR(context->device, next, nullptr); return false; }
        destroy_swapchain(); swapchain = next; format = selected.format; width = extent.width; height = extent.height;
        if (!vk_check(vkGetSwapchainImagesKHR(context->device, swapchain, &count, nullptr), "vkGetSwapchainImagesKHR", error)) return false;
        std::vector<VkImage> handles(count);
        if (!vk_check(vkGetSwapchainImagesKHR(context->device, swapchain, &count, handles.data()), "vkGetSwapchainImagesKHR", error)) return false;
        images.resize(count);
        VkSemaphoreCreateInfo semaphore{}; semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (unsigned i = 0; i < count; ++i) {
            images[i].image = handles[i];
            if (!vk_check(vkCreateSemaphore(context->device, &semaphore, nullptr, &images[i].ready), "vkCreateSemaphore(present)", error)) return false;
        }
        requested_width = wanted_width; requested_height = wanted_height;
        dirty = false; ++counts.recreations;
        return true;
    }
    ~Impl() {
        if (!context) return;
        { std::lock_guard lock(context->mutex);
          vkQueueWaitIdle(context->queue);
          destroy_swapchain();
          for (auto &frame : frames) {
              if (frame.fence) vkDestroyFence(context->device, frame.fence, nullptr);
              if (frame.acquired) vkDestroySemaphore(context->device, frame.acquired, nullptr);
              if (frame.command) vkFreeCommandBuffers(context->device, context->command_pool, 1, &frame.command);
          }
          if (surface) vkDestroySurfaceKHR(context->instance, surface, nullptr);
        } // Retained textures are released outside the context lock.
    }
};
WaylandPresenter::WaylandPresenter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WaylandPresenter::~WaylandPresenter() = default;
PresentationStats WaylandPresenter::stats() const { return impl_->counts; }
std::unique_ptr<WaylandPresenter> WaylandPresenter::create(const std::shared_ptr<Texture> &texture, void *display, void *surface, std::string &error) {
    error.clear();
#if defined(__linux__)
    if (!texture || !display || !surface || !texture->impl_->context->swapchain) { error = "MEVK: Wayland Vulkan presentation unavailable"; return nullptr; }
    auto state = std::make_unique<Impl>(); state->context = texture->impl_->context;
    std::lock_guard lock(state->context->mutex);
    VkWaylandSurfaceCreateInfoKHR info{}; info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = static_cast<wl_display *>(display); info.surface = static_cast<wl_surface *>(surface);
    if (!vk_check(vkCreateWaylandSurfaceKHR(state->context->instance, &info, nullptr, &state->surface), "vkCreateWaylandSurfaceKHR", error)) return nullptr;
    VkBool32 supported = VK_FALSE;
    if (!vk_check(vkGetPhysicalDeviceSurfaceSupportKHR(state->context->physical_device, state->context->info.queue_family, state->surface, &supported), "vkGetPhysicalDeviceSurfaceSupportKHR", error) || !supported) {
        if (error.empty()) error = "MEVK: graphics queue cannot present to Wayland";
        return nullptr;
    }
    VkCommandBufferAllocateInfo allocation{}; allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = state->context->command_pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
    VkFenceCreateInfo fence{}; fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo semaphore{}; semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto &frame : state->frames)
        if (!vk_check(vkAllocateCommandBuffers(state->context->device, &allocation, &frame.command), "vkAllocateCommandBuffers(present)", error) ||
            !vk_check(vkCreateFence(state->context->device, &fence, nullptr, &frame.fence), "vkCreateFence(present)", error) ||
            !vk_check(vkCreateSemaphore(state->context->device, &semaphore, nullptr, &frame.acquired), "vkCreateSemaphore(acquire)", error)) return nullptr;
    return std::unique_ptr<WaylandPresenter>(new WaylandPresenter(std::move(state)));
#else
    (void)texture; (void)display; (void)surface;
    error = "MEVK: Wayland presentation is available on Linux"; return nullptr;
#endif
}
bool WaylandPresenter::present(const std::shared_ptr<Texture> &texture, unsigned width, unsigned height,
    double logical_width, double logical_height, double scale, unsigned placement, std::string &error) {
    error.clear();
    if (!texture || texture->impl_->context != impl_->context || !width || !height) { error = "MEVK: incompatible presentation texture or extent"; return false; }
    // Blitting an sRGB source to UNORM would decode the already encoded UI
    // bytes. Keep such formats on the CPU path until a byte-preserving view exists.
    if (texture->pixel_format() != 70 && texture->pixel_format() != 80) {
        error = "MEVK: direct presentation requires RGBA8/BGRA8 UNORM"; return false;
    }
    VkFormatProperties source_properties{};
    vkGetPhysicalDeviceFormatProperties(impl_->context->physical_device, texture->impl_->format, &source_properties);
    if (!(source_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        error = "MEVK: drawable format cannot be blitted"; return false;
    }
    // Move the old reference out before taking the Vulkan lock: its destructor
    // may free a texture from an earlier drawable size.
    std::shared_ptr<Texture> retired;
    std::unique_lock lock(impl_->context->mutex);
    if (impl_->dirty || impl_->requested_width != width || impl_->requested_height != height)
        if (!impl_->resize(width, height, error)) return false;
    Impl::Frame *slot = nullptr;
    for (auto &frame : impl_->frames) {
        const auto status = vkGetFenceStatus(impl_->context->device, frame.fence);
        if (status == VK_SUCCESS) { slot = &frame; break; }
        if (status != VK_NOT_READY) { vk_check(status, "vkGetFenceStatus(present)", error); return false; }
    }
    if (!slot) { ++impl_->counts.skipped; return true; }
    std::uint32_t index = 0;
    auto result = vkAcquireNextImageKHR(impl_->context->device, impl_->swapchain, 0, slot->acquired, VK_NULL_HANDLE, &index);
    if (result == VK_NOT_READY || result == VK_TIMEOUT) { ++impl_->counts.skipped; return true; }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { impl_->dirty = true; ++impl_->counts.skipped; return true; }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) { vk_check(result, "vkAcquireNextImageKHR", error); return false; }
    impl_->dirty = result == VK_SUBOPTIMAL_KHR;
    retired = std::move(slot->source);
    auto &image = impl_->images[index];
    if (!vk_check(vkResetCommandBuffer(slot->command, 0), "vkResetCommandBuffer(present)", error)) return false;
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vk_check(vkBeginCommandBuffer(slot->command, &begin), "vkBeginCommandBuffer(present)", error)) return false;
    VkMemoryBarrier source{}; source.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    source.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &source, 0, nullptr, 0, nullptr);
    VkImageMemoryBarrier destination{}; destination.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    destination.oldLayout = image.initialized ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; destination.image = image.image;
    destination.srcQueueFamilyIndex = destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destination.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; destination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &destination);
    VkClearColorValue clear{};
    vkCmdClearColorImage(slot->command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &destination.subresourceRange);
    // Order the clear before the overlapping blit.
    VkMemoryBarrier clear_barrier{};clear_barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    clear_barrier.srcAccessMask=clear_barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(slot->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&clear_barrier,0,nullptr,0,nullptr);
    const auto region = presentation_region(texture->width(), texture->height(), impl_->width, impl_->height, logical_width, logical_height, scale, placement);
    if (!region.empty) {
        VkImageBlit blit{}; blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {region.sx0, region.sy0, 0}; blit.srcOffsets[1] = {region.sx1, region.sy1, 1};
        blit.dstOffsets[0] = {region.dx0, region.dy0, 0}; blit.dstOffsets[1] = {region.dx1, region.dy1, 1};
        vkCmdBlitImage(slot->command, texture->impl_->image, VK_IMAGE_LAYOUT_GENERAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
    destination.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; destination.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    destination.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; destination.dstAccessMask = 0;
    vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &destination);
    if (!vk_check(vkEndCommandBuffer(slot->command), "vkEndCommandBuffer(present)", error) ||
        !vk_check(vkResetFences(impl_->context->device, 1, &slot->fence), "vkResetFences(present)", error)) return false;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &slot->acquired; submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &slot->command;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &image.ready;
    slot->source = texture;
    if (!vk_check(vkQueueSubmit(impl_->context->queue, 1, &submit, slot->fence), "vkQueueSubmit(present)", error)) return false;
    image.initialized = true;
    VkPresentInfoKHR present{}; present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1; present.pWaitSemaphores = &image.ready;
    present.swapchainCount = 1; present.pSwapchains = &impl_->swapchain; present.pImageIndices = &index;
    result = vkQueuePresentKHR(impl_->context->queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) impl_->dirty = true;
    else if (!vk_check(result, "vkQueuePresentKHR", error)) return false;
    ++impl_->counts.presented;
    return true;
}
} // namespace mevk
