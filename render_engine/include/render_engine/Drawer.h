#pragma once
#include <render_engine/SwapChain.h>
#include <render_engine/Buffer.h>

#include <vulkan/vulkan.h>

#include <vector>
#include <cassert>
#include <memory>
#include <string>
#include <array>

namespace RenderEngine
{
	class Window;
	class Drawer
	{
	public:
		class ReinitializationCommand
		{
		public:
			explicit ReinitializationCommand(Drawer& drawer)
				:_drawer(drawer)
			{
				_drawer.resetFrameBuffers();
			}

			void finish(const SwapChain& swap_chain)
			{
				_drawer.createFrameBuffers(swap_chain);
			}
			~ReinitializationCommand()
			{
				assert(_finished);
			}
		private:
			Drawer& _drawer;
			bool _finished = false;
		};
		friend class ReinitializationCommand;

		struct Vertex {
			std::array<float, 2> pos;
			std::array<float, 3> color;
		};
		Drawer(Window& parent,
			const SwapChain& swap_chain,
			uint32_t back_buffer_size);

		void init(std::vector<Vertex> vertices);

		void draw(uint32_t swap_chain_image_index, uint32_t frame_number)
		{
			draw(_frame_buffers[swap_chain_image_index], frame_number);
		}
		std::vector<VkCommandBuffer> getCommandBuffers(uint32_t frame_number)
		{
			return { getFrameData(frame_number).command_buffer };
		}
		[[nodiscard]]
		ReinitializationCommand reinit();
		~Drawer();
	private:
		struct FrameData
		{
			VkCommandBuffer command_buffer;
		};
		void createFrameBuffers(const SwapChain& swap_chain);
		bool createFrameBuffer(const SwapChain& swap_chain, uint32_t frame_buffer_index);
		void createCommandPool(uint32_t render_queue_family);
		void createCommandBuffer();
		FrameData& getFrameData(uint32_t frame_number)
		{
			return _back_buffer[frame_number % _back_buffer.size()];
		}
		void draw(const VkFramebuffer& frame_buffer, uint32_t frame_number);
		void resetFrameBuffers();

		Window& _window;
		VkRenderPass _render_pass;
		VkPipelineLayout _pipeline_layout;
		VkPipeline _pipeline;
		std::vector<VkFramebuffer> _frame_buffers;
		VkCommandPool _command_pool;
		std::vector<FrameData> _back_buffer;
		VkRect2D _render_area;

		std::unique_ptr<Buffer> _vertex_buffer;
	};
}