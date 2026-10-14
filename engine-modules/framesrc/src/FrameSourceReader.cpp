#include "FrameSourceReader.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace FrameSource {

namespace {

const Header* headerOf (const uint8_t* map) {
    return reinterpret_cast<const Header*> (map);
}

}

Reader::~Reader () {
    close ();
}

Result Reader::open (const std::filesystem::path& path) {
    close ();

    m_fd = ::open (path.c_str (), O_RDONLY);
    if (m_fd < 0) {
        m_fd = -1;
        return Result::OpenFailed;
    }

    struct stat st {};
    if (fstat (m_fd, &st) != 0 || st.st_size < static_cast<off_t> (HeaderSize)) {
        close ();
        return Result::SizeFailed;
    }

    m_map = static_cast<uint8_t*> (mmap (nullptr, st.st_size, PROT_READ, MAP_SHARED, m_fd, 0));
    if (m_map == MAP_FAILED) {
        m_map = nullptr;
        close ();
        return Result::MapFailed;
    }
    m_size = static_cast<size_t> (st.st_size);

    const Header* header = headerOf (m_map);
    if (header->magic != Magic || header->version != Version) {
        close ();
        return Result::BadHeader;
    }
    return Result::Ok;
}

Result Reader::readLatest (std::vector<uint8_t>& outTopDown, uint32_t& width, uint32_t& height) {
    if (!valid ()) {
        return Result::OpenFailed;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint64_t seq0 = __atomic_load_n (&headerOf (m_map)->sequence, __ATOMIC_ACQUIRE);
        if (seq0 == 0) {
            return Result::NoFrame;
        }

        const uint32_t readWidth = headerOf (m_map)->width;
        const uint32_t readHeight = headerOf (m_map)->height;
        const uint32_t readStride = headerOf (m_map)->stride;
        const uint32_t readVflip = headerOf (m_map)->vflip;
        const size_t needed = HeaderSize + static_cast<size_t> (readStride) * readHeight;

        if (needed > m_size) {
            struct stat st {};
            if (fstat (m_fd, &st) != 0 || static_cast<size_t> (st.st_size) < needed) {
                return Result::NoFrame;
            }
            munmap (m_map, m_size);
            m_map = static_cast<uint8_t*> (mmap (nullptr, st.st_size, PROT_READ, MAP_SHARED, m_fd, 0));
            if (m_map == MAP_FAILED) {
                m_map = nullptr;
                m_size = 0;
                return Result::MapFailed;
            }
            m_size = static_cast<size_t> (st.st_size);
        }

        outTopDown.resize (static_cast<size_t> (readStride) * readHeight);
        const uint8_t* base = m_map + HeaderSize;
        const bool flip = readVflip != 0;
        for (uint32_t y = 0; y < readHeight; ++y) {
            const uint32_t sy = flip ? (readHeight - 1 - y) : y;
            std::memcpy (outTopDown.data () + static_cast<size_t> (y) * readStride, base + static_cast<size_t> (sy) * readStride, readStride);
        }

        const uint64_t seq1 = __atomic_load_n (&headerOf (m_map)->sequence, __ATOMIC_ACQUIRE);
        if (seq1 == seq0) {
            width = readWidth;
            height = readHeight;
            return Result::Ok;
        }
    }
    return Result::TornFrame;
}

void Reader::close () {
    if (m_map != nullptr) {
        munmap (m_map, m_size);
        m_map = nullptr;
    }
    if (m_fd >= 0) {
        ::close (m_fd);
        m_fd = -1;
    }
    m_size = 0;
}

bool Reader::valid () const {
    return m_map != nullptr;
}

}
