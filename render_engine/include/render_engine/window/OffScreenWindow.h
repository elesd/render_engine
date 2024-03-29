#pragma once

#include <render_engine/containers/ImageStream.h>
#include <render_engine/renderers/AbstractRenderer.h>
#include <render_engine/resources/Texture.h>
#include <render_engine/synchronization/SyncObject.h>
#include <render_engine/window/IWindow.h>

#include <vector>
namespace RenderEngine
{
    class OffScreenWindow : public IWindow
    {

    public:
        OffScreenWindow(Device& device,
                        std::unique_ptr<RenderEngine>&& render_engine,
                        std::vector<std::unique_ptr<Texture>>&& textures);
        ~OffScreenWindow();
        void update() override final;
        void registerRenderers(const std::vector<uint32_t>& renderer_ids) override final;
        Device& getDevice()  override final { return _device; }
        void enableRenderdocCapture() override final;
        void disableRenderdocCapture() override final;
        RenderEngine& getRenderEngine()  override final { return *_render_engine; }
        bool isClosed() const override final { return false; }

        template<typename T>
        T& getRendererAs(uint32_t renderer_id)
        {
            return static_cast<T&>(*_renderer_map.at(renderer_id));
        }
        AbstractRenderer* findRenderer(uint32_t renderer_id) const
        {
            auto it = _renderer_map.find(renderer_id);
            return it != _renderer_map.end() ? it->second : nullptr;
        }

        ImageStream& getImageStream() { return _image_stream; }
        uint32_t getFrameCounter() const { return _frame_counter; }
        void registerTunnel(WindowTunnel& tunnel) override final;
        WindowTunnel* getTunnel() override final;

        TextureFactory& getTextureFactory() final;
    private:
        struct FrameData
        {
            SyncObject synch_render;
            bool contains_image{ false };
            std::unique_ptr<Texture> render_target_texture;
            std::unique_ptr<ITextureView> render_target_texture_view;
        };
        void initSynchronizationObjects();
        void present();
        void present(FrameData& current_frame_data) const;
        void readBack();
        void destroy();
        RenderTarget createRenderTarget();
        uint32_t getCurrentImageIndex() const { return _current_render_target_index; }
        uint32_t getOldestImageIndex() const { return (_current_render_target_index + 1) % _back_buffer_size; }

        Device& _device;
        std::unique_ptr<RenderEngine> _render_engine;
        std::vector<FrameData> _back_buffer;
        uint32_t _render_queue_family{ 0 };

        std::vector<std::unique_ptr<AbstractRenderer>> _renderers;
        std::unordered_map<uint32_t, AbstractRenderer*> _renderer_map;
        uint32_t _frame_counter{ 0 };
        uint32_t _current_render_target_index = 0;
        size_t _back_buffer_size{ 2 };
        void* _renderdoc_api{ nullptr };
        ImageStream _image_stream;
        WindowTunnel* _tunnel{ nullptr };
    };
}