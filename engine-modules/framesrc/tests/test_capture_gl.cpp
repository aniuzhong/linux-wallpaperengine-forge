#include "EglContext.h"
#include "FrameSource.h"
#include "FrameSourceCapture.h"
#include "FrameSourceReader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf ("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

int main () {
    EglSession session;
    std::string error;
    CHECK (eglMakeHeadlessCurrent (session, error));

    const std::filesystem::path path = "/tmp/framesrc-capture.bin";
    std::filesystem::remove (path);

    const uint32_t width = 480;
    const uint32_t height = 320;
    uint32_t framebuffer = 0;
    uint32_t texture = 0;
    CHECK (createTestFramebuffer (width, height, framebuffer, texture, error));

    FrameSource::Writer writer;

    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glClearColor (1.0f, 1.0f, 1.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);

    FrameSource::GlStateSnapshot before = FrameSource::snapshotGlState ();
    CHECK (FrameSource::capture (path, framebuffer, width, height, FrameSource::RowOrder::BottomUp, writer) == FrameSource::Result::Ok);
    FrameSource::GlStateSnapshot after = FrameSource::snapshotGlState ();
    CHECK (FrameSource::glStateEqual (before, after));

    FrameSource::Reader reader;
    CHECK (reader.open (path) == FrameSource::Result::Ok);
    std::vector<uint8_t> frame;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    CHECK (reader.readLatest (frame, outWidth, outHeight) == FrameSource::Result::Ok);
    CHECK (outWidth == width && outHeight == height);

    const size_t centerOffset = (static_cast<size_t> (height / 2) * width + width / 2) * 3;
    CHECK (frame[centerOffset + 0] == 255 && frame[centerOffset + 1] == 255 && frame[centerOffset + 2] == 255);

    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glEnable (GL_SCISSOR_TEST);
    glScissor (0, 0, width, height / 2);
    glClearColor (0.0f, 1.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glScissor (0, height / 2, width, height - height / 2);
    glClearColor (0.0f, 0.0f, 1.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glDisable (GL_SCISSOR_TEST);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);

    before = FrameSource::snapshotGlState ();
    CHECK (FrameSource::capture (path, framebuffer, width, height, FrameSource::RowOrder::BottomUp, writer) == FrameSource::Result::Ok);
    after = FrameSource::snapshotGlState ();
    CHECK (FrameSource::glStateEqual (before, after));

    CHECK (reader.readLatest (frame, outWidth, outHeight) == FrameSource::Result::Ok);
    const size_t topRowOffset = static_cast<size_t> (width / 2) * 3;
    const size_t bottomRowOffset = (static_cast<size_t> (height - 1) * width + width / 2) * 3;
    CHECK (frame[topRowOffset + 2] == 255 && frame[topRowOffset + 0] == 0 && frame[topRowOffset + 1] == 0);
    CHECK (frame[bottomRowOffset + 1] == 255 && frame[bottomRowOffset + 0] == 0 && frame[bottomRowOffset + 2] == 0);

    const uint64_t sequenceBefore = writer.sequence ();
    CHECK (FrameSource::capture (path, framebuffer, 0, height, FrameSource::RowOrder::BottomUp, writer) == FrameSource::Result::BadArgs);
    CHECK (FrameSource::capture (path, framebuffer, width, 0, FrameSource::RowOrder::BottomUp, writer) == FrameSource::Result::BadArgs);
    CHECK (writer.sequence () == sequenceBefore);
    before = FrameSource::snapshotGlState ();
    after = FrameSource::snapshotGlState ();
    CHECK (FrameSource::glStateEqual (before, after));

    uint64_t expected = writer.sequence ();
    for (int i = 0; i < 10; ++i) {
        glClearColor (static_cast<float> (i) / 10.0f, 0.5f, 0.5f, 1.0f);
        glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
        glClear (GL_COLOR_BUFFER_BIT);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        CHECK (FrameSource::capture (path, framebuffer, width, height, FrameSource::RowOrder::BottomUp, writer) == FrameSource::Result::Ok);
        expected += 1;
        CHECK (writer.sequence () == expected);
    }

    writer.close ();
    reader.close ();
    glDeleteFramebuffers (1, &framebuffer);
    glDeleteTextures (1, &texture);
    eglTeardown (session);

    std::printf ("capture_gl: all checks passed\n");
    return 0;
}
