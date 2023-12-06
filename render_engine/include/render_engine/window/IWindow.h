#pragma once
#include <render_engine/Device.h>
#include <render_engine/RenderEngine.h>
namespace RenderEngine
{

    class IWindow
    {
    public:
        virtual ~IWindow() = default;

        virtual void update() = 0;
        virtual bool isClosed() const = 0;
        virtual void registerRenderers(const std::vector<uint32_t>& renderer_ids) = 0;
        virtual Device& getDevice() = 0;
        virtual void enableRenderdocCapture() = 0;
        virtual void disableRenderdocCapture() = 0;
        virtual RenderEngine& getRenderEngine() = 0;
        virtual TransferEngine& getTransferEngine() = 0;
        virtual AbstractRenderer* findRenderer(uint32_t renderer_id) const = 0;
    };
}