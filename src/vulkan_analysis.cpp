#include "vulkan_analysis.h"

#include "vulkan_devices.h"
#include "vulkan_shaders.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef LDCOMPRESS_HAVE_VULKAN
#define LDCOMPRESS_HAVE_VULKAN 0
#endif

#if LDCOMPRESS_HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace ldcompress::vulkan_detail {
namespace {

using opencl_detail::FlacClSubframeTask;
using opencl_detail::OpenClMonoAnalysisTaskPlan;
using opencl_detail::OpenClMonoFixedConstantAnalysisResult;

constexpr std::uint64_t kFenceTimeoutNs = 10'000'000'000ULL;
constexpr std::size_t kVulkanAnalysisMaxBlockSize = 8192;
constexpr std::size_t kVulkanAnalysisBitsPerSample = 16;
constexpr unsigned kVulkanExactMaxRicePartitionOrder = 8;
constexpr std::uint32_t kWorkGroupSize = 64;

[[maybe_unused]] std::uint64_t checked_buffer_bytes(
    std::size_t count,
    std::size_t element_size,
    const char* name)
{
    if (count == 0) {
        throw std::runtime_error(std::string("Vulkan analysis ") + name + " buffer is empty");
    }
    if (count > std::numeric_limits<std::uint64_t>::max() / element_size) {
        throw std::runtime_error(std::string("Vulkan analysis ") + name + " buffer is too large");
    }
    return static_cast<std::uint64_t>(count) * element_size;
}

[[maybe_unused]] std::uint32_t checked_u32(std::size_t value, const char* name)
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string("Vulkan analysis ") + name + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

void validate_best_method_plan(const OpenClMonoAnalysisTaskPlan& plan)
{
    if (plan.residual_tasks_per_frame == 0 || plan.estimate_tasks_per_frame == 0) {
        throw std::runtime_error("Vulkan mono analysis plan has no tasks per frame");
    }
    if (plan.residual_tasks.empty() || plan.selected_tasks.empty()) {
        throw std::runtime_error("Vulkan mono analysis plan has no task data");
    }
    if (plan.estimate_tasks_per_frame != plan.residual_tasks_per_frame) {
        throw std::runtime_error("Vulkan mono analysis currently supports full mono selection only");
    }
    if ((plan.selected_tasks.size() % plan.estimate_tasks_per_frame) != 0 ||
        (plan.residual_tasks.size() % plan.residual_tasks_per_frame) != 0) {
        throw std::runtime_error("Vulkan mono analysis task plan is not frame-aligned");
    }
    if (plan.selected_tasks.size() / plan.estimate_tasks_per_frame !=
        plan.residual_tasks.size() / plan.residual_tasks_per_frame) {
        throw std::runtime_error("Vulkan mono analysis selected/residual frame counts differ");
    }

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    for (const auto selected : plan.selected_tasks) {
        if (selected < 0 || static_cast<std::size_t>(selected) >= plan.residual_tasks.size()) {
            throw std::runtime_error("Vulkan mono analysis selected task index is out of range");
        }
    }

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto& first_task = plan.residual_tasks.at(task_base);
        for (std::size_t i = 1; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks.at(task_base + i);
            if (task.data.samplesOffs != first_task.data.samplesOffs ||
                task.data.blocksize != first_task.data.blocksize ||
                task.data.obits != first_task.data.obits) {
                throw std::runtime_error("Vulkan mono analysis frame task group mixes sample ranges");
            }
        }

        const auto selected_base = frame * plan.estimate_tasks_per_frame;
        for (std::size_t i = 0; i < plan.estimate_tasks_per_frame; ++i) {
            const auto selected = static_cast<std::size_t>(
                plan.selected_tasks.at(selected_base + i));
            if (selected < task_base || selected >= task_base + plan.residual_tasks_per_frame) {
                throw std::runtime_error("Vulkan mono analysis selected task crosses frame group");
            }
        }
    }
}

void validate_sample_range(std::int32_t sample, unsigned bits_per_sample)
{
    const auto min_value = -(std::int64_t {1} << (bits_per_sample - 1U));
    const auto max_value = (std::int64_t {1} << (bits_per_sample - 1U)) - 1;
    if (sample < min_value || sample > max_value) {
        throw std::runtime_error("Vulkan exact analysis sample is outside the selected bit depth");
    }
}

bool signed_value_fits_bits(std::int32_t value, unsigned bits)
{
    if (bits == 0 || bits > 31) {
        return false;
    }
    const auto min_value = -(std::int64_t {1} << (bits - 1U));
    const auto max_value = (std::int64_t {1} << (bits - 1U)) - 1;
    return value >= min_value && value <= max_value;
}

void validate_vulkan_exact_analysis_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order,
    bool allow_lpc)
{
    validate_best_method_plan(plan);
    if (max_rice_partition_order > kVulkanExactMaxRicePartitionOrder) {
        throw std::runtime_error("Vulkan fixed/constant analysis max Rice partition order must be 0..8");
    }

    for (const auto sample : samples) {
        validate_sample_range(sample, kVulkanAnalysisBitsPerSample);
    }

    for (const auto& task : plan.residual_tasks) {
        if (task.data.type != opencl_detail::kFlacClSubframeConstant &&
            task.data.type != opencl_detail::kFlacClSubframeFixed &&
            (!allow_lpc ||
                (task.data.type != opencl_detail::kFlacClSubframeLpc &&
                 task.data.type != opencl_detail::kFlacClSubframeVerbatim))) {
            throw std::runtime_error("Vulkan mono analysis received unsupported task type");
        }
        if (task.data.type == opencl_detail::kFlacClSubframeFixed &&
            (task.data.residualOrder < 0 || task.data.residualOrder > 4)) {
            throw std::runtime_error("Vulkan mono analysis received invalid fixed order");
        }
        if (task.data.type == opencl_detail::kFlacClSubframeLpc &&
            (task.data.residualOrder <= 0 ||
                task.data.residualOrder > static_cast<std::int32_t>(opencl_detail::kFlacClMaxOrder))) {
            throw std::runtime_error("Vulkan mono analysis received invalid LPC order");
        }
        if (task.data.obits != static_cast<std::int32_t>(kVulkanAnalysisBitsPerSample)) {
            throw std::runtime_error("Vulkan mono analysis currently supports 16-bit tasks only");
        }
        if (task.data.blocksize <= 0 ||
            static_cast<std::size_t>(task.data.blocksize) > kVulkanAnalysisMaxBlockSize) {
            throw std::runtime_error("Vulkan mono analysis block size is unsupported");
        }
        if ((task.data.type == opencl_detail::kFlacClSubframeFixed ||
                task.data.type == opencl_detail::kFlacClSubframeLpc) &&
            task.data.residualOrder >= task.data.blocksize) {
            throw std::runtime_error("Vulkan mono analysis predictor order exceeds block size");
        }
        if (task.data.samplesOffs < 0 ||
            static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
            static_cast<std::size_t>(task.data.blocksize) >
                samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
            throw std::runtime_error("Vulkan mono analysis task samples are out of range");
        }
        if (task.data.type == opencl_detail::kFlacClSubframeLpc) {
            if (task.data.shift < 0 || task.data.shift > 15) {
                throw std::runtime_error("Vulkan mono analysis LPC task shift is unsupported");
            }
            if (task.data.cbits <= 0 || task.data.cbits > 15) {
                throw std::runtime_error("Vulkan mono analysis LPC coefficient precision is unsupported");
            }
            for (int i = 0; i < task.data.residualOrder; ++i) {
                if (!signed_value_fits_bits(
                        task.coefs[static_cast<std::size_t>(i)],
                        static_cast<unsigned>(task.data.cbits))) {
                    throw std::runtime_error("Vulkan mono analysis LPC coefficient does not fit precision");
                }
            }
        } else if (task.data.shift < 0 || task.data.shift > 30) {
            throw std::runtime_error("Vulkan mono analysis task shift is unsupported");
        }
    }
}

#if LDCOMPRESS_HAVE_VULKAN

std::string vk_result_name(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    default:
        return "Vulkan error " + std::to_string(static_cast<int>(result));
    }
}

void require_vk(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + vk_result_name(result));
    }
}

template <typename Fn>
class ScopeExit final {
public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit()
    {
        if (active_) {
            fn_();
        }
    }

private:
    Fn fn_;
    bool active_ = true;
};

template <typename Fn>
ScopeExit<Fn> scope_exit(Fn fn)
{
    return ScopeExit<Fn>(std::move(fn));
}

VkInstance create_instance()
{
    const VkApplicationInfo app_info {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "ld-compress-ng",
        .applicationVersion = VK_MAKE_VERSION(1, 1, 0),
        .pEngineName = "ld-compress-ng",
        .engineVersion = VK_MAKE_VERSION(1, 1, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo create_info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
    };

    VkInstance instance = VK_NULL_HANDLE;
    require_vk(vkCreateInstance(&create_info, nullptr, &instance), "vkCreateInstance");
    return instance;
}

std::vector<VkPhysicalDevice> enumerate_physical_devices(VkInstance instance)
{
    std::uint32_t count = 0;
    require_vk(vkEnumeratePhysicalDevices(instance, &count, nullptr),
        "vkEnumeratePhysicalDevices");
    if (count == 0) {
        return {};
    }

    std::vector<VkPhysicalDevice> devices(count);
    require_vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
        "vkEnumeratePhysicalDevices");
    devices.resize(count);
    return devices;
}

std::optional<std::uint32_t> compute_queue_family(VkPhysicalDevice device)
{
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    if (count == 0) {
        return std::nullopt;
    }

    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
            families[i].queueCount > 0) {
            return i;
        }
    }
    return std::nullopt;
}

bool shader_int64_supported(VkPhysicalDevice device)
{
    VkPhysicalDeviceFeatures features {};
    vkGetPhysicalDeviceFeatures(device, &features);
    return features.shaderInt64 == VK_TRUE;
}

struct SelectedDevice {
    std::uint32_t index = 0;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    std::uint32_t queue_family_index = 0;
    std::string name;
};

SelectedDevice select_physical_device(
    VkInstance instance,
    std::optional<std::size_t> requested_device_index)
{
    const auto devices = enumerate_physical_devices(instance);
    if (devices.empty()) {
        throw std::runtime_error("no Vulkan devices found");
    }

    auto make_selection = [&](std::size_t index) {
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(devices[index], &properties);
        const auto queue_family = compute_queue_family(devices[index]);
        if (!queue_family.has_value()) {
            throw std::runtime_error("selected Vulkan device has no compute queue: " +
                std::string(properties.deviceName));
        }
        if (!shader_int64_supported(devices[index])) {
            throw std::runtime_error("selected Vulkan device does not support shaderInt64: " +
                std::string(properties.deviceName));
        }
        return SelectedDevice {
            .index = static_cast<std::uint32_t>(index),
            .physical_device = devices[index],
            .queue_family_index = *queue_family,
            .name = properties.deviceName,
        };
    };

    if (requested_device_index.has_value()) {
        if (*requested_device_index >= devices.size()) {
            throw std::runtime_error("Vulkan device index out of range: " +
                std::to_string(*requested_device_index));
        }
        return make_selection(*requested_device_index);
    }

    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (compute_queue_family(devices[i]).has_value() && shader_int64_supported(devices[i])) {
            return make_selection(i);
        }
    }
    throw std::runtime_error("no available Vulkan compute devices with shaderInt64 found");
}

std::uint32_t find_memory_type(
    VkPhysicalDevice physical_device,
    std::uint32_t type_bits,
    VkMemoryPropertyFlags required_flags,
    VkMemoryPropertyFlags preferred_flags)
{
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    std::optional<std::uint32_t> fallback;
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((type_bits & (1U << i)) == 0) {
            continue;
        }
        const auto flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((flags & required_flags) != required_flags) {
            continue;
        }
        if ((flags & preferred_flags) == preferred_flags) {
            return i;
        }
        if (!fallback.has_value()) {
            fallback = i;
        }
    }

    if (fallback.has_value()) {
        return *fallback;
    }
    throw std::runtime_error("no compatible host-visible Vulkan memory type found");
}

bool memory_type_is_coherent(VkPhysicalDevice physical_device, std::uint32_t memory_type_index)
{
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    return (memory_properties.memoryTypes[memory_type_index].propertyFlags &
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

class HostBuffer final {
public:
    HostBuffer(
        VkDevice device,
        VkPhysicalDevice physical_device,
        VkDeviceSize size,
        VkBufferUsageFlags usage)
        : device_(device), size_(size)
    {
        const VkBufferCreateInfo buffer_info {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        require_vk(vkCreateBuffer(device_, &buffer_info, nullptr, &buffer_), "vkCreateBuffer");

        VkMemoryRequirements memory_requirements {};
        vkGetBufferMemoryRequirements(device_, buffer_, &memory_requirements);
        const auto memory_type_index = find_memory_type(
            physical_device,
            memory_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        coherent_ = memory_type_is_coherent(physical_device, memory_type_index);

        const VkMemoryAllocateInfo allocate_info {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = memory_requirements.size,
            .memoryTypeIndex = memory_type_index,
        };
        require_vk(vkAllocateMemory(device_, &allocate_info, nullptr, &memory_),
            "vkAllocateMemory");
        require_vk(vkBindBufferMemory(device_, buffer_, memory_, 0), "vkBindBufferMemory");
        require_vk(vkMapMemory(device_, memory_, 0, memory_requirements.size, 0, &mapped_),
            "vkMapMemory");
    }

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    ~HostBuffer()
    {
        if (mapped_ != nullptr) {
            vkUnmapMemory(device_, memory_);
        }
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer_, nullptr);
        }
        if (memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory_, nullptr);
        }
    }

    VkBuffer get() const { return buffer_; }
    VkDeviceSize size() const { return size_; }

    template <typename T>
    void copy_from(const std::vector<T>& values)
    {
        const auto bytes = checked_buffer_bytes(values.size(), sizeof(T), "host copy");
        if (bytes > size_) {
            throw std::runtime_error("Vulkan analysis host copy exceeds buffer size");
        }
        std::memcpy(mapped_, values.data(), static_cast<std::size_t>(bytes));
    }

    template <typename T>
    void copy_to(std::vector<T>& values)
    {
        const auto bytes = checked_buffer_bytes(values.size(), sizeof(T), "host read");
        if (bytes > size_) {
            throw std::runtime_error("Vulkan analysis host read exceeds buffer size");
        }
        std::memcpy(values.data(), mapped_, static_cast<std::size_t>(bytes));
    }

    void flush() const
    {
        if (coherent_) {
            return;
        }
        const VkMappedMemoryRange range {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .pNext = nullptr,
            .memory = memory_,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        require_vk(vkFlushMappedMemoryRanges(device_, 1, &range),
            "vkFlushMappedMemoryRanges");
    }

    void invalidate() const
    {
        if (coherent_) {
            return;
        }
        const VkMappedMemoryRange range {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .pNext = nullptr,
            .memory = memory_,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        require_vk(vkInvalidateMappedMemoryRanges(device_, 1, &range),
            "vkInvalidateMappedMemoryRanges");
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize size_ = 0;
    bool coherent_ = false;
};

struct PushConstants {
    std::uint32_t mode = 0;
    std::uint32_t task_count = 0;
    std::uint32_t frame_count = 0;
    std::uint32_t selected_tasks_per_frame = 0;
    std::uint32_t residual_tasks_per_frame = 0;
    std::uint32_t max_rice_partition_order = 0;
};

std::uint32_t dispatch_groups(std::uint32_t items)
{
    return (items + kWorkGroupSize - 1U) / kWorkGroupSize;
}

#endif

}  // namespace

OpenClMonoFixedConstantAnalysisResult run_vulkan_mono_exact_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order,
    bool allow_lpc)
{
    validate_vulkan_exact_analysis_inputs(samples, plan, max_rice_partition_order, allow_lpc);

#if LDCOMPRESS_HAVE_VULKAN
    if (!vulkan_support_built()) {
        throw std::runtime_error("Vulkan support was not built");
    }

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    const auto task_count_u32 = checked_u32(plan.residual_tasks.size(), "task count");
    const PushConstants base_push {
        .mode = 0,
        .task_count = task_count_u32,
        .frame_count = checked_u32(frame_count, "frame count"),
        .selected_tasks_per_frame = checked_u32(
            plan.estimate_tasks_per_frame, "selected task count"),
        .residual_tasks_per_frame = checked_u32(
            plan.residual_tasks_per_frame, "residual task count"),
        .max_rice_partition_order = checked_u32(
            max_rice_partition_order, "max Rice partition order"),
    };

    const VkInstance instance = create_instance();
    const auto destroy_instance = scope_exit([&] {
        vkDestroyInstance(instance, nullptr);
    });

    const auto selected = select_physical_device(instance, requested_device_index);
    const float queue_priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_create_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = selected.queue_family_index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    VkPhysicalDeviceFeatures enabled_features {};
    enabled_features.shaderInt64 = VK_TRUE;
    const VkDeviceCreateInfo device_create_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
        .pEnabledFeatures = &enabled_features,
    };

    VkDevice device = VK_NULL_HANDLE;
    require_vk(vkCreateDevice(selected.physical_device, &device_create_info, nullptr, &device),
        "vkCreateDevice");
    const auto destroy_device = scope_exit([&] {
        vkDestroyDevice(device, nullptr);
    });

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, selected.queue_family_index, 0, &queue);

    const auto shader = fixed_constant_shader_spirv();
    if (shader.data == nullptr || shader.size_bytes == 0 ||
        (shader.size_bytes % sizeof(std::uint32_t)) != 0) {
        throw std::runtime_error("embedded Vulkan fixed/constant shader is invalid");
    }
    const VkShaderModuleCreateInfo shader_module_info {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = shader.size_bytes,
        .pCode = shader.data,
    };
    VkShaderModule shader_module = VK_NULL_HANDLE;
    require_vk(vkCreateShaderModule(device, &shader_module_info, nullptr, &shader_module),
        "vkCreateShaderModule");
    const auto destroy_shader_module = scope_exit([&] {
        vkDestroyShaderModule(device, shader_module, nullptr);
    });

    const std::array<VkDescriptorSetLayoutBinding, 4> layout_bindings {{
        VkDescriptorSetLayoutBinding {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding {
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding {
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
    }};
    const VkDescriptorSetLayoutCreateInfo descriptor_layout_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = static_cast<std::uint32_t>(layout_bindings.size()),
        .pBindings = layout_bindings.data(),
    };
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    require_vk(vkCreateDescriptorSetLayout(
                   device, &descriptor_layout_info, nullptr, &descriptor_set_layout),
        "vkCreateDescriptorSetLayout");
    const auto destroy_descriptor_set_layout = scope_exit([&] {
        vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
    });

    const VkPushConstantRange push_constant_range {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    const VkPipelineLayoutCreateInfo pipeline_layout_info {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    require_vk(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout),
        "vkCreatePipelineLayout");
    const auto destroy_pipeline_layout = scope_exit([&] {
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    });

    const VkPipelineShaderStageCreateInfo shader_stage_info {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shader_module,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };
    const VkComputePipelineCreateInfo pipeline_info {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = shader_stage_info,
        .layout = pipeline_layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    require_vk(vkCreateComputePipelines(
                   device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline),
        "vkCreateComputePipelines");
    const auto destroy_pipeline = scope_exit([&] {
        vkDestroyPipeline(device, pipeline, nullptr);
    });

    std::vector<FlacClSubframeTask> analyzed_tasks = plan.residual_tasks;
    std::vector<FlacClSubframeTask> best_tasks(frame_count);

    HostBuffer samples_buffer(
        device,
        selected.physical_device,
        checked_buffer_bytes(samples.size(), sizeof(std::int32_t), "samples"),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    HostBuffer tasks_buffer(
        device,
        selected.physical_device,
        checked_buffer_bytes(analyzed_tasks.size(), sizeof(FlacClSubframeTask), "tasks"),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    HostBuffer selected_buffer(
        device,
        selected.physical_device,
        checked_buffer_bytes(plan.selected_tasks.size(), sizeof(std::int32_t), "selected tasks"),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    HostBuffer best_buffer(
        device,
        selected.physical_device,
        checked_buffer_bytes(best_tasks.size(), sizeof(FlacClSubframeTask), "best tasks"),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    samples_buffer.copy_from(samples);
    tasks_buffer.copy_from(analyzed_tasks);
    selected_buffer.copy_from(plan.selected_tasks);
    samples_buffer.flush();
    tasks_buffer.flush();
    selected_buffer.flush();

    const VkDescriptorPoolSize pool_size {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 4,
    };
    const VkDescriptorPoolCreateInfo descriptor_pool_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    require_vk(vkCreateDescriptorPool(
                   device, &descriptor_pool_info, nullptr, &descriptor_pool),
        "vkCreateDescriptorPool");
    const auto destroy_descriptor_pool = scope_exit([&] {
        vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    });

    const VkDescriptorSetAllocateInfo descriptor_allocate_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout,
    };
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    require_vk(vkAllocateDescriptorSets(device, &descriptor_allocate_info, &descriptor_set),
        "vkAllocateDescriptorSets");

    const std::array<VkDescriptorBufferInfo, 4> descriptor_buffers {{
        VkDescriptorBufferInfo {
            .buffer = samples_buffer.get(),
            .offset = 0,
            .range = samples_buffer.size(),
        },
        VkDescriptorBufferInfo {
            .buffer = tasks_buffer.get(),
            .offset = 0,
            .range = tasks_buffer.size(),
        },
        VkDescriptorBufferInfo {
            .buffer = selected_buffer.get(),
            .offset = 0,
            .range = selected_buffer.size(),
        },
        VkDescriptorBufferInfo {
            .buffer = best_buffer.get(),
            .offset = 0,
            .range = best_buffer.size(),
        },
    }};
    std::array<VkWriteDescriptorSet, 4> descriptor_writes {};
    for (std::uint32_t i = 0; i < descriptor_writes.size(); ++i) {
        descriptor_writes[i] = VkWriteDescriptorSet {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = descriptor_set,
            .dstBinding = i,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo = nullptr,
            .pBufferInfo = &descriptor_buffers[i],
            .pTexelBufferView = nullptr,
        };
    }
    vkUpdateDescriptorSets(
        device,
        static_cast<std::uint32_t>(descriptor_writes.size()),
        descriptor_writes.data(),
        0,
        nullptr);

    const VkCommandPoolCreateInfo command_pool_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = selected.queue_family_index,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    require_vk(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool),
        "vkCreateCommandPool");
    const auto destroy_command_pool = scope_exit([&] {
        vkDestroyCommandPool(device, command_pool, nullptr);
    });

    const VkCommandBufferAllocateInfo command_buffer_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    require_vk(vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer),
        "vkAllocateCommandBuffers");

    const VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    require_vk(vkBeginCommandBuffer(command_buffer, &begin_info), "vkBeginCommandBuffer");
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline_layout,
        0,
        1,
        &descriptor_set,
        0,
        nullptr);

    auto push = base_push;
    push.mode = 0;
    vkCmdPushConstants(
        command_buffer,
        pipeline_layout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    vkCmdDispatch(command_buffer, dispatch_groups(push.task_count), 1, 1);

    const VkMemoryBarrier analysis_barrier {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };
    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1,
        &analysis_barrier,
        0,
        nullptr,
        0,
        nullptr);

    push.mode = 1;
    vkCmdPushConstants(
        command_buffer,
        pipeline_layout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    vkCmdDispatch(command_buffer, dispatch_groups(push.frame_count), 1, 1);

    const VkMemoryBarrier host_barrier {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        1,
        &host_barrier,
        0,
        nullptr,
        0,
        nullptr);

    require_vk(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

    const VkFenceCreateInfo fence_info {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    VkFence fence = VK_NULL_HANDLE;
    require_vk(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");
    const auto destroy_fence = scope_exit([&] {
        vkDestroyFence(device, fence, nullptr);
    });

    const VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    require_vk(vkQueueSubmit(queue, 1, &submit_info, fence), "vkQueueSubmit");
    require_vk(vkWaitForFences(device, 1, &fence, VK_TRUE, kFenceTimeoutNs),
        "vkWaitForFences");

    tasks_buffer.invalidate();
    best_buffer.invalidate();
    tasks_buffer.copy_to(analyzed_tasks);
    best_buffer.copy_to(best_tasks);

    return OpenClMonoFixedConstantAnalysisResult {
        .analyzed_tasks = std::move(analyzed_tasks),
        .best_tasks = std::move(best_tasks),
        .device_name = selected.name,
    };
#else
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)max_rice_partition_order;
    throw std::runtime_error("Vulkan support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult run_vulkan_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    return run_vulkan_mono_exact_analysis(
        samples, plan, requested_device_index, max_rice_partition_order, false);
}

OpenClMonoFixedConstantAnalysisResult run_vulkan_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    return run_vulkan_mono_exact_analysis(
        samples, plan, requested_device_index, max_rice_partition_order, true);
}

}  // namespace ldcompress::vulkan_detail
