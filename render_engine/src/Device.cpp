#include <render_engine/Device.h>

#include <volk.h>

#include <render_engine/GpuResourceManager.h>
#include <render_engine/RenderContext.h>
#include <render_engine/RenderEngine.h>
#include <render_engine/resources/Texture.h>
#include <render_engine/TransferEngine.h>
#include <render_engine/window/OffScreenWindow.h>
#include <render_engine/window/Window.h>

#include <GLFW/glfw3native.h>

#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#include <vulkan/vulkan_win32.h>

#include <render_engine/cuda_compute/CudaDevice.h>

namespace
{
    // TODO support multiple queue count
    constexpr uint32_t k_supported_queue_count = 1;
    constexpr uint32_t k_num_of_cuda_streams = 8;

    VkDevice createVulkanLogicalDevice(uint32_t queue_count,
                                       VkPhysicalDevice physical_device,
                                       uint32_t queue_family_index_graphics,
                                       uint32_t queue_family_index_presentation,
                                       uint32_t queue_family_index_transfer,
                                       const std::vector<const char*>& device_extensions,
                                       const std::vector<const char*>& validation_layers)
    {

        std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
        std::set<uint32_t> unique_queue_families = { queue_family_index_graphics, queue_family_index_presentation, queue_family_index_transfer };

        std::vector<float> queuePriority(queue_count, 1.0f);
        for (uint32_t queueFamily : unique_queue_families)
        {
            VkDeviceQueueCreateInfo queue_create_info{};
            queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_create_info.queueFamilyIndex = queueFamily;
            queue_create_info.queueCount = queue_count;
            queue_create_info.pQueuePriorities = queuePriority.data();
            queue_create_infos.push_back(queue_create_info);
        }

        {


            VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore_feature{};
            timeline_semaphore_feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;

            VkPhysicalDeviceSynchronization2Features synchronization_2_feature{};
            synchronization_2_feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
            synchronization_2_feature.pNext = &timeline_semaphore_feature;

            VkPhysicalDeviceFeatures2KHR features =
            {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
                .pNext = &synchronization_2_feature,
            };

            vkGetPhysicalDeviceFeatures2KHR(physical_device, &features);
            if (synchronization_2_feature.synchronization2 == false)
            {
                throw std::runtime_error("synchronization2 feature is not supported");
            }
            if (timeline_semaphore_feature.timelineSemaphore == false)
            {
                throw std::runtime_error("timeline semaphores feature is not supported");
            }
        }
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore_feature{};
        timeline_semaphore_feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        timeline_semaphore_feature.timelineSemaphore = true;

        VkPhysicalDeviceSynchronization2Features synchronization_2_feature{};
        synchronization_2_feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        synchronization_2_feature.synchronization2 = true;
        synchronization_2_feature.pNext = &timeline_semaphore_feature;

        VkPhysicalDeviceFeatures2 device_features{};
        device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        device_features.pNext = &synchronization_2_feature;

        VkDeviceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.pNext = &device_features;

        create_info.pQueueCreateInfos = queue_create_infos.data();
        create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());

        create_info.pEnabledFeatures = VK_NULL_HANDLE;

        create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        create_info.ppEnabledExtensionNames = device_extensions.data();


        if (validation_layers.empty())
        {
            create_info.enabledLayerCount = 0;
        }
        else
        {
            create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers.size());
            create_info.ppEnabledLayerNames = validation_layers.data();
        }
        VkDevice device{ VK_NULL_HANDLE };
        if (vkCreateDevice(physical_device, &create_info, nullptr, &device) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create logical device!");
        }
        return device;

    }

    VkPhysicalDeviceIDProperties getDeviceUUID(VkPhysicalDevice physical_device)
    {
        VkPhysicalDeviceIDProperties device_id_property = {};
        device_id_property.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        device_id_property.pNext = nullptr;
        VkPhysicalDeviceProperties2 device_property = {};
        device_property.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        device_property.pNext = &device_id_property;
        vkGetPhysicalDeviceProperties2(physical_device, &device_property);
        return device_id_property;
    }
}

namespace RenderEngine
{
    PerformanceMarkerFactory::Marker::~Marker()
    {
        if (_command_buffer == VK_NULL_HANDLE)
        {
            return;
        }
        vkCmdEndDebugUtilsLabelEXT(_command_buffer);
        _command_buffer = VK_NULL_HANDLE;
    }
    void PerformanceMarkerFactory::Marker::start(const std::string_view& name)
    {
        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = name.data();
        label.color[1] = 1.0f;
        vkCmdBeginDebugUtilsLabelEXT(_command_buffer, &label);
    }
    void PerformanceMarkerFactory::Marker::finish()
    {
        vkCmdEndDebugUtilsLabelEXT(_command_buffer);
        _command_buffer = VK_NULL_HANDLE;
    }

    Device::StagingArea::StagingArea(std::unique_ptr<TransferEngine> transfer_engine,
                                     std::set<uint32_t> queue_family_indexes,
                                     VkPhysicalDevice physical_device,
                                     LogicalDevice& logical_device)
        : _scheduler(std::make_unique<DataTransferScheduler>())
        , _transfer_engine(std::move(transfer_engine))
        , _texture_factory(std::make_unique<TextureFactory>(*_transfer_engine,
                                                            *_scheduler,
                                                            std::move(queue_family_indexes),
                                                            physical_device,
                                                            logical_device))
    {

    }
    void Device::StagingArea::destroy()
    {
        _scheduler.reset();
        _transfer_engine.reset();
        _texture_factory.reset();
    }
    void Device::StagingArea::synchronizeStagingArea(SyncOperations sync_operations)
    {
        _scheduler->executeJobs(sync_operations, *_transfer_engine);
    }

    Device::StagingArea::~StagingArea() = default;

    Device::Device(VkInstance instance,
                   VkPhysicalDevice physical_device,
                   uint32_t queue_family_index_graphics,
                   uint32_t queue_family_index_presentation,
                   uint32_t queue_family_index_transfer,
                   const std::vector<const char*>& device_extensions,
                   const std::vector<const char*>& validation_layers,
                   DeviceLookup::DeviceInfo device_info)
        : _instance(instance)
        , _physical_device(physical_device)
        , _logical_device(createVulkanLogicalDevice(k_supported_queue_count,
                                                    physical_device,
                                                    queue_family_index_graphics,
                                                    queue_family_index_presentation,
                                                    queue_family_index_transfer,
                                                    device_extensions,
                                                    validation_layers))
        , _queue_family_present(queue_family_index_presentation)
        , _queue_family_graphics(queue_family_index_graphics)
        , _queue_family_transfer(queue_family_index_transfer)
        , _cuda_device(CudaCompute::CudaDevice::createDeviceForUUID(std::span{ &getDeviceUUID(physical_device).deviceUUID[0], VK_UUID_SIZE }, k_num_of_cuda_streams))
        , _device_info(std::move(device_info))
        , _staging_area(createTransferEngine(),
                        std::set{ _queue_family_transfer, _queue_family_graphics },
                        _physical_device,
                        _logical_device)
    {}

    Device::~Device()
    {
        destroy();
    }
    void Device::destroy() noexcept
    {
        _cuda_device.reset();
        _staging_area.destroy();
    }
    std::unique_ptr<Window> Device::createWindow(std::string_view name, uint32_t back_buffer_size)
    {
        constexpr auto width = 1024;
        constexpr auto height = 764;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow* window = glfwCreateWindow(width, height, name.data(), nullptr, nullptr);

        VkWin32SurfaceCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        create_info.hwnd = glfwGetWin32Window(window);
        create_info.hinstance = GetModuleHandle(nullptr);
        VkSurfaceKHR surface;

        if (vkCreateWin32SurfaceKHR(_instance, &create_info, nullptr, &surface) != VK_SUCCESS)
        {
            glfwDestroyWindow(window);
            throw std::runtime_error("Failed to create window surface!");
        }
        try
        {
            std::unique_ptr<SwapChain> swap_chain = std::make_unique<SwapChain>(SwapChain::CreateInfo{ window,
                                                                                _instance,
                                                                                _physical_device,
                                                                                &_logical_device,
                                                                                std::move(surface),
                                                                                this,
                                                                                _queue_family_graphics,
                                                                                _queue_family_present,
                                                                                back_buffer_size });
            std::unique_ptr<RenderEngine> render_engine = createRenderEngine(back_buffer_size);
            auto command_context = CommandContext::create(_logical_device, _queue_family_present, _device_info.queue_families[_queue_family_present]);
            return std::make_unique<Window>(*this, std::move(render_engine),
                                            window,
                                            std::move(swap_chain)
                                            , std::move(command_context));
        }
        catch (const std::runtime_error&)
        {
            glfwDestroyWindow(window);
            throw;
        }
    }

    std::unique_ptr<OffScreenWindow> Device::createOffScreenWindow(uint32_t back_buffer_size)
    {
        VkQueue render_queue;
        VkQueue present_queue;
        _logical_device->vkGetDeviceQueue(*_logical_device, _queue_family_graphics, 0, &render_queue);
        _logical_device->vkGetDeviceQueue(*_logical_device, _queue_family_present, 0, &present_queue);

        constexpr auto width = 1024;
        constexpr auto height = 764;

        std::unique_ptr<RenderEngine> render_engine = createRenderEngine(back_buffer_size);

        Image render_target_image(width, height, VK_FORMAT_R8G8B8A8_SRGB);
        std::vector<std::unique_ptr<Texture>> render_target_textures;
        for (uint32_t i = 0; i < back_buffer_size; ++i)
        {
            render_target_textures.push_back(getTextureFactory().createNoUpload(render_target_image,
                                                                                VK_IMAGE_ASPECT_COLOR_BIT,
                                                                                VK_SHADER_STAGE_ALL,
                                                                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        }
        return std::make_unique<OffScreenWindow>(*this,
                                                 std::move(render_engine),
                                                 std::move(render_target_textures));
    }

    std::unique_ptr<RenderEngine> Device::createRenderEngine(uint32_t back_buffer_size)
    {
        return std::make_unique<RenderEngine>(*this,
                                              CommandContext::create(_logical_device,
                                                                     _queue_family_graphics,
                                                                     _device_info.queue_families[_queue_family_graphics]),
                                              back_buffer_size);
    }

    std::unique_ptr<TransferEngine> Device::createTransferEngine()
    {
        return std::make_unique<TransferEngine>(CommandContext::create(_logical_device, _queue_family_transfer, _device_info.queue_families[_queue_family_transfer]));
    }

    void Device::waitIdle()
    {
        _logical_device->vkDeviceWaitIdle(*_logical_device);
    }

}