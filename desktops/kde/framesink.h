#pragma once

#include <QtQuick/QQuickItem>
#include <QObject>
#include <QTimer>

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

class FrameSink : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)

public:
    explicit FrameSink(QQuickItem *parent = nullptr);

    QString source() const;
    void setSource(const QString &source);
    void componentComplete() override;

signals:
    void sourceChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

private:
    void openSink();

    QString m_source;
    QTimer *m_timer = nullptr;
    int m_fd = -1;
    uint8_t *m_map = nullptr;
    FrameOutHeader *m_header = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_drawnSequence = 0;
};
