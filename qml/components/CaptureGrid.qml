import QtQuick
import QtQuick.Controls.Basic

// The library itself: a recycling grid, newest first. Date sorts and similar
// sets show sections, laid out by ReviewSetsView; name, size and rating sorts
// are one flat GridView.
//
// Both views recycle delegates, so the cost of scrolling is set by what is on
// screen rather than by how large the library is.
//
// Selection works as in a file manager: an anchor, Shift+click and Shift+keys
// to extend from it, Ctrl+click for one tile, and a rubber band dragged from
// empty space (Ctrl adds to what is selected).
FocusScope {
    id: root

    property var model: null
    property alias currentIndex: grid.currentIndex
    property alias count: grid.count
    property string currentDayLabel: ""
    property bool layoutReady: true
    // False from a resize until the grid has been rebuilt around the new
    // columns. The model is detached for part of that, so keys pressed then
    // have no rows to move through.
    readonly property bool layoutSettled: layoutReady && grid.reuseItems && !restoringLayout
                                          && !relayoutTimer.running

    // Multi-select is held by path, not by row. Sorting, filtering and rescans
    // all move rows around, and a selection that silently retargets is how bulk
    // delete trashes the wrong files.
    property var checkedSet: ({})
    property int checkedCount: 0
    property string selectedPath: ""
    property string layoutAnchorPath: ""
    property real layoutAnchorOffset: 0
    property bool restoringLayout: false
    // False until a tile is picked by key, pointer or an open request. Until
    // then the current tile is simply the first one, so a re-sort, such as
    // camera dates arriving after the scan, keeps the first tile current
    // rather than following the one that sorted first a moment ago.
    property bool currentPicked: false
    // True while a re-sort happens with the flat grid at its top.
    property bool layoutAtTop: false
    // Date sorts show a section per day or month, similar pictures one per
    // set. The sections lay out Captures' own rows, so only for that model.
    readonly property var sectionModel: Captures.sectionRows
    readonly property bool sectioned: root.model === Captures && sectionModel.mode !== 0
    // The two smallest tile sizes group by month: a day header over every
    // row or two of small tiles is more header than picture.
    readonly property bool compactTiles: Settings.tileWidth <= 200
    // Shift extends from here. The base is what was selected when the anchor
    // was placed, so shrinking a range gives back exactly what it added.
    property string anchorPath: ""
    property var anchorBase: ({})
    // The rows the last Shift range covered, so the next one only touches
    // the rows that change.
    property int rangeLow: -1
    property int rangeHigh: -1
    // Rows moved since the last range (a re-sort, a rescan, a capture added
    // or removed), so its row numbers no longer name the same tiles and the
    // next range counts the selection again in full.
    property bool rangeStale: false
    // Checked tiles per section, by the section's first row, for the section
    // headers, valid for the selection named in `set`. Each entry keeps the
    // section's length too: a recycled header waiting in the view's pool can
    // still ask about its old section, which may start at the same row and
    // be longer or shorter. Small changes update it; anything else leaves it
    // for another selection, and a header counts its section again when it
    // next asks. Mutated in place, never assigned, so header bindings do not
    // re-run on it.
    readonly property var sectionCounts: ({ set: null, counts: ({}) })
    // Set from a change to Captures' rows until the sections are rebuilt for
    // it. In between, a section's first row may name another section's tiles,
    // so counts are worked out but not kept.
    property bool sectionCountsDirty: false
    // Bumped after each rebuild, so headers ask for their state again even
    // when their own row did not change.
    property int sectionCountsRevision: 0

    signal chosen(int index)
    signal deleteRequested(string path)
    signal detailRequested(int index)
    signal contextRequested(int index, real x, real y)
    // Tab and Shift+Tab walk the sections. Handled here as key events rather
    // than window shortcuts: on a real keyboard Shift+Tab arrives as Backtab,
    // and Qt's shortcut matcher also offers plain Tab for that key, so two
    // shortcuts match, the press counts as ambiguous, and neither fires.
    signal sectionStepRequested(int step)
    signal compareRequested(var paths)

    // Include every intersecting cell, including the partial bottom row.
    // Cached offscreen delegates do not count, nor does an empty library.
    function viewportReady() {
        if (sectioned) return visible && reviewSets.viewportReady()
        if (!visible || !layoutReady || grid.count === 0 || grid.width <= 0 || grid.height <= 0)
            return false
        const firstRow = Math.max(0, Math.floor((grid.contentY - grid.originY) / grid.cellHeight))
        const lastRow = Math.ceil((grid.contentY - grid.originY + grid.height) / grid.cellHeight)
        const first = firstRow * columns
        const end = Math.min(grid.count, lastRow * columns)
        if (first >= end) return false
        for (let i = first; i < end; ++i) {
            const cell = grid.itemAtIndex(i)
            if (!cell || !cell.thumbnailPresented) return false
        }
        return true
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function isChecked(path) { return checkedSet[path] === true }

    // What a drag from this tile carries: the whole selection when the tile
    // is part of it. Asked only as a drag starts.
    function dragPathsFor(path) { return isChecked(path) ? checkedPaths() : [path] }

    // Replaces the selection. `changed` lists the paths that may differ
    // from the current one, when the caller knows; then the count and the
    // per-section counts are updated from those alone.
    function assignChecked(next, changed) {
        let count
        if (changed === undefined || changed.length > 2000) {
            count = Object.keys(next).length
        } else {
            count = checkedCount
            const cache = sectionCounts
            const tracked = root.sectioned && cache.set === checkedSet
            for (const path of changed) {
                const was = checkedSet[path] === true
                const now = next[path] === true
                if (was === now) continue
                count += now ? 1 : -1
                if (!tracked) continue
                const first = sectionModel.sectionFirstOf(Captures.rowOf(path))
                const entry = first >= 0 ? cache.counts[first] : undefined
                if (entry !== undefined) entry.checked += now ? 1 : -1
            }
            if (tracked) cache.set = next
        }
        // The set first: a header asking in between counts the new set.
        checkedSet = next
        checkedCount = count
    }

    function setAnchor(path) {
        anchorPath = path
        anchorBase = checkedSet
        resetRange()
    }

    function resetRange() {
        rangeLow = -1
        rangeHigh = -1
        rangeStale = false
    }

    function toggleChecked(path) {
        if (path === "") {
            return
        }
        const next = Object.assign({}, checkedSet)
        if (next[path]) {
            delete next[path]
        } else {
            next[path] = true
        }
        assignChecked(next, [path])
        setAnchor(path)
    }

    function clearChecked() {
        assignChecked({})
        anchorBase = ({})
        resetRange()
    }

    function checkAll() {
        const next = {}
        for (let i = 0; i < grid.count; i++) {
            next[Captures.pathAt(i)] = true
        }
        assignChecked(next)
        anchorBase = checkedSet
        resetRange()
    }

    // The anchor's range to row, on top of what was selected before it.
    function selectRange(row) {
        let from = Captures.rowOf(anchorPath)
        if (from < 0) {
            from = row
            setAnchor(Captures.pathAt(row))
        }
        const low = Math.min(from, row)
        const high = Math.max(from, row)
        // Only rows in one of the old and new ranges can change.
        const changed = []
        if (rangeLow < 0) {
            for (let i = low; i <= high; ++i) changed.push(Captures.pathAt(i))
        } else {
            for (let i = Math.min(low, rangeLow); i <= Math.max(high, rangeHigh); ++i) {
                const inOld = i >= rangeLow && i <= rangeHigh
                const inNew = i >= low && i <= high
                if (inOld !== inNew) changed.push(Captures.pathAt(i))
            }
        }
        const next = Object.assign({}, anchorBase)
        for (let i = low; i <= high; ++i) next[Captures.pathAt(i)] = true
        // After rows moved, the old range's rows hold other tiles: count all.
        assignChecked(next, rangeStale ? undefined : changed)
        rangeLow = low
        rangeHigh = high
        rangeStale = false
    }

    // 0 when none of the rows are selected, 1 for some, 2 for all.
    function sectionCheckState(first, count) {
        if (checkedCount === 0 || count <= 0) return 0
        const cache = sectionCounts
        if (cache.set !== checkedSet) {
            cache.set = checkedSet
            cache.counts = {}
        }
        const entry = cache.counts[first]
        let checked
        if (entry !== undefined && entry.count === count) {
            checked = entry.checked
        } else {
            checked = 0
            for (let i = first; i < first + count; ++i) {
                if (checkedSet[Captures.pathAt(i)] === true) ++checked
            }
            if (!root.sectionCountsDirty) cache.counts[first] = { count: count, checked: checked }
        }
        return checked === 0 ? 0 : (checked >= count ? 2 : 1)
    }

    // The files of the section with this label. A capture that just arrived
    // changes Captures at once but the sections a turn later, when a
    // header's own first row and count would cover another day's files, so
    // the sections are brought up to date first.
    function sectionPaths(label) {
        if (sectionModel.rebuildPending()) sectionModel.rebuildNow()
        const section = sectionModel.sectionNamed(label)
        const paths = []
        if (section.first === undefined) return paths
        for (let i = section.first; i < section.first + section.count; ++i) paths.push(Captures.pathAt(i))
        return paths
    }

    // A section header's control: select the section, or clear it once all
    // of it is selected. Taken by paths, read before a waiting search
    // applies, and only those still shown count: the search can move rows
    // and fold days into months.
    function toggleSectionPaths(paths) {
        const shown = paths.filter(function (path) { return Captures.rowOf(path) >= 0 })
        if (shown.length === 0) return
        const all = shown.every(function (path) { return checkedSet[path] === true })
        const next = Object.assign({}, checkedSet)
        for (const path of shown) {
            if (all) delete next[path]
            else next[path] = true
        }
        assignChecked(next, shown)
        setAnchor(shown[0])
    }
    function toggleSection(label) { toggleSectionPaths(sectionPaths(label)) }

    // Shift+X: the section holding the current tile.
    function toggleCurrentSection() {
        if (!root.sectioned || sectionModel.mode !== 1 || grid.currentIndex < 0) return
        if (sectionModel.rebuildPending()) sectionModel.rebuildNow()
        const section = sectionModel.sectionOfItem(grid.currentIndex)
        if (section.label === undefined) return
        toggleSection(section.label)
    }

    // The current tile follows the pointer without scrolling: a tile that
    // moved under the pointer between two clicks would open the wrong file.
    function pointTo(row) {
        if (row < 0) return
        reviewSets.suppressReveal = true
        root.pick(row)
        reviewSets.suppressReveal = false
    }

    // Makes a tile current on purpose, so it stays current when a re-sort
    // moves it. Anything that opens, reveals or acts on a tile picks it.
    function pick(row) {
        if (row < 0) return
        root.currentPicked = true
        root.selectedPath = Captures.pathAt(row)
        grid.currentIndex = row
    }

    function activateTile(path) {
        const row = Captures.rowOf(path)
        if (row < 0) return
        pointTo(row)
        setAnchor(path)
    }

    function pointerExtend(path) {
        const row = Captures.rowOf(path)
        if (row < 0) return
        if (Captures.rowOf(anchorPath) < 0) {
            setAnchor(root.currentIndex >= 0 ? Captures.pathAt(root.currentIndex) : path)
        }
        pointTo(row)
        selectRange(row)
    }

    function pointerToggle(path) {
        const row = Captures.rowOf(path)
        if (row < 0) return
        pointTo(row)
        toggleChecked(path)
    }

    // Where a key moves the current tile in the flat grid. Unextended moves
    // wrap as GridView's own key navigation does.
    function flatTarget(current, direction, wrap) {
        const count = grid.count
        const columns = root.columns
        if (count === 0) return -1
        if (current < 0) return 0
        const lastRow = Math.floor((count - 1) / columns)
        const rows = Math.max(1, Math.floor(grid.height / grid.cellHeight))
        switch (direction) {
        case "left": return current > 0 ? current - 1 : (wrap ? count - 1 : 0)
        case "right": return current < count - 1 ? current + 1 : (wrap ? 0 : count - 1)
        case "up":
            if (current >= columns) return current - columns
            return wrap ? count - 1 : current
        case "down":
            if (current < count - columns) return current + columns
            if (wrap) return 0
            return Math.floor(current / columns) < lastRow ? count - 1 : current
        case "pageUp": return Math.max(current % columns, current - rows * columns)
        case "pageDown": return Math.min(count - 1, current + rows * columns)
        case "home": return 0
        case "end": return count - 1
        }
        return current
    }

    function moveCurrent(direction, extending) {
        const before = grid.currentIndex
        // The flat grid has no days or months to jump between.
        if (!root.sectioned && direction === "sectionNext") direction = "down"
        if (!root.sectioned && direction === "sectionPrevious") direction = "up"
        const target = root.sectioned ? reviewSets.targetIndex(before, direction, !extending)
                                      : root.flatTarget(before, direction, !extending)
        if (target < 0) return
        root.currentPicked = true
        if (extending && Captures.rowOf(anchorPath) < 0) {
            setAnchor(Captures.pathAt(before >= 0 ? before : target))
        }
        root.selectedPath = Captures.pathAt(target)
        grid.currentIndex = target
        if (!root.sectioned) {
            if (direction === "home") grid.positionViewAtBeginning()
            else if (direction === "end") grid.positionViewAtEnd()
            else grid.positionViewAtIndex(target, GridView.Contain)
        } else {
            // Also when the key leaves the current tile where it is.
            reviewSets.revealCurrent()
        }
        if (extending) selectRange(target)
        else setAnchor(Captures.pathAt(target))
    }

    // Both views send their keys here. Arrows, vim keys, the page keys and
    // Home/End move the current tile, with Shift extending the selection.
    // Single-letter action keys live in Main so they work whether or not the
    // grid has focus.
    function handleKey(event) {
        if (root.Window.window) root.Window.window.keyboardNavigation = true
        Captures.commitPendingSearch()
        const current = grid.currentIndex
        const extending = (event.modifiers & Qt.ShiftModifier) !== 0
        // Ctrl with the arrows jumps to the previous or next day or month.
        const jumping = (event.modifiers & Qt.ControlModifier) !== 0
        let direction = ""
        switch (event.key) {
        case ShortcutCatalog.libraryKeys.Return:
        case ShortcutCatalog.libraryKeys.Enter:
            if (current >= 0) {
                root.currentPicked = true
                root.detailRequested(current)
                event.accepted = true
            }
            return
        case ShortcutCatalog.libraryKeys.Space:
            if (current >= 0) {
                root.currentPicked = true
                root.chosen(current)
                event.accepted = true
            }
            return
        case ShortcutCatalog.libraryKeys.Delete:
            // Shift+Delete belongs to the non-repeating permanent-delete
            // shortcut. A repeat rejected there must not turn into Trash.
            if (extending) {
                event.accepted = true
                return
            }
            if (current >= 0) {
                root.deleteRequested(Captures.pathAt(current))
                event.accepted = true
            }
            return
        case ShortcutCatalog.libraryKeys.X:
            if (current >= 0) {
                if (extending) root.toggleCurrentSection()
                else root.toggleChecked(Captures.pathAt(current))
                event.accepted = true
            }
            return
        case ShortcutCatalog.libraryKeys.Tab:
            root.sectionStepRequested(extending ? -1 : 1)
            event.accepted = true
            return
        case ShortcutCatalog.libraryKeys.Backtab:
            root.sectionStepRequested(-1)
            event.accepted = true
            return
        case ShortcutCatalog.libraryKeys.Left:
        case ShortcutCatalog.libraryKeys.H:
            direction = "left"
            break
        case ShortcutCatalog.libraryKeys.Right:
        case ShortcutCatalog.libraryKeys.L:
            direction = "right"
            break
        case ShortcutCatalog.libraryKeys.Up:
        case ShortcutCatalog.libraryKeys.K:
            direction = jumping && event.key === ShortcutCatalog.libraryKeys.Up ? "sectionPrevious" : "up"
            break
        case ShortcutCatalog.libraryKeys.Down:
        case ShortcutCatalog.libraryKeys.J:
            direction = jumping && event.key === ShortcutCatalog.libraryKeys.Down ? "sectionNext" : "down"
            break
        case ShortcutCatalog.libraryKeys.PageUp:
            direction = "pageUp"
            break
        case ShortcutCatalog.libraryKeys.PageDown:
            direction = "pageDown"
            break
        case ShortcutCatalog.libraryKeys.Home:
            direction = "home"
            break
        case ShortcutCatalog.libraryKeys.End:
            direction = "end"
            break
        default:
            return
        }
        event.accepted = true
        root.moveCurrent(direction, extending)
    }

    function checkedPaths() { return Object.keys(checkedSet) }

    // The scroll position in exact content pixels, whichever view shows. The
    // sections' list only estimates where unseen rows sit, so jumps and the
    // probe go through these rather than its contentY.
    function scrollExtent() {
        return root.sectioned ? reviewSets.scrollExtent() : Math.max(0, grid.contentHeight - grid.height)
    }
    function scrollPosition() {
        return root.sectioned ? reviewSets.exactTop : grid.contentY - grid.originY
    }
    function scrollTo(y) {
        if (root.sectioned) reviewSets.scrollToExact(y)
        else grid.contentY = grid.originY + Math.max(0, Math.min(y, root.scrollExtent()))
    }

    function rememberLayout() {
        restoringLayout = true
        // A view at its top stays there, as a list newest first should.
        layoutAtTop = !root.sectioned && grid.contentY <= grid.originY + 1
        if (layoutAtTop) {
            layoutAnchorPath = ""
            layoutAnchorOffset = 0
            return
        }
        const anchor = grid.indexAt(grid.contentX + 4, grid.contentY + 4)
        layoutAnchorPath = anchor >= 0 ? Captures.pathAt(anchor) : ""
        layoutAnchorOffset = anchor >= 0
                             ? grid.contentY - Math.floor(anchor / root.columns) * grid.cellHeight
                             : 0
    }

    function restoreLayout() {
        if (!grid.reuseItems) {
            restoringLayout = false
            return
        }
        const selected = Captures.rowOf(selectedPath)
        if (!root.currentPicked && grid.count > 0) {
            grid.currentIndex = 0
            selectedPath = Captures.pathAt(0)
        } else if (selected >= 0) {
            grid.currentIndex = selected
        } else if (grid.count > 0) {
            if (grid.currentIndex < 0 || grid.currentIndex >= grid.count) {
                grid.currentIndex = 0
            }
            selectedPath = Captures.pathAt(grid.currentIndex)
        } else {
            selectedPath = ""
        }
        restoringLayout = false
        Qt.callLater(root.restoreLayoutAnchor)
    }

    function restoreLayoutAnchor() {
        const anchor = Captures.rowOf(layoutAnchorPath)
        if (layoutAtTop) {
            layoutAtTop = false
            if (!root.sectioned) grid.positionViewAtBeginning()
        } else if (anchor >= 0) {
            const wanted = Math.floor(anchor / root.columns) * grid.cellHeight
                           + layoutAnchorOffset
            grid.contentY = Math.max(0, Math.min(wanted, grid.contentHeight - grid.height))
        }
        layoutAnchorPath = ""
        root.updateDay()
    }

    // A file deleted outside, or filtered out of view, must not stay counted
    // in "Trash 3". Anything no longer in the visible model is dropped.
    function pruneChecked() {
        if (checkedCount === 0) {
            return
        }
        const next = {}
        for (const path of Object.keys(checkedSet)) {
            if (Captures.rowOf(path) >= 0) {
                next[path] = true
            }
        }
        checkedSet = next
        checkedCount = Object.keys(next).length
        const base = {}
        for (const path of Object.keys(anchorBase)) {
            if (next[path] === true) base[path] = true
        }
        anchorBase = base
    }
    // Rows of Captures moved: row numbers kept for the selection no longer
    // name the same tiles.
    function rowsShifted() {
        rangeStale = true
        band.rowsShifted()
        sectionCounts.set = null
        sectionCountsDirty = true
    }
    Connections {
        target: root.sectionModel
        function onRebuilt() {
            root.sectionCounts.set = null
            root.sectionCountsDirty = false
            root.sectionCountsRevision++
            band.rebase()
        }
    }
    Connections {
        target: Captures
        function onCountChanged() { root.pruneChecked() }
        function onLayoutAboutToBeChanged() { root.rememberLayout() }
        function onLayoutChanged() { root.rowsShifted(); root.restoreLayout() }
        function onModelAboutToBeReset() { root.rememberLayout() }
        function onModelReset() { root.rowsShifted(); root.restoreLayout() }
        function onRowsInserted() { root.rowsShifted() }
        function onRowsRemoved() { root.rowsShifted() }
        function onRowsMoved() { root.rowsShifted() }
    }

    // Cells stay near a target width and flex to fill the row exactly, so there
    // is never a ragged gutter down the right edge. The scrollbar's width comes
    // out of the available space rather than overlapping the last column.
    readonly property int targetCellWidth: Settings.tileWidth
    readonly property int tileStep: 40
    readonly property int cellSpacing: 12
    readonly property int scrollbarWidth: 12
    readonly property int available: Math.max(1, width - scrollbarWidth)
    readonly property int columns: Math.max(1, Math.floor(available / targetCellWidth))
    readonly property int cellWidth: Math.floor(available / columns)
    // Behind the sections the flat grid has no viewport and so no tiles to
    // misplace; it lays out afresh when it comes back. Reattaching its model
    // then instead would drop the current tile for a moment, and a choice
    // made in that moment, such as the copy to keep, would be lost.
    onCellWidthChanged: if (!root.sectioned) relayoutTimer.restart()
    onSectionedChanged: {
        if (!root.sectioned) grid.forceLayout()
        Qt.callLater(root.updateDay)
    }

    // GridView resizes existing delegates when cellWidth changes, but does not
    // reliably move them to their new columns. Reattach the same model after a
    // resize settles, preserving the user's place while positions are rebuilt.
    Timer {
        id: relayoutTimer
        objectName: "gridRelayoutTimer"
        interval: 80
        onTriggered: {
            if (grid.count === 0) {
                return
            }
            const scroll = grid.contentY
            // Drop the delegate pool across the reattach. Delegates pooled
            // before the model went away and taken back for the same row
            // afterwards kept their old file while their index pointed at
            // the row's new one, so the tile showed one capture and opened
            // another.
            grid.reuseItems = false
            root.layoutReady = false
            Qt.callLater(function () {
                root.layoutReady = true
                Qt.callLater(function () {
                    grid.reuseItems = true
                    root.restoreLayout()
                    grid.contentY = Math.max(0, Math.min(scroll, grid.contentHeight - grid.height))
                })
            })
        }
    }

    function updateDay() {
        if (root.sectioned) {
            // Each section has its own header there; the pill names the day
            // only while that header is scrolled away.
            root.currentDayLabel = reviewSets.stickyLabel
            return
        }
        const index = grid.indexAt(grid.contentX + 4, grid.contentY + 4)
        if (index >= 0) {
            root.currentDayLabel = Captures.gridLabelAt(index)
        } else if (grid.count > 0) {
            // indexAt() answers -1 until the view has laid out, which is the
            // state the very first countChanged arrives in. Fall back to the
            // first row rather than leaving the header blank until a scroll.
            root.currentDayLabel = Captures.gridLabelAt(0)
        } else {
            root.currentDayLabel = ""
        }
    }

    GridView {
        id: grid
        objectName: "flatGrid"
        // Left from the first tile lands on the last and Right from the last
        // on the first, matching the viewer's Previous and Next.
        keyNavigationWraps: true
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: root.scrollbarWidth
        // Behind the sections it keeps the current index but no viewport,
        // so it creates no tiles and requests no thumbnails.
        height: root.sectioned ? 0 : root.height
        focus: !root.sectioned
        visible: !root.sectioned
        clip: true

        model: root.layoutReady ? root.model : null
        reuseItems: true

        cellWidth: root.cellWidth
        cellHeight: Math.round(root.cellWidth * 0.68)

        cacheBuffer: root.sectioned ? 0 : cellHeight * 4
        boundsBehavior: Flickable.StopAtBounds
        highlight: null

        // A sort or filter change reshuffles rows; moving rather than snapping
        // makes it read as the same library reordering itself.
        displaced: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: MediaMetadata.indexing ? 0 : 200
                easing.type: Easing.OutQuad
            }
        }
        onContentYChanged: root.updateDay()
        // Rows arriving above can move where the rows start under a band.
        onOriginYChanged: if (!root.sectioned) band.rebase()
        onCountChanged: {
            // Removing the selected last row can leave GridView's index one
            // past the end until its next polish pass. Keep every keyboard
            // action on a real row immediately after a trash or rescan.
            if (count === 0) {
                currentIndex = -1
            } else if (currentIndex < 0 || currentIndex >= count) {
                currentIndex = currentIndex < 0 ? 0 : count - 1
            }
            if (root.selectedPath === "" && currentIndex >= 0) {
                root.selectedPath = Captures.pathAt(currentIndex)
            }
            Qt.callLater(root.updateDay)
        }
        onCurrentIndexChanged: {
            if (!root.restoringLayout && grid.reuseItems) {
                const path = currentIndex >= 0 ? Captures.pathAt(currentIndex) : ""
                // Another file made current on purpose, such as one opened
                // from outside. Rows arriving above only move the index.
                if (root.selectedPath !== "" && path !== "" && path !== root.selectedPath)
                    root.currentPicked = true
                root.selectedPath = path
            }
        }
        Connections {
            target: Library
            function onDayLabelsChanged() { root.updateDay() }
        }
        onWidthChanged: Qt.callLater(root.updateDay)
        Component.onCompleted: Qt.callLater(root.updateDay)

        delegate: Item {
            id: cell
            readonly property bool thumbnailPresented: card.thumbnailPresented

            required property int index
            required property string path
            required property string fileName
            required property string kindLabel
            required property string timeLabel
            required property string dateLabel
            required property string sizeLabel
            required property bool isVideo
            required property bool isDocument
            required property bool favorite
            required property int rating
            required property bool hidden
            required property double stamp
            required property string thumbnailVersion
            required property string ocrSnippet
            required property string caption
            required property string rawFormat

            width: grid.cellWidth
            height: grid.cellHeight

            CaptureCard {
                id: card
                anchors.fill: parent
                anchors.margins: Math.round(root.cellSpacing / 2)

                path: root.sectioned ? "" : cell.path
                fileName: cell.fileName
                kindLabel: cell.kindLabel
                timeLabel: cell.timeLabel
                dateLabel: cell.dateLabel
                sizeLabel: cell.sizeLabel
                isVideo: cell.isVideo
                isDocument: cell.isDocument
                stamp: cell.stamp
                thumbnailVersion: cell.thumbnailVersion
                favorite: cell.favorite
                rating: cell.rating
                hiddenMark: cell.hidden
                ocrSnippet: cell.ocrSnippet
                caption: cell.caption
                rawFormat: cell.rawFormat
                selected: grid.currentIndex === cell.index
                keyboardCurrent: selected && grid.activeFocus
                checked: root.isChecked(cell.path)
                selectionMode: root.checkedCount > 0
                // Dragging a checked tile takes the whole selection with it.
                onDragStarting: dragPaths = root.dragPathsFor(cell.path)

                onActivated: {
                    Captures.commitPendingSearch()
                    root.activateTile(cell.path)
                    grid.forceActiveFocus()
                }
                onChosen: {
                    const path = cell.path
                    Captures.commitPendingSearch()
                    const row = Captures.rowOf(path)
                    if (row < 0) return
                    root.pointTo(row)
                    root.detailRequested(row)
                }
                onContextRequested: function(x, y) {
                    const path = cell.path
                    Captures.commitPendingSearch()
                    const row = Captures.rowOf(path)
                    if (row < 0) return
                    root.pointTo(row)
                    root.contextRequested(row, x, y)
                }
                onToggleChecked: {
                    const path = cell.path
                    Captures.commitPendingSearch()
                    if (Captures.rowOf(path) >= 0) root.toggleChecked(path)
                }
                onExtendRequested: {
                    Captures.commitPendingSearch()
                    root.pointerExtend(cell.path)
                    grid.forceActiveFocus()
                }
                onToggleRequested: {
                    Captures.commitPendingSearch()
                    root.pointerToggle(cell.path)
                    grid.forceActiveFocus()
                }
            }
        }

        Keys.onPressed: function (event) { root.handleKey(event) }

        function pointInfo(x, y) {
            return grid.infoAt(grid.contentY + Math.max(0, Math.min(y, grid.height - 1)))
        }
        // The same for a point in the grid's content, measured from where
        // the rows start now.
        function infoAt(contentY) {
            const total = Math.ceil(grid.count / root.columns)
            const cy = contentY - grid.originY
            const row = Math.floor(cy / grid.cellHeight)
            if (row >= total) return {row: total, offset: 0, header: false, past: true}
            return {row: row, offset: cy - row * grid.cellHeight, header: false, past: false}
        }
        function rowInfo(row) {
            const first = row * root.columns
            return {header: false, first: first, count: Math.max(0, Math.min(root.columns, grid.count - first))}
        }
        function rowTotal() { return Math.ceil(grid.count / root.columns) }
    }

    // Standalone rather than GridView's attached ScrollBar. The attached one is
    // reparented into the Flickable, so it cannot be anchored beside the grid;
    // driving a sibling keeps the bar in its own column where it can never sit
    // on top of a thumbnail.
    ReviewSetsView {
        id: reviewSets
        objectName: "reviewSets"
        anchors.fill: parent
        host: root
        visible: root.sectioned
        focus: root.sectioned
        columns: root.columns
        cellWidth: root.cellWidth
        cellHeight: grid.cellHeight
        cellSpacing: root.cellSpacing
        compact: root.compactTiles
        onVisibleChanged: Qt.callLater(root.updateDay)
        onStickyLabelChanged: root.updateDay()
    }

    // The view that is showing, as a flickable plus the geometry the rubber
    // band needs.
    function activeView() { return root.sectioned ? reviewSets : grid }
    function activeFlickable() { return root.sectioned ? reviewSets.listView : grid }

    // True where a press lands on no tile and no header: the gaps between
    // tiles, the end of a short row and the space below the last one.
    function emptyAt(x, y) {
        const view = root.activeView()
        const info = view.pointInfo(x, y)
        if (info.past) return true
        if (info.header) return false
        const row = view.rowInfo(info.row)
        if (!row || row.header) return false
        const column = Math.floor(x / root.cellWidth)
        if (column >= row.count) return true
        const margin = Math.round(root.cellSpacing / 2)
        const inX = x - column * root.cellWidth
        return inX < margin || inX > root.cellWidth - margin
               || info.offset < margin || info.offset > grid.cellHeight - margin
    }

    // Tiles the band touches, from where it started to where it is now.
    function bandRows(start, end, x0, x1) {
        const view = root.activeView()
        let top = start
        let bottom = end
        if (end.row < start.row || (end.row === start.row && end.offset < start.offset)) {
            top = end
            bottom = start
        }
        const margin = Math.round(root.cellSpacing / 2)
        const height = grid.cellHeight
        const last = Math.min(bottom.row, view.rowTotal() - 1)
        const rows = []
        for (let r = Math.max(0, top.row); r <= last; ++r) {
            const row = view.rowInfo(r)
            if (!row || row.header) continue
            const from = r === top.row ? top.offset : -1
            const to = r === bottom.row && !bottom.past ? bottom.offset : height + 1
            if (from > height - margin || to < margin) continue
            for (let c = 0; c < row.count; ++c) {
                const left = c * root.cellWidth + margin
                const right = (c + 1) * root.cellWidth - margin
                if (right >= x0 && left <= x1) rows.push(row.first + c)
            }
        }
        return rows
    }

    // The rubber band. It takes a press only on empty space, from a mouse,
    // so tiles keep their clicks and drags and touch still scrolls.
    MouseArea {
        id: band
        objectName: "rubberBand"
        anchors.fill: parent
        anchors.rightMargin: root.scrollbarWidth
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        clip: true
        z: 5
        property bool dragging: false
        // Ctrl+wheel resizes the tiles over either view; a plain wheel still
        // scrolls, because the handler leaves anything without Ctrl to the
        // view underneath.
        WheelHandler {
            objectName: "tileSizeWheel"
            target: null
            acceptedModifiers: Qt.ControlModifier
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (event) {
                const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y
                if (delta === 0) return
                Settings.tileWidth += delta > 0 ? root.tileStep : -root.tileStep
                event.accepted = true
            }
        }
        property real startX: 0
        property real startY: 0
        property real startContentY: 0
        property var startInfo: null
        // The list row the band started on, by section and place in it, so
        // rows added or removed above it while dragging do not move the
        // band's start to another row.
        property var startKey: null
        property var base: ({})
        // null until the first step, so a first step that touches no tile
        // still replaces the selection.
        property var lastKey: null
        // Paths the band covered at its last step: rows can move under a
        // band, paths cannot.
        property var lastPaths: null
        readonly property real viewStartY: startContentY - root.activeFlickable().contentY

        onPressed: function (mouse) {
            if (mouse.source !== Qt.MouseEventNotSynthesized || !root.emptyAt(mouse.x, mouse.y)) {
                mouse.accepted = false
                return
            }
            Captures.commitPendingSearch()
            startX = mouse.x
            startY = mouse.y
            startContentY = root.activeFlickable().contentY + mouse.y
            startInfo = root.activeView().pointInfo(mouse.x, mouse.y)
            startKey = null
            if (root.sectioned && !startInfo.past) {
                const row = root.sectionModel.rowAt(startInfo.row)
                if (row.label !== undefined) startKey = { label: row.label, offset: row.offset }
            }
            base = (mouse.modifiers & Qt.ControlModifier) ? root.checkedSet : ({})
            lastKey = null
            lastPaths = null
            dragging = false
            root.forceActiveFocus()
        }
        onPositionChanged: function (mouse) {
            if (!dragging && Math.abs(mouse.x - startX) + Math.abs(mouse.y - startY) < 6) return
            dragging = true
            band.refresh()
        }
        onReleased: band.finish()
        onCanceled: band.finish()

        function refresh() {
            if (!dragging || !startInfo) return
            const info = root.activeView().pointInfo(mouseX, mouseY)
            const rows = root.bandRows(startInfo, info, Math.min(startX, mouseX), Math.max(startX, mouseX))
            const key = rows.join(",")
            if (key === lastKey) return
            lastKey = key
            const next = Object.assign({}, base)
            const paths = rows.map(function (row) { return Captures.pathAt(row) })
            for (const path of paths) next[path] = true
            // After the first step only tiles entering or leaving the band
            // change.
            let changed = undefined
            if (lastPaths !== null) changed = paths.concat(lastPaths)
            lastPaths = paths
            root.assignChecked(next, changed)
        }

        // Rows moved mid-drag: the next step counts in full, and the base
        // loses what left the library, as the selection did.
        function rowsShifted() {
            if (startInfo === null) return
            // The flat grid lays its rows out again on its next pass.
            if (!root.sectioned) Qt.callLater(band.rebase)
            lastKey = null
            lastPaths = null
            const kept = {}
            for (const path of Object.keys(base)) {
                if (Captures.rowOf(path) >= 0) kept[path] = true
            }
            base = kept
        }

        // Rows moved under the band: find the start again where the band is
        // drawn, then select what it covers now. The flat grid's rows start
        // at its origin, which a rescan can move; the sections' rows by
        // their place in the list, or by their row key when that spot has
        // scrolled out of the laid-out rows.
        function rebase() {
            if (startInfo === null) return
            if (!root.sectioned) {
                startInfo = grid.infoAt(startContentY)
                lastKey = null
                refresh()
                return
            }
            let info = reviewSets.infoAt(startX, startContentY)
            if (!info.found) {
                info = Object.assign({}, startInfo)
                const row = startKey !== null ? root.sectionModel.rowOfKey(startKey.label, startKey.offset) : -1
                if (row >= 0) info.row = row
            }
            startInfo = info
            lastKey = null
            refresh()
        }

        function finish() {
            if (dragging && lastPaths !== null) {
                // The tile nearest where the band started anchors a Shift+click.
                // A band that touched nothing keeps the anchor, but either way
                // the band's selection is what a Shift range now builds on.
                if (lastPaths.length > 0) {
                    const downward = mouseY >= viewStartY
                    root.anchorPath = downward ? lastPaths[0] : lastPaths[lastPaths.length - 1]
                }
                root.anchorBase = root.checkedSet
                root.resetRange()
            }
            dragging = false
            startInfo = null
            root.forceActiveFocus()
        }

        // Dragging past the top or bottom edge scrolls, faster the further out.
        Timer {
            interval: 16
            repeat: true
            running: band.dragging && (band.mouseY < 24 || band.mouseY > band.height - 24)
            onTriggered: {
                const view = root.activeFlickable()
                const step = band.mouseY < 24 ? -Math.min(40, (24 - band.mouseY) / 2 + 4)
                                              : Math.min(40, (band.mouseY - band.height + 24) / 2 + 4)
                const lowest = view.originY
                const highest = view.originY + Math.max(0, view.contentHeight - view.height)
                view.contentY = Math.max(lowest, Math.min(highest, view.contentY + step))
                band.refresh()
            }
        }

        Rectangle {
            objectName: "rubberBandRect"
            visible: band.dragging
            Accessible.ignored: true
            x: Math.min(band.startX, band.mouseX)
            y: Math.min(band.viewStartY, band.mouseY)
            width: Math.abs(band.mouseX - band.startX)
            height: Math.abs(band.mouseY - band.viewStartY)
            radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 4) : 0
            color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.16)
            border.width: 1
            border.color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.8)
        }
    }

    ScrollBar {
        id: scrollbar
        visible: !root.sectioned
        orientation: Qt.Vertical
        policy: grid.contentHeight > grid.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff

        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: root.scrollbarWidth

        size: grid.height / Math.max(grid.contentHeight, 1)
        position: grid.visibleArea.yPosition
        onPositionChanged: {
            if (pressed) {
                grid.contentY = position * grid.contentHeight
            }
        }

        contentItem: Rectangle {
            implicitWidth: scrollbar.hovered || scrollbar.pressed ? 8 : 6
            radius: width / 2
            color: root.shade(Theme.foreground,
                              scrollbar.pressed ? 0.62
                                                : (scrollbar.hovered ? 0.46 : 0.32))
            Behavior on implicitWidth { NumberAnimation { duration: 120 } }
            Behavior on color { ColorAnimation { duration: 140 } }
        }

        background: Rectangle {
            implicitWidth: 8
            radius: width / 2
            color: root.shade(Theme.foreground, scrollbar.hovered ? 0.10 : 0.05)
            Behavior on color { ColorAnimation { duration: 140 } }
        }
    }
}
