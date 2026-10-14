#pragma once

#include "FrameSource.h"

namespace FrameSource {

class Reader {
public:
    Reader () = default;
    ~Reader ();
    Reader (const Reader&) = delete;
    Reader& operator = (const Reader&) = delete;

    Result open (const std::filesystem::path& path);
    Result readLatest (std::vector<uint8_t>& outTopDown, uint32_t& width, uint32_t& height);
    void close ();

    bool valid () const;

private:
    int m_fd = -1;
    uint8_t* m_map = nullptr;
    size_t m_size = 0;
};

}
