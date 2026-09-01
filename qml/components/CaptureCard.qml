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
    property bool favorite: false
    property bool hiddenMark: false
    property bool selected: false
    property bool checked: false
    property bool selectionMode: false

    signal activated()
    signal chosen()
    signal toggleChecked()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    // Hovering a recording walks a few frames through the clip, so you can tell
    // two similar recordings apart without opening either. Frames are generated
    // lazily on first hover and cached, never during a scan.
    readonly property var scrubStops: [20, 40, 60, 80]
    property int scrubIndex: 0
    readonly property int scrubPercent: scrubStops[scrubIndex]

    Timer {
        id: scrubTimer
        interval: 700
        repeat: true
        running: root.isVideo && hover.hovered && root.path !== ""
        onTriggered: root.scrubIndex = (root.scrubIndex + 1) % root.scrubStops.length
    }

    onPathChanged: root.scrubIndex = 0

    Rectangle {
        id: frame
        anchors.fill: parent
        color: root.shade(Theme.background, root.selected ? 0.62 : 0.34)
        radius: Theme.cornerRadius
        border.width: root.selected || root.checked ? 2 : 1
        border.color: root.checked
                      ? Theme.accent
                      : (root.selected
                         ? root.shade(Theme.accent, 0.75)
                         : root.shade(Theme.foreground, hover.hovered ? 0.22 : 0.10))
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
            // "image://thumbs/<ratio>[@<seek%>]<absolute path>". The path keeps
            // its leading slash, so the head reads as the first URL path segment
            // and no separator has to survive percent-encoding.
            source: root.path === ""
                    ? ""
                    : "image://thumbs/" + Screen.devicePixelRatio
                      + (root.isVideo ? "@" + root.scrubPercent : "")
                      + root.path
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

        // Hidden entries stay legible but visibly set aside, so "show hidden"
        // does not just look like the filter failed.
        Rectangle {
            anchors.fill: parent
            visible: root.hiddenMark
            color: root.shade(Theme.background, 0.55)
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
            spacing: 8

            Text {
                text: root.timeLabel
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: "#ffffff"
                opacity: 0.94
            }

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
                text: (scrubTimer.running ? "◉ " : "▶ ") + root.kindLabel
                font.family: Theme.fontFamily
                font.pixelSize: 10
                color: "#ffffff"
            }
        }

        Text {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 8
            visible: root.favorite
            text: "★"
            font.pixelSize: 15
            color: Theme.yellow
            style: Text.Outline
            styleColor: Qt.rgba(0, 0, 0, 0.6)
        }

        // Multi-select checkbox. Only present once selection is under way or the
        // card is hovered, so the resting grid stays pictures rather than chrome.
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 7
            visible: root.selectionMode || hover.hovered
            width: 18
            height: 18
            radius: 3
            color: root.checked ? Theme.accent : Qt.rgba(0, 0, 0, 0.5)
            border.width: 1
            border.color: root.checked ? Theme.accent : Qt.rgba(1, 1, 1, 0.5)

            Text {
                anchors.centerIn: parent
                visible: root.checked
                text: "✓"
                font.pixelSize: 11
                font.weight: Font.Bold
                color: Theme.background
            }

            TapHandler { onSingleTapped: root.toggleChecked() }
        }

        HoverHandler { id: hover }

        TapHandler {
            onSingleTapped: root.activated()
            onDoubleTapped: root.chosen()
        }
    }
}
