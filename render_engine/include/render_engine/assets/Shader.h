#pragma once

#include <volk.h>

#include <filesystem>
#include <unordered_map>

namespace RenderEngine
{
	class ShaderModule;
	class Shader
	{
	public:

		struct MetaData
		{
			struct Attribute
			{
				uint32_t location{ 0 };
				VkFormat format{};
				uint32_t offset{ 0 };
			};

			struct Uniforms
			{
				int32_t binding{ -1 };
				int32_t size{ -1 };

			};

			uint32_t attributes_stride;
			std::vector<Attribute> input_attributes;
			std::unordered_map<int32_t, Uniforms> global_uniform_buffers;
		};
		explicit Shader(std::filesystem::path spriv_path, MetaData meta_data)
			: _spirv_path(std::move(spriv_path))
			, _meta_data(std::move(meta_data))
		{}

		ShaderModule loadOn(VkDevice logical_device) const;
		const MetaData& metaData() const
		{
			return _meta_data;
		}
		void addGlobalUniform(int32_t binding, int32_t size)
		{
			_meta_data.global_uniform_buffers[binding].binding = binding;
			_meta_data.global_uniform_buffers[binding].size = size;
		}

	private:
		std::filesystem::path _spirv_path;
		MetaData _meta_data;
	};
}