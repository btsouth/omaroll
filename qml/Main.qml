import QtQuick
import QtQuick.Controls.Basic
import Omaroll

ApplicationWindow {
    id: root

    width: 1180
    height: 760
    minimumWidth: 560
    minimumHeight: 420
    visible: true
    title: "Omaroll"

    // The window paints nothing. Chrome carries the theme's alpha, and every
    // thumbnail is drawn opaque on top of it.
    color: "transparent"

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    // Deleting acts on a path captured when the sheet opened, not on whatever
    // happens to be selected when it is confirmed. Sorting or a rescan can move
    // the selection under a modal, and trashing the wrong file is unforgivable.
    property string pendingDeletePath: ""

    function requestDelete(path) {
        if (path === "") {
            return
        }
        root.pendingDeletePath = path
        confirm.detail = path
        confirm.open()
    }

    Chrome {
        anchors.fill: parent
    }

    // Header
    Item {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 58

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "omaroll"
                font.family: Theme.fontFamily
                font.pixelSize: 17
                font.weight: Font.Bold
                color: Theme.brightForeground
                Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 1
                height: 16
                color: root.shade(Theme.foreground, 0.18)
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    if (Captures.count === 0) {
                        return "Nothing to show"
                    }
                    const shown = Captures.count
                    const total = Captures.sourceCount()
                    const noun = shown === 1 ? " capture" : " captures"
                    return shown === total ? shown + noun : shown + " of " + total + noun
                }
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.mutedText
                Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
            }
        }

        // Current day, tracking the top of the grid.
        DayHeader {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            label: library.currentDayLabel
            shown: Captures.count > 0
        }
    }

    FilterBar {
        id: filters
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
    }

    Rectangle {
        id: divider
        anchors.top: filters.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: root.shade(Theme.foreground, 0.10)
        Behavior on color { ColorAnimation { duration: 180 } }
    }

    CaptureGrid {
        id: library
        anchors.top: divider.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        anchors.topMargin: 8
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.bottomMargin: 4
        focus: true

        model: Captures
        visible: Captures.count > 0

        onChosen: function (path) { Actions.open(path) }
        onDeleteRequested: function (path) { root.requestDelete(path) }
    }

    // Empty state. Two different empties: nothing exists, or nothing matches
    // the current filter. Telling them apart is the difference between useful
    // and useless.
    Column {
        anchors.centerIn: library
        width: Math.min(420, parent.width - 80)
        spacing: 10
        visible: Captures.count === 0 && !Library.scanning

        readonly property bool filtered: Captures.sourceCount() > 0

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: parent.filtered ? "Nothing matches" : "No captures yet"
            font.family: Theme.fontFamily
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: Theme.brightForeground
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: parent.filtered
                  ? "Try a different filter, or clear the search."
                  : "Take a screenshot with Super + Shift + S, or record from the Capture menu. "
                    + "Anything that lands in your Pictures and Videos folders shows up here."
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.mutedText
        }
    }

    // Footer
    Item {
        id: footer
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 32

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: root.shade(Theme.foreground, 0.10)
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.right: themeLabel.left
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: notice.text !== ""
                  ? notice.text
                  : "Enter open   ·   F files   ·   Del trash   ·   / search   ·   R rescan   ·   hjkl move"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: notice.text !== "" ? Theme.red : root.shade(Theme.foreground, 0.42)
            Behavior on color { ColorAnimation { duration: 180 } }
        }

        Text {
            id: themeLabel
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.themeName
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: root.shade(Theme.foreground, 0.32)
        }
    }

    ConfirmSheet {
        id: confirm
        title: "Move this capture to Trash?"
        confirmLabel: "Move to Trash"
        onAccepted: {
            if (Actions.moveToTrash(root.pendingDeletePath)) {
                notice.text = "Moved to Trash"
                noticeTimer.restart()
                Library.refresh()
            }
            root.pendingDeletePath = ""
        }
    }

    // Transient status line, so a result or a missing handler is reported where
    // the user is already looking rather than swallowed.
    QtObject {
        id: notice
        property string text: ""
    }

    Timer {
        id: noticeTimer
        interval: 4000
        onTriggered: notice.text = ""
    }

    Connections {
        target: Actions
        function onFailed(message) {
            notice.text = message
            noticeTimer.restart()
        }
    }

    Shortcut {
        sequences: ["F"]
        enabled: !confirm.visible
        onActivated: {
            const path = Captures.pathAt(library.currentIndex)
            if (path !== "") {
                Actions.showInFiles(path)
            }
        }
    }

    Shortcut {
        sequences: ["R"]
        enabled: !confirm.visible
        onActivated: Library.refresh()
    }

    Shortcut {
        sequences: ["/"]
        enabled: !confirm.visible
        onActivated: filters.focusSearch()
    }

    Shortcut {
        sequence: StandardKey.Quit
        onActivated: Qt.quit()
    }

    Shortcut {
        sequence: "Escape"
        enabled: !confirm.visible
        onActivated: root.close()
    }
}
