#include "FrameSinkChannel.h"
#include "WallpaperEngine/Logging/Log.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace FrameSource {

FrameChannelClient::~FrameChannelClient () {
    reset ();
}

void FrameChannelClient::start (const std::string &socketPath, const FrameChannelHello &hello, const std::vector<int> &slotFds) {
    m_path = socketPath;
    m_hello = hello;
    m_slotFds = slotFds;
}

bool FrameChannelClient::connectAndHello () {
    m_fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0)
        return false;
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (m_path.size () >= sizeof (addr.sun_path)) {
        close (m_fd);
        m_fd = -1;
        return false;
    }
    strncpy (addr.sun_path, m_path.c_str (), sizeof (addr.sun_path) - 1);
    if (connect (m_fd, (const sockaddr *) &addr, sizeof (addr)) != 0) {
        static int throttled = 0;
        if (++throttled % 300 == 1)
            sLog.out ("[channel] connect wait errno=", errno, " path=", m_path, " fd=", m_fd);
        close (m_fd);
        m_fd = -1;
        return false;
    }
    union {
        char buf[CMSG_SPACE (sizeof (int) * 8)];
        struct cmsghdr align;
    } control {};
    iovec iov {};
    iov.iov_base = &m_hello;
    iov.iov_len = sizeof (m_hello);
    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    const size_t count = m_slotFds.size () < 8 ? m_slotFds.size () : 8;
    if (count > 0) {
        msg.msg_control = control.buf;
        msg.msg_controllen = CMSG_SPACE (sizeof (int) * count);
        cmsghdr *cmsg = CMSG_FIRSTHDR (&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN (sizeof (int) * count);
        int fds[8];
        for (size_t i = 0; i < count; ++i)
            fds[i] = m_slotFds[i];
        memcpy (CMSG_DATA (cmsg), fds, sizeof (int) * count);
        msg.msg_controllen = CMSG_LEN (sizeof (int) * count);
    }
    if (sendmsg (m_fd, &msg, MSG_NOSIGNAL) != ssize_t (sizeof (m_hello))) {
        sLog.error ("[channel] hello send fail errno=", errno, " fd=", m_fd, " slots=",
                    m_slotFds.empty () ? -1 : m_slotFds[0]);
        close (m_fd);
        m_fd = -1;
        return false;
    }
    sLog.out ("[channel] hello sent path=", m_path);
    m_peerGone = false;
    m_helloed = true;
    return true;
}

bool FrameChannelClient::justHelloed () {
    if (!m_helloed)
        return false;
    m_helloed = false;
    return true;
}

void FrameChannelClient::sendFrame (uint64_t sequence, uint32_t slot, int fenceFd) {
    if (m_fd < 0 && !connectAndHello ())
        return;
    FrameChannelFrame frame {sequence, slot, 0};
    iovec iov {};
    iov.iov_base = &frame;
    iov.iov_len = sizeof (frame);
    alignas (cmsghdr) char control[CMSG_SPACE (sizeof (int))] = {};
    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (fenceFd >= 0) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_LEN (sizeof (int));
        cmsghdr *cmsg = CMSG_FIRSTHDR (&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN (sizeof (int));
        memcpy (CMSG_DATA (cmsg), &fenceFd, sizeof (int));
    }
    if (sendmsg (m_fd, &msg, MSG_NOSIGNAL) != ssize_t (sizeof (frame))) {
        reset ();
        return;
    }
    if (fenceFd >= 0)
        close (fenceFd);
}

void FrameChannelClient::reset () {
    if (m_fd >= 0) {
        close (m_fd);
        m_fd = -1;
    }
}

bool FrameChannelClient::waitReadable (int timeoutMs) {
    if (m_fd < 0 || m_peerGone)
        return false;
    pollfd p {m_fd, POLLIN, 0};
    return poll (&p, 1, timeoutMs) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

bool FrameChannelClient::recvRelease (uint32_t &slot, uint64_t &sequence, int &fenceFd) {
    if (m_fd < 0 || m_peerGone)
        return false;
    FrameChannelFrame frame {};
    iovec iov {};
    iov.iov_base = &frame;
    iov.iov_len = sizeof (frame);
    alignas (cmsghdr) char control[CMSG_SPACE (sizeof (int))] = {};
    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof (control);
    ssize_t n = recvmsg (m_fd, &msg, MSG_DONTWAIT);
    if (n == 0) {
        m_peerGone = true;
        reset ();
        return false;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            m_peerGone = true;
            reset ();
        }
        return false;
    }
    if (n < ssize_t (sizeof (frame))) {
        m_peerGone = true;
        reset ();
        return false;
    }
    fenceFd = -1;
    for (cmsghdr *c = CMSG_FIRSTHDR (&msg); c; c = CMSG_NXTHDR (&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN (sizeof (int)))
            memcpy (&fenceFd, CMSG_DATA (c), sizeof (int));
    }
    slot = frame.slot;
    sequence = frame.sequence;
    return true;
}
}
