#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace FrameSource {

struct FrameChannelHello {
    char magic[4];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t stride;
    uint64_t modifier;
    uint32_t slotCount;
    uint32_t reserved;
};

struct FrameChannelFrame {
    uint64_t sequence;
    uint32_t slot;
    uint32_t reserved;
};

class FrameChannelClient {
public:
    ~FrameChannelClient ();
    void start (const std::string &socketPath, const FrameChannelHello &hello, const std::vector<int> &slotFds);
    bool connected () const { return m_fd >= 0; }
    bool peerGone () const { return m_peerGone; }
    bool justHelloed ();
    void sendFrame (uint64_t sequence, uint32_t slot, int fenceFd);
    bool waitReadable (int timeoutMs);
    bool recvRelease (uint32_t &slot, uint64_t &sequence, int &fenceFd);
    void reset ();

private:
    bool connectAndHello ();
    int m_fd = -1;
    std::string m_path;
    FrameChannelHello m_hello {};
    std::vector<int> m_slotFds;
    bool m_peerGone = false;
    bool m_helloed = false;
};
}
