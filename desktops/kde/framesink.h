#pragma once

#include <QtQuick/QQuickItem>
#include <QtQuick/QSGSimpleTextureNode>

#include <QImage>
#include <QObject>
#include <QSize>
#include <QTimer>
#include <QSocketNotifier>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <vector>

struct FrameOutHeader {
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

class FrameSink : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)

public:
    explicit FrameSink(QQuickItem *parent = nullptr);
    ~FrameSink() override;

    QString source() const;
    void setSource(const QString &source);
    void componentComplete() override;

signals:
    void sourceChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

private:
    enum class Mode { Undecided, DmaBuf, Shm };

    struct DmaSlot {
        int dmabufFd = -1;
        void *eglImage = nullptr;
        GLuint glTex = 0;
        QSGTexture *texture = nullptr;
        int fenceFd = -1;
        uint64_t seq = 0;
        bool released = false;
    };

    void tryAttach();
    void startSocketServer();
    void stopSocketServer();
    void onServerReadable();
    void onConnectionReadable();
    bool readHello(msghdr &msg, ssize_t n);
    void readFrameEvent(ssize_t n);
    void dropConnection();
    void handleConnectionLost();
    bool openSink();
    void closeSink();
    void scheduleRetry();
    void startRepaint();
    void stopRepaint();
    void dmabufDraw(QSGSimpleTextureNode *node);
    void shmDraw(QSGSimpleTextureNode *node);
    void drawHint(QSGSimpleTextureNode *node);
    void releaseSlotResources(DmaSlot &slot, void *eglDisplay);
    void releaseRetiredSlots(void *eglDisplay);
    int takeRenderFence(void *eglDisplay);
    void sendRelease(int slot, uint64_t sequence, int fenceFd);

    QString m_source;
    QTimer *m_timer = nullptr;
    Mode m_mode = Mode::Undecided;
    bool m_retryPending = false;

    int m_listenFd = -1;
    int m_connFd = -1;
    QSocketNotifier *m_listenNotifier = nullptr;
    QSocketNotifier *m_connNotifier = nullptr;
    bool m_helloDone = false;

    DmaSlot m_slots[8];
    std::vector<DmaSlot> m_retired;
    uint32_t m_slotCount = 0;
    uint32_t m_dmaWidth = 0;
    uint32_t m_dmaHeight = 0;
    uint64_t m_dmaModifier = 0;
    uint32_t m_fourcc = 0;
    uint32_t m_stride = 0;
    uint64_t m_bestSeq = 0;
    int m_bestSlot = -1;

    int m_fd = -1;
    uint8_t *m_map = nullptr;
    FrameOutHeader *m_header = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_drawnSequence = 0;

    QSGTexture *m_hintTexture = nullptr;
    QImage m_hintImage;
    QSize m_hintSize;
    bool m_detachPending = false;
    std::chrono::steady_clock::time_point m_lastProgress;
    qint64 m_stalledSec = 0;
    long m_stalledNsec = -1;
    int m_lastDrawnSlot = -1;
};
