#include "framesink.h"
#include <QtQuick/QQuickWindow>
#include <QtGui/QImage>
#include <QtQuick/QSGSimpleTextureNode>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>

FrameSink::FrameSink(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &QQuickItem::update);
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
    openSink();
    m_timer->start();
}

QSGNode *FrameSink::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node)
        node = new QSGSimpleTextureNode;

    if (m_map && (m_header->width != m_width || m_header->height != m_height)) {
        closeSink();
        openSink();
    }

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
            if (m_drawnSequence == 0)
                qWarning("framesink: first frame drawn item=%fx%f node=%fx%f centerpx=%s seq=%llu",
                         width(), height(), node->rect().width(), node->rect().height(),
                         qPrintable(image.pixelColor(image.width()/2, image.height()/2).name()),
                         (unsigned long long) seq);
            m_drawnSequence = seq;
        } else if (node->texture()) {
            node->setRect(QRectF(0, 0, width(), height()));
        }
    }
    return node;
}

void FrameSink::openSink()
{
    if (m_map || m_source.isEmpty())
        return;
    m_fd = open(m_source.toUtf8().constData(), O_RDONLY);
    if (m_fd < 0) {
        scheduleRetry();
        return;
    }
    FrameOutHeader probe {};
    struct stat st {};
    if (pread(m_fd, &probe, sizeof(probe), 0) != sizeof(probe) || probe.magic != 0x4C574531
        || probe.width == 0 || probe.height == 0
        || fstat(m_fd, &st) != 0
        || size_t(st.st_size) < sizeof(FrameOutHeader) + size_t(probe.width) * probe.height * 3) {
        close(m_fd);
        m_fd = -1;
        scheduleRetry();
        return;
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
        return;
    }
    m_header = reinterpret_cast<FrameOutHeader *>(m_map);
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
        openSink();
    });
}
