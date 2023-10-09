#include <render_engine/window/Window.h>

#include <render_engine/RenderEngine.h>
#include <render_engine/GpuResourceManager.h>
#include <render_engine/RenderContext.h>

#include <data_config.h>

#include <stdexcept>
#include <vector>
#include <algorithm>
#include <limits>
#include <fstream>

#include <backends/imgui_impl_vulkan.h>
#include <renderdoc_app.h>

namespace
{
	constexpr uint32_t kMaxNumOfResources = 10;
}
namespace RenderEngine
{
	Window::Window(RenderEngine& engine,
		GLFWwindow* window,
		std::unique_ptr<SwapChain> swap_chain,
		VkQueue render_queue,
		VkQueue present_queue,
		uint32_t render_queue_family,
		uint32_t back_buffer_size)
		: _render_queue{ render_queue }
		, _present_queue{ present_queue }
		, _window(window)
		, _swap_chain(std::move(swap_chain))
		, _engine(engine)
		, _gpuResourceManager(std::make_unique<GpuResourceManager>(engine.getPhysicalDevice(), engine.getLogicalDevice(), back_buffer_size, kMaxNumOfResources))
		, _render_queue_family(render_queue_family)
		, _back_buffer_size(back_buffer_size)
	{
		initSynchronizationObjects();

	}

	void Window::update()
	{
		if (_closed)
		{
			destroy();
			return;
		}
		present();
		handleEvents();
	}

	void Window::registerRenderers(const std::vector<uint32_t>& renderer_ids)
	{
		_renderers = RenderContext::context().getRendererFactory().generateRenderers(renderer_ids, *this, *_swap_chain, _back_buffer.size());
		for (size_t i = 0; i < renderer_ids.size(); ++i)
		{
			_renderer_map[renderer_ids[i]] = _renderers[i].get();
		}
	}

	void Window::enableRenderdocCapture()
	{
		if (_renderdoc_api != nullptr)
		{
			return;
		}
		_renderdoc_api = RenderContext::context().getRenderdocApi();
	}

	void Window::disableRenderdocCapture()
	{
		_renderdoc_api = nullptr;
	}


	Window::~Window()
	{

		destroy();
	}

	void Window::initSynchronizationObjects()
	{
		for (FrameData& frame_data : _back_buffer)
		{
			VkSemaphoreCreateInfo semaphore_info{};
			semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

			VkFenceCreateInfo fence_info{};
			fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			auto logical_device = _engine.getLogicalDevice();
			if (vkCreateSemaphore(logical_device, &semaphore_info, nullptr, &frame_data.image_available_semaphore) != VK_SUCCESS ||
				vkCreateSemaphore(logical_device, &semaphore_info, nullptr, &frame_data.render_finished_semaphore) != VK_SUCCESS ||
				vkCreateFence(logical_device, &fence_info, nullptr, &frame_data.in_flight_fence) != VK_SUCCESS)
			{
				vkDestroySemaphore(logical_device, frame_data.image_available_semaphore, nullptr);
				vkDestroySemaphore(logical_device, frame_data.render_finished_semaphore, nullptr);
				vkDestroyFence(logical_device, frame_data.in_flight_fence, nullptr);
				throw std::runtime_error("failed to create synchronization objects for a frame!");
			}
		}
	}

	void Window::handleEvents()
	{
		bool should_be_closed = glfwWindowShouldClose(_window);
		if (should_be_closed)
		{
			glfwHideWindow(_window);
			_closed = true;
			return;
		}
		glfwPollEvents();
	}

	void Window::present()
	{
		if (_renderdoc_api) ((RENDERDOC_API_1_1_2*)_renderdoc_api)->StartFrameCapture(NULL, NULL);

		present(_back_buffer[_frame_counter % _back_buffer.size()]);
		if (_renderdoc_api) ((RENDERDOC_API_1_1_2*)_renderdoc_api)->EndFrameCapture(NULL, NULL);

		_frame_counter++;
	}
	void Window::reinitSwapChain()
	{
		auto logical_device = _engine.getLogicalDevice();
		vkDeviceWaitIdle(logical_device);
		std::vector<AbstractRenderer::ReinitializationCommand> commands;
		for (auto& drawer : _renderers)
		{
			commands.emplace_back(drawer->reinit());
		}
		_swap_chain->reinit();
		for (auto& command : commands)
		{
			command.finish(*_swap_chain);
		}


	}
	void Window::destroy()
	{
		if (_window == nullptr)
		{
			return;
		}
		auto logical_device = _engine.getLogicalDevice();
		vkDeviceWaitIdle(logical_device);
		for (FrameData& frame_data : _back_buffer)
		{
			vkDestroySemaphore(logical_device, frame_data.image_available_semaphore, nullptr);
			vkDestroySemaphore(logical_device, frame_data.render_finished_semaphore, nullptr);
			vkDestroyFence(logical_device, frame_data.in_flight_fence, nullptr);
		}
		_swap_chain.reset();
		_renderers.clear();

		_gpuResourceManager.reset();
		glfwDestroyWindow(_window);

		_window = nullptr;
		_closed = true;
	}
	void Window::present(FrameData& frame_data)
	{
		if (_renderers.empty())
		{
			return;
		}
		auto logical_device = _engine.getLogicalDevice();
		vkWaitForFences(logical_device, 1, &frame_data.in_flight_fence, VK_TRUE, UINT64_MAX);
		uint32_t image_index = 0;
		{
			auto call_result = vkAcquireNextImageKHR(logical_device,
				_swap_chain->getDetails().swap_chain,
				UINT64_MAX,
				frame_data.image_available_semaphore,
				VK_NULL_HANDLE, &image_index);
			switch (call_result)
			{
			case VK_ERROR_OUT_OF_DATE_KHR:
			case VK_SUBOPTIMAL_KHR:
				reinitSwapChain();
				return;
			case VK_SUCCESS:
				break;
			default:
				throw std::runtime_error("Failed to query swap chain image");
			}
		}
		vkResetFences(logical_device, 1, &frame_data.in_flight_fence);

		std::vector<VkCommandBuffer> command_buffers;
		for (auto& drawer : _renderers)
		{
			drawer->draw(image_index, _frame_counter);
			auto current_command_buffers = drawer->getCommandBuffers(_frame_counter);
			std::copy(current_command_buffers.begin(), current_command_buffers.end(),
				std::back_inserter(command_buffers));
		}
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		VkSemaphore waitSemaphores[] = { frame_data.image_available_semaphore };
		VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;

		submitInfo.commandBufferCount = command_buffers.size();
		submitInfo.pCommandBuffers = command_buffers.data();

		VkSemaphore signalSemaphores[] = { frame_data.render_finished_semaphore };
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		if (vkQueueSubmit(_render_queue, 1, &submitInfo, frame_data.in_flight_fence) != VK_SUCCESS) {
			throw std::runtime_error("failed to submit draw command buffer!");
		}

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = { _swap_chain->getDetails().swap_chain };
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;

		presentInfo.pImageIndices = &image_index;

		vkQueuePresentKHR(_present_queue, &presentInfo);
	}

}