#pragma once

#include "Output/FrameSinkOutput.h"
#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Input/MouseInput.h"
#include <glm/vec2.hpp>

namespace WallpaperEngine::Input {
class NullMouseInput final : public MouseInput {
public:
    void update () override { }
    [[nodiscard]] glm::dvec2 position () const override { return { 0.0, 0.0 }; }
    [[nodiscard]] MouseClickStatus leftClick () const override { return MouseClickStatus::Released; }
    [[nodiscard]] MouseClickStatus rightClick () const override { return MouseClickStatus::Released; }
};
}
#include "WallpaperEngine/Render/Drivers/Detectors/FullScreenDetector.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"
#include <GL/glew.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace WallpaperEngine::Render::Drivers {
class HeadlessOpenGLDriver final : public VideoDriver {
public:
    explicit HeadlessOpenGLDriver (ApplicationContext& context, WallpaperApplication& app);
    ~HeadlessOpenGLDriver () override;

    [[nodiscard]] Output::Output& getOutput () override;
    [[nodiscard]] float getRenderTime () const override;
    bool closeRequested () override;
    void resizeWindow (glm::ivec2 size) override;
    void resizeWindow (glm::ivec4 positionAndSize) override;
    void showWindow () override;
    void hideWindow () override;
    [[nodiscard]] glm::ivec2 getFramebufferSize () const override;
    [[nodiscard]] uint32_t getFrameCounter () const override;
    void dispatchEventQueue () override;
    [[nodiscard]] void* getProcAddress (const char* name) const override;

    [[nodiscard]] EGLDisplay eglDisplay () const;
    [[nodiscard]] EGLContext eglDisplayContext () const;

private:
    bool bootstrapEGL (std::string& error);
    bool bootstrapContext (std::string& error);

    ApplicationContext& m_context;
    Input::NullMouseInput m_mouseInput;
    Output::FrameSinkOutput* m_output = nullptr;
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLSurface m_eglSurface = EGL_NO_SURFACE;
    FrameSource::Writer m_frameOutWriter;
    bool m_destinationRegistered = false;
    uint32_t m_frameCounter = 0;
    std::chrono::steady_clock::time_point m_startTime;
};
}
