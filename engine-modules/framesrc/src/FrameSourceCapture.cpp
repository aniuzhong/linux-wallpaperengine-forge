#include "FrameSourceCapture.h"

#include <cstring>
#include <vector>

namespace FrameSource {

Result capture (const std::filesystem::path& path, GLuint framebuffer, uint32_t width, uint32_t height, RowOrder rowOrder, Writer& writer) {
    if (width == 0 || height == 0 || path.empty ()) {
        return Result::BadArgs;
    }
    if (!writer.valid ()) {
        const Result openStatus = writer.open (path, width, height);
        if (openStatus != Result::Ok) {
            return openStatus;
        }
    } else if (writer.width () != width || writer.height () != height) {
        const Result resizeStatus = writer.resize (width, height);
        if (resizeStatus != Result::Ok) {
            return resizeStatus;
        }
    }

    static thread_local std::vector<uint8_t> raw;
    static thread_local std::vector<uint8_t> topDown;
    const size_t frameBytes = static_cast<size_t> (width) * height * 3;
    raw.resize (frameBytes);
    topDown.resize (frameBytes);

    GLint prevDraw = 0;
    GLint prevRead = 0;
    GLint prevPack = 0;
    GLint prevReadBuffer = 0;
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv (GL_PACK_ALIGNMENT, &prevPack);
    glGetIntegerv (GL_READ_BUFFER, &prevReadBuffer);

    glBindFramebuffer (GL_READ_FRAMEBUFFER, framebuffer);
    glReadBuffer (GL_COLOR_ATTACHMENT0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, raw.data ());
    const GLenum error = glGetError ();

    glBindFramebuffer (GL_READ_FRAMEBUFFER, static_cast<GLuint> (prevRead));
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, static_cast<GLuint> (prevDraw));
    glPixelStorei (GL_PACK_ALIGNMENT, prevPack);
    glReadBuffer (static_cast<GLenum> (prevReadBuffer));

    if (error != GL_NO_ERROR) {
        return Result::GlError;
    }

    const size_t stride = static_cast<size_t> (width) * 3;
    if (rowOrder == RowOrder::BottomUp) {
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy (topDown.data () + static_cast<size_t> (y) * stride, raw.data () + static_cast<size_t> (height - 1 - y) * stride, stride);
        }
        return writer.publish (topDown.data ());
    }
    return writer.publish (raw.data ());
}

GlStateSnapshot snapshotGlState () {
    GlStateSnapshot snapshot {};
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &snapshot.drawFramebuffer);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &snapshot.readFramebuffer);
    glGetIntegerv (GL_READ_BUFFER, &snapshot.readBuffer);
    glGetIntegerv (GL_PACK_ALIGNMENT, &snapshot.packAlignment);
    glGetIntegerv (GL_VIEWPORT, snapshot.viewport);
    glGetIntegerv (GL_SCISSOR_TEST, &snapshot.scissorTest);
    glGetIntegerv (GL_SCISSOR_BOX, snapshot.scissorBox);
    glGetIntegerv (GL_BLEND, &snapshot.blendEnabled);
    glGetFloatv (GL_COLOR_CLEAR_VALUE, reinterpret_cast<GLfloat*> (snapshot.clearColor));
    return snapshot;
}

bool glStateEqual (const GlStateSnapshot& a, const GlStateSnapshot& b) {
    return a.drawFramebuffer == b.drawFramebuffer
        && a.readFramebuffer == b.readFramebuffer
        && a.readBuffer == b.readBuffer
        && a.packAlignment == b.packAlignment
        && a.viewport[0] == b.viewport[0]
        && a.viewport[1] == b.viewport[1]
        && a.viewport[2] == b.viewport[2]
        && a.viewport[3] == b.viewport[3]
        && a.scissorTest == b.scissorTest
        && a.scissorBox[0] == b.scissorBox[0]
        && a.scissorBox[1] == b.scissorBox[1]
        && a.scissorBox[2] == b.scissorBox[2]
        && a.scissorBox[3] == b.scissorBox[3]
        && a.blendEnabled == b.blendEnabled
        && a.clearColor[0] == b.clearColor[0]
        && a.clearColor[1] == b.clearColor[1]
        && a.clearColor[2] == b.clearColor[2]
        && a.clearColor[3] == b.clearColor[3];
}

}
