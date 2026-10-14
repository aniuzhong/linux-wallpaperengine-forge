#include "EglContext.h"
#include "FrameSource.h"
#include "FrameSourceCapture.h"
#include "FrameSourceReader.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf ("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

namespace {

struct PerfResult {
    double avgMs;
    double effectiveFps;
    double bandwidthMBs;
};

PerfResult measure (uint32_t width, uint32_t height, int frames, const std::string& label) {
    uint32_t framebuffer = 0;
    uint32_t texture = 0;
    std::string error;
    if (!createTestFramebuffer (width, height, framebuffer, texture, error)) {
        std::printf ("FAIL perf %ux%u: %s\n", width, height, error.c_str ());
        return { -1.0, 0.0, 0.0 };
    }

    FrameSource::Writer writer;
    const std::string path = "/tmp/framesrc-perf-" + std::to_string (width) + "x" + std::to_string (height) + ".bin";
    std::filesystem::remove (path);

    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glViewport (0, 0, static_cast<GLsizei> (width), static_cast<GLsizei> (height));

    for (int warmup = 0; warmup < 10; ++warmup) {
        glClearColor (0.2f, 0.4f, 0.6f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        if (FrameSource::capture (path, framebuffer, width, height, FrameSource::RowOrder::BottomUp, writer) != FrameSource::Result::Ok) {
            std::printf ("FAIL perf %ux%u: capture failed during warmup\n", width, height);
            return { -1.0, 0.0, 0.0 };
        }
    }

    const auto start = std::chrono::steady_clock::now ();
    for (int i = 0; i < frames; ++i) {
        const float channel = static_cast<float> (i % 256) / 255.0f;
        glClearColor (channel, 0.5f, 1.0f - channel, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        if (FrameSource::capture (path, framebuffer, width, height, FrameSource::RowOrder::BottomUp, writer) != FrameSource::Result::Ok) {
            std::printf ("FAIL perf %ux%u: capture failed at frame %d\n", width, height, i);
            return { -1.0, 0.0, 0.0 };
        }
    }
    const auto end = std::chrono::steady_clock::now ();
    const double totalMs = std::chrono::duration<double, std::milli> (end - start).count ();
    const double avgMs = totalMs / frames;
    const double effectiveFps = avgMs > 0.0 ? 1000.0 / avgMs : 0.0;
    const double bandwidthMBs = (static_cast<double> (width) * height * 3 * frames / (1024.0 * 1024.0)) / (totalMs / 1000.0);

    std::printf ("perf %ux%u: avg=%.2f ms/frame effective=%.1f fps bandwidth=%.0f MB/s (%d frames)\n",
                 width, height, avgMs, effectiveFps, bandwidthMBs, frames);

    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers (1, &framebuffer);
    glDeleteTextures (1, &texture);
    writer.close ();
    std::filesystem::remove (path);
    std::filesystem::remove (path + ".writer");
    return { avgMs, effectiveFps, bandwidthMBs };
}

}

int main () {
    EglSession session;
    std::string error;
    CHECK (eglMakeHeadlessCurrent (session, error));

    const PerfResult small = measure (480, 320, 300, "small");
    const PerfResult screen = measure (1366, 768, 300, "screen-size");
    const PerfResult fullHd = measure (1920, 1080, 300, "fullhd");

    eglTeardown (session);

    CHECK (small.avgMs >= 0.0 && small.avgMs < 50.0);
    CHECK (screen.avgMs >= 0.0 && screen.avgMs < 100.0);
    CHECK (fullHd.avgMs >= 0.0 && fullHd.avgMs < 100.0);

    std::printf ("perf: all checks passed\n");
    return 0;
}
