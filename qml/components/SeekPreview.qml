import QtQuick

// The frame and time under the pointer on a seek bar. The frame comes from
// the video's storyboard, nearest the time; until there is one, or for a clip
// too short to have one, only the time shows.
Rectangle {
    id: root

    property var storyboard: null
    // Milliseconds under the pointer.
    property real time: 0
    property string label: ""
    // The window's room for it, in logical pixels.
    property real maximumFrameWidth: 192
    property real maximumFrameHeight: 108

    readonly property int frameIndex: root.storyboard && root.storyboard.frameCount > 0
                                      ? root.storyboard.frameAt(root.time) : -1
    readonly property bool showsFrame: root.frameIndex >= 0 && root.frameWidth >= 48
    readonly property real aspect: root.storyboard ? root.storyboard.aspectRatio : 16 / 9
    readonly property real frameWidth: Math.floor(Math.min(root.maximumFrameWidth,
                                                           root.maximumFrameHeight * root.aspect))
    readonly property real frameHeight: Math.round(root.frameWidth / root.aspect)
    readonly property int padding: 3

    width: Math.max(timeText.implicitWidth + 12, root.showsFrame ? root.frameWidth + 2 * root.padding : 0)
    height: (root.showsFrame ? root.frameHeight + root.padding : 0) + timeText.implicitHeight + 6
    radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, root.showsFrame ? 8 : 4) : 3
    color: Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0.94)
    border.width: 1
    border.color: Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.16)

    Image {
        id: frame
        objectName: "seekPreviewFrame"
        visible: root.showsFrame
        x: root.padding
        y: root.padding
        width: root.frameWidth
        height: root.frameHeight
        // Storyboard frames decode in well under a millisecond, so the frame
        // keeps up with the pointer instead of flashing empty while it loads.
        asynchronous: false
        smooth: true
        fillMode: Image.PreserveAspectCrop
        source: root.showsFrame && root.storyboard.frameCount > 0
                ? root.storyboard.frameUrl(root.frameIndex) : ""
    }

    Text {
        id: timeText
        objectName: "seekPreviewTime"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 3
        text: root.label
        font.family: Theme.fontFamily
        font.pixelSize: 11
        color: Theme.brightForeground
    }
}
