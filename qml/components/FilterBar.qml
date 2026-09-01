import QtQuick
import QtQuick.Controls.Basic

// Kind filter, search and sort. One row, left to right in the order someone
// actually reaches for them: narrow what is shown, then find one thing, then
// change the order.
Item {
    id: root

    // Mirrors CaptureRecord::Kind. -1 is every kind.
    readonly property int kindAll: -1
    readonly property int kindScreenshot: 0
    readonly property int kindRecording: 1
    readonly property int kindPicture: 2
    readonly property int kindVideo: 3

    readonly property var sortLabels: ["Newest", "Oldest", "Largest", "Smallest", "Name"]

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function focusSearch() {
        search.forceActiveFocus()
        search.selectAll()
    }

    implicitHeight: 40

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        PillButton {
            label: "All"
            active: Captures.kindFilter === root.kindAll
            onClicked: Captures.kindFilter = root.kindAll
        }
        PillButton {
            label: "Screenshots"
            active: Captures.kindFilter === root.kindScreenshot
            onClicked: Captures.kindFilter = root.kindScreenshot
        }
        PillButton {
            label: "Recordings"
            active: Captures.kindFilter === root.kindRecording
            onClicked: Captures.kindFilter = root.kindRecording
        }
        PillButton {
            label: "Pictures"
            active: Captures.kindFilter === root.kindPicture
            onClicked: Captures.kindFilter = root.kindPicture
        }
        PillButton {
            label: "Videos"
            active: Captures.kindFilter === root.kindVideo
            onClicked: Captures.kindFilter = root.kindVideo
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        // Search
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 200
            height: 26
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            color: root.shade(Theme.foreground, search.activeFocus ? 0.10 : 0.05)
            border.width: 1
            border.color: search.activeFocus
                          ? root.shade(Theme.accent, 0.6)
                          : root.shade(Theme.foreground, 0.12)

            Behavior on color { ColorAnimation { duration: 140 } }
            Behavior on border.color { ColorAnimation { duration: 140 } }

            TextInput {
                id: search
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.foreground
                selectionColor: root.shade(Theme.accent, 0.5)
                selectedTextColor: Theme.brightForeground

                onTextChanged: Captures.searchText = text
                Keys.onEscapePressed: {
                    text = ""
                    focus = false
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 10
                visible: search.text === "" && !search.activeFocus
                text: "Search  /"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: root.shade(Theme.foreground, 0.35)
            }
        }

        // Sort. A menu rather than five more pills, because the sort is changed
        // far less often than the filter and does not deserve equal weight.
        PillButton {
            id: sortButton
            anchors.verticalCenter: parent.verticalCenter
            label: root.sortLabels[Captures.sortMode] + "  ▾"
            active: sortMenu.visible
            onClicked: sortMenu.visible ? sortMenu.close() : sortMenu.popup(sortButton, 0, sortButton.height + 4)
        }
    }

    Menu {
        id: sortMenu

        background: Rectangle {
            implicitWidth: 150
            color: root.shade(Theme.background, 0.97)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }

        Repeater {
            model: root.sortLabels

            MenuItem {
                required property int index
                required property string modelData

                height: 30

                contentItem: Text {
                    text: modelData
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Captures.sortMode === index ? Font.DemiBold : Font.Normal
                    color: Captures.sortMode === index ? Theme.accent : Theme.foreground
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 12
                }

                background: Rectangle {
                    color: parent.hovered ? root.shade(Theme.foreground, 0.08) : "transparent"
                }

                onTriggered: Captures.sortMode = index
            }
        }
    }
}
