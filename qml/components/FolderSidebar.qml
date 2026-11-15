import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models

FocusScope {
    id: root

    property real paneWidth: Settings.folderSidebarWidth
    readonly property bool menuOpen: folderMenu.visible || collections.menuOpen
    readonly property bool foldersOpen: Settings.sidebarSections.indexOf("folders") >= 0
    property var expandedPathsBeforeReset: []
    property bool expansionRestorePending: false
    signal chosen(string path)
    // All media: the whole library, every facet but the search cleared.
    signal allMediaChosen()
    signal addFolderRequested()
    signal gridFocusRequested()
    signal message(string text)
    signal renameCollectionRequested(string mode, string name)
    signal deleteCollectionRequested(string mode, string name)

    function shade(base, amount) { return Qt.rgba(base.r, base.g, base.b, amount) }
    // The keyboard ring shows while the keyboard is in use, not after a click.
    readonly property bool keyboardShown: !root.Window.window || root.Window.window.keyboardNavigation !== false
    function keyboardUsed() { if (root.Window.window) root.Window.window.keyboardNavigation = true }
    function pointerUsed() { if (root.Window.window) root.Window.window.keyboardNavigation = false }
    // Nothing narrows the library but, perhaps, the search.
    readonly property bool wholeLibrary: Captures.folderFilter === "" && Captures.albumFilter === ""
        && Captures.tagFilter === "" && Captures.dateFrom === "" && Captures.dateTo === ""
        && Captures.modifiedAfter === "" && Captures.cameraFilter === "" && Captures.lensFilter === ""
        && Captures.minimumRating === 0 && !Captures.duplicatesOnly && !Captures.similarOnly
        && Captures.smartCollectionFilter === ""
    // After a rename from a row's menu, the keyboard goes back to that row
    // under its new name.
    function focusCollection(kind, name) { collections.focusRow(kind, name) }
    function basename(path) { return String(path).split("/").filter(Boolean).pop() || "/" }
    // The sidebar's keyboard home: the folder tree, or with it collapsed or
    // empty, the Folders header.
    function focusTree() {
        if (!root.foldersOpen || tree.rows === 0) {
            foldersHeader.forceActiveFocus()
            return
        }
        tree.forceActiveFocus()
        if (tree.currentRow < 0 && tree.rows > 0) tree.focusRow(0)
    }
    // After a menu closes: the collections list keeps the keyboard if it had
    // it, as the tree does.
    function focusAfterMenu() {
        if (collections.activeFocus) collections.listView.forceActiveFocus()
        else root.focusTree()
    }
    function setFoldersOpen(open) {
        const sections = Settings.sidebarSections.filter(function (name) { return name !== "folders" })
        if (open) sections.push("folders")
        Settings.sidebarSections = sections
    }
    // The collections list takes the keyboard only with room for a row; in
    // a short window with several pins it can have none.
    readonly property bool collectionsReachable: collections.visible && collections.height >= 32
    // All media, then the pinned folders, in order.
    function topRows() {
        return [allMedia].concat(pinnedRows.children.filter(function (item) {
            return item.activeFocusOnTab === true
        }))
    }
    // Up out of the collections list, or out of the Folders header when
    // there is no list: the last pinned folder, else All media.
    function focusAbove() {
        const rows = root.topRows()
        rows[rows.length - 1].forceActiveFocus()
    }
    // Down from the last pinned folder, or All media: the collections, else
    // the Folders header.
    function focusBelowPins() {
        if (root.collectionsReachable) collections.focusFirst()
        else foldersHeader.forceActiveFocus()
    }
    // Up and Down on All media and the pins walk them in order and on into
    // the collections and the folders. False for any other row.
    function walk(item, step) {
        const rows = root.topRows()
        const at = rows.indexOf(item)
        if (at < 0) return false
        if (at + step >= rows.length) root.focusBelowPins()
        else if (at + step >= 0) rows[at + step].forceActiveFocus()
        return true
    }
    function syncSelection() {
        if (!root.visible) return
        const index = FolderTree.indexForPath(Captures.folderFilter)
        if (!index.valid) return
        tree.expandToIndex(index)
        selection.setCurrentIndex(index, ItemSelectionModel.NoUpdate)
        Qt.callLater(function() {
            // A scan can reset the model before this layout turn.
            const current = FolderTree.indexForPath(Captures.folderFilter)
            if (!root.visible || !current.valid) return
            tree.forceLayout()
            const row = tree.rowAtIndex(current)
            if (row >= 0) tree.positionViewAtRow(row, TableView.Contain)
        })
    }
    onVisibleChanged: if (visible) Qt.callLater(syncSelection)
    Connections {
        target: Captures
        function onFolderFilterChanged() { Qt.callLater(root.syncSelection) }
    }
    Connections {
        target: FolderTree
        function onModelAboutToBeReset() {
            // Keep the first snapshot if scans reset again before the layout turn.
            if (root.expansionRestorePending) return
            const paths = []
            for (let row = 0; row < tree.rows; ++row) {
                if (tree.isExpanded(row)) paths.push(FolderTree.pathForIndex(tree.index(row, 0)))
            }
            root.expandedPathsBeforeReset = paths
            root.expansionRestorePending = true
        }
        function onModelReset() { Qt.callLater(root.restoreExpansion) }
    }
    function restoreExpansion() {
        for (const path of expandedPathsBeforeReset) {
            const index = FolderTree.indexForPath(path)
            if (!index.valid) continue
            tree.expandToIndex(index)
            tree.expand(tree.rowAtIndex(index))
        }
        expansionRestorePending = false
        syncSelection()
    }
    Connections {
        target: Settings
        function onFolderSidebarWidthChanged() { root.paneWidth = Settings.folderSidebarWidth }
    }

    Rectangle {
        anchors.fill: parent
        color: root.shade(Theme.background, 0.85)
    }

    // Drawn in a gutter of its own beside the rows, which leave room for it
    // when it is needed.
    component FolderScrollBar: ScrollBar {
        policy: size < 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        width: 6
        Accessible.ignored: size >= 1
        contentItem: Rectangle {
            visible: parent.size < 1
            implicitWidth: 4
            radius: width / 2
            color: root.shade(Theme.foreground, 0.32)
        }
        background: Rectangle { color: "transparent" }
    }

    // Pins and tree rows share the same read-only folder actions.
    component FolderRow: Rectangle {
        id: rowItem
        property string path: ""
        property string label: ""
        property int count: -1
        property int indentation: 0
        property bool branch: false
        property bool expanded: false
        property bool active: Captures.folderFilter === path
        property bool keyboardCurrent: activeFocus && root.keyboardShown
        signal activate()
        signal toggle()
        implicitHeight: 34
        radius: Math.min(6, Theme.cornerRadius)
        color: active ? root.shade(Theme.accent, 0.18)
                 : mouse.containsMouse ? root.shade(Theme.foreground, 0.07) : "transparent"
        border.width: keyboardCurrent ? 1 : 0
        border.color: Theme.accent
        Accessible.role: Accessible.Button
        Accessible.name: label + (count >= 0 ? ", " + count + " items" : "")
        Accessible.description: path
        Accessible.focusable: true
        Accessible.selectable: true
        Accessible.selected: active
        Accessible.onPressAction: activate()

        Text {
            id: indicator
            x: 6 + rowItem.indentation
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            text: rowItem.branch ? (rowItem.expanded ? "▾" : "▸") : ""
            color: Theme.mutedText
            font.family: Theme.fontFamily
            font.pixelSize: 14
        }
        Text {
            anchors.left: indicator.right
            anchors.right: countText.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: rowItem.label
            elide: Text.ElideMiddle
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.foreground
        }
        Text {
            id: countText
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: rowItem.count >= 0 ? Number(rowItem.count).toLocaleString(Qt.locale(), "f", 0) : ""
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.mutedText
        }
        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: function(event) {
                root.pointerUsed()
                if (event.button === Qt.RightButton && rowItem.path !== "") {
                    folderMenu.path = rowItem.path
                    folderMenu.popup(rowItem, event.x, event.y)
                } else if (rowItem.branch && event.x < indicator.x + indicator.width) {
                    rowItem.toggle()
                } else {
                    rowItem.activate()
                }
            }
        }
        Keys.onPressed: function (event) {
            root.keyboardUsed()
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Down)
                event.accepted = root.walk(rowItem, event.key === Qt.Key_Up ? -1 : 1)
        }
        Keys.onReturnPressed: activate()
        Keys.onEnterPressed: activate()
        Keys.onMenuPressed: {
            if (path !== "") {
                folderMenu.path = path
                folderMenu.popup(rowItem, 0, height)
            }
        }
        ThemedToolTip {
            requested: mouse.containsMouse && path !== ""
            text: path
        }
    }

    FolderRow {
        id: allMedia
        objectName: "sidebarAllMedia"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 8
        height: implicitHeight
        label: "All media"
        activeFocusOnTab: true
        active: root.wholeLibrary
        Accessible.focused: activeFocus
        onActivate: { forceActiveFocus(); root.allMediaChosen() }
    }

    Column {
        id: pinnedSection
        anchors.top: allMedia.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        visible: Settings.pinnedFolders.length > 0
        height: visible ? implicitHeight : 0
        spacing: 4
        // Framed like the other section titles, but always open.
        SidebarHeader {
            objectName: "sidebarPinnedHeader"
            width: parent.width
            height: 28
            label: "Pinned"
            collapsible: false
        }
        Flickable {
            id: pinnedList
            width: parent.width
            // Whole rows.
            height: Math.min(pinnedRows.height, Math.max(34, Math.floor(root.height / 4 / 34) * 34))
            contentWidth: width
            contentHeight: pinnedRows.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Accessible.role: Accessible.List
            Accessible.name: "Pinned folders"
            readonly property real gutter: pinnedRows.height > height + 0.5 ? 10 : 0
            ScrollBar.vertical: FolderScrollBar {}
            Column {
                id: pinnedRows
                width: parent.width - pinnedList.gutter
                Repeater {
                    model: Settings.pinnedFolders
                    FolderRow {
                        required property string modelData
                        width: pinnedRows.width
                        height: implicitHeight
                        path: modelData
                        label: root.basename(path)
                        activeFocusOnTab: true
                        Accessible.focused: activeFocus
                        onActiveFocusChanged: if (activeFocus) pinnedList.contentY = Math.max(0, y + height - pinnedList.height)
                        onActivate: { forceActiveFocus(); root.chosen(path) }
                    }
                }
            }
        }
    }

    // Albums, tags and dates share the room below the pins with the folder
    // tree. The tree asks for its rows and keeps at most half when both
    // want more; a short or empty tree gives the rest of its share up.
    readonly property real collectionsRoom: Math.max(0, addFolder.y - 8 - pinnedSection.y
                                                    - pinnedSection.height - 8 - foldersHeader.height - 8)
    readonly property real treeWants: root.foldersOpen ? tree.rows * 34 + 16 : 0
    SidebarCollections {
        id: collections
        objectName: "sidebarCollectionsPane"
        anchors.top: pinnedSection.bottom
        anchors.topMargin: visible ? 8 : 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        height: visible ? implicitHeight : 0
        maximumHeight: root.foldersOpen
                       ? Math.max(Math.floor(root.collectionsRoom / 2), root.collectionsRoom - root.treeWants)
                       : root.collectionsRoom
        keyboardReachable: root.collectionsReachable
        onExitUp: root.focusAbove()
        onExitDown: foldersHeader.forceActiveFocus()
        onRenameRequested: function (mode, name) { root.renameCollectionRequested(mode, name) }
        onDeleteRequested: function (mode, name) { root.deleteCollectionRequested(mode, name) }
    }

    SidebarHeader {
        id: foldersHeader
        objectName: "sidebarFoldersHeader"
        anchors.top: collections.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        height: implicitHeight
        label: "Folders"
        expanded: root.foldersOpen
        activeFocusOnTab: true
        keyboardCurrent: activeFocus && root.keyboardShown
        Accessible.focusable: true
        Accessible.focused: activeFocus
        onToggled: { root.pointerUsed(); forceActiveFocus(); root.setFoldersOpen(!root.foldersOpen) }
        Keys.onPressed: function (event) {
            root.keyboardUsed()
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                root.setFoldersOpen(!root.foldersOpen)
            } else if (event.key === Qt.Key_Right) {
                if (!root.foldersOpen) root.setFoldersOpen(true)
                else if (tree.rows > 0) { tree.forceActiveFocus(); tree.focusRow(0) }
            } else if (event.key === Qt.Key_Left) {
                if (root.foldersOpen) root.setFoldersOpen(false)
            } else if (event.key === Qt.Key_Down) {
                if (root.foldersOpen && tree.rows > 0) { tree.forceActiveFocus(); tree.focusRow(0) }
            } else if (event.key === Qt.Key_Up) {
                if (root.collectionsReachable) collections.focusLast()
                else root.focusAbove()
            } else {
                return
            }
            event.accepted = true
        }
    }

    ItemSelectionModel { id: selection; model: FolderTree }
    TreeView {
        id: tree
        objectName: "folderSidebarTree"
        anchors.top: foldersHeader.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 8
        // Down to Add folder, ending on a row's edge when the rows run on.
        readonly property real room: Math.max(0, addFolder.y - 8 - y)
        readonly property real gutter: rows * 34 > room + 0.5 ? 10 : 0
        height: rows * 34 > room ? Math.floor(room / 34) * 34 : room
        visible: root.foldersOpen
        clip: true
        model: FolderTree
        selectionModel: selection
        activeFocusOnTab: true
        keyNavigationEnabled: false
        pointerNavigationEnabled: false
        columnWidthProvider: function(column) { return tree.width - tree.gutter }
        onGutterChanged: forceLayout()
        rowHeightProvider: function(row) { return 34 }
        boundsBehavior: Flickable.StopAtBounds
        Accessible.role: Accessible.Tree
        Accessible.name: "Library folders"
        ScrollBar.vertical: FolderScrollBar {}

        function focusRow(row) {
            if (row < 0 || row >= rows) return
            selection.setCurrentIndex(index(row, 0), ItemSelectionModel.NoUpdate)
            positionViewAtRow(row, TableView.Contain)
        }
        onActiveFocusChanged: if (activeFocus && currentRow < 0) focusRow(0)
        Keys.onPressed: function(event) {
            root.keyboardUsed()
            const row = currentRow < 0 ? 0 : currentRow
            const index = tree.index(row, 0)
            if (event.key === Qt.Key_Down) focusRow(Math.min(rows - 1, row + 1))
            else if (event.key === Qt.Key_Up) {
                // Up from the first folder goes back to the Folders header.
                if (row <= 0) foldersHeader.forceActiveFocus()
                else focusRow(row - 1)
            }
            else if (event.key === Qt.Key_Right) {
                if (!isExpanded(row)) expand(row)
                else if (FolderTree.hasChildren(index)) focusRow(row + 1)
            } else if (event.key === Qt.Key_Left) {
                if (isExpanded(row)) collapse(row)
                else {
                    const parent = FolderTree.parent(index)
                    if (parent.valid) focusRow(rowAtIndex(parent))
                }
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                if (index.valid) root.chosen(FolderTree.pathForIndex(index))
            } else if (event.key === Qt.Key_Menu) {
                if (index.valid) {
                    folderMenu.path = FolderTree.pathForIndex(index)
                    folderMenu.popup(tree, 8, 8)
                }
            } else return
            event.accepted = true
        }
        delegate: FolderRow {
            required property int row
            required property int depth
            required property bool hasChildren
            required property string folderName
            required property string folderPath
            required property int mediaCount
            // TreeView supplies the inherited expanded property too.
            required property bool current
            implicitWidth: tree.width - tree.gutter
            path: folderPath
            label: folderName
            count: mediaCount
            indentation: Math.min(depth * 16, tree.width / 2)
            branch: hasChildren
            required expanded
            keyboardCurrent: current && tree.activeFocus && root.keyboardShown
            Accessible.role: Accessible.TreeItem
            // The row with the ring is the one read as focused, not the
            // tree's first row.
            Accessible.focused: current && tree.activeFocus
            onActivate: {
                tree.forceActiveFocus()
                tree.focusRow(row)
                root.chosen(path)
            }
            onToggle: {
                tree.forceActiveFocus()
                tree.focusRow(row)
                tree.toggleExpanded(row)
            }
        }
    }

    PillButton {
        id: addFolder
        objectName: "sidebarAddFolder"
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        label: "Add folder…"
        onClicked: root.addFolderRequested()
        Keys.onTabPressed: function(event) { root.gridFocusRequested(); event.accepted = true }
    }

    Menu {
        id: folderMenu
        objectName: "sidebarFolderMenu"
        property string path: ""
        // Modal, undimmed, and drawn like the app's other menus: an item in
        // the window, opaque whatever the theme's alpha.
        modal: true
        dim: false
        popupType: Popup.Item
        background: Rectangle {
            objectName: "sidebarFolderMenuBackground"
            implicitWidth: 220
            color: Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 1)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }
        delegate: MenuItem {
            id: menuItem
            height: 30
            contentItem: Text {
                text: menuItem.text
                color: menuItem.highlighted ? Theme.brightForeground : Theme.foreground
                font.family: Theme.fontFamily
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
                rightPadding: 12
                elide: Text.ElideRight
            }
            background: Rectangle {
                color: menuItem.highlighted ? root.shade(Theme.foreground, 0.09) : "transparent"
                radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 0
            }
        }
        Action {
            text: {
                void Settings.pinnedFolders
                return Settings.isFolderPinned(folderMenu.path) ? "Unpin" : "Pin"
            }
            onTriggered: {
                if (Settings.isFolderPinned(folderMenu.path)) Settings.unpinFolder(folderMenu.path)
                else if (!Settings.pinFolderPath(folderMenu.path)) root.message("That folder is unavailable")
            }
        }
        Action {
            text: "Copy path"
            onTriggered: Actions.copyPlainText(folderMenu.path)
        }
        Action {
            text: "Open in file manager"
            onTriggered: Actions.open(folderMenu.path)
        }
        onClosed: root.focusTree()
    }

    Rectangle {
        anchors.right: parent.right
        height: parent.height
        width: 1
        color: root.shade(Theme.foreground, 0.14)
    }
    MouseArea {
        id: resizeHandle
        objectName: "folderSidebarResizeHandle"
        anchors.right: parent.right
        width: 6
        height: parent.height
        cursorShape: Qt.SplitHCursor
        property real startWidth: 0
        property real startX: 0
        onPressed: function(event) {
            startWidth = root.width
            startX = mapToItem(root.parent, event.x, event.y).x
        }
        onPositionChanged: function(event) {
            if (pressed) root.paneWidth = Math.max(180, Math.min(400,
                startWidth + mapToItem(root.parent, event.x, event.y).x - startX))
        }
        onReleased: Settings.folderSidebarWidth = Math.round(root.paneWidth)
        onCanceled: root.paneWidth = Settings.folderSidebarWidth
    }
}
