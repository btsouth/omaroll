import QtQuick

Item {
    id: root

    property var paths: []
    property string mode: "album"
    property var savedView: ({})
    property string errorMessage: ""
    // Set when the sheet is renaming an existing collection rather than
    // creating one. Membership moves with the name.
    property bool renaming: false
    property string renameFrom: ""
    signal saved(string name, int count, string mode)

    function open(files, requestedMode, view) {
        paths = files === undefined ? [] : files
        mode = requestedMode === undefined ? "album" : requestedMode
        savedView = view === undefined ? ({}) : view
        renaming = false
        renameFrom = ""
        errorMessage = ""
        nameInput.text = ""
        visible = true
        nameInput.forceActiveFocus()
    }

    function openRename(requestedMode, oldName) {
        paths = []
        mode = requestedMode === undefined ? "album" : requestedMode
        savedView = ({})
        renaming = true
        renameFrom = oldName
        errorMessage = ""
        nameInput.text = oldName
        visible = true
        nameInput.forceActiveFocus()
        nameInput.selectAll()
    }

    function close() { visible = false }

    function save() {
        const name = nameInput.text.trim().replace(/\s+/g, " ")
        if (renaming) {
            const renamed = mode === "tag" ? Settings.renameTag(renameFrom, name)
                                           : Settings.renameAlbum(renameFrom, name)
            if (!renamed) {
                errorMessage = Settings.organizationError || root.whyNot(name)
                return
            }
            root.saved(name, 0, mode + "Rename")
            root.close()
            return
        }
        // Created and filled in one step, so one Undo takes both back.
        const created = mode === "tag" ? Settings.createTagFor(name, paths, true).created
                        : mode === "smart" ? Settings.saveSmartCollection(name, savedView)
                                           : Settings.createAlbumFor(name, paths, true).created
        if (!created) {
            errorMessage = Settings.organizationError || root.whyNot(name)
            return
        }
        if (Settings.organizationError !== "") {
            errorMessage = Settings.organizationError
            return
        }
        root.saved(name, paths.length, mode)
        root.close()
    }

    // Why a name was refused, naming what is in the way.
    function whyNot(name) {
        if (name === "" || name.replace(/\//g, "").trim() === "") return "Type a name."
        if (mode !== "tag" && name.indexOf("/") >= 0)
            return (mode === "smart" ? "Collection" : "Album") + " names cannot contain a slash."
        const lower = name.toLowerCase()
        const same = function (other) { return other.toLowerCase() === lower }
        const taken = mode === "tag" ? Settings.existingTag(name)
                    : (mode === "smart" ? Settings.smartCollectionNames : Settings.albumNames).find(same)
        if (taken) {
            return "There is already " + (mode === "tag" ? "a tag" : mode === "smart" ? "a smart collection"
                                                                    : "an album")
                   + " called " + taken + "."
        }
        return "That name cannot be used."
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
        objectName: "collectionNamePanel"
        // Sized to what it holds, so the space under the buttons matches
        // the space over the title, with or without a message.
        height: form.implicitHeight + 2 * form.anchors.margins
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
            id: form
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 22
            spacing: 12

            Text {
                text: root.renaming ? "Rename " + root.mode
                      : root.mode === "smart" ? "Save smart collection"
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
                    // What AppSettings keeps: 80 for a tag, 60 otherwise.
                    maximumLength: root.mode === "tag" ? 80 : 60
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
                    text: root.renaming ? "New name"
                          : root.mode === "smart" ? "Collection name"
                          : root.mode.charAt(0).toUpperCase() + root.mode.slice(1) + " name"
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.mutedText
                }
            }

            Text {
                objectName: "collectionNamePanelMessage"
                width: parent.width
                visible: text !== ""
                text: root.errorMessage
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.red
            }

            Row {
                anchors.right: parent.right
                spacing: 8

                PillButton {
                    id: cancelButton
                    label: "Cancel"
                    onClicked: root.close()
                }
                PillButton {
                    id: createButton
                    label: root.renaming ? "Rename"
                          : root.mode === "smart" ? "Save" : "Create"
                    active: true
                    onClicked: root.save()
                }
            }
        }
    }

    Keys.onEscapePressed: root.close()
}
