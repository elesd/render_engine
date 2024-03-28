#include <render_engine/QueueSubmitTracker.h>

namespace RenderEngine
{
    QueueSubmitTracker::~QueueSubmitTracker()
    {
        if (_logical_device == nullptr)
        {
            return;
        }
        try
        {
            std::lock_guard lock{ _fence_mutex };

            noLockingClear();
            for (auto fence : _fence_pool)
            {
                (*_logical_device)->vkDestroyFence(**_logical_device, fence, nullptr);
            }
            _logical_device = VK_NULL_HANDLE;
        }
        catch (const std::exception&)
        {

        }
    }

    void QueueSubmitTracker::queueSubmit(VkSubmitInfo2&& submit_info,
                                         const SyncOperations& sync_operations,
                                         AbstractCommandContext& command_context)
    {
        command_context.queueSubmit(std::move(submit_info), sync_operations, createFence());
    }

    void QueueSubmitTracker::wait()
    {
        std::lock_guard lock{ _fence_mutex };

        noLockingWait();
    }

    uint32_t QueueSubmitTracker::queryNumOfSuccess() const
    {
        std::lock_guard lock{ _fence_mutex };

        uint32_t result{ 0 };
        for (auto fence : _fences)
        {
            if ((*_logical_device)->vkGetFenceStatus(**_logical_device, fence) == VK_SUCCESS)
            {
                result++;
            }
        }
        return result;
    }
    void QueueSubmitTracker::clear()
    {
        std::lock_guard lock{ _fence_mutex };
        noLockingClear();
    }
    VkFence QueueSubmitTracker::createFence()
    {
        std::lock_guard lock{ _fence_mutex };

        if (_fence_pool.empty())
        {
            VkFenceCreateInfo create_info{};
            create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

            VkFence fence{ VK_NULL_HANDLE };
            if ((*_logical_device)->vkCreateFence(**_logical_device, &create_info, nullptr, &fence) != VK_SUCCESS)
            {
                throw std::runtime_error("Cannot create fence for queue submit tracking");
            }
            _fences.push_back(fence);
        }
        else
        {
            if ((*_logical_device)->vkResetFences(**_logical_device, 1, &_fence_pool.back()) != VK_SUCCESS)
            {
                throw std::runtime_error("Cannot reset fence for queue submit tracking");
            }
            _fences.push_back(_fence_pool.back());
            _fence_pool.pop_back();
        }
        return _fences.back();
    }
    void QueueSubmitTracker::noLockingWait()
    {
        if (_fences.empty())
        {
            return;
        }
        if ((*_logical_device)->vkWaitForFences(**_logical_device, static_cast<uint32_t>(_fences.size()), _fences.data(), VK_TRUE, UINT64_MAX))
        {
            throw std::runtime_error("Cannot wait for fences");
        }
    }
    void QueueSubmitTracker::noLockingClear()
    {
        noLockingWait();
        std::ranges::copy(_fences, std::back_inserter(_fence_pool));

        _fences.clear();
    }
}