#pragma once

#include <volk.h>

#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

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

            struct UniformBuffer
            {
                int32_t binding{ -1 };
                int32_t size{ -1 };
            };

            struct PushConstants
            {
                int32_t size{ -1 };
                int32_t offset{ 0 };
                VkShaderStageFlags shared_with{ 0 };
            };

            struct Sampler
            {
                int32_t binding{ -1 };
            };

            uint32_t attributes_stride;
            std::vector<Attribute> input_attributes;
            std::unordered_map<int32_t, UniformBuffer> global_uniform_buffers;
            std::unordered_map<int32_t, Sampler> samplers;
            std::optional<PushConstants> push_constants{ std::nullopt };
        };
        Shader(const std::filesystem::path& spriv_path, MetaData meta_data)
            : _meta_data(std::move(meta_data))
        {
            readFromFile(spriv_path);
        }
        Shader(std::span<const uint32_t> spirv_code, MetaData meta_data)
            : _meta_data(std::move(meta_data))
            , _spirv_code(spirv_code.begin(), spirv_code.end())
        {}

        ShaderModule loadOn(VkDevice logical_device) const;
        const MetaData& getMetaData() const
        {
            return _meta_data;
        }

        void addGlobalUniform(int32_t binding, int32_t size)
        {
            _meta_data.global_uniform_buffers[binding].binding = binding;
            _meta_data.global_uniform_buffers[binding].size = size;
        }
        void addPushConstants(int32_t size)
        {
            _meta_data.push_constants = MetaData::PushConstants{ .size = size };
        }

    private:
        void readFromFile(const std::filesystem::path& spriv_path);
        MetaData _meta_data;
        std::vector<uint32_t> _spirv_code;
    };
}
