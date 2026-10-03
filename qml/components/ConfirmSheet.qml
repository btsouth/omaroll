import QtQuick
import QtQuick.Controls.Basic

// A modal confirm, themed rather than borrowed from the platform dialog set.
//
// Callers describe the captured targets and distinguish recoverable Trash
// from explicit permanent deletion.
Item {
    id: root

    property string title: ""
    property string detail: ""
    property string confirmLabel: "Confirm"

    signal accepted()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function open() {
        visible = true
        forceActiveFocus()
    }

    function close() {
        visible = false
    }

    visible: false
    anchors.fill: parent
    focus: visible

    // Own wheel input for the whole modal so it cannot scroll the library
    // underneath, as Settings does.
    WheelHandler {
        target: null
        enabled: root.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) { event.accepted = true }
    }

    // Scrim. Swallows clicks so nothing behind it can be triggered by accident.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.55)

        // A MouseArea rather than a TapHandler: a TapHandler only takes a
        // passive grab, so a tap on a control inside the card also arrived
        // here and closed the sheet. Every button is accepted so a right
        // click cannot fall through to a tile behind the scrim.
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
        width: Math.min(400, root.width - 60)
        height: column.implicitHeight + 40
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        // Clicks inside the card must not reach the scrim behind it.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        Column {
            id: column
            anchors.centerIn: parent
            width: parent.width - 40
            spacing: 8

            Text {
                width: parent.width
                text: root.title
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: Font.DemiBold
                color: Theme.brightForeground
                wrapMode: Text.WordWrap
            }

            Flickable {
                id: details
                objectName: "confirmationDetails"
                width: parent.width
                height: Math.min(detailText.implicitHeight, Math.max(60, root.height - 180))
                contentWidth: width
                contentHeight: detailText.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                visible: root.detail !== ""
                ScrollBar.vertical: ScrollBar { width: 6; policy: ScrollBar.AsNeeded }
                Text {
                    id: detailText
                    width: parent.width - 12
                    text: root.detail
                    textFormat: Text.PlainText
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.mutedText
                    wrapMode: Text.WrapAnywhere
                }
            }

            Item { width: 1; height: 8 }

            Row {
                anchors.right: parent.right
                spacing: 8

                PillButton {
                    label: "Cancel"
                    onClicked: root.close()
                }

                PillButton {
                    label: root.confirmLabel
                    active: true
                    onClicked: {
                        root.close()
                        root.accepted()
                    }
                }
            }
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.close()
            root.accepted()
            event.accepted = true
        }
    }
}
