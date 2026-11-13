#include "internal.h"

#include <utility>
#include <vector>

namespace mevk {
std::uint64_t Context::allocated_size()const{std::lock_guard lock(impl_->mutex);return impl_->allocated_bytes;}
std::uint64_t Context::recommended_working_set_size()const {
    std::uint64_t total=0;
    for(unsigned i=0;i<impl_->memory_properties.memoryHeapCount;++i)
        if(impl_->memory_properties.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)total+=impl_->memory_properties.memoryHeaps[i].size;
    return total/4*3;
}
namespace {

int device_score(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
    default: return 0;
    }
}

} // namespace

Context::Impl::~Impl() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        for (auto allocation : buffer_cache) {
            vkUnmapMemory(device, allocation.memory);
            vkDestroyBuffer(device, allocation.buffer, nullptr);
            vkFreeMemory(device, allocation.memory, nullptr);
        }
        for (auto allocation : command_cache) {
            if (allocation.descriptors) vkDestroyDescriptorPool(device, allocation.descriptors, nullptr);
            vkDestroyFence(device, allocation.fence, nullptr);
        }
        if(sampler!=VK_NULL_HANDLE)vkDestroySampler(device,sampler,nullptr);
        if (command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device, command_pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (instance != VK_NULL_HANDLE)
        vkDestroyInstance(instance, nullptr);
}

Context::Context(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Context::~Context() = default;

std::unique_ptr<Context> Context::create(std::string& error) {
    error.clear();
    auto impl = std::make_shared<Impl>();

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MEVK";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "MEVK";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    // Start with Vulkan 1.0; negotiate later features when implemented.
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    std::vector<const char *> instance_extensions;
#if defined(__linux__)
    std::uint32_t extension_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extensions.data());
    bool surface = false, wayland = false;
    for (const auto &extension : available_extensions) {
        surface |= std::string(extension.extensionName) == "VK_KHR_surface";
        wayland |= std::string(extension.extensionName) == "VK_KHR_wayland_surface";
    }
    if (surface && wayland) {
        instance_extensions = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
        impl->wayland_surface = true;
    }
#endif
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(instance_extensions.size());
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    if (!vk_check(vkCreateInstance(&instance_info, nullptr, &impl->instance),
               "vkCreateInstance", error)) return nullptr;

    std::vector<VkPhysicalDevice> devices;
    VkResult result;
    do {
        std::uint32_t count = 0;
        if (!vk_check(vkEnumeratePhysicalDevices(impl->instance, &count, nullptr),
                   "vkEnumeratePhysicalDevices", error)) return nullptr;
        if (count == 0) {
            error = "MEVK: no Vulkan physical devices found";
            return nullptr;
        }
        devices.resize(count);
        result = vkEnumeratePhysicalDevices(impl->instance, &count, devices.data());
        devices.resize(count);
    } while (result == VK_INCOMPLETE);
    if (!vk_check(result, "vkEnumeratePhysicalDevices", error)) return nullptr;

    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        const int score = device_score(properties.deviceType);
        for (std::uint32_t i = 0; i < count; ++i) {
            constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if (families[i].queueCount == 0 ||
                (families[i].queueFlags & required) != required) continue;
            if (score > best_score) {
                best_score = score;
                impl->physical_device = candidate;
                impl->info = {properties.deviceName, properties.apiVersion, i};
            }
            break;
        }
    }
    if (impl->physical_device == VK_NULL_HANDLE) {
        error = "MEVK: no Vulkan device with a graphics and compute queue found";
        return nullptr;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = impl->info.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkPhysicalDeviceFeatures supported{},enabled{};
    vkGetPhysicalDeviceFeatures(impl->physical_device,&supported);
    enabled.samplerAnisotropy=supported.samplerAnisotropy;
    enabled.textureCompressionBC=supported.textureCompressionBC;
    impl->texture_compression_bc=enabled.textureCompressionBC!=0;
    enabled.depthClamp=supported.depthClamp;enabled.depthBiasClamp=supported.depthBiasClamp;
    enabled.fillModeNonSolid=supported.fillModeNonSolid;enabled.robustBufferAccess=supported.robustBufferAccess;
    impl->depth_clamp=enabled.depthClamp!=0;impl->depth_bias_clamp=enabled.depthBiasClamp!=0;impl->wireframe=enabled.fillModeNonSolid!=0;
    enabled.shaderStorageImageWriteWithoutFormat=supported.shaderStorageImageWriteWithoutFormat;
    enabled.shaderStorageImageExtendedFormats=supported.shaderStorageImageExtendedFormats;
    impl->storage_write_without_format=enabled.shaderStorageImageWriteWithoutFormat!=0;
    impl->sampler_anisotropy=enabled.samplerAnisotropy!=0;
    device_info.pEnabledFeatures=&enabled;
    const char *swapchain_extension = "VK_KHR_swapchain";
    std::uint32_t device_extension_count = 0;
    vkEnumerateDeviceExtensionProperties(impl->physical_device, nullptr, &device_extension_count, nullptr);
    std::vector<VkExtensionProperties> device_extensions(device_extension_count);
    vkEnumerateDeviceExtensionProperties(impl->physical_device, nullptr, &device_extension_count, device_extensions.data());
    for (const auto &extension : device_extensions)
        impl->swapchain |= impl->wayland_surface && std::string(extension.extensionName) == swapchain_extension;
    if (impl->swapchain) {
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = &swapchain_extension;
    }
    if (!vk_check(vkCreateDevice(impl->physical_device, &device_info, nullptr, &impl->device),
               "vkCreateDevice", error)) return nullptr;
    vkGetDeviceQueue(impl->device, impl->info.queue_family, 0, &impl->queue);
    vkGetPhysicalDeviceMemoryProperties(impl->physical_device, &impl->memory_properties);
    vkGetPhysicalDeviceProperties(impl->physical_device,&impl->properties);
    VkSamplerCreateInfo sampler{};
    sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter=VK_FILTER_LINEAR;sampler.minFilter=VK_FILTER_LINEAR;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(!vk_check(vkCreateSampler(impl->device,&sampler,nullptr,&impl->sampler),"vkCreateSampler",error))return nullptr;

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = impl->info.queue_family;
    if (!vk_check(vkCreateCommandPool(impl->device, &pool_info, nullptr, &impl->command_pool),
               "vkCreateCommandPool", error)) return nullptr;

    return std::unique_ptr<Context>(new Context(std::move(impl)));
}

const DeviceInfo& Context::device_info() const {
    return impl_->info;
}
AllocationStats Context::allocation_stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->allocations;
}

bool Context::wait_idle(std::string& error) {
    error.clear();
    std::lock_guard lock(impl_->mutex);
    return vk_check(vkDeviceWaitIdle(impl_->device), "vkDeviceWaitIdle", error);
}

} // namespace mevk
