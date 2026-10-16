#include "HeadlessOpenGLDriver.h"

#include "VideoFactories.h"
#include "WallpaperEngine/Logging/Log.h"

#include <chrono>
#include <vector>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#endif

using namespace WallpaperEngine::Render::Drivers;
using namespace WallpaperEngine::Render::Drivers::Output;

namespace {
bool tryPlatformDisplay (EGLenum platform, EGLDisplay& display) {
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (eglGetProcAddress ("eglGetPlatformDisplayEXT"));
    if (getPlatformDisplay == nullptr) {
	return false;
    }

    EGLDisplay candidate = getPlatformDisplay (platform, EGL_DEFAULT_DISPLAY, nullptr);
    if (candidate == EGL_NO_DISPLAY || eglInitialize (candidate, nullptr, nullptr) != EGL_TRUE) {
	if (candidate != EGL_NO_DISPLAY) {
	    eglTerminate (candidate);
	}
	return false;
    }
    display = candidate;
    return true;
}

bool tryDeviceDisplay (EGLDisplay& display, std::string& error) {
    auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC> (eglGetProcAddress ("eglQueryDevicesEXT"));
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (eglGetProcAddress ("eglGetPlatformDisplayEXT"));
    if (queryDevices == nullptr || getPlatformDisplay == nullptr) {
	error = "EGL device enumeration unsupported";
	return false;
    }

    EGLint count = 0;
    if (queryDevices (0, nullptr, &count) != EGL_TRUE || count <= 0) {
	error = "no EGL devices found";
	return false;
    }

    std::vector<EGLDeviceEXT> devices (static_cast<size_t> (count));
    if (queryDevices (count, devices.data (), &count) != EGL_TRUE) {
	error = "eglQueryDevicesEXT failed";
	return false;
    }

    for (const EGLDeviceEXT& device : devices) {
	EGLDisplay candidate = getPlatformDisplay (EGL_PLATFORM_DEVICE_EXT, static_cast<EGLDeviceEXT> (device), nullptr);
	if (candidate == EGL_NO_DISPLAY) {
	    continue;
	}
	EGLint major = 0;
	EGLint minor = 0;
	if (eglInitialize (candidate, &major, &minor) == EGL_TRUE) {
	    display = candidate;
	    return true;
	}
	eglTerminate (candidate);
    }
    error = "no usable EGL device display";
    return false;
}
}

HeadlessOpenGLDriver::HeadlessOpenGLDriver (ApplicationContext& context, WallpaperApplication& app) :
    VideoDriver (app, m_mouseInput), m_context (context), m_mouseInput () {
    const uint32_t width = this->m_context.settings.render.framesink.width;
    const uint32_t height = this->m_context.settings.render.framesink.height;
    if (width == 0 || height == 0) {
	sLog.exception ("Framesink mode requires a valid --framesink WxH resolution");
    }

    std::string error;
    if (!this->bootstrapEGL (error)) {
	sLog.exception ("Cannot bootstrap headless EGL display: ", error);
    }
    if (eglBindAPI (EGL_OPENGL_API) != EGL_TRUE) {
	sLog.exception ("eglBindAPI (EGL_OPENGL_API) failed");
    }
    if (!this->bootstrapContext (error)) {
	sLog.exception ("Cannot bootstrap headless EGL context: ", error);
    }
    if (eglMakeCurrent (this->m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, this->m_eglContext) != EGL_TRUE) {
	sLog.exception ("eglMakeCurrent failed for the headless context");
    }

    const GLenum glewResult = glewInit ();
    if (glewResult != GLEW_OK) {
	sLog.error ("Failed to initialize GLEW: ", reinterpret_cast<const char*> (glewGetErrorString (glewResult)));
    }
    glGetError ();

    this->m_startTime = std::chrono::steady_clock::now ();
    this->m_output = new FrameSinkOutput (this->m_context, *this, this->m_frameOutWriter);
    if (this->m_context.settings.frameOut.enabled
	&& !this->m_context.settings.frameOut.path.empty ()
	&& this->m_output->transport () == FrameSinkOutput::Transport::Shm) {
	const auto status = this->m_frameOutWriter.open (
	    this->m_context.settings.frameOut.path, width, height
	);
	if (status != FrameSource::Result::Ok) {
	    sLog.error ("FrameSource writer open failed: ", FrameSource::resultText (status));
	}
    }
}

HeadlessOpenGLDriver::~HeadlessOpenGLDriver () {
    delete this->m_output;
    if (this->m_eglDisplay != EGL_NO_DISPLAY) {
	eglMakeCurrent (this->m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (this->m_eglContext != EGL_NO_CONTEXT) {
	    eglDestroyContext (this->m_eglDisplay, this->m_eglContext);
	}
	if (this->m_eglSurface != EGL_NO_SURFACE) {
	    eglDestroySurface (this->m_eglDisplay, this->m_eglSurface);
	}
	eglTerminate (this->m_eglDisplay);
    }
}

bool HeadlessOpenGLDriver::bootstrapEGL (std::string& error) {
    if (tryDeviceDisplay (this->m_eglDisplay, error)) {
	return true;
    }
    if (tryPlatformDisplay (EGL_PLATFORM_SURFACELESS_MESA, this->m_eglDisplay)) {
	return true;
    }
    this->m_eglDisplay = eglGetDisplay (EGL_DEFAULT_DISPLAY);
    if (this->m_eglDisplay != EGL_NO_DISPLAY && eglInitialize (this->m_eglDisplay, nullptr, nullptr) == EGL_TRUE) {
	return true;
    }
    error = "no usable EGL display platform";
    this->m_eglDisplay = EGL_NO_DISPLAY;
    return false;
}

bool HeadlessOpenGLDriver::bootstrapContext (std::string& error) {
    EGLConfig config = nullptr;

    const EGLint pbufferFirstAttribs[] = {
	EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
	EGL_NONE
    };
    EGLint pbufferCount = 0;
    if (eglChooseConfig (this->m_eglDisplay, pbufferFirstAttribs, &config, 1, &pbufferCount) != EGL_TRUE || pbufferCount < 1) {
	const EGLint bareAttribs[] = {
	    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	    EGL_NONE
	};
	EGLint bareCount = 0;
	if (eglChooseConfig (this->m_eglDisplay, bareAttribs, &config, 1, &bareCount) != EGL_TRUE || bareCount < 1) {
	    error = "eglChooseConfig failed";
	    return false;
	}
    }

    this->m_eglContext = eglCreateContext (this->m_eglDisplay, config, EGL_NO_CONTEXT, nullptr);
    if (this->m_eglContext == EGL_NO_CONTEXT) {
	const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
	this->m_eglContext = eglCreateContext (this->m_eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    }
    if (this->m_eglContext == EGL_NO_CONTEXT) {
	error = "eglCreateContext failed";
	return false;
    }

    if (eglMakeCurrent (this->m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, this->m_eglContext) == EGL_TRUE) {
	return true;
    }

    const EGLint pbufferAttribs[] = {
	EGL_WIDTH, 32,
	EGL_HEIGHT, 32,
	EGL_NONE
    };
    this->m_eglSurface = eglCreatePbufferSurface (this->m_eglDisplay, config, pbufferAttribs);
    if (this->m_eglSurface == EGL_NO_SURFACE) {
	error = "eglCreatePbufferSurface failed";
	return false;
    }
    if (eglMakeCurrent (this->m_eglDisplay, this->m_eglSurface, this->m_eglSurface, this->m_eglContext) != EGL_TRUE) {
	error = "eglMakeCurrent (pbuffer) failed";
	return false;
    }
    return true;
}

Output::Output& HeadlessOpenGLDriver::getOutput () { return *this->m_output; }

void* HeadlessOpenGLDriver::getProcAddress (const char* name) const {
    return reinterpret_cast<void*> (eglGetProcAddress (name));
}

float HeadlessOpenGLDriver::getRenderTime () const {
    return std::chrono::duration<float> (std::chrono::steady_clock::now () - this->m_startTime).count ();
}

bool HeadlessOpenGLDriver::closeRequested () { return false; }

void HeadlessOpenGLDriver::resizeWindow (glm::ivec2 size) { }

void HeadlessOpenGLDriver::resizeWindow (glm::ivec4 positionAndSize) { }

void HeadlessOpenGLDriver::showWindow () { }

void HeadlessOpenGLDriver::hideWindow () { }

glm::ivec2 HeadlessOpenGLDriver::getFramebufferSize () const {
    return { static_cast<int> (this->m_context.settings.render.framesink.width),
	     static_cast<int> (this->m_context.settings.render.framesink.height) };
}

uint32_t HeadlessOpenGLDriver::getFrameCounter () const { return this->m_frameCounter; }

void HeadlessOpenGLDriver::dispatchEventQueue () {
    static float startTime, endTime, minimumTime = 1.0f / this->m_context.settings.render.maximumFPS;
    startTime = this->getRenderTime ();

    if (!this->m_destinationRegistered) {
	this->m_destinationRegistered = true;
	this->getApp ().setDestinationFramebuffer (this->m_output->framebuffer ());
    }

    for (const auto& viewport : this->getOutput ().getViewports () | std::views::values) {
	this->getApp ().update (viewport);
    }

    this->m_frameCounter++;

    endTime = this->getRenderTime ();
    this->m_tickSumMs += (endTime - startTime) * 1000.0;

    if (this->m_frameCounter % 60 == 0) {
	const auto epoch = std::chrono::duration_cast<std::chrono::seconds> (
	    std::chrono::system_clock::now ().time_since_epoch ()
	).count ();
	sLog.out ("[heartbeat] driver frame=", this->m_frameCounter,
		  " renderTime=", this->getRenderTime (), " epoch=", epoch,
		  " tickAvg=", this->m_tickSumMs / 60.0, "ms");
	this->m_tickSumMs = 0.0;
    }

    if ((endTime - startTime) < minimumTime) {
	usleep ((minimumTime - (endTime - startTime)) * CLOCKS_PER_SEC);
    }
}

EGLDisplay HeadlessOpenGLDriver::eglDisplay () const { return this->m_eglDisplay; }

EGLContext HeadlessOpenGLDriver::eglDisplayContext () const { return this->m_eglContext; }

__attribute__ ((constructor)) void registerHeadlessOpenGLDriver () {
    sVideoFactories.registerDriver (
	ApplicationContext::FRAMESINK, DEFAULT_WINDOW_NAME,
	[] (ApplicationContext& context, WallpaperApplication& application) -> std::unique_ptr<VideoDriver> {
	    return std::make_unique<HeadlessOpenGLDriver> (context, application);
	}
    );
}
