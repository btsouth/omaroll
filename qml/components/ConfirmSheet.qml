import QtQuick

// A modal confirm, themed rather than borrowed from the platform dialog set.
//
// Deleting is the one destructive thing omaroll does, so it asks first and says
// exactly which file and where it is going. "Move to Trash" rather than
// "Delete" because that is literally what happens and the file is recoverable.
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

            Text {
                width: parent.width
                text: root.detail
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.mutedText
                wrapMode: Text.WrapAnywhere
                visible: text !== ""
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
