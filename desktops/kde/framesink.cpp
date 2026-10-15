#include "framesink.h"
#include <QtQuick/QQuickWindow>
#include <QtGui/QImage>
#include <QtGui/QOpenGLContext>
#include <QtGui/qopenglcontext_platform.h>
#include <QtQuick/QSGSimpleTextureNode>
#include <QtQuick/qsgtexture_platform.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <ctime>

#ifndef GL_EGL_IMAGE_TARGET_TEXTURE_2D_OES
#define GL_EGL_IMAGE_TARGET_TEXTURE_2D_OES 0x8F65
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif

FrameSink::FrameSink(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &QQuickItem::update);
    memset(m_slots, 0, sizeof(m_slots));
}

FrameSink::~FrameSink()
{
    stopSocketServer();
    for (auto &s : m_slots) {
        delete s.texture;
        if (s.fenceFd >= 0)
            close(s.fenceFd);
        if (s.dmabufFd >= 0)
            close(s.dmabufFd);
    }
    closeSink();
}

QString FrameSink::source() const { return m_source; }

void FrameSink::setSource(const QString &source)
{
    if (m_source == source)
        return;
    m_source = source;
    emit sourceChanged();
}

void FrameSink::componentComplete()
{
    QQuickItem::componentComplete();
    tryAttach();
    m_timer->start();
}

void FrameSink::tryAttach()
{
    if (m_mode != Mode::Undecided)
        return;
    startSocketServer();
    if (m_connFd >= 0 && m_helloDone) {
        m_mode = Mode::DmaBuf;
        return;
    }
    if (openSink()) {
        m_mode = Mode::Shm;
        return;
    }
    scheduleRetry();
}

void FrameSink::startSocketServer()
{
    if (m_listenFd >= 0 || m_source.isEmpty())
        return;
    const std::string path = m_source.toUtf8().constData() + std::string(".sock");
    m_listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_listenFd < 0)
        return;
    unlink(path.c_str());
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        close(m_listenFd);
        m_listenFd = -1;
        return;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(m_listenFd, (const sockaddr *) &addr, sizeof(addr)) != 0 || listen(m_listenFd, 1) != 0) {
        close(m_listenFd);
        m_listenFd = -1;
        return;
    }
    m_listenNotifier = new QSocketNotifier(m_listenFd, QSocketNotifier::Read, this);
    connect(m_listenNotifier, &QSocketNotifier::activated, this, [this] { onServerReadable(); });
}

void FrameSink::stopSocketServer()
{
    dropConnection();
    delete m_listenNotifier;
    m_listenNotifier = nullptr;
    if (m_listenFd >= 0) {
        close(m_listenFd);
        m_listenFd = -1;
    }
}

void FrameSink::onServerReadable()
{
    int conn = accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (conn < 0)
        return;
    dropConnection();
    m_connFd = conn;
    m_helloDone = false;
    m_connNotifier = new QSocketNotifier(m_connFd, QSocketNotifier::Read, this);
    connect(m_connNotifier, &QSocketNotifier::activated, this, [this] { onConnectionReadable(); });
}

bool FrameSink::readHello(msghdr &msg, ssize_t n)
{
    if (n != ssize_t(sizeof(FrameChannelHello))) {
        qWarning("framesink: hello size mismatch %zd", n);
        return false;
    }
    auto *hello = static_cast<FrameChannelHello *>(msg.msg_iov->iov_base);
    if (memcmp(hello->magic, "LWE2", 4) != 0 || hello->version != 1
        || hello->width == 0 || hello->height == 0
        || hello->slotCount == 0 || hello->slotCount > 8) {
        qWarning("framesink: hello invalid");
        return false;
    }
    m_dmaWidth = hello->width;
    m_dmaHeight = hello->height;
    m_stride = hello->stride;
    m_dmaModifier = hello->modifier;
    m_fourcc = hello->fourcc ? hello->fourcc : 0x34324241;
    m_slotCount = hello->slotCount;
    int fds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        memcpy(fds, CMSG_DATA(c), count * sizeof(int));
    }
    for (uint32_t i = 0; i < m_slotCount; ++i)
        m_slots[i].dmabufFd = fds[i];
    m_helloDone = true;
    m_mode = Mode::DmaBuf;
    qWarning("framesink: dmabuf attached %ux%u slots=%u", m_dmaWidth, m_dmaHeight, m_slotCount);
    return true;
}

void FrameSink::onConnectionReadable()
{
    if (!m_helloDone) {
        FrameChannelHello hello {};
        iovec iov {};
        iov.iov_base = &hello;
        iov.iov_len = sizeof(hello);
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 8)] = {};
        msghdr msg {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        ssize_t n = recvmsg(m_connFd, &msg, MSG_DONTWAIT);
        if (n <= 0)
            return;
        if (!readHello(msg, n))
            dropConnection();
        return;
    }
    for (;;) {
        FrameChannelFrame frame {};
        iovec iov {};
        iov.iov_base = &frame;
        iov.iov_len = sizeof(frame);
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
        msghdr msg {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        ssize_t n = recvmsg(m_connFd, &msg, MSG_DONTWAIT | MSG_TRUNC);
        if (n < ssize_t(sizeof(FrameChannelFrame)))
            break;
        int fenceFd = -1;
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS
                && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
                memcpy(&fenceFd, CMSG_DATA(c), sizeof(int));
            }
        }
        if (frame.slot >= m_slotCount) {
            if (fenceFd >= 0)
                close(fenceFd);
            continue;
        }
        DmaSlot &s = m_slots[frame.slot];
        if (s.fenceFd >= 0)
            close(s.fenceFd);
        s.fenceFd = fenceFd;
        s.seq = frame.sequence;
        if (s.seq > m_bestSeq) {
            m_bestSeq = s.seq;
            m_bestSlot = int(frame.slot);
            QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
        }
    }
}

void FrameSink::dropConnection()
{
    delete m_connNotifier;
    m_connNotifier = nullptr;
    if (m_connFd >= 0) {
        close(m_connFd);
        m_connFd = -1;
    }
    m_helloDone = false;
    for (auto &s : m_slots) {
        if (s.fenceFd >= 0) {
            close(s.fenceFd);
            s.fenceFd = -1;
        }
    }
    m_bestSlot = -1;
}

bool FrameSink::openSink()
{
    if (m_map || m_source.isEmpty())
        return true;
    m_fd = open(m_source.toUtf8().constData(), O_RDONLY);
    if (m_fd < 0) {
        scheduleRetry();
        return false;
    }
    FrameOutHeader probe {};
    struct stat st {};
    if (pread(m_fd, &probe, sizeof(probe), 0) != sizeof(probe) || probe.magic != 0x4C574531
        || probe.width == 0 || probe.height == 0
        || fstat(m_fd, &st) != 0
        || size_t(st.st_size) < sizeof(FrameOutHeader) + size_t(probe.width) * probe.height * 3
        || time(nullptr) - st.st_mtime > 5) {
        close(m_fd);
        m_fd = -1;
        scheduleRetry();
        return false;
    }
    m_width = probe.width;
    m_height = probe.height;
    const size_t total = sizeof(FrameOutHeader) + size_t(m_width) * m_height * 3;
    m_map = static_cast<uint8_t *>(mmap(nullptr, total, PROT_READ, MAP_SHARED, m_fd, 0));
    if (m_map == MAP_FAILED) {
        m_map = nullptr;
        close(m_fd);
        m_fd = -1;
        scheduleRetry();
        return false;
    }
    m_header = reinterpret_cast<FrameOutHeader *>(m_map);
    return true;
}

void FrameSink::closeSink()
{
    if (m_map) {
        munmap(m_map, sizeof(FrameOutHeader) + size_t(m_width) * m_height * 3);
        m_map = nullptr;
        m_header = nullptr;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

void FrameSink::scheduleRetry()
{
    if (m_retryPending)
        return;
    m_retryPending = true;
    QTimer::singleShot(500, this, [this] {
        m_retryPending = false;
        tryAttach();
    });
}

void FrameSink::dmabufDraw(QSGSimpleTextureNode *node)
{
    int slot = -1;
    for (int i = 0; i < int(m_slotCount); ++i) {
        DmaSlot &s = m_slots[i];
        if (s.seq == 0)
            continue;
        if (s.fenceFd >= 0) {
            pollfd p {s.fenceFd, POLLIN, 0};
            if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN))
                continue;
        }
        if (slot < 0 || s.seq > m_slots[size_t(slot)].seq)
            slot = i;
    }
    if (slot < 0)
        return;
    DmaSlot &s = m_slots[size_t(slot)];
    if (s.fenceFd >= 0) {
        close(s.fenceFd);
        s.fenceFd = -1;
    }
    if (s.texture && m_drawnSequence == s.seq) {
        node->setTexture (s.texture);
        node->setRect (QRectF (0, 0, width (), height ()));
        node->setFiltering (QSGTexture::Linear);
        return;
    }
    void *nativeDisplay = nullptr;
    if (auto *ri = window()->rendererInterface()) {
        if (auto *glctx = static_cast<QOpenGLContext *> (ri->getResource (window (), QSGRendererInterface::OpenGLContextResource))) {
            if (auto *eglCtx = glctx->nativeInterface<QNativeInterface::QEGLContext>())
                nativeDisplay = eglCtx->display ();
        }
    }
    if (!nativeDisplay)
        return;
    auto createImage = reinterpret_cast<EGLImageKHR (*) (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *)>
        (eglGetProcAddress ("eglCreateImageKHR"));
    auto imageTarget = reinterpret_cast<void (*) (GLenum, EGLClientBuffer)>
        (eglGetProcAddress ("glEGLImageTargetTexture2DOES"));
    if (!createImage || !imageTarget)
        return;
    if (!s.glTex) {
        EGLint attrs[32];
        int n = 0;
        attrs[n++] = EGL_WIDTH; attrs[n++] = EGLint (m_dmaWidth);
        attrs[n++] = EGL_HEIGHT; attrs[n++] = EGLint (m_dmaHeight);
        attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[n++] = EGLint (m_fourcc);
        attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT; attrs[n++] = s.dmabufFd;
        attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[n++] = 0;
        attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; attrs[n++] = EGLint (m_stride);
        constexpr uint64_t drmFormatModInvalid = 0x00ffffffffffffffULL;
        const char *exts = eglQueryString ((EGLDisplay) nativeDisplay, EGL_EXTENSIONS);
        if (m_dmaModifier != 0 && m_dmaModifier != drmFormatModInvalid
            && exts && strstr (exts, "EGL_EXT_image_dma_buf_import_modifiers")) {
            attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attrs[n++] = EGLint (m_dmaModifier & 0xffffffffu);
            attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attrs[n++] = EGLint (m_dmaModifier >> 32);
        }
        attrs[n] = EGL_NONE;
        EGLImageKHR image = createImage ((EGLDisplay) nativeDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (image == EGL_NO_IMAGE_KHR) {
            qWarning("framesink: dmabuf import fail eglErr=%d", (int) eglGetError ());
            return;
        }
        s.eglImage = image;
        close(s.dmabufFd);
        s.dmabufFd = -1;
        glGenTextures (1, &s.glTex);
        glBindTexture (GL_TEXTURE_2D, s.glTex);
        imageTarget (GL_TEXTURE_2D, image);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!s.texture) {
        s.texture = QNativeInterface::QSGOpenGLTexture::fromNative (
            s.glTex, window (), QSize (int(m_dmaWidth), int(m_dmaHeight)), QQuickWindow::CreateTextureOptions {});
        if (!s.texture)
            return;
        qWarning("framesink: dmabuf zero-copy texture attached %ux%u", m_dmaWidth, m_dmaHeight);
    }
    node->setTexture (s.texture);
    node->setRect (QRectF (0, 0, width (), height ()));
    node->setFiltering (QSGTexture::Linear);
    m_drawnSequence = s.seq;
}

void FrameSink::shmDraw(QSGSimpleTextureNode *node)
{
    if (m_map) {
        const uint64_t seq = __atomic_load_n(&m_header->sequence, __ATOMIC_ACQUIRE);
        if (seq != 0 && seq != m_drawnSequence
            && seq == __atomic_load_n(&m_header->sequence, __ATOMIC_ACQUIRE)) {
            QImage image(m_header->width, m_header->height, QImage::Format_RGB888);
            const uint8_t *src = m_map + sizeof(FrameOutHeader);
            const bool flip = m_header->vflip != 0;
            for (uint32_t y = 0; y < m_header->height; ++y) {
                const uint32_t sy = flip ? m_header->height - 1 - y : y;
                std::memcpy(image.scanLine(y), src + size_t(sy) * m_header->stride, m_header->stride);
            }
            if (auto *old = node->texture())
                old->deleteLater();
            node->setTexture(window()->createTextureFromImage(image));
            node->setRect(QRectF(0, 0, width(), height()));
            node->setFiltering(QSGTexture::Linear);
            m_drawnSequence = seq;
        } else if (node->texture()) {
            node->setRect(QRectF(0, 0, width(), height()));
        }
    }
}

QSGNode *FrameSink::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node)
        node = new QSGSimpleTextureNode;

    if (m_mode == Mode::DmaBuf)
        dmabufDraw(node);
    else if (m_mode == Mode::Shm)
        shmDraw(node);

    return node;
}
