import QtQuick

Item {
    id: root

    property var paths: []
    property string errorMessage: ""
    signal saved(string name, int count)

    function open(files) {
        paths = files === undefined ? [] : files
        errorMessage = ""
        nameInput.text = ""
        visible = true
        nameInput.forceActiveFocus()
    }

    function close() { visible = false }

    function save() {
        const name = nameInput.text.trim().replace(/\s+/g, " ")
        if (!Settings.createAlbum(name)) {
            errorMessage = "Use a unique name without a slash."
            return
        }
        if (paths.length > 0) {
            Settings.addToAlbum(name, paths)
        }
        root.saved(name, paths.length)
        root.close()
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    visible: false
    anchors.fill: parent
    focus: visible

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.6)
        TapHandler { onSingleTapped: root.close() }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(420, root.width - 60)
        height: 190
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        TapHandler { onSingleTapped: {} }

        Column {
            anchors.fill: parent
            anchors.margins: 22
            spacing: 12

            Text {
                text: root.paths.length > 0 ? "Add to a new album" : "New album"
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

                TextInput {
                    id: nameInput
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    maximumLength: 60
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.foreground
                    selectionColor: root.shade(Theme.accent, 0.5)
                    selectedTextColor: Theme.brightForeground
                    Keys.onReturnPressed: root.save()
                    Keys.onEnterPressed: root.save()
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    visible: nameInput.text === ""
                    text: "Album name"
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: root.shade(Theme.foreground, 0.35)
                }
            }

            Row {
                width: parent.width
                spacing: 8

                Text {
                    width: parent.width - cancelButton.width - createButton.width - 16
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
                    id: createButton
                    label: "Create"
                    active: true
                    onClicked: root.save()
                }
            }
        }
    }

    Keys.onEscapePressed: root.close()
}
