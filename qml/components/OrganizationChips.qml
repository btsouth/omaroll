import QtQuick
import QtQuick.Controls.Basic

// The albums and tags on one file, as chips, and a field that adds a tag.
// Albums come first. A tag that only the file's own keywords put there has a
// file icon and an outline instead of a fill: removing it hides it here and
// leaves the file as it is. Each chip takes focus; Delete or Backspace
// removes it.
Column {
    id: root

    property string path: ""
    // Over a picture or video: dark in every theme (see Osd).
    property bool osd: false
    readonly property bool editing: field.editing
    readonly property Item tagField: field
    // What changed, and the undo step that takes it back (0 for none).
    signal statusRequested(string message, int undoId)
    // A chip just added, for the scrolled panel around it to bring into view.
    signal revealRequested(Item item)

    readonly property color textColour: root.osd ? Osd.foreground : Theme.foreground
    readonly property color mutedColour: root.osd ? Osd.mutedText : Theme.mutedText
    readonly property color accentColour: root.osd ? Osd.accent : Theme.accent

    property int revision: 0
    Connections {
        target: Settings
        function onTagsChanged() { root.rebuild() }
        function onAlbumsChanged() { root.rebuild() }
    }

    // The chips are made again on every change, Undo included. A chip that
    // had the keyboard hands it to the same chip made again, else to the one
    // now in its place, else to the field; otherwise it would go nowhere and
    // the next Tab would start over at the top of the window.
    property var focusedChip: null
    function rebuild() {
        const window = root.Window.window
        const focused = window ? window.activeFocusItem : null
        for (let index = 0; focused && index < chipRepeater.count; ++index) {
            const chip = chipRepeater.itemAt(index)
            if (chip === focused) {
                root.focusedChip = {kind: chip.modelData.kind, name: chip.modelData.name, index: index}
                Qt.callLater(root.restoreChipFocus)
                break
            }
        }
        root.revision++
    }
    function restoreChipFocus() {
        const wanted = root.focusedChip
        root.focusedChip = null
        if (!wanted) return
        if (!root.visible) return
        const same = root.chipItem(wanted.kind, wanted.name)
        const next = same ? same
                   : chipRepeater.count > 0 ? chipRepeater.itemAt(Math.min(wanted.index, chipRepeater.count - 1))
                   : null
        if (next) next.forceActiveFocus(Qt.TabFocusReason)
        else field.focusField()
    }
    function chipItem(kind, name) {
        for (let index = 0; index < chipRepeater.count; ++index) {
            const chip = chipRepeater.itemAt(index)
            if (chip && chip.modelData.kind === kind && chip.modelData.name === name) return chip
        }
        return null
    }

    property string addedTag: ""
    function revealAdded() {
        const chip = root.chipItem("tag", root.addedTag)
        root.addedTag = ""
        if (chip) root.revealRequested(chip)
    }
    readonly property var chips: {
        root.revision
        if (root.path === "") return []
        const albums = Settings.albumsForPath(root.path).map(function (name) {
            return {kind: "album", name: name, fromFile: false}
        })
        const tags = Settings.tagSources(root.path).map(function (tag) {
            return {kind: "tag", name: tag.name, fromFile: tag.fromFile}
        })
        return albums.concat(tags)
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function focusTagField() {
        field.focusField()
    }

    // Escape while typing a tag: clears it, and says whether it did.
    function clearField() {
        return field.clearTyped()
    }

    function remove(index) {
        const chip = root.chips[index]
        if (!chip) return
        const before = Settings.latestUndoId
        let message
        if (chip.kind === "album") {
            Settings.removeFromAlbum(chip.name, [root.path])
            message = "Removed from " + chip.name
        } else {
            Settings.removeTag(chip.name, [root.path])
            message = chip.fromFile ? "Hid tag " + chip.name + " here; the file is unchanged"
                                    : "Removed tag " + chip.name
        }
        root.statusRequested(Settings.organizationError || message,
                             Settings.latestUndoId !== before ? Settings.latestUndoId : 0)
        // The chip that takes its place, or the field after the last one.
        Qt.callLater(function () {
            const next = chipRepeater.count > 0 ? chipRepeater.itemAt(Math.min(index, chipRepeater.count - 1)) : null
            if (next) next.forceActiveFocus(Qt.TabFocusReason)
            else field.focusField()
        })
    }

    spacing: 6

    Flow {
        objectName: "organizationChipFlow"
        width: parent.width
        spacing: 6
        visible: root.chips.length > 0

        Repeater {
            id: chipRepeater
            model: root.chips

            Rectangle {
                id: chip
                required property var modelData
                required property int index
                readonly property bool album: modelData.kind === "album"
                readonly property bool fromFile: modelData.fromFile === true
                objectName: "organizationChip_" + modelData.kind + "_" + modelData.name
                height: 24
                width: Math.min(root.width, chipRow.implicitWidth + 16)
                radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 12) : 3
                color: chip.fromFile ? "transparent"
                       : chip.album ? root.shade(root.textColour, 0.09)
                                    : root.shade(root.accentColour, 0.16)
                border.width: chip.activeFocus ? 2 : 1
                border.color: chip.activeFocus ? root.accentColour
                              : chip.fromFile ? root.shade(root.textColour, 0.40)
                              : chip.album ? root.shade(root.textColour, 0.20)
                                           : root.shade(root.accentColour, 0.45)
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: (chip.album ? "Album " : "Tag ") + modelData.name
                Accessible.description: chip.fromFile
                    ? "From the file's keywords. Removing hides it here; the file is not changed."
                    : "Delete removes it"
                Keys.onPressed: function (event) {
                    if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
                            && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.ShiftModifier))) {
                        root.remove(chip.index)
                        event.accepted = true
                    }
                }

                Row {
                    id: chipRow
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4

                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: chip.album || chip.fromFile
                        name: chip.album ? "grid" : "file"
                        size: 12
                        color: root.mutedColour
                    }
                    Text {
                        id: chipLabel
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, Math.max(24, root.width - 56))
                        text: chip.modelData.name
                        elide: Text.ElideMiddle
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: root.textColour
                    }
                    Item {
                        width: 16
                        height: 16
                        anchors.verticalCenter: parent.verticalCenter
                        Accessible.role: Accessible.Button
                        Accessible.name: (chip.album ? "Remove from album " : "Remove tag ") + chip.modelData.name
                        Accessible.onPressAction: root.remove(chip.index)

                        Icon {
                            anchors.centerIn: parent
                            name: "close"
                            size: 12
                            color: removeMouse.containsMouse ? root.textColour : root.mutedColour
                        }
                        MouseArea {
                            id: removeMouse
                            anchors.fill: parent
                            anchors.margins: -3
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.remove(chip.index)
                        }
                    }
                }

                HoverHandler { id: chipHover }
                ThemedToolTip {
                    requested: chipHover.hovered
                    keyboardRequested: chip.activeFocus && chip.Window.window
                                       && chip.Window.window.keyboardNavigation === true
                    text: chip.fromFile ? chip.modelData.name + "\nFrom the file's keywords. Removing hides it here; the file is not changed."
                          : chipLabel.truncated ? chip.modelData.name : ""
                    delay: 500
                }
            }
        }
    }

    TagField {
        id: field
        objectName: "organizationTagField"
        width: parent.width
        osd: root.osd
        paths: root.path !== "" ? [root.path] : []
        present: root.chips.filter(function (chip) { return chip.kind === "tag" })
                     .map(function (chip) { return chip.name })
        onTagged: function (message, undoId) { root.statusRequested(message, undoId) }
        onAdded: function (name) {
            root.addedTag = name
            Qt.callLater(root.revealAdded)
        }
        onUndoRequested: if (Settings.canUndo()) root.statusRequested(Settings.undo(), 0)
    }
}
