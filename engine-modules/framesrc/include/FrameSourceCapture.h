#pragma once

#include "FrameSource.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

namespace FrameSource {

enum class RowOrder {
    TopDown,
    BottomUp,
};

Result capture (const std::filesystem::path& path, GLuint framebuffer, uint32_t width, uint32_t height, RowOrder rowOrder, Writer& writer);

struct GlStateSnapshot {
    GLint drawFramebuffer;
    GLint readFramebuffer;
    GLint readBuffer;
    GLint packAlignment;
    GLint viewport[4];
    GLint scissorTest;
    GLint scissorBox[4];
    GLint blendEnabled;
    GLint clearColor[4];
};

GlStateSnapshot snapshotGlState ();
bool glStateEqual (const GlStateSnapshot& a, const GlStateSnapshot& b);

}
