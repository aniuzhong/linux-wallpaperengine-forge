#pragma once

#include "WallpaperEngine/FrameSource/FrameSource.h"
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

    void publishFrame (const std::vector<uint8_t>& pixels);
    void captureFrame (uint32_t width, uint32_t height);
    [[nodiscard]] GLuint framebuffer () const;
    [[nodiscard]] EGLDisplay eglDisplay () const;
    [[nodiscard]] EGLContext eglDisplayContext () const;

private:
    void setupFramebuffer (uint32_t width, uint32_t height);
    void setupPixelBuffers (uint32_t width, uint32_t height);

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
