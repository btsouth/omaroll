import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models

FocusScope {
    id: root

    property real paneWidth: Settings.folderSidebarWidth
    readonly property bool menuOpen: folderMenu.visible
    signal chosen(string path)
    signal addFolderRequested()
    signal gridFocusRequested()
    signal message(string text)

    function shade(base, amount) { return Qt.rgba(base.r, base.g, base.b, amount) }
    function basename(path) { return String(path).split("/").filter(Boolean).pop() || "/" }
    function focusTree() {
        tree.forceActiveFocus()
        if (tree.currentRow < 0 && tree.rows > 0) tree.focusRow(0)
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
        function onModelReset() { Qt.callLater(root.syncSelection) }
    }
    Connections {
        target: Settings
        function onFolderSidebarWidthChanged() { root.paneWidth = Settings.folderSidebarWidth }
    }

    Rectangle {
        anchors.fill: parent
        color: root.shade(Theme.background, 0.85)
    }

    component FolderScrollBar: ScrollBar {
        policy: ScrollBar.AsNeeded
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
        property bool keyboardCurrent: activeFocus
        signal activate()
        signal toggle()
        implicitHeight: 34
        radius: Math.min(6, Theme.cornerRadius)
        color: Captures.folderFilter === path ? root.shade(Theme.accent, 0.18)
                 : mouse.containsMouse ? root.shade(Theme.foreground, 0.07) : "transparent"
        border.width: keyboardCurrent ? 1 : 0
        border.color: Theme.accent
        Accessible.role: Accessible.Button
        Accessible.name: label + (count >= 0 ? ", " + count + " items" : "")
        Accessible.description: path
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
        Keys.onReturnPressed: activate()
        Keys.onEnterPressed: activate()
        Keys.onMenuPressed: {
            if (path !== "") {
                folderMenu.path = path
                folderMenu.popup(rowItem, 0, height)
            }
        }
        ToolTip.visible: mouse.containsMouse && path !== ""
        ToolTip.text: path
        ToolTip.delay: 600
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
        onActivate: { forceActiveFocus(); root.chosen("") }
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
        Text {
            text: "Pinned"
            height: 24
            leftPadding: 8
            color: Theme.mutedText
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
        Flickable {
            id: pinnedList
            width: parent.width
            height: Math.min(pinnedRows.height, Math.max(34, root.height / 4))
            contentWidth: width
            contentHeight: pinnedRows.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FolderScrollBar {}
            Column {
                id: pinnedRows
                width: parent.width
                Repeater {
                    model: Settings.pinnedFolders
                    FolderRow {
                        required property string modelData
                        width: pinnedRows.width
                        height: implicitHeight
                        path: modelData
                        label: root.basename(path)
                        activeFocusOnTab: true
                        onActiveFocusChanged: if (activeFocus) pinnedList.contentY = Math.max(0, y + height - pinnedList.height)
                        onActivate: { forceActiveFocus(); root.chosen(path) }
                    }
                }
            }
        }
    }

    Text {
        id: foldersLabel
        anchors.top: pinnedSection.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.leftMargin: 16
        height: 24
        text: "Folders"
        color: Theme.mutedText
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.DemiBold
    }

    ItemSelectionModel { id: selection; model: FolderTree }
    TreeView {
        id: tree
        objectName: "folderSidebarTree"
        anchors.top: foldersLabel.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: addFolder.top
        anchors.margins: 8
        clip: true
        model: FolderTree
        selectionModel: selection
        activeFocusOnTab: true
        keyNavigationEnabled: false
        pointerNavigationEnabled: false
        columnWidthProvider: function(column) { return tree.width }
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
            const row = currentRow < 0 ? 0 : currentRow
            const index = tree.index(row, 0)
            if (event.key === Qt.Key_Down) focusRow(Math.min(rows - 1, row + 1))
            else if (event.key === Qt.Key_Up) focusRow(Math.max(0, row - 1))
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
            implicitWidth: tree.width
            path: folderPath
            label: folderName
            count: mediaCount
            indentation: Math.min(depth * 16, tree.width / 2)
            branch: hasChildren
            required expanded
            keyboardCurrent: current && tree.activeFocus
            Accessible.role: Accessible.TreeItem
            Accessible.selected: Captures.folderFilter === path
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
        width: 220
        background: Rectangle {
            color: Theme.background
            border.color: root.shade(Theme.foreground, 0.18)
            radius: Math.min(8, Theme.cornerRadius)
        }
        delegate: MenuItem {
            id: menuItem
            implicitHeight: 36
            contentItem: Text {
                text: menuItem.text
                color: Theme.foreground
                font.family: Theme.fontFamily
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: menuItem.highlighted ? root.shade(Theme.accent, 0.2) : "transparent"
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
