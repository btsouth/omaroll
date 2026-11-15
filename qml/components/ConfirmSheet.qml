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
    property string cancelLabel: "Cancel"

    signal accepted()
    // Only from the cancel button; Escape and the scrim just close.
    signal cancelled()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    // The confirm button, which Enter also presses, takes the keyboard, so
    // a screen reader starts on the question's answer.
    function open() {
        visible = true
        confirmButton.forceActiveFocus(Qt.PopupFocusReason)
    }

    function close() {
        visible = false
    }

    visible: false
    anchors.fill: parent
    focus: visible
    // A dialog, named by its question, with the detail read after it.
    Accessible.role: Accessible.Dialog
    Accessible.name: root.title
    Accessible.description: root.detail

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
                objectName: "confirmationTitle"
                width: parent.width
                Accessible.role: Accessible.Heading
                Accessible.name: text
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
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                    width: parent.width - 12
                    text: root.detail
                    textFormat: Text.PlainText
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.mutedText
                    // Words stay whole; a long path still breaks where it must.
                    wrapMode: Text.Wrap
                }
            }

            Item { width: 1; height: 8 }

            Row {
                anchors.right: parent.right
                spacing: 8

                PillButton {
                    id: cancelButton
                    objectName: "confirmSheetCancel"
                    label: root.cancelLabel
                    // Tab is the sheet's own (below): a button that took it
                    // would pass focus on to whatever lies behind the scrim.
                    activeFocusOnTab: false
                    onClicked: {
                        root.close()
                        root.cancelled()
                    }
                }

                PillButton {
                    id: confirmButton
                    objectName: "confirmSheetConfirm"
                    label: root.confirmLabel
                    active: true
                    activeFocusOnTab: false
                    Accessible.defaultButton: true
                    onClicked: {
                        root.close()
                        root.accepted()
                    }
                }
            }
        }
    }

    Keys.onPressed: function (event) {
        // Tab stays on the two buttons; nothing behind the scrim is reachable.
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            (cancelButton.activeFocus ? confirmButton : cancelButton).forceActiveFocus(Qt.TabFocusReason)
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.close()
            root.accepted()
            event.accepted = true
        }
    }
}
