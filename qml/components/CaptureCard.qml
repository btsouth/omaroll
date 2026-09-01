import QtQuick

// One capture. The thumbnail is drawn at full opacity on top of the translucent
// chrome: dimming the pixels someone is trying to look at is a bug, not a style,
// which is why every media app in Omarchy opts out of window transparency.
Item {
    id: root

    property string path: ""
    property string fileName: ""
    property string kindLabel: ""
    property string timeLabel: ""
    property string sizeLabel: ""
    property bool isVideo: false
    property bool selected: false

    signal activated()
    signal chosen()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        color: root.shade(Theme.background, root.selected ? 0.62 : 0.34)
        radius: Theme.cornerRadius
        border.width: root.selected ? 2 : 1
        border.color: root.selected
                      ? Theme.accent
                      : root.shade(Theme.foreground, hover.hovered ? 0.22 : 0.10)
        clip: true

        Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
        Behavior on border.color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }

        Image {
            id: thumbnail
            anchors.fill: parent
            anchors.margins: 1
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: true
            // Decode at the size actually drawn, on the ratio of the screen this
            // window is on. Without the ratio, a 1.5x monitor shows an upscaled
            // tile and the whole grid reads as soft.
            sourceSize: Qt.size(Math.round(root.width), Math.round(root.height))
            // "image://thumbs/<ratio><absolute path>". The path keeps its leading
            // slash, so the ratio reads as the first URL path segment and no
            // separator character has to survive percent-encoding.
            source: root.path === ""
                    ? ""
                    : "image://thumbs/" + Screen.devicePixelRatio + root.path
            smooth: true
            mipmap: true
            opacity: status === Image.Ready ? 1 : 0

            Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutQuad } }
        }

        // Placeholder while a thumbnail is being made, and the resting state for
        // anything that cannot produce one.
        Text {
            anchors.centerIn: parent
            visible: thumbnail.status !== Image.Ready
            text: root.isVideo ? "▶" : "▦"
            font.family: Theme.fontFamily
            font.pixelSize: 22
            color: root.shade(Theme.foreground, 0.28)
        }

        // Footer scrim. Sits over the image so the metadata stays readable on a
        // bright photograph, and only over the bottom strip so it costs the
        // picture almost nothing.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 40
            visible: thumbnail.status === Image.Ready
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.62) }
            }
        }

        Row {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
            spacing: 6

            Text {
                text: root.timeLabel
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: "#ffffff"
                opacity: 0.94
            }

            Item { width: 2; height: 1 }

            Text {
                text: root.sizeLabel
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: "#ffffff"
                opacity: 0.62
            }
        }

        // Kind marker. A recording needs to be tellable from a screenshot at a
        // glance, before any text is read.
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 8
            visible: root.isVideo
            width: badge.implicitWidth + 12
            height: badge.implicitHeight + 6
            radius: 3
            color: Qt.rgba(0, 0, 0, 0.55)

            Text {
                id: badge
                anchors.centerIn: parent
                text: "▶ " + root.kindLabel
                font.family: Theme.fontFamily
                font.pixelSize: 10
                color: "#ffffff"
            }
        }

        HoverHandler { id: hover }

        TapHandler {
            onSingleTapped: root.activated()
            onDoubleTapped: root.chosen()
        }
    }
}
