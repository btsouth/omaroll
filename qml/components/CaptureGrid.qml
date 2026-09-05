import QtQuick
import QtQuick.Controls.Basic

// The library itself: a recycling grid, newest first, with the current day
// tracked as you scroll.
//
// GridView recycles delegates, so the cost of scrolling is set by what is on
// screen rather than by how large the library is.
FocusScope {
    id: root

    property var model: null
    property alias currentIndex: grid.currentIndex
    property alias count: grid.count
    property string currentDayLabel: ""
    property bool layoutReady: true

    // Multi-select is held by path, not by row. Sorting, filtering and rescans
    // all move rows around, and a selection that silently retargets is how bulk
    // delete trashes the wrong files.
    property var checkedSet: ({})
    property int checkedCount: 0
    property string selectedPath: ""
    property string layoutAnchorPath: ""
    property real layoutAnchorOffset: 0
    property bool restoringLayout: false

    signal chosen(int index)
    signal deleteRequested(string path)
    signal detailRequested(int index)

    // Include every intersecting cell, including the partial bottom row.
    // Cached offscreen delegates do not count, nor does an empty library.
    function viewportReady() {
        if (!visible || !layoutReady || grid.count === 0 || grid.width <= 0 || grid.height <= 0)
            return false
        const firstRow = Math.max(0, Math.floor(grid.contentY / grid.cellHeight))
        const lastRow = Math.ceil((grid.contentY + grid.height) / grid.cellHeight)
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
        checkedSet = next
        checkedCount = Object.keys(next).length
    }

    function clearChecked() {
        checkedSet = ({})
        checkedCount = 0
    }

    function checkAll() {
        const next = {}
        for (let i = 0; i < grid.count; i++) {
            next[Captures.pathAt(i)] = true
        }
        checkedSet = next
        checkedCount = Object.keys(next).length
    }

    function checkedPaths() { return Object.keys(checkedSet) }

    function rememberLayout() {
        restoringLayout = true
        const anchor = grid.indexAt(grid.contentX + 4, grid.contentY + 4)
        layoutAnchorPath = anchor >= 0 ? Captures.pathAt(anchor) : ""
        layoutAnchorOffset = anchor >= 0
                             ? grid.contentY - Math.floor(anchor / root.columns) * grid.cellHeight
                             : 0
    }

    function restoreLayout() {
        const selected = Captures.rowOf(selectedPath)
        if (selected >= 0) {
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
        if (anchor >= 0) {
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
    }
    Connections {
        target: Captures
        function onCountChanged() { root.pruneChecked() }
        function onLayoutAboutToBeChanged() { root.rememberLayout() }
        function onLayoutChanged() { root.restoreLayout() }
        function onModelAboutToBeReset() { root.rememberLayout() }
        function onModelReset() { root.restoreLayout() }
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
    onCellWidthChanged: relayoutTimer.restart()

    // GridView resizes existing delegates when cellWidth changes, but does not
    // reliably move them to their new columns. Reattach the same model after a
    // resize settles, preserving the user's place while positions are rebuilt.
    Timer {
        id: relayoutTimer
        interval: 80
        onTriggered: {
            if (grid.count === 0) {
                return
            }
            const selected = grid.currentIndex
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
                    grid.currentIndex = Math.min(selected, grid.count - 1)
                    grid.contentY = Math.max(0, Math.min(scroll, grid.contentHeight - grid.height))
                })
            })
        }
    }

    function updateDay() {
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
        // Left from the first tile lands on the last and Right from the last
        // on the first, matching the viewer's Previous and Next.
        keyNavigationWraps: true
        anchors.fill: parent
        anchors.rightMargin: root.scrollbarWidth
        focus: true
        clip: true

        model: root.layoutReady ? root.model : null
        reuseItems: true

        cellWidth: root.cellWidth
        cellHeight: Math.round(root.cellWidth * 0.68)

        cacheBuffer: cellHeight * 4
        boundsBehavior: Flickable.StopAtBounds
        highlight: null

        // A sort or filter change reshuffles rows; moving rather than snapping
        // makes it read as the same library reordering itself.
        displaced: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: MediaDates.indexing ? 0 : 200
                easing.type: Easing.OutQuad
            }
        }
        // Ctrl+wheel resizes the tiles; a plain wheel still scrolls, because
        // the handler leaves anything without Ctrl to the Flickable.
        WheelHandler {
            target: null
            acceptedModifiers: Qt.ControlModifier
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (event) {
                const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y
                Settings.tileWidth += delta > 0 ? root.tileStep : -root.tileStep
                event.accepted = true
            }
        }

        onContentYChanged: root.updateDay()
        onCountChanged: {
            // Removing the selected last row can leave GridView's index one
            // past the end until its next polish pass. Keep every keyboard
            // action on a real row immediately after a trash or rescan.
            if (count === 0) {
                currentIndex = -1
            } else if (currentIndex >= count) {
                currentIndex = count - 1
            }
            if (root.selectedPath === "" && currentIndex >= 0) {
                root.selectedPath = Captures.pathAt(currentIndex)
            }
            Qt.callLater(root.updateDay)
        }
        onCurrentIndexChanged: {
            if (!root.restoringLayout) {
                root.selectedPath = currentIndex >= 0 ? Captures.pathAt(currentIndex) : ""
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
            required property string sizeLabel
            required property bool isVideo
            required property bool isDocument
            required property bool favorite
            required property bool hidden
            required property double stamp
            required property string ocrSnippet

            width: grid.cellWidth
            height: grid.cellHeight

            CaptureCard {
                id: card
                anchors.fill: parent
                anchors.margins: Math.round(root.cellSpacing / 2)

                path: cell.path
                fileName: cell.fileName
                kindLabel: cell.kindLabel
                timeLabel: cell.timeLabel
                sizeLabel: cell.sizeLabel
                isVideo: cell.isVideo
                isDocument: cell.isDocument
                stamp: cell.stamp
                favorite: cell.favorite
                hiddenMark: cell.hidden
                ocrSnippet: cell.ocrSnippet
                selected: grid.currentIndex === cell.index
                checked: root.isChecked(cell.path)
                selectionMode: root.checkedCount > 0
                // Dragging a checked tile takes the whole selection with it.
                dragPaths: root.isChecked(cell.path) ? root.checkedPaths() : [cell.path]

                onActivated: {
                    grid.currentIndex = cell.index
                    grid.forceActiveFocus()
                }
                onChosen: {
                    grid.currentIndex = cell.index
                    root.detailRequested(cell.index)
                }
                onToggleChecked: root.toggleChecked(cell.path)
            }
        }

        // Arrows are handled by GridView itself; this adds the vim keys and the
        // ones that act on the selection. Single-letter action keys live in
        // Main so they work whether or not the grid has focus.
        Keys.onPressed: function (event) {
            switch (event.key) {
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (grid.currentIndex >= 0) {
                    root.detailRequested(grid.currentIndex)
                    event.accepted = true
                }
                break
            case Qt.Key_Space:
                if (grid.currentIndex >= 0) {
                    root.chosen(grid.currentIndex)
                    event.accepted = true
                }
                break
            case Qt.Key_Delete:
                if (grid.currentIndex >= 0) {
                    root.deleteRequested(Captures.pathAt(grid.currentIndex))
                    event.accepted = true
                }
                break
            case Qt.Key_X:
                if (grid.currentIndex >= 0) {
                    root.toggleChecked(Captures.pathAt(grid.currentIndex))
                    event.accepted = true
                }
                break
            case Qt.Key_H:
                grid.moveCurrentIndexLeft()
                event.accepted = true
                break
            case Qt.Key_J:
                grid.moveCurrentIndexDown()
                event.accepted = true
                break
            case Qt.Key_K:
                grid.moveCurrentIndexUp()
                event.accepted = true
                break
            case Qt.Key_L:
                grid.moveCurrentIndexRight()
                event.accepted = true
                break
            case Qt.Key_Home:
                grid.currentIndex = 0
                grid.positionViewAtBeginning()
                event.accepted = true
                break
            case Qt.Key_End:
                grid.currentIndex = grid.count - 1
                grid.positionViewAtEnd()
                event.accepted = true
                break
            }
        }
    }

    // Standalone rather than GridView's attached ScrollBar. The attached one is
    // reparented into the Flickable, so it cannot be anchored beside the grid;
    // driving a sibling keeps the bar in its own column where it can never sit
    // on top of a thumbnail.
    ScrollBar {
        id: scrollbar
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
