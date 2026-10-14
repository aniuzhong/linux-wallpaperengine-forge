import QtQuick
import org.kde.plasma.plasmoid
import org.forge.framesink

WallpaperItem {
    id: root

    FrameSink {
        anchors.fill: parent
        source: "/tmp/lwe-frames.bin"
    }
}
