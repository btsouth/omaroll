import QtQuick

// One capture, large, with everything you can do to it.
//
// The action list is not hard-coded here: it comes from the registry, which
// knows which already-installed tool owns each job. An action whose program is
// missing is shown greyed with the package to install rather than hidden, so
// the window is also a map of what the system can do.
Item {
    id: root

    property string path: ""
    property string fileName: ""
    property string kindLabel: ""
    property string dayLabel: ""
    property string timeLabel: ""
    property string sizeLabel: ""
    property bool isVideo: false
    property bool favorite: false
    property int kind: 0

    signal actionTriggered(string id)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function open() {
        visible = true
        forceActiveFocus()
    }

    function close() { visible = false }

    visible: false
    anchors.fill: parent
    focus: visible

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.62)
        TapHandler { onSingleTapped: root.close() }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(1000, root.width - 60)
        height: Math.min(700, root.height - 60)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.97)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        TapHandler { onSingleTapped: {} }

        // Preview
        Rectangle {
            id: stage
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: sidebar.left
            anchors.margins: 1
            color: root.shade(Theme.darkerBackground, 0.85)

            Image {
                anchors.fill: parent
                anchors.margins: 16
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
                mipmap: true
                sourceSize: Qt.size(Math.round(stage.width * Screen.devicePixelRatio),
                                    Math.round(stage.height * Screen.devicePixelRatio))
                // Videos have no still to show at full size, so the detail view
                // reuses the same frame the grid does rather than pretending to
                // be a player. Play hands it to mpv.
                source: root.path === "" ? "" : (root.isVideo
                        ? "image://thumbs/" + Screen.devicePixelRatio + "@40" + root.path
                        : "file://" + root.path)
            }

            Rectangle {
                anchors.centerIn: parent
                visible: root.isVideo
                width: 64
                height: 64
                radius: 32
                color: Qt.rgba(0, 0, 0, 0.5)
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.4)

                Text {
                    anchors.centerIn: parent
                    text: "▶"
                    font.pixelSize: 24
                    color: "#ffffff"
                }

                TapHandler { onSingleTapped: root.actionTriggered("play") }
            }
        }

        // Sidebar
        Item {
            id: sidebar
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: 280

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Column {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 18
                spacing: 4

                Text {
                    width: parent.width
                    text: root.fileName
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Theme.brightForeground
                    elide: Text.ElideMiddle
                }

                Text {
                    width: parent.width
                    text: root.kindLabel + "  ·  " + root.sizeLabel
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                Text {
                    width: parent.width
                    text: root.dayLabel + "  ·  " + root.timeLabel
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }
            }

            // Actions, from the registry
            ListView {
                id: actions
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: 96
                anchors.bottomMargin: 14
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                clip: true
                spacing: 1

                model: root.visible ? Registry.actionsFor(root.kind) : []

                delegate: Item {
                    id: row
                    required property var modelData
                    width: actions.width
                    height: 34

                    readonly property bool usable: modelData.available

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                        color: rowHover.hovered && row.usable
                               ? root.shade(Theme.foreground, 0.09)
                               : "transparent"
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: shortcut.left
                        anchors.rightMargin: 8
                        elide: Text.ElideRight
                        text: row.modelData.label
                              + (row.usable ? "" : "  ·  needs " + row.modelData.hint)
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: row.modelData.primary ? Font.DemiBold : Font.Normal
                        color: !row.usable
                               ? root.shade(Theme.foreground, 0.32)
                               : (row.modelData.primary ? Theme.accent : Theme.foreground)
                    }

                    Text {
                        id: shortcut
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.shortcut
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: root.shade(Theme.foreground, 0.35)
                    }

                    HoverHandler {
                        id: rowHover
                        cursorShape: row.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }

                    TapHandler {
                        enabled: row.usable
                        onSingleTapped: root.actionTriggered(row.modelData.id)
                    }
                }
            }
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Space) {
            root.close()
            event.accepted = true
        }
    }
}
