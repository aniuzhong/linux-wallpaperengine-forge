#pragma once

#include <QtQuick/QQuickItem>
#include <QtQuick/QSGSimpleTextureNode>

#include <QObject>
#include <QTimer>
#include <QSocketNotifier>
#include <sys/socket.h>

#include <cstdint>
#include <fcntl.h>

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

    void tryAttach();
    void startSocketServer();
    void stopSocketServer();
    void onServerReadable();
    void onConnectionReadable();
    bool readHello(msghdr &msg, ssize_t n);
    void readFrameEvent(ssize_t n);
    void dropConnection();
    bool openSink();
    void closeSink();
    void scheduleRetry();
    void dmabufDraw(QSGSimpleTextureNode *node);
    void shmDraw(QSGSimpleTextureNode *node);

    QString m_source;
    QTimer *m_timer = nullptr;
    Mode m_mode = Mode::Undecided;
    bool m_retryPending = false;

    int m_listenFd = -1;
    int m_connFd = -1;
    QSocketNotifier *m_listenNotifier = nullptr;
    QSocketNotifier *m_connNotifier = nullptr;
    bool m_helloDone = false;

    struct DmaSlot {
        int dmabufFd = -1;
        void *eglImage = nullptr;
        GLuint glTex = 0;
        QSGTexture *texture = nullptr;
        int fenceFd = -1;
        uint64_t seq = 0;
    };
    DmaSlot m_slots[8];
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
};
