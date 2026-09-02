import QtQuick

Item {
    id: root

    property string path: ""
    property string suffix: ""
    property string errorMessage: ""
    signal renamed(string oldPath, string newPath, string fileName)

    function open(filePath, fileName) {
        path = filePath
        const dot = fileName.lastIndexOf(".")
        suffix = dot >= 0 ? fileName.substring(dot) : ""
        nameInput.text = dot >= 0 ? fileName.substring(0, dot) : fileName
        errorMessage = ""
        visible = true
        nameInput.forceActiveFocus()
        nameInput.selectAll()
    }

    function close() { visible = false }

    function save() {
        const oldPath = path
        const result = Actions.renameFile(oldPath, nameInput.text)
        if (!result.ok) {
            errorMessage = result.error
            return
        }
        root.close()
        root.renamed(oldPath, result.path, result.fileName)
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
        onWheel: function (event) { event.accepted = true }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.6)

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
        width: Math.min(440, root.width - 60)
        height: 194
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        Column {
            anchors.fill: parent
            anchors.margins: 22
            spacing: 12

            Text {
                text: "Rename file"
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }

            Rectangle {
                width: parent.width
                height: 34
                radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                color: root.shade(Theme.foreground, 0.06)
                border.width: 1
                border.color: nameInput.activeFocus
                              ? root.shade(Theme.accent, 0.65)
                              : root.shade(Theme.foreground, 0.16)

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10

                    TextInput {
                        id: nameInput
                        objectName: "renameInput"
                        width: parent.width - extension.width
                        height: parent.height
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        maximumLength: 220
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                        selectionColor: root.shade(Theme.accent, 0.5)
                        selectedTextColor: Theme.brightForeground
                        onTextChanged: root.errorMessage = ""
                        Keys.onReturnPressed: root.save()
                        Keys.onEnterPressed: root.save()
                    }

                    Text {
                        id: extension
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.suffix
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.mutedText
                    }
                }
            }

            Row {
                width: parent.width
                spacing: 8

                Text {
                    width: parent.width - cancelButton.width - renameButton.width - 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.errorMessage
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    color: Theme.red
                }

                PillButton {
                    id: cancelButton
                    label: "Cancel"
                    onClicked: root.close()
                }
                PillButton {
                    id: renameButton
                    label: "Rename"
                    active: true
                    onClicked: root.save()
                }
            }
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.save()
            event.accepted = true
        }
    }
}
