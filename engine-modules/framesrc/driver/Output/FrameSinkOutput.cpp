#include "FrameSinkOutput.h"

#include "../HeadlessOpenGLDriver.h"
#include "WallpaperEngine/Logging/Log.h"

#include <EGL/egl.h>
#include <chrono>
#include <cstring>

using namespace WallpaperEngine::Render::Drivers;
using namespace WallpaperEngine::Render::Drivers::Output;

namespace {
class FrameSinkOutputViewport final : public OutputViewport {
public:
    FrameSinkOutputViewport (glm::ivec4 viewport, const std::string& name, FrameSinkOutput* output) :
	OutputViewport (viewport, name), m_output (output) { }

    void makeCurrent () override {
	if (eglMakeCurrent (m_output->eglDisplay (), EGL_NO_SURFACE, EGL_NO_SURFACE, m_output->eglDisplayContext ()) != EGL_TRUE) {
	    sLog.exception ("eglMakeCurrent failed for the framesink viewport");
	}
	glBindFramebuffer (GL_FRAMEBUFFER, m_output->framebuffer ());
	glViewport (0, 0, viewport.z, viewport.w);
	sLog.out ("framesink viewport makeCurrent");
    }

    void swapOutput () override {
	m_output->captureFrame (uint32_t (viewport.z), uint32_t (viewport.w));
    }

private:
    FrameSinkOutput* m_output;
};
}

FrameSinkOutput::FrameSinkOutput (ApplicationContext& context, VideoDriver& driver, FrameSource::Writer& writer) :
    Output (context, driver), m_writer (writer) {
    this->m_fullWidth = static_cast<int> (context.settings.render.framesink.width);
    this->m_fullHeight = static_cast<int> (context.settings.render.framesink.height);

    this->setupFramebuffer (this->m_fullWidth, this->m_fullHeight);

    this->m_viewports["default"] = new FrameSinkOutputViewport {
	{ 0, 0, this->m_fullWidth, this->m_fullHeight }, "default", this
    };
}

FrameSinkOutput::~FrameSinkOutput () {
    for (int i = 0; i < kPixelBufferCount; ++i) {
	if (m_fences[i])
	    glDeleteSync (m_fences[i]);
	if (m_pixelBuffers[i] != 0)
	    glDeleteBuffers (1, &m_pixelBuffers[i]);
    }
    if (this->m_framebuffer != 0) {
	glDeleteFramebuffers (1, &this->m_framebuffer);
    }
    if (this->m_colorTexture != 0) {
	glDeleteTextures (1, &this->m_colorTexture);
    }
    if (this->m_depthRenderbuffer != 0) {
	glDeleteRenderbuffers (1, &this->m_depthRenderbuffer);
    }
}

void FrameSinkOutput::setupFramebuffer (uint32_t width, uint32_t height) {
    glGenTextures (1, &this->m_colorTexture);
    glBindTexture (GL_TEXTURE_2D, this->m_colorTexture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB8, static_cast<GLsizei> (width), static_cast<GLsizei> (height), 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenRenderbuffers (1, &this->m_depthRenderbuffer);
    glBindRenderbuffer (GL_RENDERBUFFER, this->m_depthRenderbuffer);
    glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, static_cast<GLsizei> (width), static_cast<GLsizei> (height));

    glGenFramebuffers (1, &this->m_framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_colorTexture, 0);
    glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, this->m_depthRenderbuffer);

    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	sLog.exception ("Framesink framebuffer is incomplete");
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    setupPixelBuffers (width, height);
}

void FrameSinkOutput::setupPixelBuffers (uint32_t width, uint32_t height) {
    const char *opt = getenv ("LWE_ASYNC_READBACK");
    m_asyncReadback = glewIsSupported ("GL_ARB_sync") != 0 && opt && opt[0] == '1';
    const GLsizeiptr size = GLsizeiptr (width) * height * 3;
    for (int i = 0; i < kPixelBufferCount; ++i) {
	if (m_pixelBuffers[i] == 0)
	    glGenBuffers (1, &m_pixelBuffers[i]);
	glBindBuffer (GL_PIXEL_PACK_BUFFER, m_pixelBuffers[i]);
	glBufferData (GL_PIXEL_PACK_BUFFER, size, nullptr, GL_STREAM_READ);
    }
    glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
    sLog.out ("framesink readback mode: ", m_asyncReadback ? "async pbo" : "sync (set LWE_ASYNC_READBACK=1 to enable pbo)");
}

void FrameSinkOutput::captureFrame (uint32_t width, uint32_t height) {
    if (!m_writer.valid ())
	return;
    const size_t frameBytes = size_t (width) * height * 3;
    glBindFramebuffer (GL_READ_FRAMEBUFFER, m_framebuffer);
    glReadBuffer (GL_COLOR_ATTACHMENT0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    if (!m_asyncReadback) {
	m_stage.resize (frameBytes);
	glReadPixels (0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, m_stage.data ());
	const GLenum error = glGetError ();
	glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);
	if (error != GL_NO_ERROR) {
	    sLog.error ("framesink readback failed with GL error ", error);
	    return;
	}
	publishFrame (m_stage);
	return;
    }
    const int idx = int (m_captureIndex % kPixelBufferCount);
    const int prev = int ((m_captureIndex + kPixelBufferCount - 1) % kPixelBufferCount);
    if (m_captureIndex >= 1 && m_fences[prev]) {
	glClientWaitSync (m_fences[prev], 0, GLuint64 (1000000000));
	glDeleteSync (m_fences[prev]);
	m_fences[prev] = nullptr;
	glBindBuffer (GL_PIXEL_PACK_BUFFER, m_pixelBuffers[prev]);
	if (auto *srcBytes = static_cast<const uint8_t *> (glMapBuffer (GL_PIXEL_PACK_BUFFER, GL_READ_ONLY))) {
	    m_stage.resize (frameBytes);
	    std::memcpy (m_stage.data (), srcBytes, frameBytes);
	    glUnmapBuffer (GL_PIXEL_PACK_BUFFER);
	    publishFrame (m_stage);
	} else {
	    sLog.error ("framesink pbo map failed");
	}
	glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
    }
    glBindBuffer (GL_PIXEL_PACK_BUFFER, m_pixelBuffers[idx]);
    glReadPixels (0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    const GLenum error = glGetError ();
    glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);
    if (error == GL_NO_ERROR)
	m_fences[idx] = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    ++m_captureIndex;
}

void FrameSinkOutput::publishFrame (const std::vector<uint8_t>& pixels) {
    if (!this->m_writer.valid ()) {
	return;
    }
    this->m_writer.publish (pixels.data ());
    if (++this->m_publishedFrames % 60 == 0) {
	const auto epoch = std::chrono::duration_cast<std::chrono::seconds> (
	    std::chrono::system_clock::now ().time_since_epoch ()
	).count ();
	sLog.out ("[heartbeat] publish seq=", this->m_writer.sequence (),
		  " published=", this->m_publishedFrames, " epoch=", epoch);
    }
}

EGLDisplay FrameSinkOutput::eglDisplay () const {
    const auto* driver = static_cast<const HeadlessOpenGLDriver*> (&this->m_driver);
    return driver->eglDisplay ();
}


GLuint FrameSinkOutput::framebuffer () const { return this->m_framebuffer; }

EGLContext FrameSinkOutput::eglDisplayContext () const {
    const auto* driver = static_cast<const HeadlessOpenGLDriver*> (&this->m_driver);
    return driver->eglDisplayContext ();
}

void FrameSinkOutput::reset () { }

bool FrameSinkOutput::renderVFlip () const { return false; }

bool FrameSinkOutput::renderMultiple () const { return false; }

bool FrameSinkOutput::haveImageBuffer () const { return false; }

void* FrameSinkOutput::getImageBuffer () const { return nullptr; }

uint32_t FrameSinkOutput::getImageBufferSize () const { return 0; }

void FrameSinkOutput::updateRender () const {
    this->m_viewports["default"]->viewport = { 0, 0, this->m_fullWidth, this->m_fullHeight };
}
