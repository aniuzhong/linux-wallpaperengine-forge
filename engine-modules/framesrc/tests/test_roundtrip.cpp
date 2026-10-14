#include "FrameSource.h"
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

namespace {

std::vector<uint8_t> makeGradient (uint32_t width, uint32_t height, uint8_t seed) {
    std::vector<uint8_t> pixels (static_cast<size_t> (width) * height * 3);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t> (y) * width + x) * 3;
            pixels[offset + 0] = static_cast<uint8_t> (x * 251u % 256u);
            pixels[offset + 1] = static_cast<uint8_t> (y * 241u % 256u);
            pixels[offset + 2] = seed;
        }
    }
    return pixels;
}

}

int main () {
    const std::filesystem::path path = "/tmp/framesrc-roundtrip.bin";
    std::filesystem::remove (path);

    FrameSource::Writer writer;
    CHECK (writer.open (path, 480, 320) == FrameSource::Result::Ok);
    CHECK (writer.sequence () == 0);
    CHECK (std::filesystem::file_size (path) == FrameSource::HeaderSize + static_cast<uintmax_t> (480) * 320 * 3);

    FrameSource::Reader reader;
    CHECK (reader.open (path) == FrameSource::Result::Ok);

    std::vector<uint8_t> frame;
    uint32_t width = 0;
    uint32_t height = 0;
    CHECK (reader.readLatest (frame, width, height) == FrameSource::Result::NoFrame);

    const std::vector<uint8_t> patternA = makeGradient (480, 320, 7);
    CHECK (writer.publish (patternA.data ()) == FrameSource::Result::Ok);
    CHECK (writer.sequence () == 1);
    CHECK (reader.readLatest (frame, width, height) == FrameSource::Result::Ok);
    CHECK (width == 480 && height == 320);
    CHECK (std::memcmp (frame.data (), patternA.data (), patternA.size ()) == 0);

    const std::vector<uint8_t> patternB = makeGradient (480, 320, 200);
    CHECK (writer.publish (patternB.data ()) == FrameSource::Result::Ok);
    CHECK (writer.sequence () == 2);
    CHECK (reader.readLatest (frame, width, height) == FrameSource::Result::Ok);
    CHECK (std::memcmp (frame.data (), patternB.data (), patternB.size ()) == 0);

    CHECK (writer.resize (640, 512) == FrameSource::Result::Ok);
    const std::vector<uint8_t> patternC = makeGradient (640, 512, 99);
    CHECK (writer.publish (patternC.data ()) == FrameSource::Result::Ok);
    CHECK (writer.sequence () == 3);
    CHECK (reader.readLatest (frame, width, height) == FrameSource::Result::Ok);
    CHECK (width == 640 && height == 512);
    CHECK (std::memcmp (frame.data (), patternC.data (), patternC.size ()) == 0);
    const uintmax_t sizeAfterGrow = std::filesystem::file_size (path);
    CHECK (sizeAfterGrow >= FrameSource::HeaderSize + static_cast<uintmax_t> (640) * 512 * 3);

    CHECK (writer.resize (320, 240) == FrameSource::Result::Ok);
    const std::vector<uint8_t> patternD = makeGradient (320, 240, 41);
    CHECK (writer.publish (patternD.data ()) == FrameSource::Result::Ok);
    CHECK (writer.sequence () == 4);
    CHECK (std::filesystem::file_size (path) == sizeAfterGrow);
    CHECK (reader.readLatest (frame, width, height) == FrameSource::Result::Ok);
    CHECK (width == 320 && height == 240);
    CHECK (std::memcmp (frame.data (), patternD.data (), patternD.size ()) == 0);

    CHECK (writer.publish (nullptr) == FrameSource::Result::BadArgs);
    CHECK (writer.sequence () == 4);

    uint64_t previous = writer.sequence ();
    for (int i = 0; i < 100; ++i) {
        CHECK (writer.publish (patternD.data ()) == FrameSource::Result::Ok);
        CHECK (writer.sequence () == previous + 1);
        previous = writer.sequence ();
    }
    CHECK (reader.readLatest (frame, width, height) == FrameSource::Result::Ok);
    CHECK (std::memcmp (frame.data (), patternD.data (), patternD.size ()) == 0);

    writer.close ();
    reader.close ();

    FrameSource::Writer restart;
    CHECK (restart.open (path, 64, 48) == FrameSource::Result::Ok);
    CHECK (std::filesystem::file_size (path) == FrameSource::HeaderSize + static_cast<uintmax_t> (64) * 48 * 3);
    CHECK (restart.sequence () == 0);

    FrameSource::Reader readerAfterRestart;
    CHECK (readerAfterRestart.open (path) == FrameSource::Result::Ok);
    std::vector<uint8_t> restartFrame;
    uint32_t restartWidth = 0;
    uint32_t restartHeight = 0;
    CHECK (readerAfterRestart.readLatest (restartFrame, restartWidth, restartHeight) == FrameSource::Result::NoFrame);

    FrameSource::Writer badWriter;
    CHECK (badWriter.open ("/nonexistent-dir-framesrc/test.bin", 64, 48) == FrameSource::Result::OpenFailed);
    CHECK (badWriter.publish (patternD.data ()) == FrameSource::Result::BadArgs);
    CHECK (badWriter.resize (10, 10) == FrameSource::Result::BadArgs);

    std::printf ("roundtrip: all checks passed\n");
    return 0;
}
