import QtQuick
import QtQuick.Controls.Basic
import Qt.labs.qmlmodels

// The library in sections: a header naming each section and its size, then
// its tiles in rows. A GridView has no sections, so the rows come from
// Captures.sectionRows, which lays the proxy's rows out per day, month or
// similar set; selection, marks, keys and actions stay with the grid that
// hosts this view.
FocusScope {
    id: root

    // The CaptureGrid this view stands in for.
    property Item host: null
    property int columns: 1
    // Tiles per row delegate. It grows at once and shrinks only once a
    // resize has settled, so dragging a window edge hides the tiles a row
    // no longer needs rather than rebuilding every row at each step, and
    // after zooming to small tiles rows do not keep a pile of hidden ones.
    property int poolColumns: 1
    onColumnsChanged: {
        // The tile at the top stays there through the new rows.
        root.rememberTop()
        if (root.columns > root.poolColumns) root.poolColumns = root.columns
        else poolTrim.restart()
    }
    onCompactChanged: root.rememberTop()
    Component.onCompleted: root.poolColumns = Math.max(root.poolColumns, root.columns)
    Timer {
        id: poolTrim
        objectName: "tilePoolTrim"
        interval: 600
        onTriggered: root.poolColumns = root.columns
    }
    property int cellWidth: 100
    property int cellHeight: 68
    property int cellSpacing: 12
    // Month sections where a day header would sit over a row or two.
    property bool compact: false
    // Bumped once per event-loop turn after the library or the rows change,
    // so tiles read their capture again.
    property int revision: 0
    property bool revisionPending: false
    readonly property int headerHeight: 40
    readonly property var sections: Captures.sectionRows
    readonly property bool reviewMode: sections.mode === 2
    readonly property int setCount: sections.sectionCount
    readonly property alias listView: list
    // The section label of the top row while its header is scrolled away;
    // the window's floating day pill shows it.
    property string stickyLabel: ""
    // Pointer selection must not scroll the tile out from under the pointer.
    property bool suppressReveal: false
    property bool revealPending: true
    property string topPath: ""
    // The tile last made current, to tell a move from an index shift.
    property string currentPath: ""
    property real topOffset: 0
    property bool topHeader: false
    // The view was at its top when the rows moved, so it stays there: the
    // newest section stays in view when camera dates re-sort the library.
    property bool keepTop: false

    // Exact geometry from the rows: the list's own contentHeight is an
    // estimate while header and tile rows of different heights are unseen.
    // Only similar sets have the summary. ListView deletes its old header
    // item without saying so, so headerItem alone can outlive it.
    readonly property real summaryHeight: root.reviewMode && list.headerItem ? list.headerItem.height : 0
    readonly property real exactHeight: {
        void root.revision
        return root.summaryHeight + root.sections.totalHeight(root.headerHeight, root.cellHeight)
    }
    // The exact offset of the top of the viewport.
    property real exactTop: 0
    property bool scrolling: false

    function bumpRevision() {
        root.revisionPending = false
        root.revision++
    }
    function scheduleRevision() {
        if (root.revisionPending) return
        root.revisionPending = true
        Qt.callLater(root.bumpRevision)
    }

    function updateExactTop() {
        const item = list.itemAt(20, list.contentY)
        if (item && item.index >= 0) {
            root.exactTop = root.summaryHeight
                    + root.sections.rowY(item.index, root.headerHeight, root.cellHeight)
                    + (list.contentY - item.y)
        } else if (root.summaryHeight > 0 && list.headerItem
                   && list.contentY < list.headerItem.y + list.headerItem.height) {
            root.exactTop = Math.max(0, list.contentY - list.headerItem.y)
        } else {
            root.exactTop = Math.max(0, list.contentY - list.originY)
        }
    }

    // The largest exact offset the viewport can start at.
    function scrollExtent() { return Math.max(0, root.exactHeight - list.height) }

    // Puts exact offset y at the top of the viewport. The same y shows the
    // same row every time, wherever the list's estimate has drifted.
    function scrollToExact(y) {
        if (root.sections.rowCount() === 0) return
        root.scrolling = true
        const extent = root.scrollExtent()
        y = Math.max(0, Math.min(y, extent))
        // The end too goes by exact rows: positionViewAtEnd would trust the
        // list's estimate of where the end is.
        if (y < root.summaryHeight) {
            list.positionViewAtBeginning()
            list.contentY += y
        } else {
            const within = y - root.summaryHeight
            const row = root.sections.rowAtY(within, root.headerHeight, root.cellHeight)
            // Near the end the list stops short of putting the row at the
            // top, by its own estimate; place it from the row's real spot.
            list.positionViewAtIndex(row, ListView.Beginning)
            const item = list.itemAtIndex(row)
            const offset = within - root.sections.rowY(row, root.headerHeight, root.cellHeight)
            list.contentY = (item ? item.y : list.contentY) + offset
        }
        root.scrolling = false
        root.updateExactTop()
    }

    Binding { target: root.sections; property: "active"; value: root.visible }
    Binding { target: root.sections; property: "columns"; value: root.columns }
    Binding { target: root.sections; property: "compact"; value: root.compact }

    function setPaths(first, count) {
        const paths = []
        for (let i = first; i < first + count; ++i) paths.push(Captures.pathAt(i))
        return paths
    }

    function revealCurrent() {
        if (!root.host || root.host.currentIndex < 0) return
        const row = root.sections.rowOfItem(root.host.currentIndex)
        if (row < 0) return
        // The first row of a section brings its header into view with it.
        const before = row > 0 && root.sections.rowAt(row - 1).header ? row - 1 : row
        list.positionViewAtIndex(before, ListView.Contain)
        list.positionViewAtIndex(row, ListView.Contain)
    }

    // Where a key moves the current tile. Left and Right run through the
    // library in order; Up, Down and the page keys keep the column through
    // the tile rows, across section headers.
    function targetIndex(current, direction, wrap) {
        const count = Captures.count
        if (count === 0) return -1
        if (current < 0) return 0
        switch (direction) {
        case "left": return current > 0 ? current - 1 : (wrap ? count - 1 : 0)
        case "right": return current < count - 1 ? current + 1 : (wrap ? 0 : count - 1)
        case "up": return root.vertical(current, -1, 1)
        case "down": return root.vertical(current, 1, 1)
        case "pageUp": return root.vertical(current, -1, root.rowsPerPage())
        case "pageDown": return root.vertical(current, 1, root.rowsPerPage())
        case "home": return 0
        case "end": return count - 1
        case "sectionNext": return root.sectionStep(current, 1)
        case "sectionPrevious": return root.sectionStep(current, -1)
        }
        return current
    }

    // The first tile of the next section, or going back, of the current
    // tile's own section unless it is already there, then of the one before.
    function sectionStep(current, step) {
        if (root.sections.rebuildPending()) root.sections.rebuildNow()
        const all = root.sections.sections()
        let at = -1
        for (let i = 0; i < all.length; ++i) {
            if (current >= all[i].first && current < all[i].first + all[i].count) {
                at = i
                break
            }
        }
        if (at < 0) return current
        if (step > 0) return at + 1 < all.length ? all[at + 1].first : current
        if (current > all[at].first) return all[at].first
        return at > 0 ? all[at - 1].first : current
    }

    function rowsPerPage() { return Math.max(1, Math.floor(list.height / root.cellHeight)) }

    function vertical(current, step, rows) {
        const start = root.sections.rowOfItem(current)
        if (start < 0) return current
        const column = current - root.sections.rowAt(start).first
        let landed = -1
        let moved = 0
        for (let i = start + step; i >= 0 && i < root.sections.rowCount(); i += step) {
            if (root.sections.rowAt(i).header) continue
            landed = i
            if (++moved === rows) break
        }
        if (landed < 0) return current
        const row = root.sections.rowAt(landed)
        return row.first + Math.min(column, row.count - 1)
    }

    // A point in this view, for the rubber band: the list row under it and
    // how far into that row. Rows above the first answer row 0 at -1.
    function pointInfo(x, y) {
        return root.infoAt(x, list.contentY + Math.max(0, Math.min(y, list.height - 1)))
    }
    // The same for a point in the list's content; `found` says whether a
    // laid-out row or the end of the list answered rather than a guess.
    function infoAt(x, cy) {
        const total = root.sections.rowCount()
        if (total === 0) return {row: 0, offset: 0, header: false, past: true, found: true}
        const item = list.itemAt(Math.max(0, Math.min(x, list.width - 1)), cy)
        if (item) return {row: item.index, offset: cy - item.y, header: item.header, past: false, found: true}
        const last = list.itemAtIndex(total - 1)
        if (last && cy >= last.y + last.height) return {row: total, offset: 0, header: false, past: true, found: true}
        return {row: 0, offset: -1, header: true, past: false, found: false}
    }
    function rowInfo(row) { return root.sections.rowAt(row) }
    function rowTotal() { return root.sections.rowCount() }

    // Every tile on screen has its thumbnail, as the grid reports for renders.
    function viewportReady() {
        if (!visible || root.sections.rowCount() === 0 || list.height <= 0) return false
        const first = Math.max(0, list.indexAt(4, list.contentY + 4))
        let last = list.indexAt(4, list.contentY + list.height - 4)
        if (last < 0) last = root.sections.rowCount() - 1
        for (let i = first; i <= last; ++i) {
            const item = list.itemAtIndex(i)
            if (!item) return false
            if (!item.presented()) return false
        }
        return true
    }

    function updateSticky() {
        const item = list.itemAt(20, list.contentY + 1)
        root.stickyLabel = !root.reviewMode && item && !item.header ? item.label : ""
    }

    // A sort or a reshuffle keeps the tile that was at the top in view, as
    // the flat grid does.
    function rememberTop() {
        if (!root.visible || list.contentY <= list.originY + 1) {
            root.topPath = ""
            root.keepTop = root.visible
            return
        }
        root.keepTop = false
        const item = list.itemAt(20, list.contentY + 1)
        root.topPath = item ? Captures.pathAt(item.first) : ""
        root.topOffset = item ? list.contentY - item.y : 0
        root.topHeader = !!item && item.header
    }

    function restoreTop() {
        if (root.keepTop) {
            root.keepTop = false
            root.topPath = ""
            list.positionViewAtBeginning()
            return
        }
        if (root.topPath === "") return
        const row = Captures.rowOf(root.topPath)
        root.topPath = ""
        if (row < 0) return
        let listRow = root.sections.rowOfItem(row)
        // Back to the header only when the header was what sat at the top;
        // the offset was measured from that row.
        if (root.topHeader && listRow > 0 && root.sections.rowAt(listRow - 1).header
                && root.sections.rowAt(listRow).first === row) {
            listRow -= 1
        }
        if (listRow < 0) return
        list.positionViewAtIndex(listRow, ListView.Beginning)
        list.contentY += Math.min(root.topOffset, root.sections.rowAt(listRow).header
                                                  ? root.headerHeight : root.cellHeight)
    }

    onVisibleChanged: {
        root.revealPending = true
        // Active first: the Binding may not have caught up yet, and a rebuild
        // while inactive has no rows to reveal the current tile in.
        if (visible) {
            root.sections.active = true
            root.sections.rebuildNow()
        }
    }
    onReviewModeChanged: root.revealPending = true

    Connections {
        target: Captures
        function onLayoutAboutToBeChanged() { root.rememberTop() }
        function onModelAboutToBeReset() { root.rememberTop() }
        function onDataChanged() { root.scheduleRevision() }
    }
    Connections {
        target: root.sections
        function onRebuilt() {
            root.bumpRevision()
            // A filter or rescan can take the current tile away without
            // moving the index; start again from the tile there now.
            if (root.host && Captures.rowOf(root.currentPath) < 0)
                root.currentPath = Captures.pathAt(root.host.currentIndex)
            // Only a rebuild with rows can show the current tile.
            if (root.revealPending && root.visible && root.sections.active) {
                root.revealPending = false
                root.topPath = ""
                root.keepTop = false
                Qt.callLater(root.revealCurrent)
            } else {
                root.restoreTop()
            }
            Qt.callLater(root.updateSticky)
            Qt.callLater(root.updateExactTop)
        }
    }
    Connections {
        target: root.host
        function onCurrentIndexChanged() {
            // Rows inserted or removed before the current tile move its index
            // to follow the tile; that is no reason to scroll to it. The grid
            // applies such a move when it next lays out, so compare where the
            // tile is now rather than what was at the old index.
            const index = root.host.currentIndex
            const followed = root.currentPath !== "" && Captures.rowOf(root.currentPath) === index
            root.currentPath = Captures.pathAt(index)
            if (followed) return
            // A re-sort restores the current tile before the rows are rebuilt
            // for it; restoreTop places the view once they are.
            if (root.visible && !root.suppressReveal && !root.host.restoringLayout) root.revealCurrent()
        }
    }

    // Standalone, beside the list, as the grid's is.
    ScrollBar {
        id: scrollbar
        objectName: "sectionsScrollBar"
        orientation: Qt.Vertical
        policy: list.contentHeight > list.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 12
        size: list.height / Math.max(root.exactHeight, 1)
        position: root.exactTop / Math.max(root.exactHeight, 1)
        onPositionChanged: {
            if (pressed && !root.scrolling) root.scrollToExact(position * root.exactHeight)
        }
    }

    Component {
        id: reviewSummary
        Item {
            width: list.width
            height: 44

            Text {
                id: summary
                anchors.left: parent.left
                anchors.leftMargin: Math.round(root.cellSpacing / 2)
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - picker.width - 24)
                elide: Text.ElideRight
                text: root.setCount + (root.setCount === 1 ? " similar set" : " similar sets")
                      + " · review before removing anything"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.mutedText
            }
            SimilarStrictnessPicker {
                id: picker
                anchors.right: parent.right
                anchors.rightMargin: Math.round(root.cellSpacing / 2)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    Component {
        id: headerDelegate
        // A section header: its name and size, and Compare for a similar set
        // or Select for a day or month.
        Item {
            id: rowItem
            required property int index
            required property int first
            required property string label
            required property string detail
            required property int sectionFirst
            required property int sectionCount
            readonly property bool header: true
            width: list.width
            height: root.headerHeight
            // A header waiting in the pool is out of the layout but would
            // still be read out, under whatever sections are showing.
            ListView.onPooled: rowItem.visible = false
            ListView.onReused: rowItem.visible = true

            function presented() { return true }

            Item {
                id: headerRow
                objectName: "sectionHeader"
                anchors.fill: parent
                anchors.leftMargin: Math.round(root.cellSpacing / 2)
                anchors.rightMargin: Math.round(root.cellSpacing / 2)
                Accessible.ignored: !headerRow.visible
                Accessible.role: Accessible.Heading
                Accessible.name: rowItem.label + ", " + rowItem.detail

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    height: 1
                    color: Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.14)
                }
                Text {
                    id: setTitle
                    objectName: root.reviewMode ? "reviewSetHeader" : "dateSectionTitle"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width
                                    - (root.reviewMode ? compareButton.width : selectControl.width) - 16)
                    elide: Text.ElideRight
                    text: rowItem.label
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Theme.brightForeground
                    Accessible.ignored: true
                }
                Text {
                    anchors.left: setTitle.right
                    anchors.leftMargin: 8
                    anchors.right: root.reviewMode ? compareButton.left : selectControl.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: rowItem.detail
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                    Accessible.ignored: true
                }
                PillButton {
                    id: compareButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.reviewMode
                    width: visible ? implicitWidth : 0
                    label: "Compare"
                    accessibleName: "Compare " + rowItem.label
                    toolTip: "Compare this set side by side and keep one"
                    onClicked: root.host.compareRequested(
                                   root.setPaths(rowItem.sectionFirst, rowItem.sectionCount))
                }

                // Selects the whole day or month; deselects it once all of it
                // is selected. Shift+X does the same for the current tile's.
                Item {
                    id: selectControl
                    objectName: root.reviewMode ? "" : "sectionSelect"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !root.reviewMode
                    width: visible ? selectBox.width + selectText.implicitWidth + 14 : 0
                    height: 26
                    // 0 none, 1 some, 2 all of the section is selected.
                    readonly property int selection: {
                        void root.host.checkedSet
                        void root.host.sectionCountsRevision
                        return visible ? root.host.sectionCheckState(rowItem.sectionFirst, rowItem.sectionCount) : 0
                    }
                    Accessible.ignored: !selectControl.visible
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: "Select " + rowItem.label + ", " + rowItem.detail
                    Accessible.description: "Shift+X selects the current tile's section"
                    Accessible.checkable: true
                    Accessible.checked: selection === 2
                    Accessible.checkStateMixed: selection === 1
                    Accessible.onPressAction: root.host.toggleSection(rowItem.label)
                    Accessible.onToggleAction: root.host.toggleSection(rowItem.label)

                    Rectangle {
                        id: selectBox
                        anchors.left: parent.left
                        anchors.leftMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        radius: 3
                        color: selectControl.selection === 2 ? Theme.accent : "transparent"
                        border.width: 1
                        border.color: selectControl.selection > 0 || selectHover.hovered
                                      ? Theme.accent
                                      : Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.42)
                        Text {
                            anchors.centerIn: parent
                            visible: selectControl.selection === 2
                            text: "✓"
                            font.pixelSize: 11
                            font.weight: Font.Bold
                            color: Theme.background
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            visible: selectControl.selection === 1
                            width: 8
                            height: 2
                            color: Theme.accent
                        }
                    }
                    Text {
                        id: selectText
                        anchors.left: selectBox.right
                        anchors.leftMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Select"
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: selectHover.hovered || selectControl.selection === 2 ? Theme.accent : Theme.mutedText
                    }
                    HoverHandler { id: selectHover }
                    TapHandler {
                        onTapped: {
                            const paths = root.host.sectionPaths(rowItem.label)
                            Captures.commitPendingSearch()
                            root.host.toggleSectionPaths(paths)
                        }
                    }
                }
            }
        }
    }

    Component {
        id: tileRowDelegate
        // A row of tiles from one section. The repeater always holds a full
        // row or more, so a recycled row only changes what its tiles show;
        // tiles past the end of the row are hidden.
        Item {
            id: rowItem
            required property int index
            required property int first
            required property int count
            required property bool month
            required property string label
            readonly property bool header: false
            width: list.width
            height: root.cellHeight
            ListView.onPooled: rowItem.visible = false
            ListView.onReused: rowItem.visible = true

            function presented() {
                for (let i = 0; i < rowItem.count; ++i) {
                    const tile = tiles.itemAt(i)
                    if (!tile || !tile.presented) return false
                }
                return true
            }

            // Each tile places itself by its column. A Row positioner would
            // place tiles only at its next polish, so tiles it had just made
            // (the pool shrinking after a resize settles, or growing) sat in
            // the first column until then, on top of each other.
            Item {
                Repeater {
                    id: tiles
                    model: root.poolColumns

                    Item {
                        id: cell
                        required property int index
                        readonly property bool used: index < rowItem.count
                        readonly property int row: used ? rowItem.first + index : -1
                        readonly property var tile: {
                            void root.revision
                            return cell.row >= 0 ? Captures.tileAt(cell.row) : ({})
                        }
                        readonly property bool presented: card.thumbnailPresented
                        readonly property string path: tile.path || ""
                        visible: used
                        x: index * root.cellWidth
                        width: root.cellWidth
                        height: root.cellHeight

                        CaptureCard {
                            id: card
                            anchors.fill: parent
                            anchors.margins: Math.round(root.cellSpacing / 2)

                            path: cell.path
                            fileName: cell.tile.fileName || ""
                            kindLabel: cell.tile.kindLabel || ""
                            timeLabel: cell.tile.timeLabel || ""
                            dateLabel: cell.tile.dateLabel || ""
                            // A day's header already names the day.
                            dateShown: !(root.sections.mode === 1 && !rowItem.month)
                            sizeLabel: cell.tile.sizeLabel || ""
                            isVideo: cell.tile.isVideo === true
                            isDocument: cell.tile.isDocument === true
                            stamp: cell.tile.stamp || 0
                            thumbnailVersion: cell.tile.thumbnailVersion || ""
                            favorite: cell.tile.favorite === true
                            rating: cell.tile.rating || 0
                            hiddenMark: cell.tile.hidden === true
                            ocrSnippet: cell.tile.ocrSnippet || ""
                            caption: cell.tile.caption || ""
                            rawFormat: cell.tile.rawFormat || ""
                            selected: !!root.host && root.host.currentIndex === cell.row
                            keyboardCurrent: selected && list.activeFocus
                            checked: !!root.host && root.host.isChecked(cell.path)
                            selectionMode: !!root.host && root.host.checkedCount > 0

                            onDragStarting: dragPaths = root.host.dragPathsFor(cell.path)
                            onActivated: {
                                Captures.commitPendingSearch()
                                root.host.activateTile(cell.path)
                                list.forceActiveFocus()
                            }
                            onChosen: {
                                Captures.commitPendingSearch()
                                const row = Captures.rowOf(cell.path)
                                if (row < 0) return
                                root.host.pointTo(row)
                                root.host.detailRequested(row)
                            }
                            onContextRequested: function (x, y) {
                                Captures.commitPendingSearch()
                                const row = Captures.rowOf(cell.path)
                                if (row < 0) return
                                root.host.pointTo(row)
                                root.host.contextRequested(row, x, y)
                            }
                            onToggleChecked: {
                                Captures.commitPendingSearch()
                                if (Captures.rowOf(cell.path) >= 0) root.host.toggleChecked(cell.path)
                            }
                            onExtendRequested: {
                                Captures.commitPendingSearch()
                                root.host.pointerExtend(cell.path)
                                list.forceActiveFocus()
                            }
                            onToggleRequested: {
                                Captures.commitPendingSearch()
                                root.host.pointerToggle(cell.path)
                                list.forceActiveFocus()
                            }
                        }
                    }
                }
            }
        }
    }

    ListView {
        id: list
        objectName: "reviewSetsList"
        anchors.fill: parent
        anchors.rightMargin: 12
        focus: true
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.sections
        // Rows of the same width keep their tiles when recycled, which is
        // what keeps a long scroll as cheap as the flat grid's.
        reuseItems: true
        // As far ahead as the flat grid, so a scroll finds thumbnails ready.
        cacheBuffer: root.cellHeight * 4
        header: root.reviewMode ? reviewSummary : null
        onContentYChanged: {
            root.updateSticky()
            if (!root.scrolling) root.updateExactTop()
        }
        onHeightChanged: Qt.callLater(root.updateExactTop)
        // The list moves its origin as it learns the real height of rows it
        // had estimated, without moving contentY.
        onOriginYChanged: Qt.callLater(root.updateExactTop)

        // Headers and tile rows are separate delegates, so each kind is
        // recycled as its own kind and a tile row keeps its tiles.
        delegate: DelegateChooser {
            role: "header"
            DelegateChoice { roleValue: true; delegate: headerDelegate }
            DelegateChoice { roleValue: false; delegate: tileRowDelegate }
        }

        // The grid's keys, moving through the sections' rows rather than the
        // grid's cells.
        Keys.onPressed: function (event) { root.host.handleKey(event) }
    }
}
