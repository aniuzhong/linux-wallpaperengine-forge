#include "FrameSinkOutput.h"

#include "../HeadlessOpenGLDriver.h"
#include "WallpaperEngine/Logging/Log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_FD_ANDROID
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#endif
#ifndef EGL_NO_SYNC_KHR
#define EGL_NO_SYNC_KHR ((EGLSyncKHR) 0)
#endif

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
	// sLog.out ("framesink viewport makeCurrent");
    }

    void swapOutput () override {
	m_output->captureFrame (uint32_t (viewport.z), uint32_t (viewport.w));
    }

private:
    FrameSinkOutput* m_output;
};

struct CaptureTiming {
    FrameSinkOutput* out;
    std::chrono::steady_clock::time_point start;
    ~CaptureTiming () {
	const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
	out->m_captureSamples++;
	out->m_captureSumMs += ms;
	if (ms > out->m_captureMaxMs)
	    out->m_captureMaxMs = ms;
	if (out->m_captureSamples >= 300) {
	    sLog.out ("framesink capture: avg=", out->m_captureSumMs / out->m_captureSamples, "ms max=", out->m_captureMaxMs,
		"ms over ", out->m_captureSamples, " frames (transport=", out->m_transport == FrameSinkOutput::Transport::DmaBuf ? "dmabuf" : "shm", ")");
	    out->m_captureSamples = 0;
	    out->m_captureSumMs = 0.0;
	    out->m_captureMaxMs = 0.0;
	}
    }
};
}

FrameSinkOutput::FrameSinkOutput (ApplicationContext& context, VideoDriver& driver, FrameSource::Writer& writer) :
    Output (context, driver), m_writer (writer) {
    m_channelPath = context.settings.frameOut.path.string () + ".sock";
    this->m_fullWidth = static_cast<int> (context.settings.render.framesink.width);
    this->m_fullHeight = static_cast<int> (context.settings.render.framesink.height);

    this->setupFramebuffer (this->m_fullWidth, this->m_fullHeight);

    this->m_viewports["default"] = new FrameSinkOutputViewport {
	{ 0, 0, this->m_fullWidth, this->m_fullHeight }, "default", this
    };
}

FrameSinkOutput::~FrameSinkOutput () {
    releaseSlots ();
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
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei> (width), static_cast<GLsizei> (height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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
    setupTransport (width, height);
}

void FrameSinkOutput::setupTransport (uint32_t width, uint32_t height) {
    const char *opt = getenv ("LWE_FRAME_TRANSPORT");
    const bool allowDmaBuf = !opt || opt[0] != 's';
    if (allowDmaBuf && probeDmaBufExport ()) {
        auto createImage = (EGLImageKHR (*) (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *))
            eglGetProcAddress ("eglCreateImageKHR");
        auto queryDmaBuf = (EGLBoolean (*) (EGLDisplay, EGLImageKHR, int *, int *, uint64_t *))
            eglGetProcAddress ("eglExportDMABUFImageQueryMESA");
        auto exportDmaBuf = (EGLBoolean (*) (EGLDisplay, void *, int *, EGLint *, EGLint *))
            eglGetProcAddress ("eglExportDMABUFImageMESA");
        int fourcc = 0, stride = 0;
        uint64_t modifier = 0;
        for (int i = 0; i < kSlotCount; ++i) {
            glGenTextures (1, &m_slotTextures[i]);
            glBindTexture (GL_TEXTURE_2D, m_slotTextures[i]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei> (width), static_cast<GLsizei> (height),
                0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            const EGLint attrs[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
            m_eglImages[i] = createImage (m_eglDisplayHandle, eglGetCurrentContext (),
                EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer) (uintptr_t) m_slotTextures[i], attrs);
            int slotFourcc = 0, slotPlanes = 0, slotStride = 0, slotOffset = 0;
            uint64_t slotModifier = 0;
            if (m_eglImages[i] == nullptr
                || !queryDmaBuf (m_eglDisplayHandle, m_eglImages[i], &slotFourcc, &slotPlanes, &slotModifier)
                || !exportDmaBuf (m_eglDisplayHandle, m_eglImages[i], &m_dmabufFds[i], &slotStride, &slotOffset)
                || m_dmabufFds[i] < 0 || slotPlanes != 1 || slotFourcc == 0 || slotStride <= 0
                || (i > 0 && (slotFourcc != fourcc || slotStride != stride || slotModifier != modifier))) {
                sLog.error ("framesink: slot ", i, " dmabuf export failed, transport degraded");
                releaseSlots ();
                m_transport = Transport::Shm;
                sLog.out ("framesink transport: shm");
                return;
            }
            glBindTexture (GL_TEXTURE_2D, 0);
            glGenFramebuffers (1, &m_slotFramebuffers[i]);
            glBindFramebuffer (GL_FRAMEBUFFER, m_slotFramebuffers[i]);
            glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_slotTextures[i], 0);
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
            fourcc = slotFourcc;
            stride = slotStride;
            modifier = slotModifier;
        }
        FrameSource::FrameChannelHello hello {};
        hello.magic[0] = 'L'; hello.magic[1] = 'W'; hello.magic[2] = 'E'; hello.magic[3] = '2';
        hello.version = 1;
        hello.width = width; hello.height = height;
        hello.fourcc = uint32_t (fourcc);
        hello.stride = uint32_t (stride);
        hello.modifier = modifier;
        hello.slotCount = kSlotCount;
        const char* eglExts = eglQueryString (reinterpret_cast<EGLDisplay> (m_eglDisplayHandle), EGL_EXTENSIONS);
        sLog.out ("framesink fence: EGL_ANDROID_native_fence_sync ",
            (m_dupNativeFence && eglExts && strstr (eglExts, "EGL_ANDROID_native_fence_sync")) ? "supported" : "missing");
        m_channel.start (m_channelPath, hello, std::vector<int> (m_dmabufFds, m_dmabufFds + kSlotCount));
        m_transport = Transport::DmaBuf;
        char fcc[5] = {char (fourcc & 0xff), char ((fourcc >> 8) & 0xff), char ((fourcc >> 16) & 0xff),
                       char ((fourcc >> 24) & 0xff), 0};
        char mod[32];
        snprintf (mod, sizeof (mod), "0x%016llx", (unsigned long long) modifier);
        sLog.out ("framesink transport: dmabuf (", kSlotCount, " slots, fourcc=", fcc,
            ", stride=", stride, ", modifier=", mod, ")");
    } else {
        m_transport = Transport::Shm;
        sLog.out ("framesink transport: shm");
    }
}

void FrameSinkOutput::releaseSlots () {
    auto destroyImage = (EGLBoolean (*) (EGLDisplay, EGLImageKHR)) eglGetProcAddress ("eglDestroyImageKHR");
    for (int i = 0; i < kSlotCount; ++i) {
        if (m_consumerFence[i] >= 0) {
            close (m_consumerFence[i]);
            m_consumerFence[i] = -1;
        }
        m_inFlight[i] = false;
        if (m_dmabufFds[i] >= 0) {
            close (m_dmabufFds[i]);
            m_dmabufFds[i] = -1;
        }
        if (destroyImage && m_eglImages[i])
            destroyImage (reinterpret_cast<EGLDisplay> (m_eglDisplayHandle), static_cast<EGLImageKHR> (m_eglImages[i]));
        m_eglImages[i] = nullptr;
        if (m_slotTextures[i] != 0) {
            glDeleteTextures (1, &m_slotTextures[i]);
            m_slotTextures[i] = 0;
        }
        if (m_slotFramebuffers[i] != 0) {
            glDeleteFramebuffers (1, &m_slotFramebuffers[i]);
            m_slotFramebuffers[i] = 0;
        }
    }
}

bool FrameSinkOutput::probeDmaBufExport () {
    m_dupNativeFence = reinterpret_cast<void *> (eglGetProcAddress ("eglDupNativeFenceFDANDROID"));
    m_eglDisplayHandle = eglDisplay ();
    const int w = 256, h = 256;
    auto createImage = reinterpret_cast<EGLImageKHR (*) (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *)>
	(eglGetProcAddress ("eglCreateImageKHR"));
    auto queryDmaBuf = reinterpret_cast<EGLBoolean (*) (EGLDisplay, EGLImageKHR, int *, int *, uint64_t *)>
	(eglGetProcAddress ("eglExportDMABUFImageQueryMESA"));
    auto exportDmaBuf = reinterpret_cast<EGLBoolean (*) (EGLDisplay, void *, int *, EGLint *, EGLint *)>
	(eglGetProcAddress ("eglExportDMABUFImageMESA"));
    const char *variants[] = {"mutable-teximage", "immutable-texstorage"};
    for (int variant = 0; variant < 2; ++variant) {
	GLuint tex = 0;
	glGenTextures (1, &tex);
	glBindTexture (GL_TEXTURE_2D, tex);
	std::vector<uint8_t> pattern (size_t (w) * h * 4, 0);
	for (size_t k = 0; k < pattern.size (); k += 4) { pattern[k] = 0x10; pattern[k+1] = 0x20; pattern[k+2] = 0x40; pattern[k+3] = 0xFF; }
	if (variant == 0) {
	    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data ());
	} else {
	    glTexStorage2D (GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
	    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data ());
	}
	glFinish ();
	const EGLint attrs[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
	EGLImageKHR img = createImage (m_eglDisplayHandle, eglGetCurrentContext (), EGL_GL_TEXTURE_2D_KHR,
	    (EGLClientBuffer) (uintptr_t) tex, attrs);
	if (img == EGL_NO_IMAGE_KHR) {
	    sLog.out ("probe: variant ", variants[variant], " create fail eglErr=", (int) eglGetError ());
	    glDeleteTextures (1, &tex);
	    continue;
	}
	int fd = -1, stride = 0, offset = 0;
	int fourcc = 0, planes = 0;
	uint64_t modifier = 0;
	if (!queryDmaBuf (m_eglDisplayHandle, img, &fourcc, &planes, &modifier)
	    || !exportDmaBuf (m_eglDisplayHandle, img, &fd, &stride, &offset) || fd < 0) {
	    sLog.out ("probe: variant ", variants[variant], " export fail");
	    if (fd >= 0) close (fd);
	    glDeleteTextures (1, &tex);
	    continue;
	}
	close (fd);
	glDeleteTextures (1, &tex);
	sLog.out ("probe: variant ", variants[variant], " OK");
	return true;
    }
    sLog.error ("probe: all variants failed");
    return false;
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

int FrameSinkOutput::acquireSlot () {
    if (m_channel.justHelloed ()) {
        for (int i = 0; i < kSlotCount; ++i)
            m_inFlight[i] = false;
    }
    const int want = int (m_frameSeq % kSlotCount);
    const auto freeSlot = [this, want] () -> int {
        int freeCount = 0;
        for (int i = 0; i < kSlotCount; ++i)
            if (!m_inFlight[i])
                ++freeCount;
        if (freeCount < 2)
            return -1;
        for (int i = 0; i < kSlotCount; ++i) {
            const int idx = (want + i) % kSlotCount;
            if (!m_inFlight[idx])
                return idx;
        }
        return -1;
    };
    int idx = freeSlot ();
    if (idx >= 0 || m_legacyConsumer || m_channel.peerGone ())
        return idx >= 0 ? idx : want;
    const auto waitStart = std::chrono::steady_clock::now ();
    const auto deadline = waitStart + std::chrono::milliseconds (200);
    while (idx < 0) {
        if (m_channel.waitReadable (50)) {
            uint32_t slot = 0;
            uint64_t seq = 0;
            int fenceFd = -1;
            while (m_channel.recvRelease (slot, seq, fenceFd)) {
                m_releaseSeen = true;
                if (slot < uint32_t (kSlotCount)) {
                    if (m_consumerFence[slot] >= 0)
                        close (m_consumerFence[slot]);
                    m_consumerFence[slot] = fenceFd;
                    m_inFlight[slot] = false;
                } else if (fenceFd >= 0) {
                    close (fenceFd);
                }
            }
            idx = freeSlot ();
        }
        if (std::chrono::steady_clock::now () >= deadline)
            break;
    }
    if (idx < 0) {
        if (!m_releaseSeen) {
            m_legacyConsumer = true;
            sLog.out ("framesink: consumer does not ack releases, slot gating disabled");
        } else if (++m_forceStreak % 300 == 1) {
            sLog.error ("framesink: slot wait timeout, forcing slot (consumer stalled?)");
        }
        return want;
    }
    m_forceStreak = 0;
    const auto waitedMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - waitStart).count ();
    if (waitedMs > 40.0) {
        static int waitLogs = 0;
        if (++waitLogs % 100 == 1)
            sLog.out ("[gating] slot wait ", waitedMs, "ms (release cadence)");
    }
    return idx;
}

void FrameSinkOutput::waitConsumerFence (int idx) {
    const int fd = m_consumerFence[idx];
    if (fd < 0)
        return;
    m_consumerFence[idx] = -1;
    const auto fenceStart = std::chrono::steady_clock::now ();
    EGLDisplay display = reinterpret_cast<EGLDisplay> (m_eglDisplayHandle);
    auto createSync = reinterpret_cast<EGLSyncKHR (*) (EGLDisplay, EGLenum, const EGLint *)>
        (eglGetProcAddress ("eglCreateSyncKHR"));
    auto destroySync = reinterpret_cast<EGLBoolean (*) (EGLDisplay, EGLSyncKHR)>
        (eglGetProcAddress ("eglDestroySyncKHR"));
    auto waitSync = reinterpret_cast<EGLBoolean (*) (EGLDisplay, EGLSyncKHR, EGLint)>
        (eglGetProcAddress ("eglWaitSyncKHR"));
    auto clientWaitSync = reinterpret_cast<EGLint (*) (EGLDisplay, EGLSyncKHR, EGLint, GLuint64)>
        (eglGetProcAddress ("eglClientWaitSyncKHR"));
    if (!createSync || !destroySync) {
        close (fd);
        return;
    }
    const EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    EGLSyncKHR sync = createSync (display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (sync == EGL_NO_SYNC_KHR) {
        close (fd);
        return;
    }
    if (waitSync && waitSync (display, sync, 0) != EGL_TRUE && clientWaitSync)
        clientWaitSync (display, sync, 0, GLuint64 (100000000));
    destroySync (display, sync);
    const auto fenceMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - fenceStart).count ();
    if (fenceMs > 40.0) {
        static int fenceLogs = 0;
        if (++fenceLogs % 100 == 1)
            sLog.out ("[gating] fence wait ", fenceMs, "ms (consumer gpu backlog)");
    }
}

void FrameSinkOutput::captureFrame (uint32_t width, uint32_t height) {
    CaptureTiming timing {this, std::chrono::steady_clock::now ()};
    if (m_transport == Transport::DmaBuf) {
        const int idx = acquireSlot ();
        waitConsumerFence (idx);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, m_framebuffer);
        glReadBuffer (GL_COLOR_ATTACHMENT0);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glBindTexture (GL_TEXTURE_2D, m_slotTextures[idx]);
        glCopyTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 0, 0, static_cast<GLsizei> (width), static_cast<GLsizei> (height));
        const GLenum copyError = glGetError ();
        glBindTexture (GL_TEXTURE_2D, 0);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);
        if (copyError != GL_NO_ERROR) {
            sLog.error ("framesink dmabuf copy failed with GL error ", copyError);
            return;
        }
        glBindFramebuffer (GL_FRAMEBUFFER, m_slotFramebuffers[idx]);
        glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        int fenceFd = -1;
        EGLDisplay display = reinterpret_cast<EGLDisplay> (m_eglDisplayHandle);
        if (m_dupNativeFence) {
            auto createSync = reinterpret_cast<EGLSyncKHR (*) (EGLDisplay, EGLenum, const EGLint *)>
                (eglGetProcAddress ("eglCreateSyncKHR"));
            auto destroySync = reinterpret_cast<EGLBoolean (*) (EGLDisplay, EGLSyncKHR)>
                (eglGetProcAddress ("eglDestroySyncKHR"));
            auto dupFence = reinterpret_cast<int (*) (EGLDisplay, EGLSyncKHR)> (m_dupNativeFence);
            if (createSync && destroySync) {
                EGLSyncKHR sync = createSync (display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
                if (sync != EGL_NO_SYNC_KHR) {
                    glFlush ();
                    fenceFd = dupFence (display, sync);
                    destroySync (display, sync);
                }
            }
        }
        if (fenceFd < 0) {
            if (!m_fenceWarned) {
                m_fenceWarned = true;
                sLog.error ("framesink fence: inactive, consumer samples slots without gpu sync");
            }
            glFlush ();
        }
        if (++m_frameSeq % 60 == 0)
            sLog.out ("[transport] dmabuf frame sent seq=", m_frameSeq);
        m_inFlight[idx] = true;
        m_channel.sendFrame (m_frameSeq, idx, fenceFd);
        return;
    }
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
