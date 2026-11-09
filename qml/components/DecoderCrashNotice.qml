import QtQuick
import QtQuick.Window

// Said once after Omaroll crashed while reading a picture: which file it was,
// that it is skipped until it changes, and where it is.
Rectangle {
    id: root

    readonly property bool wanted: DecodeCrash.path !== ""

    visible: wanted
    implicitWidth: row.implicitWidth + 24
    implicitHeight: row.implicitHeight + 14
    radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
    color: Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0.96)
    border.width: 1
    border.color: Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.16)

    Accessible.role: Accessible.AlertMessage
    Accessible.name: message.text

    // Once it has been on screen, a later launch does not say it again: the
    // library's window shown and not minimized, not only this item visible.
    readonly property bool onScreen: visible && Window.window !== null
                                     && Window.visibility !== Window.Hidden
                                     && Window.visibility !== Window.Minimized
    onOnScreenChanged: if (onScreen) DecodeCrash.shown()
    Component.onCompleted: if (onScreen) DecodeCrash.shown()

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 12

        Text {
            id: message
            objectName: "decoderCrashNotice"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(120, root.width - 24 - showButton.width
                                                         - dismissButton.width - 2 * row.spacing))
            elide: Text.ElideMiddle
            text: "Omaroll closed while reading " + DecodeCrash.fileName
                  + " last time. It is skipped until the file changes."
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.foreground
        }

        PillButton {
            id: showButton
            objectName: "decoderCrashShow"
            anchors.verticalCenter: parent.verticalCenter
            visible: Registry.available("files")
            label: "Show in folder"
            onClicked: Registry.run("files", DecodeCrash.path)
        }

        PillButton {
            id: dismissButton
            objectName: "decoderCrashDismiss"
            anchors.verticalCenter: parent.verticalCenter
            label: "Dismiss"
            onClicked: DecodeCrash.dismiss()
        }
    }
}
