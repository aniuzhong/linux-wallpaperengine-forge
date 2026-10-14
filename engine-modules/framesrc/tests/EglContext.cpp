#include "EglContext.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>
#include <vector>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

bool eglMakeHeadlessCurrent (EglSession& session, std::string& error) {
    EGLDisplay display = EGL_NO_DISPLAY;

#ifdef EGL_PLATFORM_SURFACELESS_MESA
    auto eglGetPlatformDisplayEXT = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (eglGetProcAddress ("eglGetPlatformDisplayEXT"));
    if (eglGetPlatformDisplayEXT != nullptr) {
        EGLDisplay surfaceless = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (surfaceless != EGL_NO_DISPLAY) {
            EGLint major = 0;
            EGLint minor = 0;
            if (eglInitialize (surfaceless, &major, &minor) == EGL_TRUE) {
                display = surfaceless;
                session.surfaceless = true;
            } else {
                eglTerminate (surfaceless);
            }
        }
    }
#endif

    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay (EGL_DEFAULT_DISPLAY);
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize (display, &major, &minor) != EGL_TRUE) {
            error = "eglInitialize failed";
            return false;
        }
    }

    if (eglBindAPI (EGL_OPENGL_API) != EGL_TRUE) {
        error = "eglBindAPI(OPENGL) failed";
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint configCount = 0;
    if (eglChooseConfig (display, configAttribs, &config, 1, &configCount) != EGL_TRUE || configCount < 1) {
        error = "eglChooseConfig failed";
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };
    EGLContext context = eglCreateContext (display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        context = eglCreateContext (display, config, EGL_NO_CONTEXT, nullptr);
    }
    if (context == EGL_NO_CONTEXT) {
        error = "eglCreateContext failed";
        return false;
    }

    if (eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_TRUE) {
        session.display = display;
        session.context = context;
        session.surface = EGL_NO_SURFACE;
        session.surfaceless = true;
        return true;
    }

    const EGLint pbufferAttribs[] = {
        EGL_WIDTH, 512,
        EGL_HEIGHT, 512,
        EGL_NONE
    };
    EGLSurface surface = eglCreatePbufferSurface (display, config, pbufferAttribs);
    if (surface == EGL_NO_SURFACE) {
        error = "no surfaceless context and pbuffer creation failed";
        return false;
    }
    if (eglMakeCurrent (display, surface, surface, context) != EGL_TRUE) {
        error = "eglMakeCurrent(pbuffer) failed";
        return false;
    }
    session.display = display;
    session.context = context;
    session.surface = surface;
    session.surfaceless = false;
    return true;
}

void eglTeardown (EglSession& session) {
    if (session.display != nullptr) {
        eglMakeCurrent (static_cast<EGLDisplay> (session.display), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (session.surface != nullptr) {
            eglDestroySurface (static_cast<EGLDisplay> (session.display), static_cast<EGLSurface> (session.surface));
        }
        if (session.context != nullptr) {
            eglDestroyContext (static_cast<EGLDisplay> (session.display), static_cast<EGLContext> (session.context));
        }
        eglTerminate (static_cast<EGLDisplay> (session.display));
    }
    session = {};
}

bool createTestFramebuffer (uint32_t width, uint32_t height, uint32_t& framebuffer, uint32_t& texture, std::string& error) {
    GLuint fbo = 0;
    GLuint tex = 0;
    glGenFramebuffers (1, &fbo);
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, static_cast<GLsizei> (width), static_cast<GLsizei> (height), 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        error = "test framebuffer incomplete";
        return false;
    }
    framebuffer = fbo;
    texture = tex;
    return true;
}
