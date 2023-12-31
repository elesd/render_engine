#pragma once

#include <render_engine/assets/Material.h>
#include <render_engine/assets/VolumetricObject.h>
#include <render_engine/renderers/ForwardRenderer.h>
#include <render_engine/renderers/SingleColorOutputRenderer.h>
#include <render_engine/resources/RenderTarget.h>
#include <render_engine/resources/Texture.h>

namespace RenderEngine
{
    class VolumeRenderer : public SingleColorOutputRenderer
    {
    public:
        static constexpr uint32_t kRendererId = 4u;

        VolumeRenderer(IWindow& window,
                       const RenderTarget& render_target,
                       bool last_renderer);
        ~VolumeRenderer() override = default;
        void onFrameBegin(uint32_t image_index) override;
        void addVolumeObject(const VolumetricObjectInstance* mesh_instance);
        void draw(uint32_t swap_chain_image_index) override;
        std::vector<VkSemaphoreSubmitInfo> getWaitSemaphores(uint32_t image_index) override
        {
            return {};
        }
    private:
        struct MeshBuffers
        {
            std::unique_ptr<Buffer> vertex_buffer;
            std::unique_ptr<Buffer> index_buffer;
            std::unique_ptr<Buffer> color_buffer;
            std::unique_ptr<Buffer> normal_buffer;
            std::unique_ptr<Buffer> texture_buffer;
        };
        struct FrameBufferData
        {
            std::vector<std::unique_ptr<Texture>> textures_per_back_buffer;
            std::vector<std::unique_ptr<TextureView>> texture_views_per_back_buffer;
        };
        struct TechniqueData
        {
            std::vector<std::unique_ptr<Material>> subpass_materials;
            std::vector<std::unique_ptr<MaterialInstance>> subpass_material_instance;
            std::unique_ptr<Technique> front_face_technique;
            std::unique_ptr<Technique> back_face_technique;
            std::unique_ptr<Technique> volume_technique;
        };
        struct MeshGroup
        {
            TechniqueData technique_data;
            std::vector<const VolumetricObjectInstance*> meshes;
        };

        void initializeFrameBuffers(uint32_t back_buffer_count, const Image& ethalon_image);
        void initializeFrameBufferData(uint32_t back_buffer_count, const Image& ethalon_image, FrameBufferData* frame_buffer_data);
        TechniqueData createTechniqueDataFor(const VolumetricObjectInstance& mesh);
        std::vector<AttachmentInfo> reinitializeAttachments(const RenderTarget& render_target) override final;
        std::vector<AttachmentInfo> createFrameBuffersAndAttachments(const RenderTarget& render_target);
        void resetResourceStatesOf(Technique& technique, uint32_t image_index);
        void drawWithTechnique(Technique& technique,
                               const std::vector<const VolumetricObjectInstance*>& meshes,
                               FrameData& frame_data, uint32_t swap_chain_image_index);

        RenderTarget _render_target;
        std::vector<MeshGroup> _meshes;
        FrameBufferData _front_face_frame_buffer;
        FrameBufferData _back_face_frame_buffer;
        std::map<const Mesh*, MeshBuffers> _mesh_buffers;

    };
}