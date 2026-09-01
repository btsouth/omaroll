import QtQuick
import QtQuick.Controls.Basic
import Omaroll

ApplicationWindow {
    id: root

    width: 1180
    height: 760
    minimumWidth: 520
    minimumHeight: 400
    visible: true
    title: "Omaroll"

    // The window paints nothing. Chrome carries the theme's alpha, and every
    // thumbnail is drawn opaque on top of it.
    color: "transparent"

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
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
        height: 62

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
                text: Captures.empty
                      ? "Nothing yet"
                      : Captures.count + (Captures.count === 1 ? " capture" : " captures")
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
            shown: !Captures.empty
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: root.shade(Theme.foreground, 0.10)
        }
    }

    CaptureGrid {
        id: library
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        anchors.margins: 8
        focus: true

        model: Captures
        visible: !Captures.empty

        onChosen: function (path) { Actions.open(path) }
    }

    // Empty state. Says what omaroll is looking for and where, because "no
    // items" tells someone nothing about how to fix it.
    Column {
        anchors.centerIn: library
        width: Math.min(420, parent.width - 80)
        spacing: 10
        visible: Captures.empty && !Captures.scanning

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "No captures yet"
            font.family: Theme.fontFamily
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: Theme.brightForeground
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Take a screenshot with Super + Shift + S, or record with the Capture menu. "
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
        height: 34

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
            anchors.verticalCenter: parent.verticalCenter
            text: notice.text !== ""
                  ? notice.text
                  : "Enter open   ·   F show in files   ·   R rescan   ·   hjkl move"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: notice.text !== "" ? Theme.red : root.shade(Theme.foreground, 0.45)
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.themeName
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: root.shade(Theme.foreground, 0.35)
        }
    }

    // Transient error line, so a missing handler is reported where the user is
    // looking rather than swallowed.
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
        onActivated: {
            const path = Captures.pathAt(library.currentIndex)
            if (path !== "") {
                Actions.showInFiles(path)
            }
        }
    }

    Shortcut {
        sequences: ["R"]
        onActivated: Captures.refresh()
    }

    Shortcut {
        sequence: StandardKey.Quit
        onActivated: Qt.quit()
    }

    Shortcut {
        sequence: "Escape"
        onActivated: root.close()
    }
}
