import QtQuick
import QtQuick.Controls.Basic

// Kind filter, search and sort. One row, left to right in the order someone
// actually reaches for them: narrow what is shown, then find one thing, then
// change the order.
Item {
    id: root

    // Mirrors CaptureRecord::Kind. -1 is every kind.
    readonly property int kindAll: -1
    readonly property int kindScreenshot: 0
    readonly property int kindRecording: 1
    readonly property int kindPicture: 2
    readonly property int kindVideo: 3
    readonly property int kindDownload: 4
    readonly property int kindDocument: 5

    // Number keys keep the existing 7 shortcut for favourites. PDFs take 8.
    function selectSection(index) {
        if (index === 6) {
            Captures.favoritesOnly = !Captures.favoritesOnly
            return
        }
        if (index === 7) {
            Captures.kindFilter = kindDocument
            return
        }
        const kinds = [kindAll, kindScreenshot, kindRecording, kindPicture, kindVideo,
                       kindDownload]
        if (index === 5 && !Settings.scanDownloads) {
            return
        }
        Captures.kindFilter = kinds[index]
    }

    function sectionShortcut(index) { return ShortcutCatalog.sequence("library", "sections")[index] || "" }

    // Tab walks the sections in the order the pills show them and wraps.
    // Favourites is orthogonal to the kind and stays out of the cycle.
    function cycleSection(step) {
        const kinds = [kindAll, kindScreenshot, kindRecording, kindPicture, kindVideo,
                       kindDocument]
        if (Settings.scanDownloads) {
            kinds.splice(5, 0, kindDownload)
        }
        const current = Math.max(0, kinds.indexOf(Captures.kindFilter))
        Captures.kindFilter = kinds[(current + step + kinds.length) % kinds.length]
    }

    readonly property var sortLabels: ["Newest", "Oldest", "Largest", "Smallest", "Name",
                                       "Top rated"]

    // True while the search field owns the keyboard, so window-level
    // shortcuts that the field would not swallow (Escape, Ctrl+H) stand down.
    readonly property bool searchActive: search.activeFocus
    // A menu is up. Main disables the surfaces under it: a modal popup stops
    // items from getting the press that closes it, but not the passive tap
    // handlers on the tiles, which would otherwise still fire.
    readonly property bool menuOpen: sortMenu.visible || kindMenu.visible
    property bool browserOpen: false

    // The user left the search field with Enter or Escape.
    signal done()
    signal browseRequested()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function focusSearch() {
        if (!searchBox.visible) return
        search.forceActiveFocus()
        search.selectAll()
    }

    function applySearch() {
        Captures.searchText = search.text
    }

    // Token values (tag:, in:, camera: and the rest) for the word at the
    // cursor, while the field has the keyboard.
    function refreshSuggestions() {
        const rows = search.activeFocus
                     ? Captures.searchSuggestions(search.text, search.cursorPosition) : []
        suggestions.rows = rows
        if (rows.length > 0) suggestions.open()
        else suggestions.close()
    }

    function acceptSuggestion(row) {
        const before = search.text.substring(0, row.start)
        const after = search.text.substring(row.end)
        const insert = row.insert + (row.complete && !after.startsWith(" ") ? " " : "")
        search.text = before + insert + after
        search.cursorPosition = before.length + insert.length
        if (row.complete) root.applySearch()
    }

    // Saved views and Open With can change the query outside this field.
    Component.onCompleted: search.text = Captures.searchText
    Connections {
        target: Captures
        function onSearchTextChanged() {
            if (search.text !== Captures.searchText) {
                search.text = Captures.searchText
            }
        }
    }

    function folderName(path) {
        const parts = path.split("/").filter(function (part) { return part !== "" })
        return parts.length > 0 ? parts[parts.length - 1] : path
    }

    // The kinds in the order the number keys pick them. Downloads only
    // while that folder is scanned.
    readonly property var kindChoices: {
        const list = [
            {kind: kindAll, label: "All", key: 0},
            {kind: kindScreenshot, label: "Screenshots", key: 1},
            {kind: kindRecording, label: "Recordings", key: 2},
            {kind: kindPicture, label: "Pictures", key: 3},
            {kind: kindVideo, label: "Videos", key: 4}
        ]
        if (Settings.scanDownloads) list.push({kind: kindDownload, label: "Downloads", key: 5})
        list.push({kind: kindDocument, label: "PDFs", key: 7})
        return list
    }
    function kindLabel(kind) {
        for (const choice of root.kindChoices)
            if (choice.kind === kind) return choice.label
        return "All"
    }

    // Where Main wants the search field: the header's primary row.
    property Item searchSlot: null
    readonly property alias searchField: searchBox
    // The header shows the bar only once there is something to filter.
    readonly property bool shown: Captures.sourceCount > 0 || chips.chips.length > 0
                                  || Captures.kindFilter !== kindAll || Captures.favoritesOnly

    signal message(string text)

    // Kind as one segmented control with full names. When the row cannot
    // hold it next to Browse and the sort, it folds into a menu button; the
    // decision reads implicit widths only, so it cannot feed back on itself.
    readonly property real fixedRowWidth: 40 + favouriteButton.implicitWidth + 12
                                          + 110 + 12 + sortButton.implicitWidth + 12
    readonly property bool kindsSegmented: root.width >= root.fixedRowWidth + kindSegments.implicitWidth
    readonly property real kindControlWidth: root.kindsSegmented ? kindSegments.implicitWidth
                                                                 : kindMenuButton.implicitWidth
    readonly property int controlsHeight: 40
    implicitHeight: !root.shown ? 0
                    : controlsHeight + (chipRow.visible ? chipRow.height + 7 : 0)
    visible: root.shown

    Row {
        id: kinds
        anchors.left: parent.left
        anchors.leftMargin: 20
        anchors.top: parent.top
        anchors.topMargin: 7
        spacing: 6

        Rectangle {
            id: kindSegmentsFrame
            objectName: "kindSegments"
            visible: root.kindsSegmented
            width: kindSegments.implicitWidth
            height: 26
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            color: root.shade(Theme.foreground, 0.04)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.12)

            Row {
                id: kindSegments
                anchors.verticalCenter: parent.verticalCenter

                Repeater {
                    model: root.kindChoices

                    PillButton {
                        required property var modelData
                        segment: true
                        label: modelData.label
                        shortcut: root.sectionShortcut(modelData.key)
                        checkable: true
                        active: Captures.kindFilter === modelData.kind
                        onClicked: Captures.kindFilter = modelData.kind
                    }
                }
            }
        }

        PillButton {
            id: kindMenuButton
            objectName: "kindMenuButton"
            visible: !root.kindsSegmented
            label: root.kindLabel(Captures.kindFilter) + "  ▾"
            accessibleName: "Kind: " + root.kindLabel(Captures.kindFilter)
            toolTip: "Show one kind of file"
            active: Captures.kindFilter !== root.kindAll || kindMenu.visible
            onClicked: kindMenu.visible ? kindMenu.close()
                                        : kindMenu.popup(kindMenuButton, 0, kindMenuButton.height + 4)
        }

        // Orthogonal to the kind: favourites of whatever section is showing.
        PillButton {
            id: favouriteButton
            icon: Captures.favoritesOnly ? "star-filled" : "star"
            label: "Favourites"
            toolTip: "Favourites"
            accessibleName: "Favourites"
            checkable: true
            shortcut: root.sectionShortcut(6)
            active: Captures.favoritesOnly
            onClicked: Captures.favoritesOnly = !Captures.favoritesOnly
        }

        PillButton {
            id: libraryButton
            objectName: "libraryBrowseButton"
            // Whatever the kind control and the sort leave.
            maximumWidth: Math.max(80, root.width - 40 - root.kindControlWidth - 6
                                       - favouriteButton.implicitWidth - 6 - 12
                                       - sortButton.implicitWidth)
            // Several facets at once are listed as chips; the button counts them.
            label: chips.chips.length > 1 ? "Browse · " + chips.chips.length + "  ▾"
                   : root.width < 700 ? (Captures.duplicatesOnly ? "Dupes  ▾"
                                      : Captures.similarOnly ? "Similar  ▾" : "Browse  ▾")
                   : (Captures.duplicatesOnly ? "Duplicates  ▾"
                      : Captures.similarOnly ? "Similar pictures  ▾"
                      : Captures.smartCollectionFilter !== "" ? Captures.smartCollectionFilter + "  ▾"
                      : Captures.tagFilter !== "" ? "#" + Captures.tagFilter + "  ▾"
                      : Captures.cameraFilter !== "" ? Captures.cameraFilter + "  ▾"
                      : Captures.lensFilter !== "" ? Captures.lensFilter + "  ▾"
                      : Captures.minimumRating > 0
                        ? "★".repeat(Captures.minimumRating) + "+  ▾"
                      : Captures.dateFrom !== "" || Captures.modifiedAfter !== "" ? "Date  ▾"
                      : Captures.albumFilter !== "" ? Captures.albumFilter + "  ▾"
                      : Captures.folderFilter !== ""
                        ? root.folderName(Captures.folderFilter) + "  ▾" : "Browse  ▾")
            active: Captures.folderFilter !== "" || Captures.albumFilter !== ""
                    || Captures.duplicatesOnly
                    || Captures.similarOnly || Captures.tagFilter !== ""
                    || Captures.cameraFilter !== "" || Captures.lensFilter !== ""
                    || Captures.minimumRating > 0
                    || Captures.dateFrom !== "" || Captures.modifiedAfter !== ""
                    || Captures.smartCollectionFilter !== ""
                    || root.browserOpen
            toolTip: Captures.folderFilter !== "" ? Captures.folderFilter : label
            onClicked: root.browseRequested()
        }
    }

    // Sort. A menu rather than five more pills, because the sort is changed
    // far less often than the filter and does not deserve equal weight.
    PillButton {
        id: sortButton
        anchors.right: parent.right
        anchors.rightMargin: 20
        anchors.top: parent.top
        anchors.topMargin: 7
        label: root.sortLabels[Captures.sortMode] + "  ▾"
        active: sortMenu.visible
        onClicked: sortMenu.visible ? sortMenu.close() : sortMenu.popup(sortButton, 0, sortButton.height + 4)
    }

    // Search: the primary control, shown in the header's first row.
    Rectangle {
        id: searchBox
        objectName: "librarySearchBox"
        parent: root.searchSlot ? root.searchSlot : root
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 30
        // Gives way entirely rather than squeeze to a few letters.
        visible: width >= 110
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        color: root.shade(Theme.foreground, search.activeFocus ? 0.10 : 0.06)
        border.width: 1
        border.color: search.activeFocus
                      ? root.shade(Theme.accent, 0.6)
                      : root.shade(Theme.foreground, searchHover.hovered ? 0.22 : 0.14)

        Behavior on color { ColorAnimation { duration: 140 } }
        Behavior on border.color { ColorAnimation { duration: 140 } }

        Icon {
            id: searchIcon
            anchors.left: parent.left
            anchors.leftMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            size: 14
            color: search.activeFocus ? Theme.accent : Theme.mutedText
        }

        TextInput {
            id: search
            objectName: "libraryFilterSearch"
            Accessible.role: Accessible.EditableText
            Accessible.name: "Search the library"
            Accessible.description: "Names, captions and text in pictures. Shortcut /"
            Accessible.searchEdit: true
            anchors.fill: parent
            anchors.leftMargin: 30
            anchors.rightMargin: 10
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.foreground
            selectionColor: root.shade(Theme.accent, 0.5)
            selectedTextColor: Theme.brightForeground

            onTextChanged: {
                Captures.queueSearchText(text)
                root.refreshSuggestions()
            }
            onCursorPositionChanged: root.refreshSuggestions()
            onActiveFocusChanged: {
                if (!activeFocus) {
                    root.applySearch()
                    suggestions.close()
                }
            }
            // Up and Down walk the suggestions; Enter or Tab takes one.
            // Without a highlighted suggestion Enter still searches.
            Keys.onDownPressed: function (event) {
                event.accepted = suggestions.visible
                if (event.accepted) suggestions.move(1)
            }
            Keys.onUpPressed: function (event) {
                event.accepted = suggestions.visible
                if (event.accepted) suggestions.move(-1)
            }
            Keys.onTabPressed: function (event) {
                event.accepted = suggestions.visible && suggestions.accept(true)
            }
            Keys.onEscapePressed: {
                if (suggestions.visible) {
                    suggestions.close()
                    return
                }
                text = ""
                root.done()
            }
            Keys.onReturnPressed: {
                if (suggestions.visible && suggestions.accept(false)) return
                root.applySearch()
                root.done()
            }
            Keys.onEnterPressed: {
                if (suggestions.visible && suggestions.accept(false)) return
                root.applySearch()
                root.done()
            }
        }

        HoverHandler { id: searchHover; cursorShape: Qt.IBeamCursor }
        ThemedToolTip {
            requested: searchHover.hovered && !search.activeFocus
            text: "Search names, text, tags, albums, folders and cameras. "
                  + "Narrow with tag:, album:, in:, camera:, lens:, type:, before: and after:, "
                  + "\"quote a phrase\" or leave out a -word."
        }

        SearchSuggestions {
            id: suggestions
            objectName: "searchSuggestions"
            width: Math.max(searchBox.width, 260)
            x: 0
            y: searchBox.height + 4
            onAccepted: function (row) { root.acceptSuggestion(row) }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 30
            anchors.right: parent.right
            anchors.rightMargin: 10
            elide: Text.ElideRight
            visible: search.text === "" && !search.activeFocus
            text: (TextIndex.available ? "Search names and text" : "Search") + "   /"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.mutedText
        }
    }

    // The chips row: the active Browse facets, and pinning for an open folder,
    // so a folder adds one row rather than two.
    Item {
        id: chipRow
        x: 20
        y: root.controlsHeight + 1
        width: root.width - 40
        height: Math.max(chips.implicitHeight, pinCurrentFolder.visible ? pinCurrentFolder.height : 0)
        visible: chips.chips.length > 0 || pinCurrentFolder.visible

        PillButton {
            id: pinCurrentFolder
            objectName: "pinCurrentFolder"
            anchors.right: parent.right
            anchors.top: parent.top
            visible: Captures.folderFilter !== ""
            readonly property bool pinned: {
                void Settings.pinnedFolders
                return Settings.isFolderPinned(Captures.folderFilter)
            }
            label: pinned ? "Unpin folder" : "Pin folder"
            onClicked: {
                if (pinned) Settings.unpinFolder(Captures.folderFilter)
                else if (!Settings.pinFolderPath(Captures.folderFilter)) root.message("That folder is unavailable")
            }
        }
    }

    // The active Browse facets, each with its own clear control.
    ActiveFilterChips {
        id: chips
        objectName: "activeFilterChips"
        parent: chipRow
        width: chipRow.width - (pinCurrentFolder.visible ? pinCurrentFolder.width + 8 : 0)
        visible: chips.chips.length > 0
    }

    Menu {
        id: kindMenu
        objectName: "kindMenu"
        modal: true
        dim: false

        background: Rectangle {
            implicitWidth: 170
            color: root.shade(Theme.background, 0.97)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }

        Repeater {
            model: root.kindChoices

            MenuItem {
                id: kindRow
                required property var modelData
                readonly property bool current: Captures.kindFilter === modelData.kind

                height: 30
                text: modelData.label
                Accessible.name: text
                checkable: true
                checked: current

                contentItem: Item {
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: kindRow.modelData.label
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: kindRow.current ? Font.DemiBold : Font.Normal
                        color: kindRow.current ? Theme.accent : Theme.foreground
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.sectionShortcut(kindRow.modelData.key)
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.mutedText
                    }
                }

                background: Rectangle {
                    color: kindRow.hovered ? root.shade(Theme.foreground, 0.08) : "transparent"
                }

                onTriggered: Captures.kindFilter = kindRow.modelData.kind
            }
        }
    }

    Menu {
        id: sortMenu
        objectName: "sortMenu"
        // Modal, undimmed: the press that closes the menu is consumed here
        // rather than also landing on the tile or pill under it.
        modal: true
        dim: false

        background: Rectangle {
            implicitWidth: 150
            color: root.shade(Theme.background, 0.97)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }

        Repeater {
            model: root.sortLabels

            MenuItem {
                id: sortRow
                required property int index
                required property string modelData

                height: 30
                text: modelData
                Accessible.name: text
                checkable: true
                checked: Captures.sortMode === index

                contentItem: Text {
                    text: sortRow.modelData
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Captures.sortMode === sortRow.index ? Font.DemiBold : Font.Normal
                    color: Captures.sortMode === sortRow.index ? Theme.accent : Theme.foreground
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 12
                }

                background: Rectangle {
                    color: sortRow.hovered ? root.shade(Theme.foreground, 0.08) : "transparent"
                }

                onTriggered: Captures.sortMode = sortRow.index
            }
        }
    }
}
