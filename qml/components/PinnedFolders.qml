import QtQuick
import QtQuick.Controls.Basic

// A compact set of shortcuts for the folders used most often.
Rectangle {
    id: root

    readonly property bool hasPins: Settings.pinnedFolders.length > 0

    signal chosen(string path)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function basename(path) {
        const clean = String(path).replace(/\/+$/, "")
        const slash = clean.lastIndexOf("/")
        return slash >= 0 ? clean.substring(slash + 1) : clean
    }

    visible: root.hasPins
    implicitWidth: Math.min(560, parent ? parent.width : 560)
    implicitHeight: 42
    radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 10) : 4
    color: root.shade(Theme.background, 0.80)
    border.width: 1
    border.color: root.shade(Theme.foreground, 0.12)
    clip: true

    ListView {
        id: list
        objectName: "pinnedFoldersList"
        anchors.fill: parent
        anchors.margins: 4
        orientation: ListView.Horizontal
        spacing: 6
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: Settings.pinnedFolders

        ScrollBar.horizontal: ScrollBar {
            policy: ScrollBar.AsNeeded
            contentItem: Rectangle {
                implicitHeight: 3
                radius: height / 2
                color: root.shade(Theme.foreground, 0.32)
            }
            background: Rectangle { color: "transparent" }
        }

        delegate: Rectangle {
            id: chip

            required property string modelData
            readonly property string label: root.basename(chip.modelData)
            readonly property bool hovered: openArea.containsMouse

            width: Math.min(220, Math.max(96, labelMetrics.advanceWidth + 52))
            height: list.height
            radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 8) : 3
            color: chip.hovered
                   ? root.shade(Theme.accent, 0.18)
                   : root.shade(Theme.foreground, 0.055)
            border.width: chip.activeFocus ? 2 : 1
            border.color: chip.activeFocus
                          ? Theme.accent
                          : root.shade(Theme.foreground, chip.hovered ? 0.24 : 0.10)
            activeFocusOnTab: true

            Accessible.role: Accessible.Button
            Accessible.name: "Open " + chip.label
            Accessible.description: chip.modelData
            Accessible.onPressAction: root.chosen(chip.modelData)

            TextMetrics {
                id: labelMetrics
                text: chip.label
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: unpin.left
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                text: chip.label
                elide: Text.ElideMiddle
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.DemiBold
                color: chip.hovered ? Theme.brightForeground : Theme.foreground
            }

            MouseArea {
                id: openArea
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: unpin.left
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.PointingHandCursor
                onPressed: chip.forceActiveFocus()
                onClicked: root.chosen(chip.modelData)
            }

            IconButton {
                id: unpin
                anchors.right: parent.right
                anchors.rightMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                width: 28
                height: 28
                icon: "close"
                iconSize: 13
                toolTip: "Unpin " + chip.modelData
                onClicked: Settings.unpinFolder(chip.modelData)
            }

            ToolTip {
                visible: chip.hovered
                text: chip.modelData
                delay: 500
            }

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        || event.key === Qt.Key_Space) {
                    root.chosen(chip.modelData)
                    event.accepted = true
                }
            }
        }
    }
}
