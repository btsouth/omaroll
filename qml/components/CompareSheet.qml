import QtQuick

// Side-by-side comparison with synchronized zoom and pan.
//
// Every picture shares one zoom factor and one pan offset, so scrolling the
// wheel or dragging the backdrop inspects the same region of each at once.
// This is for telling near-identical pictures apart, not for editing: nothing
// here changes a file.
Item {
    id: root

    property var paths: []
    property real zoom: 1.0
    property real panX: 0
    property real panY: 0

    readonly property int imageCount: paths.length

    function open(list) {
        paths = list === undefined ? [] : list
        zoom = 1.0
        panX = 0
        panY = 0
        visible = true
        forceActiveFocus()
    }

    function close() { visible = false }

    function reset() {
        zoom = 1.0
        panX = 0
        panY = 0
    }

    function zoomBy(factor) {
        zoom = Math.max(0.1, Math.min(16, zoom * factor))
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    visible: false
    anchors.fill: parent
    focus: visible

    WheelHandler {
        target: null
        enabled: root.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) {
            const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y
            root.zoomBy(delta > 0 ? 1.2 : 1 / 1.2)
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.82)
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
        width: Math.min(1500, root.width - 24)
        height: Math.min(1000, root.height - 24)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        Item {
            id: head
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 50

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "Compare"
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 100
                anchors.verticalCenter: parent.verticalCenter
                text: root.imageCount + (root.imageCount === 1 ? " picture" : " pictures")
                      + "  ·  " + Math.round(root.zoom * 100) + "%"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }
        }

        Rectangle {
            id: stage
            anchors.top: head.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: controls.top
            anchors.margins: 14
            color: root.shade(Theme.darkerBackground, 0.7)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            clip: true

            // Drag anywhere on the stage to pan every picture together.
            DragHandler {
                target: null
                property real startX: 0
                property real startY: 0
                onActiveChanged: if (active) { startX = root.panX; startY = root.panY }
                onTranslationChanged: {
                    root.panX = startX + translation.x
                    root.panY = startY + translation.y
                }
            }
            TapHandler {
                onDoubleTapped: root.reset()
            }

            Grid {
                id: grid
                anchors.fill: parent
                anchors.margins: 8
                columns: root.imageCount <= 2 ? Math.max(1, root.imageCount) : 2
                spacing: 8

                readonly property int rows: Math.max(1, Math.ceil(root.imageCount / columns))
                readonly property real cellWidth: (width - (columns - 1) * spacing) / columns
                readonly property real cellHeight: (height - (rows - 1) * spacing) / rows

                Repeater {
                    id: imageRepeater
                    objectName: "compareRepeater"
                    model: root.paths

                    Rectangle {
                        id: cell
                        required property string modelData
                        width: grid.cellWidth
                        height: grid.cellHeight
                        color: root.shade(Theme.darkerBackground, 0.5)
                        radius: Theme.cornerRadius > 0 ? Math.min(4, Theme.cornerRadius) : 3
                        clip: true

                        Image {
                            objectName: "compareImage"
                            anchors.fill: parent
                            anchors.margins: 4
                            source: Library.fileUrl(cell.modelData)
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            smooth: true
                            mipmap: true
                            scale: root.zoom
                            transformOrigin: Item.Center
                            // The transform list is applied before scale, so
                            // divide by zoom to keep the pan in screen pixels
                            // and make the picture follow the pointer 1:1 at
                            // every zoom level.
                            transform: Translate {
                                x: root.panX / root.zoom
                                y: root.panY / root.zoom
                            }
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 8
                            width: parent.width - 16
                            elide: Text.ElideMiddle
                            text: cell.modelData.substring(cell.modelData.lastIndexOf("/") + 1)
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            color: Theme.brightForeground
                            style: Text.Outline
                            styleColor: Qt.rgba(0, 0, 0, 0.7)
                        }
                    }
                }
            }
        }

        Row {
            id: controls
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.margins: 16
            height: 34
            spacing: 8

            PillButton {
                label: "Fit"
                onClicked: root.reset()
            }
            PillButton {
                label: "Zoom out"
                onClicked: root.zoomBy(1 / 1.25)
            }
            PillButton {
                label: "Zoom in"
                onClicked: root.zoomBy(1.25)
            }
        }

        PillButton {
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.margins: 16
            label: "Done"
            active: true
            onClicked: root.close()
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        } else if (event.key === Qt.Key_0) {
            root.reset()
            event.accepted = true
        } else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
            root.zoomBy(1.25)
            event.accepted = true
        } else if (event.key === Qt.Key_Minus) {
            root.zoomBy(1 / 1.25)
            event.accepted = true
        }
    }
}
