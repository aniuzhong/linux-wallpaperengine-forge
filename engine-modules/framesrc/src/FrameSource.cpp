#include "FrameSource.h"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace FrameSource {

const char* resultText (Result status) {
    switch (status) {
        case Result::Ok: return "ok";
        case Result::OpenFailed: return "open failed";
        case Result::SizeFailed: return "size failed";
        case Result::MapFailed: return "map failed";
        case Result::BadArgs: return "bad arguments";
        case Result::BadHeader: return "bad header";
        case Result::GlError: return "gl error";
        case Result::NoFrame: return "no frame yet";
        case Result::TornFrame: return "torn frame";
    }
    return "unknown";
}

namespace {

Header* headerOf (uint8_t* map) {
    return reinterpret_cast<Header*> (map);
}

}

Writer::~Writer () {
    close ();
}

Result Writer::open (const std::filesystem::path& path, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return Result::BadArgs;
    }
    close ();

    m_fd = ::open (path.c_str (), O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (m_fd < 0) {
        m_fd = -1;
        return Result::OpenFailed;
    }

    const size_t total = HeaderSize + static_cast<size_t> (width) * height * 3;
    if (ftruncate (m_fd, static_cast<off_t> (total)) != 0) {
        close ();
        return Result::SizeFailed;
    }

    m_map = static_cast<uint8_t*> (mmap (nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0));
    if (m_map == MAP_FAILED) {
        m_map = nullptr;
        close ();
        return Result::MapFailed;
    }
    m_size = total;
    m_width = width;
    m_height = height;

    auto* header = headerOf (m_map);
    header->magic = Magic;
    header->version = Version;
    header->width = width;
    header->height = height;
    header->stride = width * 3;
    header->format = FormatRgb888;
    header->vflip = 0;
    header->reserved = 0;
    header->sequence = 0;
    m_sequence = 0;
    return Result::Ok;
}

Result Writer::resize (uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || !valid ()) {
        return Result::BadArgs;
    }
    if (width == m_width && height == m_height) {
        return Result::Ok;
    }

    const size_t needed = HeaderSize + static_cast<size_t> (width) * height * 3;
    if (needed > m_size) {
        if (ftruncate (m_fd, static_cast<off_t> (needed)) != 0) {
            return Result::SizeFailed;
        }
        munmap (m_map, m_size);
        m_map = static_cast<uint8_t*> (mmap (nullptr, needed, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0));
        if (m_map == MAP_FAILED) {
            m_map = nullptr;
            m_size = 0;
            return Result::MapFailed;
        }
        m_size = needed;
        auto* header = headerOf (m_map);
        header->magic = Magic;
        header->version = Version;
        header->format = FormatRgb888;
        header->vflip = 0;
        header->reserved = 0;
    }

    auto* header = headerOf (m_map);
    header->width = width;
    header->height = height;
    header->stride = width * 3;
    __atomic_store_n (&header->sequence, m_sequence, __ATOMIC_RELEASE);
    m_width = width;
    m_height = height;
    return Result::Ok;
}

Result Writer::publish (const uint8_t* pixels) {
    if (!valid () || pixels == nullptr) {
        return Result::BadArgs;
    }
    const size_t frameBytes = static_cast<size_t> (m_width) * m_height * 3;
    std::memcpy (m_map + HeaderSize, pixels, frameBytes);
    auto* header = headerOf (m_map);
    __atomic_store_n (&header->sequence, ++m_sequence, __ATOMIC_RELEASE);
    return Result::Ok;
}

void Writer::close () {
    if (m_map != nullptr) {
        munmap (m_map, m_size);
        m_map = nullptr;
    }
    if (m_fd >= 0) {
        ::close (m_fd);
        m_fd = -1;
    }
    m_size = 0;
    m_width = 0;
    m_height = 0;
    m_sequence = 0;
}

uint32_t Writer::width () const {
    return m_width;
}

uint32_t Writer::height () const {
    return m_height;
}

uint64_t Writer::sequence () const {
    return m_sequence;
}

bool Writer::valid () const {
    return m_map != nullptr;
}

}
