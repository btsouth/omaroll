import QtQuick
import QtQuick.Controls.Basic

// Organize: the albums and tags for the checked or right-clicked files, each
// with a check that says whether all, some or none of them hold it. A click
// or Space on an unchecked or mixed row adds every file; on a checked row it
// takes every file out. The field at the top filters the rows, and a name
// that matches nothing can be created as an album or a tag.
Popup {
    id: menu
    // The library window, for its colors and spoken status.
    required property var host
    required property Item nameSheet
    // The grid whose checked files the actions apply to.
    required property var grid
    property var targetPaths: []
    objectName: "albumActionMenu"
    // Modal, undimmed: the press that closes the menu is consumed here
    // rather than also landing on the tile or pill under it.
    modal: true
    dim: false
    focus: true
    padding: 6
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    parent: host.contentItem
    width: Math.min(320, host.width - 16)
    // One size for as long as it is open, set when it opens from every row:
    // filtering shortens the list inside it rather than the menu, so the
    // menu neither jumps nor resizes while typing.
    property real openHeight: 0
    height: Math.min(openHeight, host.contentItem.height - topMargin - bottomMargin)
    margins: 8
    // Clear of the footer, which reports each change.
    bottomMargin: 40

    property int revision: 0
    Connections {
        target: Settings
        function onAlbumsChanged() { menu.revision++ }
        function onTagsChanged() { menu.revision++ }
    }
    readonly property var allRows: {
        menu.revision
        if (!menu.visible) return []
        // Folded once here, not for every row on every key.
        return Settings.organizationFor(menu.targetPaths).map(function (row) {
            row.folded = menu.folded(row.name)
            return row
        })
    }
    readonly property string typed: filter.text.trim().replace(/\s+/g, " ")
    // The rows the filter leaves, then a create row for each kind the typed
    // name matches nothing of, or the New album and New tag rows.
    readonly property var rows: {
        const query = menu.folded(menu.typed)
        const shown = query === "" ? menu.allRows : menu.allRows.filter(function (row) {
            return row.folded.indexOf(query) >= 0
        })
        const out = shown.slice()
        if (menu.typed === "") {
            out.push({kind: "album", name: "", create: true})
            out.push({kind: "tag", name: "", create: true})
            return out
        }
        const exact = function (kind) {
            return menu.allRows.some(function (row) {
                return row.kind === kind && row.folded === query
            })
        }
        // An album name is cut at 60 characters; compare what would be made,
        // and the row says it was cut.
        const album = menu.typed.slice(0, menu.albumLimit).trim()
        const albumFolded = menu.folded(album)
        const albumExists = menu.allRows.some(function (row) {
            return row.kind === "album" && row.folded === albumFolded
        })
        if (!albumExists && album.indexOf("/") < 0)
            out.push({kind: "album", name: album, create: true, cut: menu.typed.length > menu.albumLimit})
        if (!exact("tag")) out.push({kind: "tag", name: menu.typed, create: true})
        return out
    }

    // What AppSettings keeps of a name.
    readonly property int albumLimit: 60
    readonly property int tagLimit: 80

    function folded(text) {
        return text.normalize("NFKD").replace(/[̀-ͯ]/g, "").toLowerCase()
    }

    // Opens at |x|, |y| in |item|'s coordinates, like Menu.popup.
    function popup(item, x, y) {
        const point = (item || host.contentItem).mapToItem(host.contentItem, x || 0, y || 0)
        menu.x = Math.max(8, Math.min(point.x, host.contentItem.width - menu.width - 8))
        menu.y = point.y
        filter.text = ""
        // The field, then every row of the unfiltered list, up to what the
        // window holds.
        const rows = Settings.organizationFor(menu.targetPaths).length + 2
        const room = host.contentItem.height - menu.topMargin - menu.bottomMargin
        menu.openHeight = Math.max(120, Math.min(room, 30 + 6 + rows * 30 + 2 * menu.padding))
        menu.open()
        list.currentIndex = -1
        // Upward when the space below cannot hold it.
        const bottom = host.contentItem.height - menu.bottomMargin
        if (menu.y + menu.height > bottom)
            menu.y = Math.max(menu.topMargin, Math.min(point.y, bottom) - menu.height)
        filter.forceActiveFocus()
    }

    function stateOf(row) {
        if (row.create) return Qt.Unchecked
        return row.count <= 0 ? Qt.Unchecked
             : row.count >= menu.targetPaths.length ? Qt.Checked : Qt.PartiallyChecked
    }

    function accessibleName(row) {
        if (row.create) return row.name === "" ? (row.kind === "album" ? "New album" : "New tag")
                                               : "Create " + row.kind + " " + row.name
                                                 + (row.cut ? ", cut to " + menu.albumLimit + " characters" : "")
        return row.name + ", " + row.kind + ", on " + row.count + " of " + menu.targetPaths.length
    }

    function items(count) {
        return count === 1 ? "1 item" : count + " items"
    }

    function activate(index) {
        const row = menu.rows[index]
        if (!row) return
        const paths = menu.targetPaths
        if (row.create && row.name === "") {
            menu.close()
            nameSheet.open(paths, row.kind === "tag" ? "tag" : undefined)
            return
        }
        const before = Settings.latestUndoId
        // The rows are made again after each change; keep the place.
        const keep = list.currentIndex
        let message
        if (row.create) {
            // Create never adds to an album that is already there.
            const result = row.kind === "album" ? Settings.createAlbumFor(row.name, paths, true)
                                                : Settings.createTagFor(row.name, paths)
            if (result.name === "") {
                host.say(Settings.organizationError || "Use a shorter name"
                         + (row.kind === "album" ? " without a slash" : ""))
                return
            }
            message = (result.created ? "Created " + row.kind + " " : "Tagged as ") + result.name
                      + (result.created ? " with " : " on ") + menu.items(result.count)
            filter.text = ""
        } else if (menu.stateOf(row) === Qt.Checked) {
            if (row.kind === "album") {
                Settings.removeFromAlbum(row.name, paths)
                message = "Removed " + menu.items(paths.length) + " from " + row.name
            } else {
                Settings.removeTag(row.name, paths)
                message = "Removed tag " + row.name + " from " + menu.items(paths.length)
            }
        } else {
            const missing = paths.length - row.count
            if (row.kind === "album") {
                Settings.addToAlbum(row.name, paths)
                message = "Added " + menu.items(missing) + " to " + row.name
            } else {
                Settings.addTag(row.name, paths)
                message = "Tagged " + menu.items(missing) + " as " + row.name
            }
        }
        list.currentIndex = Math.min(keep, list.count - 1)
        const undoId = Settings.latestUndoId !== before ? Settings.latestUndoId : 0
        // An undo step does not mean the change was saved.
        if (Settings.organizationError !== "") host.say(Settings.organizationError)
        else if (undoId > 0) host.sayUndoable(message, undoId)
        else host.say(message)
    }

    // Arrow keys from the field and the list; typing goes back to the field.
    function handleKey(event, fromField) {
        const count = list.count
        // Ctrl+Z takes back the last change, as it does in the library; the
        // field keeps it for its own text while there is any.
        if (event.key === Qt.Key_Z && (event.modifiers & Qt.ControlModifier)
                && !(event.modifiers & (Qt.ShiftModifier | Qt.AltModifier))
                && (!fromField || filter.text === "")) {
            const keep = list.currentIndex
            host.undo()
            list.currentIndex = Math.min(keep, list.count - 1)
            return true
        }
        if (event.key === Qt.Key_Down || event.key === Qt.Key_Up
                || event.key === Qt.Key_Home || event.key === Qt.Key_End) {
            if (fromField && (event.key === Qt.Key_Home || event.key === Qt.Key_End)) return false
            if (count === 0 || (fromField && event.key === Qt.Key_Up)) return true
            let next = list.currentIndex
            // From the field, Down enters the list at the row Enter would take.
            if (fromField) next = Math.max(0, next)
            else if (event.key === Qt.Key_Down) next = Math.min(count - 1, next + 1)
            else if (event.key === Qt.Key_Up) next = next - 1
            else if (event.key === Qt.Key_Home) next = 0
            else next = count - 1
            if (next < 0) {
                list.currentIndex = -1
                filter.forceActiveFocus()
            } else {
                list.currentIndex = next
                list.positionViewAtIndex(next, ListView.Contain)
                list.forceActiveFocus()
            }
            return true
        }
        if (event.key === Qt.Key_Escape) {
            menu.close()
            return true
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || (!fromField && event.key === Qt.Key_Space)) {
            // From the field, Enter takes the first row once something is
            // typed, so a new name is one key away.
            const index = !fromField ? list.currentIndex
                        : menu.typed !== "" && count > 0 ? Math.max(0, list.currentIndex) : -1
            if (index >= 0) menu.activate(index)
            return true
        }
        if (!fromField && event.text !== "" && event.text.charCodeAt(0) >= 32
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
            filter.text += event.text
            filter.cursorPosition = filter.text.length
            list.currentIndex = -1
            filter.forceActiveFocus()
            return true
        }
        if (!fromField && event.key === Qt.Key_Backspace) {
            filter.text = filter.text.slice(0, -1)
            list.currentIndex = -1
            filter.forceActiveFocus()
            return true
        }
        return false
    }

    onClosed: filter.text = ""

    background: Rectangle {
        color: Theme.background
        border.width: 1
        border.color: host.shade(Theme.foreground, 0.16)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
    }

    contentItem: Column {
        spacing: 6

        Rectangle {
            width: parent.width
            height: 30
            radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 4) : 3
            color: host.shade(Theme.foreground, 0.06)
            border.width: 1
            border.color: filter.activeFocus ? host.shade(Theme.accent, 0.65)
                                             : host.shade(Theme.foreground, 0.16)

            TextInput {
                id: filter
                objectName: "organizeFilter"
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                focus: true
                // The longest name anything here keeps (a tag's); a longer
                // album name is cut, and its create row says so.
                maximumLength: menu.tagLimit
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.foreground
                selectionColor: host.shade(Theme.accent, 0.5)
                selectedTextColor: Theme.brightForeground
                Accessible.role: Accessible.EditableText
                Accessible.name: "Filter albums and tags, or name a new one"
                // After the rows follow the text: the first row is what Enter takes.
                onTextChanged: Qt.callLater(function () { list.currentIndex = menu.typed !== "" ? 0 : -1 })
                Keys.onPressed: function (event) {
                    if (menu.handleKey(event, true)) event.accepted = true
                }
            }
            Text {
                anchors.fill: filter
                verticalAlignment: Text.AlignVCenter
                visible: filter.text === ""
                text: "Filter or name a new album or tag"
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.mutedText
            }
        }

        ListView {
            id: list
            objectName: "organizeList"
            width: parent.width
            height: Math.max(0, menu.availableHeight - 36)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: menu.rows
            currentIndex: -1
            keyNavigationEnabled: false
            Accessible.role: Accessible.List
            Accessible.name: "Albums and tags"
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Keys.onPressed: function (event) {
                if (menu.handleKey(event, false)) event.accepted = true
            }

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property int checkState: menu.stateOf(modelData)
                readonly property bool current: ListView.isCurrentItem
                // A new kind starts after a gap, so albums and tags read as groups.
                readonly property bool firstOfKind: index > 0 && !modelData.create
                                                    && menu.rows[index - 1].kind !== modelData.kind
                objectName: !modelData.create ? "organizeRow_" + modelData.kind + "_" + modelData.name
                            : modelData.name !== "" ? "organizeCreate_" + modelData.kind
                            : modelData.kind === "album" ? "albumMenuNewAlbum" : "albumMenuNewTag"
                width: ListView.view.width
                height: 30
                radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 0
                color: row.current || rowMouse.containsMouse ? host.shade(Theme.foreground, 0.09) : "transparent"
                border.width: row.current && list.activeFocus ? 1 : 0
                border.color: host.shade(Theme.accent, 0.65)
                Accessible.role: modelData.create ? Accessible.Button : Accessible.CheckBox
                Accessible.name: menu.accessibleName(modelData)
                Accessible.checkable: !modelData.create
                Accessible.checked: row.checkState === Qt.Checked
                Accessible.checkStateMixed: row.checkState === Qt.PartiallyChecked
                Accessible.onPressAction: menu.activate(row.index)
                Accessible.onToggleAction: menu.activate(row.index)

                Rectangle {
                    visible: row.firstOfKind
                    anchors.bottom: parent.top
                    anchors.bottomMargin: -1
                    width: parent.width
                    height: 1
                    color: host.shade(Theme.foreground, 0.12)
                }

                // The check: filled for all, a bar for some, empty for none.
                Rectangle {
                    id: box
                    visible: !row.modelData.create
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    radius: Theme.cornerRadius > 0 ? 3 : 2
                    color: row.checkState === Qt.Unchecked ? "transparent" : Theme.accent
                    border.width: 1
                    border.color: row.checkState === Qt.Unchecked ? Theme.mutedText : Theme.accent
                    Icon {
                        anchors.centerIn: parent
                        visible: row.checkState === Qt.Checked
                        name: "check"
                        size: 12
                        color: Theme.background
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        visible: row.checkState === Qt.PartiallyChecked
                        width: 8
                        height: 2
                        color: Theme.background
                    }
                }

                Text {
                    id: label
                    anchors.left: parent.left
                    anchors.leftMargin: row.modelData.create ? 12 : 30
                    anchors.right: trailing.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.create
                          ? (row.modelData.name === ""
                             ? (row.modelData.kind === "album" ? "+ New album" : "+ New tag")
                             : (row.modelData.kind === "album" ? "Create album “" : "Create tag “")
                               + row.modelData.name + "”"
                               + (row.modelData.cut ? " (cut to " + menu.albumLimit + " characters)" : ""))
                          : row.modelData.name
                    elide: Text.ElideMiddle
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: row.modelData.create ? Theme.accent
                           : row.current ? Theme.brightForeground : Theme.foreground
                }

                Row {
                    id: trailing
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: row.modelData.fromFile === true
                        name: "file"
                        size: 12
                        color: Theme.mutedText
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !row.modelData.create
                        text: (row.checkState === Qt.PartiallyChecked
                               ? row.modelData.count + " of " + menu.targetPaths.length + "  " : "")
                              + (row.modelData.kind === "album" ? "Album" : "Tag")
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.mutedText
                    }
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        list.currentIndex = row.index
                        menu.activate(row.index)
                    }
                }
            }
        }
    }
}
