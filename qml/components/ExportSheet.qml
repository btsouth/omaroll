import QtQuick

// Format and size choices are owned by omarchy-transcode. This sheet only
// collects them and hands each original path to the house command unchanged.
Item {
    id: root

    property var paths: []
    property bool isVideo: false
    property string format: ""
    property string resolution: ""
    readonly property var formatValues: root.isVideo ? ["mp4", "gif"] : ["jpg", "png"]
    readonly property var formatLabels: root.isVideo ? ["MP4", "GIF"] : ["JPEG", "PNG"]
    readonly property var resolutionValues: root.isVideo ? ["4k", "1080p", "720p"]
                                                           : ["high", "medium", "low"]
    readonly property var resolutionLabels: root.isVideo ? ["4K", "1080p", "720p"]
                                                           : ["High · 3160", "Medium · 2160", "Low · 1080"]

    signal exported(int count, string description)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function open(files, moving) {
        paths = files
        isVideo = moving
        format = moving ? "mp4" : "jpg"
        resolution = moving ? "1080p" : "medium"
        visible = true
        forceActiveFocus()
    }

    function close() { visible = false }

    function save() {
        if (paths.length === 0 || format === "" || resolution === "") {
            return
        }
        if (Registry.runBatchWith("export", {"format": format, "resolution": resolution}, paths)) {
            const description = format.toUpperCase() + " · " + resolution
            root.close()
            root.exported(paths.length, description)
        }
    }

    visible: false
    anchors.fill: parent
    focus: visible

    WheelHandler {
        target: null
        enabled: root.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) { event.accepted = true }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.6)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
            onClicked: function (mouse) {
                if (mouse.button === Qt.LeftButton) {
                    root.close()
                }
            }
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(470, root.width - 60)
        height: column.implicitHeight + 44
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        Column {
            id: column
            anchors.centerIn: parent
            width: parent.width - 44
            spacing: 12

            Text {
                width: parent.width
                text: root.paths.length === 1
                      ? (root.isVideo ? "Convert video" : "Convert picture")
                      : "Convert " + root.paths.length + (root.isVideo ? " videos" : " pictures")
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }

            Text {
                width: parent.width
                text: "The original stays untouched. The new file is saved beside it."
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }

            Text {
                text: "Format"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }

            Row {
                spacing: 8
                Repeater {
                    model: root.formatValues.length
                    PillButton {
                        required property int index
                        label: root.formatLabels[index]
                        active: root.format === root.formatValues[index]
                        onClicked: root.format = root.formatValues[index]
                    }
                }
            }

            Text {
                text: "Maximum size"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }

            Row {
                spacing: 8
                Repeater {
                    model: root.resolutionValues.length
                    PillButton {
                        required property int index
                        label: root.resolutionLabels[index]
                        active: root.resolution === root.resolutionValues[index]
                        onClicked: root.resolution = root.resolutionValues[index]
                    }
                }
            }

            Item { width: 1; height: 4 }

            Row {
                anchors.right: parent.right
                spacing: 8

                PillButton {
                    label: "Cancel"
                    onClicked: root.close()
                }

                PillButton {
                    label: root.paths.length > 1 ? "Convert " + root.paths.length : "Convert"
                    active: true
                    onClicked: root.save()
                }
            }
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.save()
            event.accepted = true
        }
    }
}
