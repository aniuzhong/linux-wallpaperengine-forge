#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace FrameSource {

enum class Result {
    Ok,
    OpenFailed,
    SizeFailed,
    MapFailed,
    BadArgs,
    BadHeader,
    GlError,
    NoFrame,
    TornFrame,
};

const char* resultText (Result status);

enum : uint32_t {
    Magic = 0x4C574531u,
    Version = 1u,
    FormatRgb888 = 1u,
};

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t vflip;
    uint32_t reserved;
    uint64_t sequence;
};

inline constexpr size_t HeaderSize = sizeof (Header);
static_assert (sizeof (Header) == 40, "header layout must stay 8-byte aligned and 40 bytes on the wire");

class Writer {
public:
    Writer () = default;
    ~Writer ();
    Writer (const Writer&) = delete;
    Writer& operator = (const Writer&) = delete;

    Result open (const std::filesystem::path& path, uint32_t width, uint32_t height);
    Result resize (uint32_t width, uint32_t height);
    Result publish (const uint8_t* pixels);
    void close ();

    uint32_t width () const;
    uint32_t height () const;
    uint64_t sequence () const;
    bool valid () const;

private:
    int m_fd = -1;
    uint8_t* m_map = nullptr;
    size_t m_size = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_sequence = 0;
};

}
