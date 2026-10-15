#pragma once

#include "WallpaperEngine/FrameSource/FrameSource.h"
#include "WallpaperEngine/FrameSource/FrameSinkChannel.h"
#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"
#include <GL/glew.h>
#include <EGL/egl.h>
#include <vector>

namespace WallpaperEngine::Render::Drivers {
class HeadlessOpenGLDriver;
}

namespace WallpaperEngine::Render::Drivers::Output {
class FrameSinkOutput final : public Output {
public:
    FrameSinkOutput (ApplicationContext& context, VideoDriver& driver, FrameSource::Writer& writer);
    ~FrameSinkOutput () override;

    void reset () override;
    bool renderVFlip () const override;
    bool renderMultiple () const override;
    bool haveImageBuffer () const override;
    void* getImageBuffer () const override;
    uint32_t getImageBufferSize () const override;
    void updateRender () const override;

    enum class Transport { Shm, DmaBuf };

    void publishFrame (const std::vector<uint8_t>& pixels);
    void captureFrame (uint32_t width, uint32_t height);
    [[nodiscard]] Transport transport () const { return m_transport; }
    [[nodiscard]] GLuint framebuffer () const;
    [[nodiscard]] EGLDisplay eglDisplay () const;
    [[nodiscard]] EGLContext eglDisplayContext () const;

    void setupFramebuffer (uint32_t width, uint32_t height);
    void setupPixelBuffers (uint32_t width, uint32_t height);
    void setupTransport (uint32_t width, uint32_t height);
    void releaseSlots ();
    bool probeDmaBufExport ();

    Transport m_transport = Transport::Shm;
    std::string m_channelPath;
    static constexpr int kSlotCount = 3;
    GLuint m_slotTextures[kSlotCount] = {};
    GLuint m_slotFramebuffers[kSlotCount] = {};
    int m_dmabufFds[kSlotCount] = {-1, -1, -1};
    void *m_eglImages[kSlotCount] = {};
    void *m_dupNativeFence = nullptr;
    void *m_eglDisplayHandle = nullptr;
    uint64_t m_frameSeq = 0;
    FrameSource::FrameChannelClient m_channel;

    FrameSource::Writer& m_writer;
    GLuint m_framebuffer = 0;
    GLuint m_colorTexture = 0;
    GLuint m_depthRenderbuffer = 0;
    uint64_t m_publishedFrames = 0;
    uint64_t m_captureIndex = 0;
    bool m_asyncReadback = false;
    static constexpr int kPixelBufferCount = 2;
    GLuint m_pixelBuffers[kPixelBufferCount] = {};
    GLsync m_fences[kPixelBufferCount] = {};
    std::vector<uint8_t> m_stage;
};
}
