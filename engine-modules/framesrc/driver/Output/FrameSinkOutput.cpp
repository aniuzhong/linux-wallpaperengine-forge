#include "FrameSinkOutput.h"

#include "../HeadlessOpenGLDriver.h"
#include "WallpaperEngine/Logging/Log.h"

#include <EGL/egl.h>
#include <chrono>

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
	glBindFramebuffer (GL_READ_FRAMEBUFFER, m_output->framebuffer ());
	glReadBuffer (GL_COLOR_ATTACHMENT0);
	glPixelStorei (GL_PACK_ALIGNMENT, 1);
	std::vector<uint8_t> pixels (static_cast<size_t> (viewport.z) * viewport.w * 3);
	glReadPixels (0, 0, viewport.z, viewport.w, GL_RGB, GL_UNSIGNED_BYTE, pixels.data ());
	glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);

	const GLenum error = glGetError ();
	if (error != GL_NO_ERROR) {
	    sLog.error ("FrameSource readback failed with GL error ", error);
	    return;
	}
	m_output->publishFrame (pixels);
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
