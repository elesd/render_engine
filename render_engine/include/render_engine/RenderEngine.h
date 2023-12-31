#pragma once

#include <volk.h>

#include <render_engine/CommandPoolFactory.h>
#include <render_engine/Device.h>
#include <render_engine/GpuResourceManager.h>
#include <render_engine/SynchronizationPrimitives.h>

#include <unordered_map>

#include <algorithm>
#include <ranges>

namespace RenderEngine
{
    class AbstractRenderer;

    class RenderEngine
    {
    public:

        RenderEngine(Device& device, VkQueue _render_queue, uint32_t _render_queue_family, size_t back_buffer_count);

        VkQueue& getRenderQueue() { return _render_queue; }
        uint32_t getQueueFamilyIndex() const { return _render_queue_family; }

        void onFrameBegin(const std::ranges::input_range auto& renderers, uint32_t image_index)
        {
            for (auto* renderer : renderers)
            {
                renderer->onFrameBegin(image_index);
            }
        }
        [[nodiscard]]
        bool render(SynchronizationPrimitives* synchronization_primitives,
                    const std::ranges::input_range auto& renderers,
                    uint32_t image_index)
        {
            std::vector<VkCommandBufferSubmitInfo> command_buffer_infos = executeDrawCalls(renderers, image_index);
            addWaitSemaphoresFromRenderers(renderers, synchronization_primitives, image_index);
            if (command_buffer_infos.empty() == false)
            {
                submitDrawCalls(command_buffer_infos, *synchronization_primitives);
                return true;
            }
            return false;
        }

        GpuResourceManager& getGpuResourceManager() { return _gpu_resource_manager; }
        CommandPoolFactory& getCommandPoolFactory() { return _command_pool_factory; }
    private:
        std::vector<VkCommandBufferSubmitInfo> executeDrawCalls(const std::ranges::input_range auto& renderers,
                                                                uint32_t image_index)
        {
            std::vector<VkCommandBufferSubmitInfo> command_buffers;
            for (AbstractRenderer* drawer : renderers)
            {
                drawer->draw(image_index);
                auto current_command_buffers = drawer->getCommandBuffers(image_index);
                std::ranges::transform(current_command_buffers,
                                       std::back_inserter(command_buffers),
                                       [](const auto& command_buffer)
                                       {
                                           return VkCommandBufferSubmitInfo{
                                                               .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                               .pNext = VK_NULL_HANDLE,
                                                               .commandBuffer = command_buffer,
                                                               .deviceMask = 0 };
                                       });
            }
            return command_buffers;
        }

        void addWaitSemaphoresFromRenderers(const std::ranges::input_range auto& renderers,
                                            SynchronizationPrimitives* synchronization_primitives,
                                            uint32_t image_index)
        {
            for (AbstractRenderer* drawer : renderers)
            {
                auto wait_semaphores = drawer->getWaitSemaphores(image_index);
                std::ranges::copy(wait_semaphores,
                                  std::back_inserter(synchronization_primitives->wait_semaphores));
            }
        }

        void submitDrawCalls(const std::vector<VkCommandBufferSubmitInfo>& command_buffers,
                             const SynchronizationPrimitives& synchronization_primitives);


        Device& _device;
        VkQueue _render_queue;
        uint32_t _render_queue_family{ 0 };
        GpuResourceManager _gpu_resource_manager;
        CommandPoolFactory _command_pool_factory;
    };
}