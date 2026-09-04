import QtQuick

Item {
    id: root

    property var paths: []
    property string mode: "album"
    property var savedView: ({})
    property string errorMessage: ""
    signal saved(string name, int count, string mode)

    function open(files, requestedMode, view) {
        paths = files === undefined ? [] : files
        mode = requestedMode === undefined ? "album" : requestedMode
        savedView = view === undefined ? ({}) : view
        errorMessage = ""
        nameInput.text = ""
        visible = true
        nameInput.forceActiveFocus()
    }

    function close() { visible = false }

    function save() {
        const name = nameInput.text.trim().replace(/\s+/g, " ")
        const created = mode === "tag" ? Settings.createTag(name)
                        : mode === "smart" ? Settings.saveSmartCollection(name, savedView)
                                           : Settings.createAlbum(name)
        if (!created) {
            errorMessage = mode === "smart" ? "Use a name without a slash."
                                             : "Use a unique name without a slash."
            return
        }
        if (mode === "album" && paths.length > 0) {
            Settings.addToAlbum(name, paths)
        } else if (mode === "tag" && paths.length > 0) {
            Settings.addTag(name, paths)
        }
        root.saved(name, paths.length, mode)
        root.close()
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
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

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.6)
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
        width: Math.min(420, root.width - 60)
        height: 190
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
            anchors.fill: parent
            anchors.margins: 22
            spacing: 12

            Text {
                text: root.mode === "smart" ? "Save smart collection"
                      : root.paths.length > 0
                      ? "Add to a new " + root.mode : "New " + root.mode
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
                    objectName: "collectionNameInput"
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
                    text: root.mode === "smart" ? "Collection name"
                          : root.mode.charAt(0).toUpperCase() + root.mode.slice(1) + " name"
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
                    label: root.mode === "smart" ? "Save" : "Create"
                    active: true
                    onClicked: root.save()
                }
            }
        }
    }

    Keys.onEscapePressed: root.close()
}
